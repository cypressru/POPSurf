/* The host backend for the clip interface: a filter on the span callback.
 *
 * Sits between a renderer and wherever pixels go, so anything that emits spans
 * is clipped by it without knowing it exists - which is why the span renderer
 * and the triangle path are masked by one implementation rather than two. See
 * ps_swf_clip.c for what the hardware does instead, and for what this file
 * costs that the hardware must not pay.
 */
#ifndef PS_SWF_CLIP_H
#define PS_SWF_CLIP_H

#include "ps_swf.h"

typedef struct {
    const ps_swf_view *view;
    ps_swf_span_fn     out;         /* where clipped spans go */
    void              *user;

    /* One coverage plane per live mask, holding that mask intersected with
     * everything that was already in force when it was pushed. Allocated on
     * first use and kept for the object's life: a movie that masks on one frame
     * masks on the next, and the buffer is the same size either way. */
    uint8_t           *plane[PS_SWF_CLIP_DEPTH];
    uint8_t            applied[PS_SWF_CLIP_DEPTH];
    int                n;

    /* The plane a span is written into, or -1 for "out". The innermost mask
     * still collecting geometry, in other words. */
    int                dest;
    /* The plane a span is multiplied by, or -1 for none. */
    int                by;

    int                failed;      /* sticky; see ps_swf_clip_failed */
} ps_swf_clip;

/* `out` is where clipped spans go. Returns 0, or -1 if the first plane could
 * not be allocated - which is reported here rather than at the first mask so a
 * caller finds out before it has drawn half a frame. */
[[nodiscard]] int ps_swf_clip_init(ps_swf_clip *c, const ps_swf_view *v,
                                   ps_swf_span_fn out, void *user);
void ps_swf_clip_free(ps_swf_clip *c);

/* The three the stage sink calls. They cannot report failure - the sink's
 * callbacks return nothing, because on hardware there is nothing to report,
 * submission either fits in the list or the list is full - so an allocation
 * that fails sets the sticky flag below and everything after it draws nothing.
 * Silence would mean a frame with the masking quietly switched off, which looks
 * like a rendering bug rather than an allocation failure. */
void ps_swf_clip_begin(ps_swf_clip *c);
void ps_swf_clip_apply(ps_swf_clip *c);
void ps_swf_clip_end(ps_swf_clip *c);

/* A ps_swf_span_fn whose user pointer is the ps_swf_clip. */
void ps_swf_clip_span(void *user, int y, int x0, int x1, uint8_t cov,
                      ps_swf_rgba color);

[[nodiscard]] int ps_swf_clip_failed(const ps_swf_clip *c);

/* Bytes of coverage plane held. Reported separately from the allocator's own
 * total because this is the number the hardware path exists to keep at zero. */
size_t ps_swf_clip_bytes(const ps_swf_clip *c);

#endif /* PS_SWF_CLIP_H */
