/* Local media: pages and assets read from the KOS virtual filesystem.
 *
 * /sd, /cd, /ide, /ram and /pc are all mounted by KOS itself, so there is
 * nothing here that mounts anything - only opening, reading and refusing. What
 * this file exists for is the four things that are not "call fs_open":
 *
 * A missing device does not fail, it waits. cdrom_get_status is asked first for
 * anything under /cd, because an empty drive answers a read by retrying for
 * long enough to look like a hang, and we have already shipped that bug once
 * (shell/main.c load_boot_asset says the same thing from the other side).
 *
 * A directory is a page. Nobody authoring an SD card should have to write an
 * index.html before they can look at what is on it, so a URL naming a
 * directory comes back as generated HTML that the renderer displays like any
 * other page.
 *
 * A local file gets the same ceiling as a fetched one. Nothing about the bytes
 * being nearby makes 16MB with no virtual memory any larger, and a card can
 * hold a file far bigger than the machine.
 *
 * And every path is untrusted, because a page from the network can link to
 * one. Containment inside the mount lives in ps_url.c, next to the resolution
 * rules it has to agree with; this file only ever opens what comes back from
 * ps_url_local_path.
 */
#ifndef PS_FILE_H
#define PS_FILE_H

#include "ps_url.h"
#include "../core/ps_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Ceiling on one local read. The same figure the transport enforces on a
 * fetched asset, for the same reason. */
#define PS_FILE_MAX_BYTES PS_CFG_MAX_ASSET_BYTES

/* Entries one generated listing will show. A card with more than this in one
 * directory is browsable to that depth rather than not at all; laying out ten
 * thousand table rows is a page the machine cannot render anyway. */
#define PS_FILE_MAX_ENTRIES 512

typedef enum {
    PS_FILE_OK = 0,
    PS_FILE_ERR_PATH,       /* not a local URL, or one we will not open */
    PS_FILE_ERR_NOT_FOUND,
    PS_FILE_ERR_NO_DEVICE,  /* nothing in the drive, or nothing mounted there */
    PS_FILE_ERR_TOO_LARGE,
    PS_FILE_ERR_MEMORY,
    PS_FILE_ERR_IO
} ps_file_result;

typedef struct {
    char  *data;        /* NUL-terminated for the parser, caller frees */
    size_t len;
    int    is_dir;      /* data is a generated listing, not file contents */

    /* What the document should treat as its base. A directory gains a trailing
     * slash here, which is the difference between a link on the listing
     * resolving inside the directory and beside it. */
    char   final_url[PS_URL_MAX];
} ps_file_response;

/* Reads a file, or renders a directory as HTML. Runs on the loader thread:
 * a card read is fast but not free, and the render loop must not wait on it.
 *
 * On anything but PS_FILE_OK the response is left zeroed and needs no free. */
[[nodiscard]] ps_file_result ps_file_fetch(const ps_url *u,
                                           ps_file_response *out);

void ps_file_response_free(ps_file_response *r);

const char *ps_file_strerror(ps_file_result r);

/* Builds the listing HTML. Separate from the directory walk so it can be
 * tested on a development machine, where there is no KOS: getting a page of
 * escaped, percent-encoded links right is exactly the sort of thing that
 * should not need a console to check.
 *
 * Begin, add each entry, finish. size is bytes, or -1 when the filesystem
 * does not say - which several of these do not for directories. finish hands
 * over the buffer and frees the builder; abort frees both. */
typedef struct ps_dirpage ps_dirpage;

[[nodiscard]] ps_dirpage *ps_dirpage_begin(const ps_url *dir);
void  ps_dirpage_add(ps_dirpage *d, const char *name, int is_dir, long size);
[[nodiscard]] char *ps_dirpage_finish(ps_dirpage *d, size_t *len);
void  ps_dirpage_abort(ps_dirpage *d);

#ifdef __cplusplus
}
#endif

#endif /* PS_FILE_H */
