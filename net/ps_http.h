/* Bounded HTTP/1.1 client. HTTPS is not implemented. */
#ifndef PS_HTTP_H
#define PS_HTTP_H

#include "ps_url.h"
#include "../core/ps_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Caps. The response ceiling is well under the DC budget on purpose: a page
 * that big cannot be laid out in the document arena anyway, so refusing it
 * early is kinder than OOMing mid-parse. */
/* Pages and assets have separate budgets; the transport just needs the larger
 * of the two, and the caller enforces the one that applies to its job. */
#if PS_CFG_MAX_ASSET_BYTES > PS_CFG_MAX_PAGE_BYTES
#define PS_HTTP_MAX_BODY     PS_CFG_MAX_ASSET_BYTES
#else
#define PS_HTTP_MAX_BODY     PS_CFG_MAX_PAGE_BYTES
#endif
#define PS_HTTP_MAX_HEADERS  (16 * 1024)
#define PS_HTTP_MAX_REDIRECT 8
#define PS_HTTP_TIMEOUT_SEC  20

/* Routes HTTP through a proxy. NULL or an empty host restores direct access. */
void        ps_http_set_proxy(const char *host, int port);
const char *ps_http_proxy_host(void);
int         ps_http_proxy_port(void);

#define PS_HTTP_UA_VERSION "0.1"
#define PS_HTTP_USER_AGENT \
    "Mozilla/4.0 (compatible; POPSurf/" PS_HTTP_UA_VERSION "; " \
    PS_PROFILE_NAME ")"

typedef enum {
    PS_HTTP_OK = 0,
    PS_HTTP_ERR_URL,       /* malformed or unsupported URL */
    PS_HTTP_ERR_DNS,
    PS_HTTP_ERR_CONNECT,
    PS_HTTP_ERR_TIMEOUT,
    PS_HTTP_ERR_PROTOCOL,  /* not a response we can parse */
    PS_HTTP_ERR_TOO_LARGE,
    PS_HTTP_ERR_MEMORY,
    PS_HTTP_ERR_REDIRECT,  /* loop, or past the cap */
    PS_HTTP_ERR_TLS        /* https requested before TLS exists */
} ps_http_result;

typedef struct {
    int    status;
    char  *body;        /* NUL-terminated for the parser's convenience */
    size_t body_len;
    size_t total_len;   /* full resource size from Content-Range; 0 if absent */
    char   content_type[128];
    ps_url final_url;   /* after redirects; the document's base */
} ps_http_response;

/* Follows redirects up to PS_HTTP_MAX_REDIRECT. On anything but PS_HTTP_OK the
 * response is left zeroed and needs no free. */
ps_http_result ps_http_get(const ps_url *url, ps_http_response *out);

/* GET one byte range. A 206 fills out->total_len with the resource's full
 * size so the caller can walk the rest.
 *
 * This exists because a soundbank is megabytes where a page is kilobytes.
 * Pulling one of those down in a single response means holding the whole
 * thing plus the growing receive buffer at once, on a machine with 16MB and
 * no virtual memory - and it gives the user no way to see progress on a
 * modem. Ranges turn it into a series of small, bounded transfers.
 *
 * A server that ignores Range answers 200 with the whole body; the caller
 * must handle that by taking it as the complete resource. */
ps_http_result ps_http_get_range(const ps_url *url, size_t off, size_t len,
                                 ps_http_response *out);

/* POST with a request body. content_type may be NULL for
 * application/x-www-form-urlencoded, which is what HTML forms send.
 *
 * Redirect handling differs from GET and has to: 301, 302 and 303 turn a POST
 * into a GET and drop the body, which is what every browser does and what the
 * "post then redirect to the result page" pattern depends on. 307 and 308
 * preserve the method and body. */
ps_http_result ps_http_post(const ps_url *url, const char *content_type,
                            const void *body, size_t body_len,
                            ps_http_response *out);

/* Progress of the body currently being received, for a progress indicator.
 * total is 0 when the length is not known in advance. */
void ps_http_progress(size_t *got, size_t *total);

void ps_http_response_free(ps_http_response *r);

/* Drops the pooled keep-alive connection. Call on navigation away or teardown;
 * holding a socket open across an idle page wastes an origin's slot. */
void ps_http_disconnect(void);

const char *ps_http_strerror(ps_http_result r);

#ifdef __cplusplus
}
#endif

#endif /* PS_HTTP_H */
