#include "ps_file.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* KOS on the console, the host's own filesystem on a development machine.
 *
 * The second one is not a courtesy. Directory listings, the size ceiling and
 * the escaping are all logic that a console can only answer slowly and one
 * page at a time, and every one of them is testable in a second here - see
 * tests/file-host. What is genuinely per-target is small and lives behind the
 * four functions below. */
#if defined(_arch_dreamcast)
#  define PS_FILE_KOS 1
#else
#  define PS_FILE_KOS 0
#endif

#if PS_FILE_KOS
#  include <kos.h>
#  include <sys/stat.h>
#  include <fcntl.h>
#  include <dc/cdrom.h>
#else
#  include <dirent.h>
#  include <sys/stat.h>
#endif

/* ------------------------------------------------------------------ buffer */

/* Growable text, for assembling a listing. Errors are sticky rather than
 * checked at every append: a page assembled from a hundred writes would
 * otherwise be a hundred branches, and there is exactly one thing to do when
 * any of them fails. */
typedef struct {
    char  *buf;
    size_t len, cap;
    int    err;
} txt;

/* A listing large enough to exceed this is one no layout engine on this
 * machine is going to finish, so the entry cap normally bites first. */
#define TXT_MAX (512 * 1024)

static void txt_add(txt *t, const char *s, size_t n)
{
    if(t->err)
        return;

    if(t->len + n + 1 > t->cap) {
        size_t want = t->cap ? t->cap * 2 : 4096;
        char  *p;

        while(want < t->len + n + 1)
            want *= 2;
        if(want > TXT_MAX) {
            t->err = 1;
            return;
        }
        p = (char *)realloc(t->buf, want);
        if(!p) {
            t->err = 1;
            return;
        }
        t->buf = p;
        t->cap = want;
    }

    memcpy(t->buf + t->len, s, n);
    t->len += n;
    t->buf[t->len] = '\0';
}

static void txt_str(txt *t, const char *s)
{
    txt_add(t, s, strlen(s));
}

static void txt_fmt(txt *t, const char *fmt, ...)
{
    char    tmp[512];
    va_list ap;
    int     n;

    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);

    if(n < 0 || (size_t)n >= sizeof tmp) {
        t->err = 1;
        return;
    }
    txt_add(t, tmp, (size_t)n);
}

/* A filename is not markup, and on a card it can contain anything. */
static void txt_escape(txt *t, const char *s)
{
    for(; *s; s++) {
        switch(*s) {
        case '&':  txt_str(t, "&amp;");  break;
        case '<':  txt_str(t, "&lt;");   break;
        case '>':  txt_str(t, "&gt;");   break;
        case '"':  txt_str(t, "&quot;"); break;
        default:   txt_add(t, s, 1);     break;
        }
    }
}

/* ----------------------------------------------------------------- listing */

/* Names are packed rather than held in a NAME_MAX array each: 512 entries of
 * 256 bytes is a third of a megabyte of transient allocation to show a
 * directory, on a machine where that is a real fraction of what a page gets. */
#define NAME_POOL (24 * 1024)

typedef struct {
    uint32_t off;
    int      is_dir;
    long     size;
} dirent_row;

struct ps_dirpage {
    char       path[PS_URL_PATH_MAX];   /* URL path, with trailing slash */
    int        have_parent;

    dirent_row ent[PS_FILE_MAX_ENTRIES];
    int        nent;
    int        dropped;                 /* entries the caps refused */

    char       pool[NAME_POOL];
    size_t     pool_len;
};

static const char *row_name(const ps_dirpage *d, const dirent_row *r)
{
    return d->pool + r->off;
}

/* Directories first, then names, case-insensitively. A card's directory comes
 * back in whatever order it was written, which for browsing is no order at
 * all. */
static int row_before(const ps_dirpage *d, const dirent_row *a,
                      const char *name, int is_dir)
{
    if(a->is_dir != is_dir)
        return a->is_dir > is_dir;
    return strcasecmp(row_name(d, a), name) <= 0;
}

ps_dirpage *ps_dirpage_begin(const ps_url *dir)
{
    ps_dirpage *d;
    char        root[PS_URL_PATH_MAX];

    if(!dir || !ps_url_is_local(dir))
        return NULL;

    d = (ps_dirpage *)calloc(1, sizeof *d);
    if(!d)
        return NULL;

    if(snprintf(d->path, sizeof d->path, "%s", dir->path) >=
       (int)sizeof d->path) {
        free(d);
        return NULL;
    }
    {
        char  *cut = strpbrk(d->path, "?#");
        size_t n;

        /* A local URL can still be carrying a cache-buster, because pages get
         * written for the network and copied onto a disc. It is not part of
         * any name shown or linked here. */
        if(cut)
            *cut = '\0';

        /* Every href on the page is relative to this, so it has to name the
         * directory and not the last thing in it. */
        n = strlen(d->path);
        if(n && d->path[n - 1] != '/' && n + 1 < sizeof d->path) {
            d->path[n]     = '/';
            d->path[n + 1] = '\0';
        }
    }

    /* Offering "up" from the mount root would be a link back to the page you
     * are on - the clamp in ps_url.c makes sure of that - and a link that
     * does nothing is worse than no link. */
    if(ps_url_local_root(dir, root, sizeof root) == 0) {
        size_t rn = strlen(root);

        d->have_parent = strlen(d->path) > rn + 1;
    }

    return d;
}

void ps_dirpage_add(ps_dirpage *d, const char *name, int is_dir, long size)
{
    size_t n;
    int    i;

    if(!d || !name || !*name)
        return;

    /* "." and ".." are the filesystem's own bookkeeping and navigating to
     * them is what the header row is for. */
    if(!strcmp(name, ".") || !strcmp(name, ".."))
        return;

    n = strlen(name);
    if(d->nent >= PS_FILE_MAX_ENTRIES || d->pool_len + n + 1 > sizeof d->pool) {
        d->dropped++;
        return;
    }

    for(i = d->nent; i > 0; i--) {
        if(row_before(d, &d->ent[i - 1], name, is_dir))
            break;
        d->ent[i] = d->ent[i - 1];
    }

    d->ent[i].off    = (uint32_t)d->pool_len;
    d->ent[i].is_dir = is_dir ? 1 : 0;
    d->ent[i].size   = size;
    d->nent++;

    memcpy(d->pool + d->pool_len, name, n + 1);
    d->pool_len += n + 1;
}

/* Sizes people can read at television distance. Exact bytes matter for a
 * handful of files and are noise for the rest. */
static void size_text(long size, char *buf, size_t buflen)
{
    if(size < 0)
        snprintf(buf, buflen, "&nbsp;");
    else if(size < 1024)
        snprintf(buf, buflen, "%ld bytes", size);
    else if(size < 1024 * 1024)
        snprintf(buf, buflen, "%ld KB", (size + 512) / 1024);
    else
        snprintf(buf, buflen, "%ld.%ld MB", size / (1024 * 1024),
                 ((size % (1024 * 1024)) * 10) / (1024 * 1024));
}

/* The path as a row of links, one per directory above this one.
 *
 * Two things at once, and both are needed. A path is a single unbreakable word
 * to a layout engine, so a deep one on a 640 pixel screen makes the page wider
 * than the display and the end of it - the part you are looking at - is what
 * falls off; separate links give it somewhere to wrap. And climbing four
 * levels on a d-pad is four selections through "up one level" otherwise.
 *
 * The hrefs are the local rule in miniature: they are absolute, so they are
 * relative to the mount and must not repeat it. The mount's own link is "/". */
static void crumbs(txt *t, const ps_dirpage *d)
{
    const char *p = d->path + 1;
    char        rel[PS_URL_PATH_MAX];
    size_t      rl  = 0;
    int         idx = 0;

    rel[0] = '\0';
    txt_str(t, "/");

    while(*p) {
        const char *slash = strchr(p, '/');
        size_t      n     = slash ? (size_t)(slash - p) : strlen(p);
        char        seg[256], enc[768];

        if(!n || n >= sizeof seg)
            break;
        memcpy(seg, p, n);
        seg[n] = '\0';

        if(ps_url_encode_segment(seg, enc, sizeof enc) < 0)
            break;

        if(idx) {
            if(rl + 1 + strlen(enc) >= sizeof rel)
                break;
            rel[rl++] = '/';
            strcpy(rel + rl, enc);
            rl += strlen(enc);
        }

        txt_str(t, "<a href=\"");
        txt_str(t, idx ? rel : "");
        txt_str(t, "/\">");
        txt_escape(t, seg);
        txt_str(t, "</a>/");

        idx++;
        if(!slash)
            break;
        p = slash + 1;
    }
}

char *ps_dirpage_finish(ps_dirpage *d, size_t *len)
{
    txt  t = { 0 };
    char href[PS_URL_PATH_MAX];
    int  i;

    if(!d)
        return NULL;

    /* Matches cd/sites.html: table layout and bgcolor rather than a
     * stylesheet, because that is what the rest of the browser's own pages
     * use and what this renderer is certain to get right. */
    txt_str(&t, "<!doctype html><html><head><title>");
    txt_escape(&t, d->path);
    txt_str(&t, "</title></head>"
                "<body bgcolor=\"#101418\" text=\"#e8e8e8\" link=\"#ffc447\" "
                "vlink=\"#c89a2f\">"
                "<table cellspacing=\"0\" cellpadding=\"8\" border=\"0\" "
                "width=\"600\"><tr><td bgcolor=\"#1c242c\" "
                "style=\"height:40px\"><font size=\"5\" face=\"sans\"><b>");
    crumbs(&t, d);
    txt_str(&t, "</b></font></td></tr></table><br>"
                "<table cellspacing=\"0\" cellpadding=\"6\" border=\"0\" "
                "width=\"600\">");

    if(d->have_parent)
        txt_str(&t, "<tr><td bgcolor=\"#18202a\" width=\"400\" "
                    "style=\"height:36px\"><font size=\"3\">"
                    "<a href=\"../\">Up one level</a></font></td>"
                    "<td bgcolor=\"#141a20\"><font size=\"2\" "
                    "color=\"#8a949e\">&nbsp;</font></td></tr>");

    for(i = 0; i < d->nent; i++) {
        const char *name = row_name(d, &d->ent[i]);
        char        sz[32];

        if(ps_url_encode_segment(name, href, sizeof href) < 0)
            continue;

        size_text(d->ent[i].is_dir ? -1 : d->ent[i].size, sz, sizeof sz);

        txt_str(&t, "<tr><td bgcolor=\"#18202a\" width=\"400\" "
                    "style=\"height:36px\"><font size=\"3\"><a href=\"");
        txt_str(&t, href);
        /* The trailing slash is what makes the listing of a subdirectory
         * resolve its own links inside itself rather than beside it. */
        if(d->ent[i].is_dir)
            txt_str(&t, "/");
        txt_str(&t, "\">");
        txt_escape(&t, name);
        if(d->ent[i].is_dir)
            txt_str(&t, "/");
        txt_str(&t, "</a></font></td><td bgcolor=\"#141a20\">"
                    "<font size=\"2\" color=\"#8a949e\">");
        txt_str(&t, d->ent[i].is_dir ? "folder" : sz);
        txt_str(&t, "</font></td></tr>");
    }

    if(!d->nent)
        txt_str(&t, "<tr><td bgcolor=\"#141a20\"><font size=\"3\" "
                    "color=\"#8a949e\">Empty</font></td></tr>");

    txt_str(&t, "</table>");

    /* Saying so beats a listing that is quietly short: someone looking for a
     * file that is on the card would otherwise conclude it is not. */
    if(d->dropped)
        txt_fmt(&t, "<br><font size=\"2\" color=\"#8a949e\">%d more not "
                    "shown.</font>", d->dropped);

    txt_str(&t, "</body></html>");
    free(d);

    if(t.err) {
        free(t.buf);
        return NULL;
    }
    if(len)
        *len = t.len;
    return t.buf;
}

void ps_dirpage_abort(ps_dirpage *d)
{
    free(d);
}

/* -------------------------------------------------------------- filesystem */

typedef enum { NODE_NONE, NODE_FILE, NODE_DIR } node_kind;

#if PS_FILE_KOS

/* An empty drive does not say so. cdrom_get_status answers immediately, where
 * a read of an absent disc retries for long enough to look like the browser
 * has hung - which is the bug shell/main.c's load_boot_asset was written
 * around, and the reason this check is here rather than left to fs_open. */
static int mount_ready(const char *root)
{
    if(!strcmp(root, "/cd")) {
        int status = 0, type = 0;

        if(cdrom_get_status(&status, &type) < 0)
            return 0;
        if(status == CD_STATUS_NO_DISC || status == CD_STATUS_OPEN)
            return 0;
        return 1;
    }

    /* Everything else is a VFS handler that is either attached or is not, and
     * a lookup against a name nothing claims fails at once. */
    return 1;
}

static node_kind probe(const char *path, long *size)
{
    struct stat st;
    file_t      f;

    *size = -1;

    if(fs_stat(path, &st, 0) == 0) {
        if(S_ISDIR(st.st_mode))
            return NODE_DIR;
        *size = (long)st.st_size;
        return NODE_FILE;
    }

    /* Not every VFS handler implements stat, and the ones that do not are the
     * interesting ones - dcload's /pc among them. Opening is the fallback that
     * always works, at the cost of doing it twice for a file. */
    f = fs_open(path, O_RDONLY | O_DIR);
    if(f != FILEHND_INVALID) {
        fs_close(f);
        return NODE_DIR;
    }

    f = fs_open(path, O_RDONLY);
    if(f != FILEHND_INVALID) {
        ssize_t total = fs_total(f);

        fs_close(f);
        if(total >= 0)
            *size = (long)total;
        return NODE_FILE;
    }

    return NODE_NONE;
}

static ps_file_result read_file(const char *path, char **out, size_t *out_len)
{
    file_t  f;
    ssize_t total;
    char   *buf;
    size_t  got = 0;

    f = fs_open(path, O_RDONLY);
    if(f == FILEHND_INVALID)
        return PS_FILE_ERR_NOT_FOUND;

    total = fs_total(f);
    if(total < 0) {
        fs_close(f);
        return PS_FILE_ERR_IO;
    }
    /* Refused before the allocation, not after: on a machine with no virtual
     * memory, asking for six megabytes to find out it is too big is the
     * failure the cap exists to avoid. */
    if((size_t)total > PS_FILE_MAX_BYTES) {
        fs_close(f);
        return PS_FILE_ERR_TOO_LARGE;
    }

    /* One byte over, and NUL: the HTML and CSS parsers are handed this as a
     * string, exactly as they are handed an HTTP body. */
    buf = (char *)malloc((size_t)total + 1);
    if(!buf) {
        fs_close(f);
        return PS_FILE_ERR_MEMORY;
    }

    while(got < (size_t)total) {
        ssize_t n = fs_read(f, buf + got, (size_t)total - got);

        if(n <= 0)
            break;
        got += (size_t)n;
    }
    fs_close(f);

    if(got != (size_t)total) {
        free(buf);
        return PS_FILE_ERR_IO;
    }

    buf[got] = '\0';
    *out     = buf;
    *out_len = got;
    return PS_FILE_OK;
}

static ps_file_result walk_dir(const char *path, ps_dirpage *d)
{
    file_t           f = fs_open(path, O_RDONLY | O_DIR);
    const dirent_t  *e;

    if(f == FILEHND_INVALID)
        return PS_FILE_ERR_NOT_FOUND;

    while((e = fs_readdir(f)) != NULL) {
        /* KOS reports a directory as a negative size, which is also how a
         * filesystem that cannot say reports an unknown one. */
        int is_dir = e->size < 0;

        ps_dirpage_add(d, e->name, is_dir, is_dir ? -1 : (long)e->size);
    }

    fs_close(f);
    return PS_FILE_OK;
}

#else /* host */

static int mount_ready(const char *root)
{
    (void)root;
    return 1;
}

static node_kind probe(const char *path, long *size)
{
    struct stat st;

    *size = -1;
    if(stat(path, &st) != 0)
        return NODE_NONE;
    if(S_ISDIR(st.st_mode))
        return NODE_DIR;
    *size = (long)st.st_size;
    return NODE_FILE;
}

static ps_file_result read_file(const char *path, char **out, size_t *out_len)
{
    FILE  *f = fopen(path, "rb");
    long   total;
    char  *buf;
    size_t got;

    if(!f)
        return PS_FILE_ERR_NOT_FOUND;

    if(fseek(f, 0, SEEK_END) != 0 || (total = ftell(f)) < 0) {
        fclose(f);
        return PS_FILE_ERR_IO;
    }
    rewind(f);

    if((size_t)total > PS_FILE_MAX_BYTES) {
        fclose(f);
        return PS_FILE_ERR_TOO_LARGE;
    }

    buf = (char *)malloc((size_t)total + 1);
    if(!buf) {
        fclose(f);
        return PS_FILE_ERR_MEMORY;
    }

    got = fread(buf, 1, (size_t)total, f);
    fclose(f);

    if(got != (size_t)total) {
        free(buf);
        return PS_FILE_ERR_IO;
    }

    buf[got] = '\0';
    *out     = buf;
    *out_len = got;
    return PS_FILE_OK;
}

static ps_file_result walk_dir(const char *path, ps_dirpage *d)
{
    DIR           *dir = opendir(path);
    struct dirent *e;

    if(!dir)
        return PS_FILE_ERR_NOT_FOUND;

    while((e = readdir(dir)) != NULL) {
        char        full[1024];
        long        size;
        node_kind   k;

        if(snprintf(full, sizeof full, "%s/%s", path, e->d_name) >=
           (int)sizeof full)
            continue;
        k = probe(full, &size);
        ps_dirpage_add(d, e->d_name, k == NODE_DIR, size);
    }

    closedir(dir);
    return PS_FILE_OK;
}

#endif

/* ------------------------------------------------------------------- fetch */

ps_file_result ps_file_fetch(const ps_url *u, ps_file_response *out)
{
    char      path[PS_URL_PATH_MAX];
    char      root[PS_URL_PATH_MAX];
    long      size;
    node_kind kind;

    if(!out)
        return PS_FILE_ERR_PATH;

    memset(out, 0, sizeof *out);

    if(!u || !ps_url_is_local(u))
        return PS_FILE_ERR_PATH;
    if(ps_url_local_path(u, path, sizeof path) != 0)
        return PS_FILE_ERR_PATH;

    /* The filesystem root is every mount at once and belongs to none, so there
     * is nothing to check the device of - listing it is how you find out which
     * mounts exist. */
    if(ps_url_local_root(u, root, sizeof root) == 0 && !mount_ready(root))
        return PS_FILE_ERR_NO_DEVICE;

    kind = probe(path, &size);
    if(kind == NODE_NONE)
        return PS_FILE_ERR_NOT_FOUND;

    if(kind == NODE_DIR) {
        /* A directory's URL has to end in a slash and carry nothing after the
         * path before anything on the generated page resolves against it: this
         * string becomes the document base, and ps_url_resolve reads the
         * directory off it by looking for the last slash. */
        ps_url         canon = *u;
        char          *cut   = strpbrk(canon.path, "?#");
        size_t         cl;
        ps_dirpage    *d;
        ps_file_result rc;
        size_t         n;

        if(cut)
            *cut = '\0';
        cl = strlen(canon.path);
        if(cl && canon.path[cl - 1] != '/' && cl + 1 < sizeof canon.path) {
            canon.path[cl]     = '/';
            canon.path[cl + 1] = '\0';
        }
        if(ps_url_format(&canon, out->final_url, sizeof out->final_url) < 0)
            return PS_FILE_ERR_PATH;

        d = ps_dirpage_begin(&canon);
        if(!d)
            return PS_FILE_ERR_MEMORY;

        rc = walk_dir(path, d);
        if(rc != PS_FILE_OK) {
            ps_dirpage_abort(d);
            return rc;
        }

        out->data = ps_dirpage_finish(d, &n);
        if(!out->data)
            return PS_FILE_ERR_MEMORY;
        out->len    = n;
        out->is_dir = 1;
        return PS_FILE_OK;
    }

    if(ps_url_format(u, out->final_url, sizeof out->final_url) < 0)
        return PS_FILE_ERR_PATH;

    if(size >= 0 && (size_t)size > PS_FILE_MAX_BYTES)
        return PS_FILE_ERR_TOO_LARGE;

    {
        ps_file_result rc = read_file(path, &out->data, &out->len);

        if(rc != PS_FILE_OK)
            memset(out, 0, sizeof *out);
        return rc;
    }
}

void ps_file_response_free(ps_file_response *r)
{
    if(!r)
        return;
    free(r->data);
    memset(r, 0, sizeof *r);
}

const char *ps_file_strerror(ps_file_result r)
{
    switch(r) {
    case PS_FILE_OK:            return "ok";
    case PS_FILE_ERR_PATH:      return "not a path we can open";
    case PS_FILE_ERR_NOT_FOUND: return "no such file";
    case PS_FILE_ERR_NO_DEVICE: return "no disc or card";
    case PS_FILE_ERR_TOO_LARGE: return "file too large";
    case PS_FILE_ERR_MEMORY:    return "out of memory";
    case PS_FILE_ERR_IO:        return "read failed";
    }
    return "unknown error";
}
