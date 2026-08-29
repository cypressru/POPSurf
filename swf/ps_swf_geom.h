/* The stages a span renderer and a triangle renderer have in common.
 *
 * Internal to the SWF drawing code; not part of ps_swf.h, because nothing
 * outside needs to know that a chord exists. What lives here is everything
 * that is the same work whether the answer ends up as horizontal runs or as
 * geometry for a tile accelerator:
 *
 *   1. flatten  - edges become straight chords in output pixel space, at a
 *                 stated tolerance.
 *   2. select   - for one style in one layer, gather the chords that bound it
 *                 and give them a consistent direction. This is where SWF's
 *                 dual-sided fill encoding is resolved, and where a stroke
 *                 becomes an outline it can bound.
 *   3. paint    - work out what colour that region is, which for a gradient
 *                 means inverting the fill matrix once per pass.
 *
 * Stage 4 is the part that differs, and there are two of them: ps_sweep in
 * ps_swf_raster.c, and the tessellator in ps_swf_tess.c. Declaring ps_sweep
 * here rather than keeping it private is deliberate - the host triangle
 * backend uses it to rasterise what the tessellator produced, and comparing
 * that against the span renderer's output is what validates the tessellation
 * without hardware.
 *
 * The scratch buffers are handed in rather than allocated per pass. A shape
 * with twenty-three layers runs twenty-three passes over the same geometry,
 * and the buffer a pass needs is a function of the chord count, not of which
 * pass it is - so it is sized once for the worst pass and reused.
 */
#ifndef PS_SWF_GEOM_H
#define PS_SWF_GEOM_H

#include "ps_swf.h"

typedef struct {
    float    x0, y0, x1, y1;
    uint16_t f0, f1, ln;
    uint8_t  layer;
} ps_chord;

/* A chord prepared for a sweep or a tessellation: y-sorted, with the sign of
 * the original direction kept so winding still works after the swap. */
typedef struct {
    float x0, y0, x1, y1;
    int   dir;
} ps_aedge;

typedef struct {
    float x;
    int   dir;
} ps_crossing;

typedef struct {
    ps_chord *ch;
    long      nch;

    /* Two indices per layer bounding that layer's chords. Every pass is over
     * one layer, and without this each pass scans every chord in the shape:
     * the sample's largest shape is twenty-three layers by twenty-three fill
     * styles by 1844 chords, which is 975k iterations of a loop that only ever
     * matches a twenty-third of what it touches. Chords come out of flatten in
     * file order and a shape's layer number never decreases, so the range is
     * contiguous and one pass over the array finds it. The min/max form is
     * used rather than a prefix sum because it degrades into a correct
     * superset if that ordering assumption is ever violated. */
    uint32_t *lrange;

    /* Four per layer bounding the style indices that layer's chords actually
     * name: lowest and highest fill, then lowest and highest line, with zero
     * for none - style numbering is 1-based, so zero cannot be a real style.
     *
     * The same argument as lrange, one level in. A shape's fill table is the
     * concatenation of every layer's, because StateNewStyles appends rather
     * than replaces and the parser rebases the indices onto the whole; so a
     * pass loop written over the table asks every layer about every other
     * layer's styles, and each of those questions is answered by rescanning
     * the layer's chords and matching nothing. The Crazy Taxi 2 logo is the
     * case that makes it visible: 23 layers, one style each, so 22 scans in
     * 23 are dead. Bounds rather than an exact set, for lrange's reason - a
     * range that is too wide is slow and still correct, and a shape whose
     * styles are interleaved across layers is a file we have never seen. */
    uint32_t *lstyle;

    uint32_t  nlayer;

    /* Sides in the polygon that stands in for a round cap or join, shared by
     * every stroke in the shape so the buffer sizing below is a constant. */
    int       sides;

    /* Directed edges the worst single pass can produce. */
    long      max_edges;
} ps_geom;

typedef struct {
    ps_aedge    *ae;
    ps_crossing *xs;
    float       *cov;
} ps_scratch;

/* `xf` maps shape twips to output pixels. It is consumed here and nowhere
 * else: everything downstream of flatten works in pixels, which is what lets a
 * rotated placement cost nothing extra in the rasteriser. */
int  ps_geom_build(const ps_swf_shape *sh, const ps_swf_view *v,
                   const ps_swf_xform *xf, ps_geom *g);
void ps_geom_free(ps_geom *g);

int  ps_scratch_init(ps_scratch *s, const ps_geom *g, const ps_swf_view *v);
void ps_scratch_free(ps_scratch *s);

/* Both return the number of directed edges written to `out`, which holds at
 * most g->max_edges of them. */
long ps_geom_fill_edges(const ps_geom *g, uint32_t layer, uint32_t style,
                        ps_aedge *out);
long ps_geom_line_edges(const ps_geom *g, uint32_t layer, uint32_t style,
                        float hw, ps_aedge *out);

float ps_geom_stroke_half(const ps_swf_shape *sh, uint32_t idx,
                          const ps_swf_xform *xf);

/* Signed area the directed edges enclose, by the shoelace identity, in square
 * output pixels. Exact, and independent of any rasteriser - which is what
 * makes it usable as the reference a tessellation's own triangle areas are
 * checked against. A hole comes out negative and cancels the contour around
 * it, so the total is the filled area rather than the sum of the outlines. */
double ps_geom_area(const ps_aedge *e, long n);

/* style is 1-based into sh->fills. `cx` may be NULL. */
void ps_geom_fill_paint(const ps_swf_shape *sh, const ps_swf_xform *xf,
                        const ps_swf_cxform *cx, uint32_t style,
                        ps_swf_paint *p);
void ps_geom_flat_paint(ps_swf_rgba color, const ps_swf_cxform *cx,
                        ps_swf_paint *p);

ps_swf_rgba ps_paint_at(const ps_swf_paint *p, float x, float y);

/* Appends one directed edge, dropping it if it is exactly horizontal. Shared
 * so that anything building an edge list - the stroker, the tessellator's host
 * backend - agrees with the sweep about what a degenerate edge is. */
void ps_add_edge(ps_aedge *e, long *n, float x0, float y0, float x1, float y1);

void ps_sweep(const ps_aedge *e, long n, const ps_swf_view *v,
              const ps_swf_paint *p, ps_swf_span_fn span, void *user,
              float *cov, ps_crossing *xs);

#endif /* PS_SWF_GEOM_H */
