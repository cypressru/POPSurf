/* URL parsing and reference resolution.
 *
 * Enough of RFC 3986 to fetch pages and follow redirects. Fixed-size fields
 * rather than allocation: every one of these has a hard cap anyway, and a URL
 * that exceeds it is refused rather than truncated, because a silently
 * truncated URL is a request to the wrong host.
 */
#ifndef PS_URL_H
#define PS_URL_H

#include "../core/ps_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PS_URL_SCHEME_MAX 8
#define PS_URL_HOST_MAX   256
#define PS_URL_PATH_MAX   1024
#define PS_URL_MAX        (PS_URL_HOST_MAX + PS_URL_PATH_MAX + 16)

typedef struct {
    char scheme[PS_URL_SCHEME_MAX];
    char host[PS_URL_HOST_MAX];
    char path[PS_URL_PATH_MAX];   /* always starts with '/' */
    int  port;
} ps_url;

/* Returns 0 on success. Rejects anything that does not fit.
 *
 * A bare absolute path - "/sd/site/index.html" - is accepted and comes back as
 * a file URL, because that is what someone typing it into the address bar
 * means and there is nothing else it could be. */
int ps_url_parse(ps_url *out, const char *url);

/* Resolves a possibly-relative reference against base, for redirects and
 * links. Handles absolute URLs, scheme-relative ("//host/x"), absolute paths
 * ("/x") and relative paths ("x", "../x"). */
int ps_url_resolve(ps_url *out, const ps_url *base, const char *ref);

/* Serializes back to text. Returns bytes written excluding the NUL, or -1. */
int ps_url_format(const ps_url *u, char *buf, size_t buflen);

/* ------------------------------------------------------------- local media */

/* A file URL names a path in the KOS virtual filesystem, and its first segment
 * is the mount: /sd, /cd, /ide, /ram, /pc. That mount is the origin, in the
 * same sense the host is for HTTP, and everything below exists to keep it one.
 *
 * The consequence that is easy to get wrong: an absolute reference on a local
 * page resolves against the mount, not against the filesystem root. A page on
 * an SD card writing <img src="/img/logo.png"> means the card's /img, and
 * resolving it to the bare /img would have it reach into whatever else happens
 * to be mounted - a disc, the development host - which the page's author never
 * consented to and the user never asked for. ".." is clamped at the mount for
 * the same reason, at parse and again after percent-decoding. */

[[nodiscard]] int ps_url_is_local(const ps_url *u);

/* The mount a local URL belongs to, with its leading slash and no trailing
 * one: "/sd". Fails when the URL names the filesystem root itself, which
 * belongs to no mount. */
[[nodiscard]] int ps_url_local_root(const ps_url *u, char *buf, size_t buflen);

/* The path to hand to fs_open: percent-decoded, query and fragment dropped,
 * and re-clamped to the mount because decoding can produce a "../" that was
 * not there when the URL was normalised. */
[[nodiscard]] int ps_url_local_path(const ps_url *u, char *buf, size_t buflen);

/* Percent-encodes one path segment for use as an href. A filename off a real
 * card can contain '?', '#' or '%', each of which means something else
 * entirely once it is in a URL. Returns the length written, or -1. */
[[nodiscard]] int ps_url_encode_segment(const char *name, char *buf,
                                        size_t buflen);

#ifdef __cplusplus
}
#endif

#endif /* PS_URL_H */
