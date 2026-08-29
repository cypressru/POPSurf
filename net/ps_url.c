#include "ps_url.h"

#include <string.h>
#include <stdio.h>
#include <ctype.h>

static int default_port(const char *scheme)
{
    if(!strcmp(scheme, "https"))
        return 443;
    return 80;
}

/* Collapses "." and ".." segments. Operates in place. A ".." that would climb
 * above the root is dropped, matching what browsers do; letting it escape
 * would let a redirect walk outside the origin's path space.
 *
 * keep is how many leading segments are floor rather than path. It is zero for
 * a network URL, where the host is the origin and the path may collapse to
 * nothing, and one for a local file, where the mount is the origin and lives
 * inside the path - so "/sd/../cd/x" has to come out as something under /sd
 * rather than reaching another card, disc or the development host. */
static void normalize_path(char *path, int keep)
{
    char *segs[64];
    int   nseg = 0;
    char *p, *next;
    char  out[PS_URL_PATH_MAX];
    size_t len = 0;
    int    i;
    int    trailing, dotted = 0;

    if(path[0] != '/')
        return;

    /* Whether this named a directory, which the walk below must preserve: a
     * relative link on a directory listing resolves inside the directory only
     * if the base still ends in a slash. A final "." or ".." is a directory
     * reference too - "a/b/.." is "a/", not "a" - while one in the middle is
     * not, so it is the last segment that decides. */
    {
        size_t n = strlen(path);

        trailing = n > 1 && path[n - 1] == '/';
    }

    for(p = path + 1; p && *p; p = next) {
        char *slash = strchr(p, '/');

        if(slash) {
            *slash = '\0';
            next   = slash + 1;
        }
        else {
            next = NULL;
        }

        if(!strcmp(p, ".")) {
            dotted = 1;
        }
        else if(!*p) {
            /* Empty segments contribute nothing; a trailing one is the
             * trailing slash already recorded above. */
        }
        else if(!strcmp(p, "..")) {
            if(nseg > keep)
                nseg--;
            dotted = 1;
        }
        else if(nseg < (int)(sizeof segs / sizeof segs[0])) {
            segs[nseg++] = p;
            dotted       = 0;
        }
    }

    out[len++] = '/';
    for(i = 0; i < nseg; i++) {
        size_t sl = strlen(segs[i]);

        if(len + sl + 2 >= sizeof out)
            break;
        memcpy(out + len, segs[i], sl);
        len += sl;
        if(i + 1 < nseg)
            out[len++] = '/';
    }
    if((trailing || dotted) && nseg > 0 && len + 1 < sizeof out)
        out[len++] = '/';
    out[len] = '\0';

    memcpy(path, out, len + 1);
}

static int is_file_scheme(const char *scheme)
{
    return !strcmp(scheme, "file");
}

/* Segments a local path keeps out of reach of "..": the mount. */
static int floor_segs(const char *scheme)
{
    return is_file_scheme(scheme) ? 1 : 0;
}

int ps_url_parse(ps_url *out, const char *url)
{
    const char *p, *host_start, *host_end, *path_start;
    size_t      n;

    if(!out || !url)
        return -1;

    memset(out, 0, sizeof *out);

    /* A bare absolute path is local media. Checked before the scheme, so a
     * filename containing "://" cannot be mistaken for one.
     *
     * Two slashes is not one of these. It is a scheme-relative reference, and
     * a reference has no meaning without the base that ps_url_resolve has and
     * this does not; reading it as the path "/host/x" would open something
     * local under a name that plainly says otherwise. */
    if(url[0] == '/' && url[1] != '/') {
        if(strlen(url) >= sizeof out->path)
            return -1;
        strcpy(out->scheme, "file");
        out->port = default_port(out->scheme);
        strcpy(out->path, url);

        {
            char *hash = strchr(out->path, '#');
            if(hash)
                *hash = '\0';
        }
        normalize_path(out->path, floor_segs(out->scheme));
        return 0;
    }

    p = strstr(url, "://");
    if(!p)
        return -1;

    n = (size_t)(p - url);
    if(n == 0 || n >= sizeof out->scheme)
        return -1;
    memcpy(out->scheme, url, n);
    out->scheme[n] = '\0';
    for(size_t i = 0; i < n; i++)
        out->scheme[i] = (char)tolower((unsigned char)out->scheme[i]);

    host_start = p + 3;

    /* Userinfo is not supported; it is a phishing vector far more often than
     * it is a real credential on the retro web. */
    path_start = strchr(host_start, '/');
    host_end   = path_start ? path_start : host_start + strlen(host_start);

    {
        const char *colon = memchr(host_start, ':', (size_t)(host_end - host_start));

        if(colon) {
            int port = 0;
            const char *q;

            for(q = colon + 1; q < host_end; q++) {
                if(!isdigit((unsigned char)*q))
                    return -1;
                port = port * 10 + (*q - '0');
                if(port > 65535)
                    return -1;
            }
            out->port = port ? port : default_port(out->scheme);
            host_end  = colon;
        }
        else {
            out->port = default_port(out->scheme);
        }
    }

    n = (size_t)(host_end - host_start);

    /* file:// URLs have no authority: "file:///rd/page.html" is host-less by
     * construction. Every other scheme needs one. */
    if(n == 0 && !is_file_scheme(out->scheme))
        return -1;
    if(n >= sizeof out->host)
        return -1;
    memcpy(out->host, host_start, n);
    out->host[n] = '\0';
    for(size_t i = 0; i < n; i++)
        out->host[i] = (char)tolower((unsigned char)out->host[i]);

    /* "file://server/share" is somebody else's machine, and there is no
     * protocol here that could reach it. Refusing is honest; opening the local
     * path instead would silently serve different content than was named.
     * "localhost" is the one spelling RFC 8089 says means this machine, so it
     * is accepted and dropped, leaving one canonical form to compare. */
    if(is_file_scheme(out->scheme) && out->host[0]) {
        if(strcmp(out->host, "localhost"))
            return -1;
        out->host[0] = '\0';
    }

    if(path_start) {
        n = strlen(path_start);
        if(n >= sizeof out->path)
            return -1;
        memcpy(out->path, path_start, n + 1);
    }
    else {
        out->path[0] = '/';
        out->path[1] = '\0';
    }

    /* Fragments never go on the wire. */
    {
        char *hash = strchr(out->path, '#');
        if(hash)
            *hash = '\0';
    }

    normalize_path(out->path, floor_segs(out->scheme));
    return 0;
}

int ps_url_resolve(ps_url *out, const ps_url *base, const char *ref)
{
    if(!out || !base || !ref)
        return -1;

    /* Absolute. */
    if(strstr(ref, "://"))
        return ps_url_parse(out, ref);

    /* Scheme-relative. */
    if(ref[0] == '/' && ref[1] == '/') {
        char tmp[PS_URL_MAX];

        if((size_t)snprintf(tmp, sizeof tmp, "%s:%s", base->scheme, ref) >= sizeof tmp)
            return -1;
        return ps_url_parse(out, tmp);
    }

    *out = *base;

    if(ref[0] == '/') {
        char root[PS_URL_PATH_MAX];

        /* Absolute within the origin. For HTTP the origin is the host and the
         * path root is the path root; for local media the origin is the mount,
         * so "/img/logo.png" on a page from the SD card is the card's /img and
         * not the filesystem's - which would be a different device. */
        root[0] = '\0';
        if(ps_url_is_local(base))
            (void)ps_url_local_root(base, root, sizeof root);

        if(strlen(root) + strlen(ref) >= sizeof out->path)
            return -1;
        strcpy(out->path, root);
        strcat(out->path, ref);
    }
    else {
        /* Relative to the base's directory, which is everything up to and
         * including its last slash. */
        char  dir[PS_URL_PATH_MAX];
        char *slash;

        strcpy(dir, base->path);
        slash = strrchr(dir, '/');
        if(slash)
            slash[1] = '\0';
        else
            strcpy(dir, "/");

        if(strlen(dir) + strlen(ref) >= sizeof out->path)
            return -1;

        strcpy(out->path, dir);
        strcat(out->path, ref);
    }

    {
        char *hash = strchr(out->path, '#');
        if(hash)
            *hash = '\0';
    }

    normalize_path(out->path, floor_segs(out->scheme));
    return 0;
}

int ps_url_format(const ps_url *u, char *buf, size_t buflen)
{
    int n;

    if(!u || !buf)
        return -1;

    if(u->port == default_port(u->scheme))
        n = snprintf(buf, buflen, "%s://%s%s", u->scheme, u->host, u->path);
    else
        n = snprintf(buf, buflen, "%s://%s:%d%s", u->scheme, u->host, u->port,
                     u->path);

    if(n < 0 || (size_t)n >= buflen)
        return -1;
    return n;
}

/* ------------------------------------------------------------- local media */

int ps_url_is_local(const ps_url *u)
{
    return u && is_file_scheme(u->scheme);
}

/* Where the path stops being a path: a query or fragment is server syntax that
 * a filesystem has no idea about, and pages written for the network hang
 * cache-busters off local URLs regardless. */
static size_t path_len(const char *path)
{
    const char *end = strpbrk(path, "?#");

    return end ? (size_t)(end - path) : strlen(path);
}

int ps_url_local_root(const ps_url *u, char *buf, size_t buflen)
{
    const char *seg;
    const char *slash;
    size_t      n;

    if(!u || !buf || !buflen || !ps_url_is_local(u) || u->path[0] != '/')
        return -1;

    seg   = u->path + 1;
    n     = path_len(seg);
    slash = memchr(seg, '/', n);
    if(slash)
        n = (size_t)(slash - seg);

    /* No first segment means the filesystem root, which is every mount at once
     * and therefore no origin at all. */
    if(!n || n + 2 > buflen)
        return -1;

    buf[0] = '/';
    memcpy(buf + 1, seg, n);
    buf[n + 1] = '\0';
    return 0;
}

static int hexval(int c)
{
    if(c >= '0' && c <= '9')
        return c - '0';
    if(c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if(c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

int ps_url_local_path(const ps_url *u, char *buf, size_t buflen)
{
    char        tmp[PS_URL_PATH_MAX];
    const char *p;
    size_t      o = 0, n;

    if(!u || !buf || !buflen || !ps_url_is_local(u) || u->path[0] != '/')
        return -1;

    for(p = u->path; *p && *p != '?' && *p != '#'; p++) {
        int c = (unsigned char)*p;

        if(c == '%' && hexval((unsigned char)p[1]) >= 0 &&
           hexval((unsigned char)p[2]) >= 0) {
            c = hexval((unsigned char)p[1]) * 16 + hexval((unsigned char)p[2]);
            p += 2;

            /* An encoded NUL would end the string handed to the VFS early, so
             * the file opened is not the file named. */
            if(c == 0)
                return -1;
        }
        if(o + 1 >= sizeof tmp)
            return -1;
        tmp[o++] = (char)c;
    }
    tmp[o] = '\0';

    /* Containment is re-checked here and not only at parse, because "%2e%2e%2f"
     * is a "../" that did not exist when the path was normalised. Decoding
     * without re-clamping is the whole traversal bug. */
    normalize_path(tmp, 1);

    /* The trailing slash is URL syntax and carries meaning there - it is what
     * makes a relative link on a listing resolve inside the directory. To
     * fs_open it is at best noise and on some of these filesystems an error. */
    n = strlen(tmp);
    while(n > 1 && tmp[n - 1] == '/')
        tmp[--n] = '\0';

    if(n >= buflen)
        return -1;
    memcpy(buf, tmp, n + 1);
    return 0;
}

int ps_url_encode_segment(const char *name, char *buf, size_t buflen)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t            o     = 0;

    if(!name || !buf || !buflen)
        return -1;

    for(; *name; name++) {
        unsigned char c = (unsigned char)*name;

        /* RFC 3986 unreserved, and nothing else. Being generous here would
         * mean deciding which of '?', '#', ';' and '&' a filesystem is allowed
         * to put in a name, and the answer on FAT is "most of them". */
        if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
           c == '~') {
            if(o + 1 >= buflen)
                return -1;
            buf[o++] = (char)c;
        }
        else {
            if(o + 3 >= buflen)
                return -1;
            buf[o++] = '%';
            buf[o++] = hex[c >> 4];
            buf[o++] = hex[c & 0x0f];
        }
    }

    buf[o] = '\0';
    return (int)o;
}
