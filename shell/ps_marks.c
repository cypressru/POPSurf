#include "ps_marks.h"

#include <kos.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <dc/vmu_pkg.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* VMU filenames are 12 characters, uppercase by convention because that is how
 * the console's own file manager renders them. */
#define MARKS_FILE "POPSURFMARKS"

/* --- payload format ------------------------------------------------------
 *
 * Fixed-width records, little-endian counts, read through the same accessors
 * as every other baked format here (ps_types.h). A memory card outlives the
 * build that wrote it, so the version byte is not optional: a card written by
 * a later build has to be recognised and declined rather than misread as
 * garbage bookmarks. */

#define MARKS_MAGIC   "PSBM"
#define MARKS_VERSION 1

#define REC_SIZE  (PS_MARK_TITLE_MAX + PS_MARK_URL_MAX)
#define HDR_SIZE  8
#define BLOB_SIZE (HDR_SIZE + PS_MARKS_MAX * REC_SIZE)

/* --- icon ----------------------------------------------------------------
 *
 * Generated rather than baked into an asset: it is a frame and three bars, and
 * a 512-byte array in the source would be 512 bytes nobody could read or
 * change.
 *
 * This is the toolbar's hamburger, not the screensaver's KallistiOS apple. The
 * two are different marks doing different jobs - this one sits in a list of
 * save files next to Shenmue and Crazy Taxi and has to say "the browser's
 * bookmarks", where the apple would name KallistiOS instead. */

#define ICON_W 32
#define ICON_H 32

static void icon_px(uint8_t *icon, int x, int y, int colour)
{
    /* 4bpp, two pixels per byte, low nibble first. */
    int i = (y * ICON_W + x) / 2;

    if(x & 1)
        icon[i] = (uint8_t)((icon[i] & 0x0f) | (colour << 4));
    else
        icon[i] = (uint8_t)((icon[i] & 0xf0) | colour);
}

static void icon_build(uint8_t *icon, uint16_t *pal)
{
    int i, x, y;

    memset(icon, 0, 512);

    /* ARGB4444. Index 0 is opaque near-black rather than transparent, because
     * the file manager draws these over its own background and a transparent
     * icon reads as a hole. */
    pal[0] = 0xf111;
    pal[1] = 0xffc4;   /* the amber accent */
    pal[2] = 0xf963;   /* the dim accent */
    for(i = 3; i < 16; i++)
        pal[i] = 0xf000;

    for(x = 2; x < ICON_W - 2; x++) {
        icon_px(icon, x, 2, 1);
        icon_px(icon, x, 3, 1);
        icon_px(icon, x, ICON_H - 4, 1);
        icon_px(icon, x, ICON_H - 3, 1);
    }
    for(y = 2; y < ICON_H - 2; y++) {
        icon_px(icon, 2, y, 1);
        icon_px(icon, 3, y, 1);
        icon_px(icon, ICON_W - 4, y, 1);
        icon_px(icon, ICON_W - 3, y, 1);
    }

    for(i = 0; i < 3; i++) {
        for(x = 9; x < ICON_W - 9; x++) {
            icon_px(icon, x, 10 + i * 5, 2);
            icon_px(icon, x, 11 + i * 5, 2);
        }
    }
}

/* --- card ---------------------------------------------------------------- */

/* First memory card on the bus. Any port, any unit: people put the card in
 * whichever slot the controller has free, and a browser that only looks in
 * A1 would tell half of them they have no card. */
static void find_card(ps_marks *m)
{
    maple_device_t *dev;
    int             i;

    m->path[0] = '\0';

    for(i = 0; (dev = maple_enum_type(i, MAPLE_FUNC_MEMCARD)) != NULL; i++) {
        snprintf(m->path, sizeof m->path, "/vmu/%c%d",
                 (char)('a' + dev->port), dev->unit);
        printf("popsurf: memory card at %s\n", m->path);
        return;
    }

    printf("popsurf: no memory card; bookmarks last this session only\n");
}

/* --- load ---------------------------------------------------------------- */

static void parse_blob(ps_marks *m, const uint8_t *blob, size_t len)
{
    int n, i;

    if(len < HDR_SIZE || memcmp(blob, MARKS_MAGIC, 4) != 0)
        return;

    if(blob[4] != MARKS_VERSION) {
        printf("popsurf: bookmark file is version %d, this build reads %d; "
               "leaving it alone\n", blob[4], MARKS_VERSION);
        return;
    }

    n = blob[5];
    if(n > PS_MARKS_MAX)
        n = PS_MARKS_MAX;

    for(i = 0; i < n; i++) {
        const uint8_t *rec = blob + HDR_SIZE + (size_t)i * REC_SIZE;

        if((size_t)(HDR_SIZE + (i + 1) * REC_SIZE) > len)
            break;

        /* Copied through snprintf rather than memcpy: the card is storage the
         * user can edit with other software, and an unterminated record must
         * not walk off the end of the struct. */
        snprintf(m->list[m->count].title, PS_MARK_TITLE_MAX, "%.*s",
                 PS_MARK_TITLE_MAX - 1, (const char *)rec);
        snprintf(m->list[m->count].url, PS_MARK_URL_MAX, "%.*s",
                 PS_MARK_URL_MAX - 1,
                 (const char *)rec + PS_MARK_TITLE_MAX);

        if(m->list[m->count].url[0])
            m->count++;
    }

    printf("popsurf: %d bookmark%s loaded\n", m->count,
           m->count == 1 ? "" : "s");
}

static void load(ps_marks *m)
{
    char     path[32];
    void    *raw = NULL;
    ssize_t  len;
    vmu_pkg_t pkg;

    if(!m->path[0])
        return;

    snprintf(path, sizeof path, "%s/%s", m->path, MARKS_FILE);

    len = fs_load(path, &raw);
    if(len <= 0 || !raw) {
        free(raw);
        return;
    }

    /* vmu_pkg_parse validates the CRC, so a card that has been sitting in a
     * drawer with a flat battery reports a bad file instead of handing back
     * bookmarks made of noise. */
    if(vmu_pkg_parse((uint8_t *)raw, (size_t)len, &pkg) == 0 && pkg.data)
        parse_blob(m, pkg.data, (size_t)pkg.data_len);
    else
        printf("popsurf: bookmark file unreadable, ignoring\n");

    free(raw);
}

void ps_marks_init(ps_marks *m)
{
    memset(m, 0, sizeof *m);
    find_card(m);
    load(m);
}

/* --- list ---------------------------------------------------------------- */

const char *ps_marks_title(const ps_marks *m, int i)
{
    if(i < 0 || i >= m->count)
        return "";
    return m->list[i].title;
}

const char *ps_marks_url(const ps_marks *m, int i)
{
    if(i < 0 || i >= m->count)
        return "";
    return m->list[i].url;
}

int ps_marks_find(const ps_marks *m, const char *url)
{
    int i;

    if(!url || !*url)
        return -1;

    for(i = 0; i < m->count; i++) {
        if(!strcmp(m->list[i].url, url))
            return i;
    }
    return -1;
}

int ps_marks_add(ps_marks *m, const char *url, const char *title)
{
    ps_mark *e;

    if(!url || !*url || m->count >= PS_MARKS_MAX)
        return -1;
    if(strlen(url) >= PS_MARK_URL_MAX)
        return -1;
    if(ps_marks_find(m, url) >= 0)
        return -1;

    e = &m->list[m->count];
    snprintf(e->url, sizeof e->url, "%s", url);

    /* An untitled page is the common case on the web this browser reads, so
     * the address stands in. A row reading nothing at all would be a bookmark
     * you cannot tell from the one below it. */
    snprintf(e->title, sizeof e->title, "%s",
             (title && *title) ? title : url);

    m->count++;
    m->dirty = 1;
    return 0;
}

int ps_marks_remove(ps_marks *m, int i)
{
    if(i < 0 || i >= m->count)
        return -1;

    memmove(&m->list[i], &m->list[i + 1],
            sizeof m->list[0] * (size_t)(m->count - i - 1));
    m->count--;
    m->dirty = 1;
    return 0;
}

/* --- save ---------------------------------------------------------------- */

int ps_marks_save(ps_marks *m)
{
    uint8_t  *blob;
    uint8_t  *out  = NULL;
    int       out_len = 0;
    uint8_t   icon[512];
    uint16_t  pal[16];
    vmu_pkg_t pkg;
    char      path[32];
    file_t    f;
    int       i, rc = -1;

    if(!m->path[0])
        return -1;
    if(!m->dirty)
        return 0;

    blob = calloc(1, BLOB_SIZE);
    if(!blob)
        return -1;

    memcpy(blob, MARKS_MAGIC, 4);
    blob[4] = MARKS_VERSION;
    blob[5] = (uint8_t)m->count;

    for(i = 0; i < m->count; i++) {
        uint8_t *rec = blob + HDR_SIZE + (size_t)i * REC_SIZE;

        memcpy(rec, m->list[i].title, PS_MARK_TITLE_MAX);
        memcpy(rec + PS_MARK_TITLE_MAX, m->list[i].url, PS_MARK_URL_MAX);
    }

    /* The whole array is written every time, empty records included, so the
     * file is one fixed size for the life of the format. A file that grows and
     * shrinks would have to be deleted and recreated on the card to change
     * size, and a delete that succeeds followed by a write that fails is how
     * somebody loses the bookmarks they already had. */
    icon_build(icon, pal);

    memset(&pkg, 0, sizeof pkg);
    snprintf(pkg.desc_short, sizeof pkg.desc_short, "POPSurf");
    snprintf(pkg.desc_long, sizeof pkg.desc_long, "Bookmarks");
    snprintf(pkg.app_id, sizeof pkg.app_id, "POPSURF");
    pkg.icon_cnt        = 1;
    pkg.icon_anim_speed = 0;
    pkg.eyecatch_type   = VMUPKG_EC_NONE;
    pkg.icon_data       = icon;
    pkg.eyecatch_data   = NULL;
    pkg.data            = blob;
    pkg.data_len        = BLOB_SIZE;
    memcpy(pkg.icon_pal, pal, sizeof pal);

    if(vmu_pkg_build(&pkg, &out, &out_len) < 0) {
        printf("popsurf: could not package bookmarks\n");
        free(blob);
        return -1;
    }

    snprintf(path, sizeof path, "%s/%s", m->path, MARKS_FILE);

    f = fs_open(path, O_WRONLY | O_TRUNC);
    if(f == FILEHND_INVALID) {
        /* Card gone, card full, or card write-protected. All three are the
         * user's business and none of them is ours to recover from. */
        printf("popsurf: could not open %s for writing\n", path);
    }
    else {
        if(fs_write(f, out, (size_t)out_len) == out_len) {
            m->dirty = 0;
            rc = 0;
            printf("popsurf: %d bookmark%s saved to %s\n", m->count,
                   m->count == 1 ? "" : "s", path);
        }
        else {
            printf("popsurf: bookmark write to %s failed\n", path);
        }
        fs_close(f);
    }

    free(out);
    free(blob);
    return rc;
}
