/* Single-worker asynchronous resource loader. */
#ifndef PS_LOADER_H
#define PS_LOADER_H

#include "ps_url.h"
#include "../core/ps_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Outstanding requests. A page of images can exceed this; the overflow is
 * dropped and those images simply do not appear, which beats unbounded queue
 * growth on hostile input. */
#define PS_LOADER_QUEUE_MAX PS_CFG_LOADER_QUEUE

typedef enum {
    PS_JOB_PAGE = 0,
    PS_JOB_IMAGE,
    PS_JOB_FRAME,
    PS_JOB_AUDIO,
    PS_JOB_BANK,

    /* A .class file for an <applet>. Kilobytes, like an image, and delivered
     * the same way - the applet cache runs it on arrival. */
    PS_JOB_APPLET,

    /* A Flash movie named by an <embed> or <object>. Same size class as an
     * applet and delivered the same way; the shell parses it on arrival. */
    PS_JOB_SWF
} ps_job_kind;

typedef struct {
    ps_job_kind kind;
    char        url[PS_URL_MAX];
    char        final_url[PS_URL_MAX];

    int    ok;         /* non-zero when data is valid */
    int    status;     /* HTTP status, when there was one */
    char  *data;       /* caller takes ownership and frees */
    size_t len;
} ps_job_result;

typedef struct ps_loader ps_loader;

ps_loader *ps_loader_create(void);
void       ps_loader_destroy(ps_loader *l);

/* Enqueues a fetch. Returns 0 if queued. Never blocks. */
int ps_loader_request(ps_loader *l, ps_job_kind kind, const char *url);

/* Same, but POSTs body as application/x-www-form-urlencoded. The body is
 * copied, so the caller may free it immediately. */
int ps_loader_request_post(ps_loader *l, const char *url, const char *body);

/* Longest form body we will send. A DC-era form that exceeds this is far more
 * likely to be a bug than a real submission. */
#define PS_LOADER_BODY_MAX PS_CFG_MAX_BODY_BYTES

/* Non-blocking. Returns 1 and fills out when a job has finished. */
int ps_loader_poll(ps_loader *l, ps_job_result *out);

/* Requests in flight or waiting, for progress display. */
int ps_loader_pending(ps_loader *l);

/* Abandons queued work, e.g. when navigating away mid-load. Results already
 * completed are discarded too. */
void ps_loader_cancel_all(ps_loader *l);

#ifdef __cplusplus
}
#endif

#endif /* PS_LOADER_H */
