#include "ps_loader.h"
#include "ps_file.h"
#include "ps_http.h"

#include <kos.h>
#include <arch/arch.h>
#include <unistd.h>
#include <kos/thread.h>
#include <kos/mutex.h>
#include <kos/cond.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    ps_job_kind kind;
    char        url[PS_URL_MAX];
    int         is_post;
    char        body[PS_LOADER_BODY_MAX];
} ps_job;

struct ps_loader {
    mutex_t   lock;
    condvar_t work;      /* signalled when a job is queued or on shutdown */

    ps_job    queue[PS_LOADER_QUEUE_MAX];
    int       q_head, q_tail, q_count;

    ps_job_result done[PS_LOADER_QUEUE_MAX];
    int           d_head, d_tail, d_count;

    /* Bumped on cancel. Results carrying an older epoch are discarded, which
     * is how a navigation away from a half-loaded page drops its images
     * without having to interrupt a fetch already in flight. */
    unsigned epoch;

    int        running;
    int        in_flight;
    kthread_t *thread;
};

/* A soundbank is megabytes where every other asset is kilobytes. Fetching it
 * as one response means the receive buffer and the finished copy are live at
 * the same time, which on a 16MB machine with no virtual memory is a large
 * bite to take in one go - and it gives no progress on a modem, where it is
 * minutes of nothing.
 *
 * So it is pulled in bounded pieces and assembled once, into a buffer sized
 * from the first reply. Servers that ignore Range answer 200 with everything,
 * which is handled by taking that as the whole resource. */
/* Few and large rather than many and small. Ordinary browsing - a page and a
 * dozen images - is many small requests and is reliable; one megabyte in a
 * single response is reliable; it is a long run of large responses that is
 * not. So the chunk is sized to keep the request count in single digits. */
#define PS_BANK_CHUNK (768 * 1024)

/* Enough for a 46MB bank at the baker's default part size. */
#define PS_BANK_MAX_PARTS 64

/* Off by default. The ranged path assembles correctly - the offsets, lengths
 * and totals all check out in a trace - but a bank that fetches cleanly as a
 * single response reliably panics the machine partway through when pulled as
 * a series of ranges. Something about issuing many requests back to back is
 * at fault and it has not been found yet, so the working path is the default
 * and this stays available to debug against.
 *
 * Which means the size problem it was written to solve is still open: see
 * Docs/dc-compat.md. */
#ifndef PS_BANK_RANGED
#define PS_BANK_RANGED 0
#endif

/* Fetch the bank as consecutive .pNN part files rather than one response.
 *
 * On by default: it is the only large-transfer shape that has proved stable,
 * it needs nothing of the server beyond serving static files, and it is what
 * a modem user wants anyway - a megabyte in one response is minutes with no
 * way to show progress or stop. gmbake writes the parts; a bank published as
 * a single file still works, because part 00 missing falls back to the whole
 * URL. */
#ifndef PS_BANK_PARTS
#define PS_BANK_PARTS 1
#endif

/* Fetches a bank published as consecutive part files - url.p00, url.p01, ...
 * - and joins them. Parts are ordinary GETs of ordinary files: no Range, no
 * 206, nothing a plain static host has to opt into.
 *
 * Ranges were the obvious way to do this and did not survive contact with the
 * hardware path: the identical bank is reliable as one response and unstable
 * as a series of ranged ones. Whatever the cause, separate files exercise only
 * the request shape that already works for every page and image. */
static ps_http_result fetch_parts(const char *base, ps_http_response *out)
{
    char             url[PS_URL_MAX];
    ps_http_response part;
    ps_url           pu;
    char            *buf = NULL;
    size_t           cap = 0, got = 0;
    int              i;

    memset(out, 0, sizeof *out);

    for(i = 0; i < PS_BANK_MAX_PARTS; i++) {
        ps_http_result rc;

        if(snprintf(url, sizeof url, "%s.p%02d", base, i) >= (int)sizeof url)
            break;
        if(ps_url_parse(&pu, url) != 0)
            break;

        memset(&part, 0, sizeof part);
        rc = ps_http_get(&pu, &part);

        /* Running off the end of the set and failing partway through it are
         * different things, and must not be confused: a 404 after part 03
         * means the bank had four parts, but a timeout after part 03 means
         * the bank is incomplete. Treating the second as the first hands back
         * a truncated bank - instruments simply missing, with nothing to say
         * why. Only "not found" ends the set. */
        int at_end = (rc == PS_HTTP_OK &&
                      (part.status == 404 || part.status == 410));

        if(rc != PS_HTTP_OK || part.status != 200 || !part.body_len) {
            ps_http_response_free(&part);

            if(got) {
                if(at_end)
                    break;          /* that was the last part */
                free(buf);          /* incomplete: better silent than wrong */
                return rc == PS_HTTP_OK ? PS_HTTP_ERR_PROTOCOL : rc;
            }

            /* No part 00 at all: the bank is published as one file. */
            free(buf);
            if(ps_url_parse(&pu, base) != 0)
                return PS_HTTP_ERR_URL;
            return ps_http_get(&pu, out);
        }

        if(got + part.body_len + 1 > cap) {
            size_t want = cap ? cap * 2 : (part.body_len * 4 + 1);
            char  *p2;

            while(want < got + part.body_len + 1)
                want *= 2;
            if(want > PS_CFG_MAX_ASSET_BYTES) {
                ps_http_response_free(&part);
                free(buf);
                return PS_HTTP_ERR_TOO_LARGE;
            }
            p2 = (char *)realloc(buf, want);
            if(!p2) {
                ps_http_response_free(&part);
                free(buf);
                return PS_HTTP_ERR_MEMORY;
            }
            buf = p2;
            cap = want;
        }

        memcpy(buf + got, part.body, part.body_len);
        got += part.body_len;
        if(i == 0) {
            out->final_url = part.final_url;
            memcpy(out->content_type, part.content_type,
                   sizeof out->content_type);
        }
        ps_http_response_free(&part);

        printf("bank: part %d, %u bytes total\n", i, (unsigned)got);
        fflush(stdout);
    }

    if(!got) {
        free(buf);
        return PS_HTTP_ERR_PROTOCOL;
    }

    /* Hitting the cap means the set did not end where we stopped looking, so
     * what was collected is a prefix of the bank rather than the bank. */
    if(i >= PS_BANK_MAX_PARTS) {
        free(buf);
        return PS_HTTP_ERR_TOO_LARGE;
    }

    buf[got]      = '\0';
    out->status   = 200;
    out->body     = buf;
    out->body_len = got;
    return PS_HTTP_OK;
}

static ps_http_result fetch_ranged(const ps_url *u, ps_http_response *out)
{
    ps_http_response part;
    ps_http_result   rc;
    char            *buf   = NULL;
    size_t           total = 0, got = 0;

    memset(out, 0, sizeof *out);

    for(;;) {
        memset(&part, 0, sizeof part);
        rc = ps_http_get_range(u, got, PS_BANK_CHUNK, &part);
        printf("bank: rc=%d st=%d len=%u tot=%u head=%dK\n", (int)rc,
               part.status, (unsigned)part.body_len, (unsigned)part.total_len,
               (int)(((uint32_t)_arch_mem_top - (uint32_t)sbrk(0)) / 1024));
        fflush(stdout);
        if(rc != PS_HTTP_OK) {
            free(buf);
            return rc;
        }

        /* 200 means Range was ignored and this is the entire resource. */
        if(part.status == 200 && got == 0) {
            *out = part;
            return PS_HTTP_OK;
        }
        if(part.status != 206 || !part.body || !part.body_len) {
            ps_http_response_free(&part);
            free(buf);
            return PS_HTTP_ERR_PROTOCOL;
        }

        if(!buf) {
            total = part.total_len ? part.total_len : part.body_len;
            if(total > PS_CFG_MAX_ASSET_BYTES) {
                ps_http_response_free(&part);
                return PS_HTTP_ERR_TOO_LARGE;
            }
            buf = (char *)malloc(total + 1);
            if(!buf) {
                ps_http_response_free(&part);
                return PS_HTTP_ERR_MEMORY;
            }
            out->final_url = part.final_url;
            memcpy(out->content_type, part.content_type,
                   sizeof out->content_type);
        }

        if(got >= total) {          /* nothing more expected */
            ps_http_response_free(&part);
            break;
        }
        if(got + part.body_len > total)
            part.body_len = total - got;
        memcpy(buf + got, part.body, part.body_len);
        got += part.body_len;
        ps_http_response_free(&part);

        if(got >= total)
            break;

        /* Deliberately NOT dropping the connection between chunks: cycling
         * sockets per chunk corrupts the transfer far faster than reusing
         * one. A short pause instead, to let the link drain rather than
         * queueing the next request behind the previous tail. */
        thd_sleep(150);
    }

    buf[total]     = '\0';
    out->status    = 200;
    out->body      = buf;
    out->body_len  = total;
    out->total_len = total;
    return PS_HTTP_OK;
}

static void *loader_main(void *param)
{
    ps_loader *l = (ps_loader *)param;

    mutex_lock(&l->lock);

    while(l->running) {
        ps_job           job;
        unsigned         job_epoch;
        ps_http_response resp;
        ps_url           u;
        ps_http_result   rc;
        ps_job_result    res;

        if(l->q_count == 0) {
            cond_wait(&l->work, &l->lock);
            continue;
        }

        job = l->queue[l->q_head];
        l->q_head = (l->q_head + 1) % PS_LOADER_QUEUE_MAX;
        l->q_count--;
        job_epoch = l->epoch;
        l->in_flight = 1;

        /* The fetch is the slow part and must not hold the lock: the render
         * thread polls every frame. */
        mutex_unlock(&l->lock);

        memset(&res, 0, sizeof res);
        res.kind = job.kind;
        strcpy(res.url, job.url);
        strcpy(res.final_url, job.url);

        if(ps_url_parse(&u, job.url) != 0) {
            printf("popsurf: %s: %s\n", job.url,
                   ps_http_strerror(PS_HTTP_ERR_URL));
        }
        else if(ps_url_is_local(&u)) {
            /* Local media - a disc, a card, the development host. Runs on the
             * worker like everything else: a card read is fast but not free,
             * and an absent one is not fast at all. */
            ps_file_response fr;
            ps_file_result   frc = ps_file_fetch(&u, &fr);

            if(frc == PS_FILE_OK) {
                res.ok     = 1;
                res.status = 200;
                res.data   = fr.data;
                res.len    = fr.len;
                strcpy(res.final_url, fr.final_url);
                fr.data = NULL;   /* ownership moves to the result */
            }
            else {
                /* Given an HTTP shape so the shell needs no second vocabulary
                 * for "it is not there": a missing file on a card and a 404
                 * are the same event to everything above this. */
                res.status = (frc == PS_FILE_ERR_NOT_FOUND) ? 404 : 0;
                printf("popsurf: %s: %s\n", job.url, ps_file_strerror(frc));
            }
            ps_file_response_free(&fr);
        }
        else {
            if(job.is_post)
                rc = ps_http_post(&u, NULL, job.body, strlen(job.body), &resp);
            else if(job.kind == PS_JOB_BANK && PS_BANK_PARTS)
                rc = fetch_parts(job.url, &resp);
            else if(job.kind == PS_JOB_BANK && PS_BANK_RANGED)
                rc = fetch_ranged(&u, &resp);
            else
                rc = ps_http_get(&u, &resp);
            if(rc == PS_HTTP_OK) {
                res.status = resp.status;
                if(resp.status == 200 && resp.body) {
                    res.ok   = 1;
                    res.data = resp.body;
                    res.len  = resp.body_len;
                    ps_url_format(&resp.final_url, res.final_url,
                                  sizeof res.final_url);
                    resp.body = NULL;   /* ownership moves to the result */
                }
                ps_http_response_free(&resp);
            }
            else {
                printf("popsurf: %s: %s\n", job.url, ps_http_strerror(rc));
            }
        }

        mutex_lock(&l->lock);
        l->in_flight = 0;

        /* Navigated away while this was in flight: throw it out. */
        if(job_epoch != l->epoch || l->d_count == PS_LOADER_QUEUE_MAX) {
            free(res.data);
        }
        else {
            l->done[l->d_tail] = res;
            l->d_tail = (l->d_tail + 1) % PS_LOADER_QUEUE_MAX;
            l->d_count++;
        }
    }

    mutex_unlock(&l->lock);
    return NULL;
}

ps_loader *ps_loader_create(void)
{
    ps_loader *l = (ps_loader *)calloc(1, sizeof *l);

    if(!l)
        return NULL;

    if(mutex_init(&l->lock, MUTEX_TYPE_NORMAL) < 0) {
        free(l);
        return NULL;
    }
    if(cond_init(&l->work) < 0) {
        mutex_destroy(&l->lock);
        free(l);
        return NULL;
    }

    l->running = 1;
    /* The fetch path is stack-hungry: ps_http_response embeds a whole ps_url,
     * and loader_main -> fetch_ranged -> http_run -> fetch_once -> read_head
     * stacks several of those alongside a 3KB header buffer, with one level of
     * retry recursion on top. KOS's default thread stack leaves little margin
     * for that, and overrunning it does not fail cleanly - it comes back as a
     * spurious exception from wherever the corrupted frame returned to. */
    {
        kthread_attr_t attr = {
            .create_detached = 0,
            .stack_size      = 64 * 1024,
            .prio            = PRIO_DEFAULT,
            .label           = "popsurf-loader",
        };

        l->thread = thd_create_ex(&attr, loader_main, l);
    }
    if(!l->thread) {
        cond_destroy(&l->work);
        mutex_destroy(&l->lock);
        free(l);
        return NULL;
    }

    return l;
}

void ps_loader_destroy(ps_loader *l)
{
    if(!l)
        return;

    mutex_lock(&l->lock);
    l->running = 0;
    cond_signal(&l->work);
    mutex_unlock(&l->lock);

    thd_join(l->thread, NULL);

    while(l->d_count) {
        free(l->done[l->d_head].data);
        l->d_head = (l->d_head + 1) % PS_LOADER_QUEUE_MAX;
        l->d_count--;
    }

    cond_destroy(&l->work);
    mutex_destroy(&l->lock);
    free(l);
}

static int loader_enqueue(ps_loader *l, ps_job_kind kind, const char *url,
                          const char *body)
{
    int rc = -1;

    if(!l || !url || strlen(url) >= PS_URL_MAX)
        return -1;
    if(body && strlen(body) >= PS_LOADER_BODY_MAX)
        return -1;

    mutex_lock(&l->lock);

    if(l->q_count < PS_LOADER_QUEUE_MAX) {
        ps_job *j = &l->queue[l->q_tail];

        j->kind = kind;
        strcpy(j->url, url);
        j->is_post = body ? 1 : 0;
        if(body)
            strcpy(j->body, body);
        else
            j->body[0] = '\0';
        l->q_tail = (l->q_tail + 1) % PS_LOADER_QUEUE_MAX;
        l->q_count++;
        cond_signal(&l->work);
        rc = 0;
    }

    mutex_unlock(&l->lock);
    return rc;
}

int ps_loader_request(ps_loader *l, ps_job_kind kind, const char *url)
{
    return loader_enqueue(l, kind, url, NULL);
}

int ps_loader_request_post(ps_loader *l, const char *url, const char *body)
{
    return loader_enqueue(l, PS_JOB_PAGE, url, body ? body : "");
}

int ps_loader_poll(ps_loader *l, ps_job_result *out)
{
    int rc = 0;

    if(!l || !out)
        return 0;

    mutex_lock(&l->lock);

    if(l->d_count) {
        *out = l->done[l->d_head];
        l->d_head = (l->d_head + 1) % PS_LOADER_QUEUE_MAX;
        l->d_count--;
        rc = 1;
    }

    mutex_unlock(&l->lock);
    return rc;
}

int ps_loader_pending(ps_loader *l)
{
    int n;

    if(!l)
        return 0;

    mutex_lock(&l->lock);
    n = l->q_count + l->in_flight;
    mutex_unlock(&l->lock);
    return n;
}

void ps_loader_cancel_all(ps_loader *l)
{
    if(!l)
        return;

    mutex_lock(&l->lock);

    l->epoch++;
    l->q_head = l->q_tail = l->q_count = 0;

    while(l->d_count) {
        free(l->done[l->d_head].data);
        l->d_head = (l->d_head + 1) % PS_LOADER_QUEUE_MAX;
        l->d_count--;
    }

    mutex_unlock(&l->lock);
}
