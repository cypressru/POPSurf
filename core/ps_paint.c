#include "ps_paint.h"

#ifdef PS_PAINT_PROFILE
#include <arch/timer.h>
#include <stdio.h>

/* Splits the page draw in two.
 *
 * Eleven milliseconds a frame goes into drawing a page that has not changed,
 * and there are two entirely different explanations. Either litehtml's
 * traversal and our quad building are the cost - in which case a retained
 * display list replays it and the header already says that belongs in this
 * file - or it is handing the vertices to the hardware, in which case a
 * retained list submits exactly the same ones and buys nothing, and the answer
 * is DCA3's pre-armed store queue instead.
 *
 * Same shape of question as the pack against the transfer, which I got wrong
 * by reasoning about it. This counts instead. */
uint64_t ps_paint_prof_submit_us;
uint64_t ps_paint_prof_quads;
#endif

#include <stdlib.h>
#include <string.h>

void ps_paint_init(ps_paint *p, const ps_gfx_backend *gfx)
{
    memset(p, 0, sizeof *p);
    p->gfx       = gfx;
    p->batch_tex = PS_TEXTURE_NONE;
}

static ps_rect current_clip(const ps_paint *p)
{
    int d = p->clip_depth;

    /* Depth can run past the stack; see ps_paint_push_clip. */
    if(d > PS_CLIP_STACK_MAX)
        d = PS_CLIP_STACK_MAX;

    if(d > 0)
        return p->clip[d - 1];

    ps_rect full;
    full.x0 = 0;
    full.y0 = 0;
    full.x1 = (int16_t)(p->bounds_w ? p->bounds_w : p->gfx->width);
    full.y1 = (int16_t)(p->bounds_h ? p->bounds_h : p->gfx->height);
    return full;
}

void ps_paint_begin(ps_paint *p, ps_color clear)
{
    p->clip_depth  = 0;
    p->batch_verts = 0;
    p->batch_tex   = PS_TEXTURE_NONE;
    p->quad_seq    = 0;
    p->gfx->begin_frame(p->gfx->self, clear);
}

void ps_paint_end(ps_paint *p)
{
    ps_paint_flush(p);
    p->gfx->end_frame(p->gfx->self);
}

void ps_paint_flush(ps_paint *p)
{
#ifdef PS_PAINT_PROFILE
    uint64_t _t0 = timer_us_gettime64();
#endif
    if(p->batch_verts > 0) {
        /* Captured as well as drawn. The batch is already grouped by texture,
         * which is exactly the grouping a replay needs. */
        if(p->recording && !p->rec_overflow) {
            if(p->rec_nverts + p->batch_verts > p->rec_cap ||
               p->rec_nsegs >= p->rec_segcap) {
                p->rec_overflow = 1;
            }
            else {
                memcpy(&p->rec_verts[p->rec_nverts], p->batch,
                       (size_t)p->batch_verts * sizeof(ps_vert));
                p->rec_segs[p->rec_nsegs].tex   = p->batch_tex;
                p->rec_segs[p->rec_nsegs].first = p->rec_nverts;
                p->rec_segs[p->rec_nsegs].count = p->batch_verts;
                p->rec_nsegs++;
                p->rec_nverts += p->batch_verts;
            }
        }

        /* Captured but not drawn. A recording is made at document
         * coordinates and shown by a replay at the right offset; submitting
         * it here as well would put an unscrolled copy of the page on screen
         * for the one frame that records it. */
        if(!p->recording)
            p->gfx->draw_quads(p->gfx->self, p->batch_tex, p->batch,
                               p->batch_verts);
#ifdef PS_PAINT_PROFILE
    ps_paint_prof_submit_us += timer_us_gettime64() - _t0;
    ps_paint_prof_quads += (uint64_t)(p->batch_verts / PS_VERTS_PER_QUAD);
#endif
        p->batch_verts = 0;
    }
}

float ps_paint_reserve_z(ps_paint *p, int n)
{
    float z = PS_Z_BASE + (float)p->quad_seq * PS_Z_STEP;

    if(n > 0)
        p->quad_seq += (uint32_t)n;
    return z;
}

/* --- retained list ------------------------------------------------------- */

/* Quads one page may record. At four vertices of thirty-two bytes each this
 * is a megabyte at the ceiling, and the measured page used 253 - so the cap is
 * four thousand times what a real page needs and exists only to stop a
 * pathological one eating the heap. */
#define PS_REC_MAX_QUADS 8192
#define PS_REC_MAX_SEGS  512

void ps_paint_set_bounds(ps_paint *p, int w, int h)
{
    p->bounds_w = w;
    p->bounds_h = h;
}

int ps_paint_recorded_quads(const ps_paint *p)
{
    return p->rec_nverts / PS_VERTS_PER_QUAD;
}

void ps_paint_record_drop(ps_paint *p)
{
    p->rec_nverts   = 0;
    p->rec_nsegs    = 0;
    p->rec_valid    = 0;
    p->rec_overflow = 0;
}

void ps_paint_record_begin(ps_paint *p)
{
    ps_paint_flush(p);

    p->rec_nverts   = 0;
    p->rec_nsegs    = 0;
    p->rec_valid    = 0;
    p->rec_overflow = 0;
    p->recording    = 1;

    if(!p->rec_verts) {
        p->rec_verts = (ps_vert *)malloc((size_t)PS_REC_MAX_QUADS *
                                         PS_VERTS_PER_QUAD * sizeof(ps_vert));
        p->rec_segs  = (struct ps_paint_seg *)
                       malloc((size_t)PS_REC_MAX_SEGS *
                              sizeof(struct ps_paint_seg));
        p->rec_cap    = p->rec_verts ? PS_REC_MAX_QUADS * PS_VERTS_PER_QUAD : 0;
        p->rec_segcap = p->rec_segs  ? PS_REC_MAX_SEGS : 0;
    }

    if(!p->rec_verts || !p->rec_segs)
        p->rec_overflow = 1;
}

void ps_paint_record_end(ps_paint *p)
{
    ps_paint_flush(p);

    p->recording = 0;
    p->rec_valid = !p->rec_overflow && p->rec_nverts > 0;
}

int ps_paint_can_replay(const ps_paint *p)
{
    return p->rec_valid;
}

void ps_paint_replay_offset(ps_paint *p, float dy)
{
    int i, k;

    if(!p->rec_valid)
        return;

    ps_paint_flush(p);

    if(dy == 0.0f) {
        ps_paint_replay(p);
        return;
    }

    /* Shifted in place through the batch, a segment at a time, so no second
     * copy of the page is held. Four adds a quad against a DOM walk. */
    for(i = 0; i < p->rec_nsegs; i++) {
        const ps_vert *src = &p->rec_verts[p->rec_segs[i].first];
        int            n   = p->rec_segs[i].count;
        int            done = 0;

        while(done < n) {
            int chunk = n - done;

            if(chunk > PS_BATCH_QUADS * PS_VERTS_PER_QUAD)
                chunk = PS_BATCH_QUADS * PS_VERTS_PER_QUAD;

            for(k = 0; k < chunk; k++) {
                p->batch[k]    = src[done + k];
                p->batch[k].y += dy;
            }
            p->gfx->draw_quads(p->gfx->self, p->rec_segs[i].tex, p->batch,
                               chunk);
            done += chunk;
        }
    }

    p->batch_verts = 0;
    p->quad_seq += (uint32_t)(p->rec_nverts / PS_VERTS_PER_QUAD);
}

void ps_paint_replay(ps_paint *p)
{
    int i;

    if(!p->rec_valid)
        return;

    ps_paint_flush(p);

    for(i = 0; i < p->rec_nsegs; i++) {
        p->gfx->draw_quads(p->gfx->self, p->rec_segs[i].tex,
                           &p->rec_verts[p->rec_segs[i].first],
                           p->rec_segs[i].count);
    }

    /* Everything drawn after this - toolbar, keyboard, cursor - has to sit in
     * front of the replayed page, and depth is what decides that. */
    p->quad_seq += (uint32_t)(p->rec_nverts / PS_VERTS_PER_QUAD);
}

void ps_paint_push_clip(ps_paint *p, const ps_rect *r)
{
    ps_rect cur  = current_clip(p);
    ps_rect next = ps_rect_isect(&cur, r);

    /* Keep counting beyond the cap so later pops remain balanced. */
    if(p->clip_depth < PS_CLIP_STACK_MAX)
        p->clip[p->clip_depth] = next;

    p->clip_depth++;
}

void ps_paint_pop_clip(ps_paint *p)
{
    if(p->clip_depth > 0)
        p->clip_depth--;
}

/* Append one quad, flushing first if the texture changes or the batch is full. */
static ps_vert *alloc_quad(ps_paint *p, ps_texture tex)
{
    ps_vert *q;

    if(p->batch_verts > 0 && p->batch_tex != tex)
        ps_paint_flush(p);

    if(p->batch_verts + PS_VERTS_PER_QUAD >
       PS_BATCH_QUADS * PS_VERTS_PER_QUAD)
        ps_paint_flush(p);

    p->batch_tex = tex;
    q = &p->batch[p->batch_verts];
    p->batch_verts += PS_VERTS_PER_QUAD;
    return q;
}

/* Fill a quad in the PVR's strip order: TL, TR, BL, BR. */
static void emit_quad(ps_vert *q, const ps_rect *r,
                      float u0, float v0, float u1, float v1,
                      ps_color c, float z)
{
    q[0].x = (float)r->x0; q[0].y = (float)r->y0; q[0].u = u0; q[0].v = v0;
    q[1].x = (float)r->x1; q[1].y = (float)r->y0; q[1].u = u1; q[1].v = v0;
    q[2].x = (float)r->x0; q[2].y = (float)r->y1; q[2].u = u0; q[2].v = v1;
    q[3].x = (float)r->x1; q[3].y = (float)r->y1; q[3].u = u1; q[3].v = v1;

    q[0].argb = q[1].argb = q[2].argb = q[3].argb = c;
    q[0].z = q[1].z = q[2].z = q[3].z = z;
}

/* Depth for the next quad, advancing paint order. */
static float next_z(ps_paint *p)
{
    return PS_Z_BASE + (float)(p->quad_seq++) * PS_Z_STEP;
}

void ps_paint_rect(ps_paint *p, const ps_rect *r, ps_color c)
{
    ps_rect  clip = current_clip(p);
    ps_rect  vis  = ps_rect_isect(&clip, r);
    ps_vert *q;

    if(ps_rect_empty(&vis))
        return;

    /* Fully transparent fills are common in CSS and cost a whole quad. */
    if(PS_COLOR_A(c) == 0)
        return;

    q = alloc_quad(p, PS_TEXTURE_NONE);
    emit_quad(q, &vis, 0.0f, 0.0f, 0.0f, 0.0f, c, next_z(p));
}

/* Per-channel blend, t in 0..256. */
static ps_color lerp_color(ps_color a, ps_color b, int t)
{
    int ia = 256 - t;

    return PS_ARGB((PS_COLOR_A(a) * ia + PS_COLOR_A(b) * t) >> 8,
                   (PS_COLOR_R(a) * ia + PS_COLOR_R(b) * t) >> 8,
                   (PS_COLOR_G(a) * ia + PS_COLOR_G(b) * t) >> 8,
                   (PS_COLOR_B(a) * ia + PS_COLOR_B(b) * t) >> 8);
}

void ps_paint_rect_v(ps_paint *p, const ps_rect *r, ps_color top,
                     ps_color bottom)
{
    ps_rect  clip = current_clip(p);
    ps_rect  vis  = ps_rect_isect(&clip, r);
    ps_vert *q;
    float    z;
    int      h;

    if(ps_rect_empty(&vis))
        return;
    if(PS_COLOR_A(top) == 0 && PS_COLOR_A(bottom) == 0)
        return;

    /* Clipping has to carry the gradient with it, the same way image clipping
     * carries UVs: otherwise a partly-hidden button shifts colour instead of
     * being cropped. */
    h = r->y1 - r->y0;
    if(h > 0 && (vis.y0 != r->y0 || vis.y1 != r->y1)) {
        ps_color t0 = lerp_color(top, bottom, ((vis.y0 - r->y0) << 8) / h);
        ps_color t1 = lerp_color(top, bottom, ((vis.y1 - r->y0) << 8) / h);

        top    = t0;
        bottom = t1;
    }

    q = alloc_quad(p, PS_TEXTURE_NONE);
    z = next_z(p);

    q[0].x = (float)vis.x0; q[0].y = (float)vis.y0;
    q[1].x = (float)vis.x1; q[1].y = (float)vis.y0;
    q[2].x = (float)vis.x0; q[2].y = (float)vis.y1;
    q[3].x = (float)vis.x1; q[3].y = (float)vis.y1;

    q[0].u = q[1].u = q[2].u = q[3].u = 0.0f;
    q[0].v = q[1].v = q[2].v = q[3].v = 0.0f;
    q[0].z = q[1].z = q[2].z = q[3].z = z;

    q[0].argb = q[1].argb = top;
    q[2].argb = q[3].argb = bottom;
}

void ps_paint_image(ps_paint *p, ps_texture tex, const ps_rect *dst,
                    float u0, float v0, float u1, float v1, ps_color tint)
{
    ps_rect  clip = current_clip(p);
    ps_rect  vis  = ps_rect_isect(&clip, dst);
    ps_vert *q;
    float    fu0, fv0, fu1, fv1;
    float    dw, dh;

    if(ps_rect_empty(&vis))
        return;

    /* Clipping the destination has to carry the UVs with it, or the image
     * shifts instead of being cropped. */
    dw = (float)(dst->x1 - dst->x0);
    dh = (float)(dst->y1 - dst->y0);

    fu0 = u0 + (u1 - u0) * ((float)(vis.x0 - dst->x0) / dw);
    fu1 = u0 + (u1 - u0) * ((float)(vis.x1 - dst->x0) / dw);
    fv0 = v0 + (v1 - v0) * ((float)(vis.y0 - dst->y0) / dh);
    fv1 = v0 + (v1 - v0) * ((float)(vis.y1 - dst->y0) / dh);

    q = alloc_quad(p, tex);
    emit_quad(q, &vis, fu0, fv0, fu1, fv1, tint, next_z(p));
}
