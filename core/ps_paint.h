/* POPSurf paint layer.
 *
 * Immediate mode with a clip stack, batching quads until the texture changes.
 * litehtml drives painting by calling back into the document container once
 * per box in paint order, so recording a separate display list buffer would
 * only duplicate a traversal that already exists. If a future feature needs a
 * retained list (partial redraw, scroll reuse), it goes in here without the
 * callers changing.
 *
 * Clipping is geometric: destination rects and their UVs are clamped to the
 * clip rect. Exact, free, and identical on every backend.
 *
 * The retained list below is the "future feature" this file's original note
 * anticipated, and the measurement that called for it is worth recording:
 *
 *   page draw 10612 us/f = traverse 10157 + submit 454, 253 quads/f
 *
 * Ten milliseconds of a sixteen millisecond frame spent walking a page that
 * had not changed, to produce two hundred and fifty quads that took under half
 * a millisecond to hand over. Ninety-six percent of the cost was deciding what
 * to draw and four percent was drawing it, which is exactly the case a
 * retained list exists for - and exactly the opposite of the case where it
 * would be pointless, since replaying submits the same vertices either way.
 */
#ifndef PS_PAINT_H
#define PS_PAINT_H

#include "ps_types.h"
#include "../gfx/ps_gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PS_CLIP_STACK_MAX 32

/* Batch size in quads. Flushed on texture change, overflow, or frame end. */
#define PS_BATCH_QUADS 256

typedef struct {
    const ps_gfx_backend *gfx;

    ps_rect clip[PS_CLIP_STACK_MAX];
    int     clip_depth;

    ps_vert    batch[PS_BATCH_QUADS * PS_VERTS_PER_QUAD];
    int        batch_verts;
    ps_texture batch_tex;

    /* Paint-order counter, reset every frame. See ps_vert::z. */
    uint32_t quad_seq;

    /* --- retained list ---------------------------------------------------
     *
     * Recording captures what a draw produced so an unchanged page can be
     * replayed instead of re-walked. Segments exist because a batch is
     * flushed whenever the texture changes, and replay has to reproduce those
     * boundaries - a page is text, boxes and images interleaved, so there are
     * a few dozen of them.
     *
     * Capped rather than grown without limit: a page that exceeds it simply
     * does not get recorded and pays what it paid before, which is the right
     * failure for a feature that is purely an optimisation. */
    ps_vert  *rec_verts;
    int       rec_nverts, rec_cap;

    struct ps_paint_seg { ps_texture tex; int first, count; } *rec_segs;
    int       rec_nsegs, rec_segcap;

    /* Base clip when the stack is empty. Zero means the screen. */
    int       bounds_w, bounds_h;

    int       recording;
    int       rec_valid;      /* a complete recording is held */
    int       rec_overflow;   /* this page is too big to record */
} ps_paint;

/* Depth handed to quad n. The step is ~128 float ulps near 1.0, so adjacent
 * quads never collide, and there is deliberately no ceiling: z simply keeps
 * climbing for as many quads as a page emits. */
#define PS_Z_BASE 1.0f
#define PS_Z_STEP (1.0f / 65536.0f)

void ps_paint_init(ps_paint *p, const ps_gfx_backend *gfx);

void ps_paint_begin(ps_paint *p, ps_color clear);
void ps_paint_end(ps_paint *p);

/* Clips intersect with the enclosing clip, so a push can never widen the
 * visible area. Overflow past PS_CLIP_STACK_MAX pushes an empty rect rather
 * than failing, which degrades to "draws nothing" instead of "draws wrong". */
void ps_paint_push_clip(ps_paint *p, const ps_rect *r);
void ps_paint_pop_clip(ps_paint *p);

void ps_paint_rect(ps_paint *p, const ps_rect *r, ps_color c);

/* Vertical gradient, interpolated across the quad by the hardware.
 *
 * Baking a gradient into an ARGB4444 texture gives 16 levels per channel and
 * bands visibly. Per-vertex colours are interpolated by the PVR and land in
 * the RGB565 framebuffer instead - 32 levels of red and blue, 64 of green -
 * which is the difference between metallic shading and stripes. It also costs
 * no VRAM and no upload. */
void ps_paint_rect_v(ps_paint *p, const ps_rect *r, ps_color top,
                     ps_color bottom);

/* Textured quad. u0/v0/u1/v1 are normalized. */
void ps_paint_image(ps_paint *p, ps_texture tex, const ps_rect *dst,
                    float u0, float v0, float u1, float v1, ps_color tint);

void ps_paint_flush(ps_paint *p);

/* Takes `n` depth steps out of the paint order and returns the first.
 *
 * For geometry that does not come through this file at all - a SWF movie hands
 * the tile accelerator its own triangles - but that has to land between the
 * boxes painted before it and the chrome painted after. Depth is the only thing
 * that decides that, since the translucent list compares with GREATER, so
 * something submitting outside the batch still has to draw its depths from the
 * same sequence. */
float ps_paint_reserve_z(ps_paint *p, int n);

/* --- retained list -------------------------------------------------------
 *
 * Between record_begin and record_end nothing reaches the screen - the draw is
 * captured only. That is deliberate: a recording is made at the document's own
 * coordinates and shown by replaying it at the right scroll offset, so
 * submitting it as it was captured would put an unscrolled copy of the page on
 * screen for exactly one frame.
 *
 * The caller decides what "changed" means, and must be conservative: a missed
 * invalidation shows a stale page, which is far worse than a redraw. What does
 * not need one is an applet or an animated image repainting into a texture it
 * already owns - the quads name the texture, not its contents, so replaying
 * them shows the new pixels for free.
 */
void ps_paint_record_begin(ps_paint *p);
void ps_paint_record_end(ps_paint *p);

/* Widens the clip that applies when the stack is empty.
 *
 * Normally the screen, because nothing off it can be seen. While recording a
 * whole document it has to be the document, or every quad above or below the
 * viewport is clamped away and the recording only holds what happened to be
 * on screen when it was made - which is the one thing that would make it
 * useless for scrolling. Restored by passing zero. */
void ps_paint_set_bounds(ps_paint *p, int w, int h);

/* Non-zero if a complete recording is held and can be replayed. */
int  ps_paint_can_replay(const ps_paint *p);

/* Submits the recorded list. Advances quad_seq past it, so anything drawn
 * afterwards - the toolbar, the cursor - still lands in front. */
void ps_paint_replay(ps_paint *p);

/* Replays shifted vertically. This is what makes scrolling free: the page was
 * recorded once at its own coordinates, and a scroll is the same quads at a
 * different offset. Quads that land off screen are the hardware's problem,
 * which it solves by tiling. */
void ps_paint_replay_offset(ps_paint *p, float dy);

/* Quads held, so a caller can tell whether a page fitted. */
int ps_paint_recorded_quads(const ps_paint *p);

/* Drops the recording. Call when the page changes shape. */
void ps_paint_record_drop(ps_paint *p);

#ifdef __cplusplus
}
#endif

#endif /* PS_PAINT_H */
