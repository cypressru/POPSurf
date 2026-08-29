#include "ps_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>

/* One persistent connection to the current origin. */

typedef struct {
    int  fd;
    char host[PS_URL_HOST_MAX];
    int  port;
} ps_conn;

static ps_conn g_conn = { -1, { 0 }, 0 };

/* Optional HTTP proxy. Empty host means direct access. */
static ps_conn g_proxy = { -1, { 0 }, 0 };

static void conn_close(void)
{
    if(g_conn.fd >= 0)
        close(g_conn.fd);
    g_conn.fd      = -1;
    g_conn.host[0] = '\0';
    g_conn.port    = 0;
}

/* ---------------------------------------------------------------------------
 * Buffered socket reader.
 *
 * Keep-alive means the response must be framed exactly: read one byte too many
 * and it belongs to the next response. So everything is read through this,
 * which never consumes past what it is asked for.
 */

typedef struct {
    int    fd;
    char   buf[2048];
    size_t pos, len;
    int    eof;
} ps_reader;

static void rd_init(ps_reader *r, int fd)
{
    r->fd  = fd;
    r->pos = r->len = 0;
    r->eof = 0;
}

static int rd_fill(ps_reader *r)
{
    ssize_t n;

    if(r->pos < r->len)
        return 1;
    if(r->eof)
        return 0;

    n = recv(r->fd, r->buf, sizeof r->buf, 0);
    if(n <= 0) {
        r->eof = 1;
        return 0;
    }

    r->pos = 0;
    r->len = (size_t)n;
    return 1;
}

/* Reads a CRLF- or LF-terminated line, stripping the terminator. Returns
 * length, or -1 on EOF or overflow. */
static int rd_line(ps_reader *r, char *out, size_t max)
{
    size_t n = 0;

    for(;;) {
        int c;

        if(!rd_fill(r))
            return n ? (int)n : -1;

        c = (unsigned char)r->buf[r->pos++];

        if(c == '\n') {
            if(n && out[n - 1] == '\r')
                n--;
            out[n] = '\0';
            return (int)n;
        }

        if(n + 1 >= max)
            return -1;
        out[n++] = (char)c;
    }
}

/* Reads exactly n bytes. Returns 0 on success. */
/* Bytes of the current body received so far, and how many are expected.
 * Written only by the loader's single worker and read by the render thread
 * for the progress bar, where a torn read costs at worst one stale frame of a
 * number that is already changing. That is much cheaper than a lock on the
 * receive path. */
static volatile size_t g_body_got, g_body_total;

void ps_http_progress(size_t *got, size_t *total)
{
    if(got)   *got   = g_body_got;
    if(total) *total = g_body_total;
}

static int rd_exact(ps_reader *r, char *out, size_t n)
{
    while(n) {
        size_t avail;

        if(!rd_fill(r))
            return -1;

        avail = r->len - r->pos;
        if(avail > n)
            avail = n;

        memcpy(out, r->buf + r->pos, avail);
        r->pos += avail;
        out    += avail;
        n      -= avail;
        g_body_got += avail;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Growable byte buffer with a hard ceiling.
 */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} ps_buf;

static void buf_free(ps_buf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static int buf_reserve(ps_buf *b, size_t need, size_t max)
{
    size_t want;
    char  *p;

    if(need + 1 <= b->cap)
        return 0;
    if(need > max)
        return -1;

    /* Doubling is right while streaming a chunked body of unknown length, but
     * when the size is already known - Content-Length, which is how a
     * soundbank arrives - it overshoots badly. A 2.1MB bank would round up to
     * 4MB and, because realloc has to hold both copies while it moves, peak
     * at over 6MB on a machine with 16MB total. Ask for what was asked for. */
    want = b->cap ? b->cap : need + 1;
    while(want < need + 1)
        want *= 2;
    if(want > max + 1)
        want = max + 1;

    p = (char *)realloc(b->data, want);
    if(!p)
        return -1;

    b->data = p;
    b->cap  = want;
    return 0;
}

/* ---------------------------------------------------------------------------
 * DNS cache.
 *
 * getaddrinfo per subresource is a needless round trip on a machine where a
 * round trip can be half a second.
 */

#define PS_DNS_CACHE_MAX 8

typedef struct {
    char               host[PS_URL_HOST_MAX];
    struct sockaddr_in addr;
    int                used;
} ps_dns_entry;

static ps_dns_entry g_dns[PS_DNS_CACHE_MAX];
static int          g_dns_next;

static int resolve_host(const char *host, int port, struct sockaddr_in *out)
{
    struct addrinfo  hints, *ai = NULL;
    char             portstr[8];
    int              i;

    for(i = 0; i < PS_DNS_CACHE_MAX; i++) {
        if(g_dns[i].used && !strcmp(g_dns[i].host, host)) {
            *out = g_dns[i].addr;
            out->sin_port = htons((uint16_t)port);
            return 0;
        }
    }

    snprintf(portstr, sizeof portstr, "%d", port);

    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if(getaddrinfo(host, portstr, &hints, &ai) != 0 || !ai)
        return -1;

    memcpy(out, ai->ai_addr, sizeof *out);
    freeaddrinfo(ai);

    if(strlen(host) < sizeof g_dns[0].host) {
        /* Round-robin eviction. There is no TTL handling: a browser session on
         * this hardware is short, and a stale entry costs one failed load. */
        ps_dns_entry *e = &g_dns[g_dns_next];

        g_dns_next = (g_dns_next + 1) % PS_DNS_CACHE_MAX;
        strcpy(e->host, host);
        e->addr = *out;
        e->used = 1;
    }

    return 0;
}

/* ---------------------------------------------------------------------------
 * Connection management.
 */

static int get_connection(const ps_url *url, ps_http_result *err)
{
    struct sockaddr_in addr;
    int                fd;

    /* Through a proxy, every request goes to the same machine whatever host
     * it names, so the connection is keyed on the proxy and reused across
     * sites rather than torn down at each hop. On a console reached by a
     * modem that is the difference between a page and a wait. */
    const char *chost = g_proxy.host[0] ? g_proxy.host : url->host;
    int         cport = g_proxy.host[0] ? g_proxy.port : url->port;

    if(g_conn.fd >= 0 && g_conn.port == cport &&
       !strcmp(g_conn.host, chost))
        return g_conn.fd;

    conn_close();

    if(resolve_host(chost, cport, &addr) != 0) {
        *err = PS_HTTP_ERR_DNS;
        return -1;
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) {
        *err = PS_HTTP_ERR_CONNECT;
        return -1;
    }

    if(connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(fd);
        *err = PS_HTTP_ERR_CONNECT;
        return -1;
    }

    /* A stalled origin must not hang the browser forever. Not every KOS build
     * honours these, so failure is not fatal; the byte caps are the backstop. */
    {
        struct timeval tv;
        tv.tv_sec  = PS_HTTP_TIMEOUT_SEC;
        tv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    }

    g_conn.fd   = fd;
    g_conn.port = cport;
    strcpy(g_conn.host, chost);
    return fd;
}

static int send_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;

    while(off < len) {
        ssize_t n = send(fd, buf + off, len - off, 0);

        if(n <= 0)
            return -1;
        off += (size_t)n;
    }
    return 0;
}

/* Range for the next request only, then cleared. The loader runs one worker
 * thread and issues one request at a time, so this needs no locking; it is
 * kept out of the signature because every redirect and retry path would
 * otherwise have to thread it through untouched. */
static char g_range[64];

static int send_request(int fd, const ps_url *url, const char *method,
                        const char *content_type, const void *body,
                        size_t body_len)
{
    char req[PS_URL_PATH_MAX + PS_URL_HOST_MAX + 384];
    int  n;

    /* No Accept-Encoding: without it servers send identity, which saves
     * shipping an inflate path before it is needed.
     *
     * The UA deliberately does not impersonate a desktop browser: origins that
     * serve a simple tree should keep doing so. */
    /* Through a proxy the request line carries the whole URL rather than just
     * the path - that absolute form is how the proxy learns which origin it is
     * being asked to fetch from, since it is not the origin itself. Host: is
     * sent either way: HTTP/1.1 requires it, and a proxy passes it along. */
    if(g_proxy.host[0]) {
        char port[8];

        /* Only a non-default port belongs in the absolute URL; writing :80
         * would work but changes the origin's idea of its own address, which
         * some servers reflect back into redirects. */
        port[0] = '\0';
        if(url->port != 80)
            snprintf(port, sizeof port, ":%d", url->port);

        n = snprintf(req, sizeof req,
                     "%s http://%s%s%s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "User-Agent: " PS_HTTP_USER_AGENT "\r\n"
                     "Accept: text/html,text/plain,image/gif,image/png,image/jpeg,*/*\r\n"
                     "Accept-Charset: utf-8, iso-8859-1\r\n"
                     "Connection: keep-alive\r\n",
                     method, url->host, port, url->path, url->host);
    } else
        n = snprintf(req, sizeof req,
                     "%s %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "User-Agent: " PS_HTTP_USER_AGENT "\r\n"
                     "Accept: text/html,text/plain,image/gif,image/png,image/jpeg,*/*\r\n"
                     "Accept-Charset: utf-8, iso-8859-1\r\n"
                     "Connection: keep-alive\r\n",
                     method, url->path, url->host);

    if(n < 0 || (size_t)n >= sizeof req)
        return -1;

    if(g_range[0]) {
        int m = snprintf(req + n, sizeof req - (size_t)n, "%s", g_range);

        if(m < 0 || (size_t)(n + m) >= sizeof req)
            return -1;
        n += m;
    }

    if(body) {
        int m = snprintf(req + n, sizeof req - (size_t)n,
                         "Content-Type: %s\r\n"
                         "Content-Length: %u\r\n",
                         content_type ? content_type
                                      : "application/x-www-form-urlencoded",
                         (unsigned)body_len);

        if(m < 0 || (size_t)(n + m) >= sizeof req)
            return -1;
        n += m;
    }

    if((size_t)n + 2 >= sizeof req)
        return -1;
    req[n++] = '\r';
    req[n++] = '\n';

    if(send_all(fd, req, (size_t)n) != 0)
        return -1;

    if(body && body_len && send_all(fd, (const char *)body, body_len) != 0)
        return -1;

    return 0;
}

/* ---------------------------------------------------------------------------
 * Response parsing.
 */

typedef struct {
    int    status;
    long   content_length;   /* -1 when absent */
    long   range_total;      /* from Content-Range; -1 when absent */
    int    chunked;
    int    close_after;
    char   location[PS_URL_MAX];
    char   content_type[128];
} ps_resp_head;

static void header_value(const char *line, const char *name,
                         char *out, size_t max)
{
    size_t nl = strlen(name);
    const char *v;

    if(strncasecmp(line, name, nl) || line[nl] != ':')
        return;

    v = line + nl + 1;
    while(*v == ' ' || *v == '\t')
        v++;

    if(strlen(v) < max)
        strcpy(out, v);
}

static ps_http_result read_head(ps_reader *r, ps_resp_head *h)
{
    char   line[1024];
    size_t total = 0;
    int    n;

    memset(h, 0, sizeof *h);
    h->content_length = -1;
    h->range_total    = -1;

    n = rd_line(r, line, sizeof line);
    if(n < 0)
        return PS_HTTP_ERR_PROTOCOL;

    if(strncmp(line, "HTTP/1.", 7))
        return PS_HTTP_ERR_PROTOCOL;

    /* HTTP/1.0 is close-by-default unless it opts in. */
    if(line[7] == '0')
        h->close_after = 1;

    h->status = atoi(line + 9);
    if(h->status < 100 || h->status > 599)
        return PS_HTTP_ERR_PROTOCOL;

    for(;;) {
        char buf[512];

        n = rd_line(r, line, sizeof line);
        if(n < 0)
            return PS_HTTP_ERR_PROTOCOL;
        if(n == 0)
            break;

        total += (size_t)n;
        if(total > PS_HTTP_MAX_HEADERS)
            return PS_HTTP_ERR_TOO_LARGE;

        buf[0] = '\0';
        header_value(line, "Content-Length", buf, sizeof buf);
        if(buf[0]) {
            h->content_length = atol(buf);
            if(h->content_length < 0)
                return PS_HTTP_ERR_PROTOCOL;
            continue;
        }

        buf[0] = '\0';
        header_value(line, "Content-Range", buf, sizeof buf);
        if(buf[0]) {
            /* "bytes <start>-<end>/<total>"; only the total is wanted. */
            const char *slash = strrchr(buf, '/');

            if(slash && slash[1] && slash[1] != '*')
                h->range_total = atol(slash + 1);
            continue;
        }

        buf[0] = '\0';
        header_value(line, "Transfer-Encoding", buf, sizeof buf);
        if(buf[0]) {
            if(!strncasecmp(buf, "chunked", 7))
                h->chunked = 1;
            continue;
        }

        buf[0] = '\0';
        header_value(line, "Connection", buf, sizeof buf);
        if(buf[0]) {
            if(!strncasecmp(buf, "close", 5))
                h->close_after = 1;
            else if(!strncasecmp(buf, "keep-alive", 10))
                h->close_after = 0;
            continue;
        }

        header_value(line, "Location", h->location, sizeof h->location);
        header_value(line, "Content-Type", h->content_type,
                     sizeof h->content_type);
    }

    return PS_HTTP_OK;
}

static ps_http_result read_body_length(ps_reader *r, ps_buf *b, size_t len)
{
    g_body_got   = 0;
    g_body_total = len;

    if(len > PS_HTTP_MAX_BODY)
        return PS_HTTP_ERR_TOO_LARGE;
    if(buf_reserve(b, len, PS_HTTP_MAX_BODY) != 0)
        return PS_HTTP_ERR_MEMORY;

    if(len && rd_exact(r, b->data, len) != 0)
        return PS_HTTP_ERR_PROTOCOL;

    b->len = len;
    b->data[len] = '\0';
    return PS_HTTP_OK;
}

static ps_http_result read_body_chunked(ps_reader *r, ps_buf *b)
{
    char line[128];

    b->len = 0;

    for(;;) {
        unsigned long chunk;
        char         *end;

        if(rd_line(r, line, sizeof line) < 0)
            return PS_HTTP_ERR_PROTOCOL;

        chunk = strtoul(line, &end, 16);
        if(end == line)
            return PS_HTTP_ERR_PROTOCOL;

        if(chunk == 0)
            break;

        if(b->len + chunk > PS_HTTP_MAX_BODY)
            return PS_HTTP_ERR_TOO_LARGE;
        if(buf_reserve(b, b->len + chunk, PS_HTTP_MAX_BODY) != 0)
            return PS_HTTP_ERR_MEMORY;

        if(rd_exact(r, b->data + b->len, chunk) != 0)
            return PS_HTTP_ERR_PROTOCOL;
        b->len += chunk;

        /* CRLF after the chunk data. */
        if(rd_line(r, line, sizeof line) < 0)
            return PS_HTTP_ERR_PROTOCOL;
    }

    /* Trailers, then the closing blank line. */
    for(;;) {
        int n = rd_line(r, line, sizeof line);

        if(n <= 0)
            break;
    }

    if(buf_reserve(b, b->len, PS_HTTP_MAX_BODY) != 0)
        return PS_HTTP_ERR_MEMORY;
    b->data[b->len] = '\0';
    return PS_HTTP_OK;
}

/* No framing headers at all: the body runs to EOF, which also means the
 * connection cannot be reused. */
static ps_http_result read_body_to_eof(ps_reader *r, ps_buf *b)
{
    b->len = 0;

    for(;;) {
        size_t avail;

        if(!rd_fill(r))
            break;

        avail = r->len - r->pos;
        if(b->len + avail > PS_HTTP_MAX_BODY)
            return PS_HTTP_ERR_TOO_LARGE;
        if(buf_reserve(b, b->len + avail, PS_HTTP_MAX_BODY) != 0)
            return PS_HTTP_ERR_MEMORY;

        memcpy(b->data + b->len, r->buf + r->pos, avail);
        b->len += avail;
        r->pos += avail;
    }

    if(buf_reserve(b, b->len, PS_HTTP_MAX_BODY) != 0)
        return PS_HTTP_ERR_MEMORY;
    b->data[b->len] = '\0';
    return PS_HTTP_OK;
}

/* One request on the shared connection, no redirect handling. */
static ps_http_result fetch_once(const ps_url *url, const char *method,
                                 const char *content_type, const void *body,
                                 size_t body_len, ps_http_response *out,
                                 char *location, size_t location_max,
                                 int allow_retry)
{
    int            fd;
    ps_reader      r;
    ps_resp_head   h;
    ps_buf         resp_body = { NULL, 0, 0 };
    ps_http_result rc   = PS_HTTP_ERR_PROTOCOL;

    if(!strcmp(url->scheme, "https"))
        return PS_HTTP_ERR_TLS;
    if(strcmp(url->scheme, "http"))
        return PS_HTTP_ERR_URL;

    fd = get_connection(url, &rc);
    if(fd < 0)
        return rc;

    if(send_request(fd, url, method, content_type, body, body_len) < 0) {
        /* A pooled socket the origin closed while idle fails on write. That is
         * expected, not an error: drop it and try once on a fresh one. */
        conn_close();
        if(allow_retry)
            return fetch_once(url, method, content_type, body, body_len, out,
                              location, location_max, 0);
        return PS_HTTP_ERR_CONNECT;
    }

    rd_init(&r, fd);

    rc = read_head(&r, &h);
    if(rc != PS_HTTP_OK) {
        conn_close();
        if(allow_retry && r.eof && r.len == 0)
            return fetch_once(url, method, content_type, body, body_len, out,
                              location, location_max, 0);
        return rc;
    }

    /* 204 and 304 carry no body regardless of what the headers claim. */
    if(h.status == 204 || h.status == 304)
        rc = read_body_length(&r, &resp_body, 0);
    else if(h.chunked)
        rc = read_body_chunked(&r, &resp_body);
    else if(h.content_length >= 0)
        rc = read_body_length(&r, &resp_body, (size_t)h.content_length);
    else {
        rc = read_body_to_eof(&r, &resp_body);
        h.close_after = 1;
    }

    if(rc != PS_HTTP_OK) {
        buf_free(&resp_body);
        conn_close();
        return rc;
    }

    if(h.close_after || r.eof)
        conn_close();

    if(location_max) {
        location[0] = '\0';
        if(h.location[0] && strlen(h.location) < location_max)
            strcpy(location, h.location);
    }

    out->status    = h.status;
    out->body      = resp_body.data;
    out->body_len  = resp_body.len;
    out->total_len = h.range_total > 0 ? (size_t)h.range_total : 0;
    out->final_url = *url;
    strcpy(out->content_type, h.content_type);
    return PS_HTTP_OK;
}

static ps_http_result http_run(const ps_url *url, const char *method,
                               const char *content_type, const void *body,
                               size_t body_len, ps_http_response *out)
{
    ps_url cur;
    int    hop;

    if(!url || !out)
        return PS_HTTP_ERR_URL;

    memset(out, 0, sizeof *out);
    cur = *url;

    for(hop = 0; hop <= PS_HTTP_MAX_REDIRECT; hop++) {
        char           location[PS_URL_MAX];
        ps_http_result r = fetch_once(&cur, method, content_type, body,
                                      body_len, out, location,
                                      sizeof location, 1);

        if(r != PS_HTTP_OK)
            return r;

        if(out->status >= 300 && out->status < 400 && location[0]) {
            ps_url next;

            ps_http_response_free(out);

            if(ps_url_resolve(&next, &cur, location) != 0)
                return PS_HTTP_ERR_REDIRECT;

            /* A redirect to where we already are is a loop, not progress. */
            if(!strcmp(next.host, cur.host) && !strcmp(next.path, cur.path) &&
               next.port == cur.port)
                return PS_HTTP_ERR_REDIRECT;

            /* 301, 302 and 303 turn a POST into a GET and drop the body.
             * That is what browsers do, and the post-then-redirect pattern
             * every form on the retro web uses depends on it. 307 and 308
             * keep the method. */
            if(body && out->status != 307 && out->status != 308) {
                method       = "GET";
                body         = NULL;
                body_len     = 0;
                content_type = NULL;
            }

            cur = next;
            continue;
        }

        return PS_HTTP_OK;
    }

    ps_http_response_free(out);
    return PS_HTTP_ERR_REDIRECT;
}

ps_http_result ps_http_get(const ps_url *url, ps_http_response *out)
{
    g_range[0] = '\0';
    return http_run(url, "GET", NULL, NULL, 0, out);
}

ps_http_result ps_http_get_range(const ps_url *url, size_t off, size_t len,
                                 ps_http_response *out)
{
    ps_http_result rc;

    snprintf(g_range, sizeof g_range, "Range: bytes=%lu-%lu\r\n",
             (unsigned long)off, (unsigned long)(off + len - 1));

    rc = http_run(url, "GET", NULL, NULL, 0, out);
    g_range[0] = '\0';
    return rc;
}

ps_http_result ps_http_post(const ps_url *url, const char *content_type,
                            const void *body, size_t body_len,
                            ps_http_response *out)
{
    return http_run(url, "POST", content_type, body ? body : "", body_len,
                    out);
}

void ps_http_response_free(ps_http_response *r)
{
    if(!r)
        return;
    free(r->body);
    r->body     = NULL;
    r->body_len = 0;
}

void ps_http_disconnect(void)
{
    conn_close();
}

const char *ps_http_strerror(ps_http_result r)
{
    switch(r) {
    case PS_HTTP_OK:            return "ok";
    case PS_HTTP_ERR_URL:       return "bad or unsupported URL";
    case PS_HTTP_ERR_DNS:       return "host not found";
    case PS_HTTP_ERR_CONNECT:   return "connection failed";
    case PS_HTTP_ERR_TIMEOUT:   return "timed out";
    case PS_HTTP_ERR_PROTOCOL:  return "bad HTTP response";
    case PS_HTTP_ERR_TOO_LARGE: return "response too large";
    case PS_HTTP_ERR_MEMORY:    return "out of memory";
    case PS_HTTP_ERR_REDIRECT:  return "too many redirects";
    case PS_HTTP_ERR_TLS:       return "https not supported yet";
    }
    return "unknown error";
}

/* --------------------------------------------------------------------------
 * Proxy configuration.
 */

void ps_http_set_proxy(const char *host, int port)
{
    /* Changing where requests go invalidates a kept-alive socket to the old
     * destination, so drop it rather than send the next request somewhere it
     * was not meant for. */
    conn_close();

    if(!host || !*host) {
        g_proxy.host[0] = '\0';
        g_proxy.port    = 0;
        return;
    }

    snprintf(g_proxy.host, sizeof g_proxy.host, "%s", host);
    g_proxy.port = (port > 0) ? port : 80;
}

const char *ps_http_proxy_host(void)
{
    return g_proxy.host;
}

int ps_http_proxy_port(void)
{
    return g_proxy.port;
}
