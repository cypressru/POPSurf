/* Flatten, select and paint: the work both renderers do before they diverge.
 *
 * Fill rule is nonzero winding. Even-odd would be wrong: a SWF shape is edge
 * soup with no guaranteed contour ordering, and the only thing that makes a
 * hole a hole is the direction its edges run. Nonzero also makes the
 * degenerate cases behave - an edge with the same style on both sides is added
 * twice with opposite signs and cancels itself out, which is exactly what an
 * interior edge between two pieces of one fill should do.
 *
 * The dual-sided reading (style 1 is the positive side, style 0 the negative,
 * so style 0's edges must be reversed before they can be used together) is
 * derived from Ruffle's render/src/shape_utils.rs, which is MIT OR
 * Apache-2.0. Ruffle stitches the segments into closed paths first because its
 * backends draw paths; neither consumer here needs them closed, only
 * consistently directed, so that step is not reproduced.
 */
#include "ps_swf_geom.h"
#include "ps_swf_image.h"
#include "ps_swf_mem.h"

#include <math.h>
#include <string.h>

/* A curve never becomes more than this many chords. Without a cap the segment
 * count is a function of the control points, which come from the file, and a
 * deliberately extreme control point would otherwise be an allocation attack.
 * 64 chords is well past the point where more helps at Dreamcast resolution. */
#define MAX_CHORDS 64

/* Likewise for the polygon standing in for a round cap or join. Sixteen sides
 * is a third of a pixel of sag on a twenty-pixel radius, which is as wide a
 * stroke as Flash 3 artwork produces at Dreamcast scale. */
#define MAX_SIDES 16

/* --- stage 1: flatten --------------------------------------------------- */

/* Shape twips through the accumulated transform, straight to output pixels.
 * Every point in the shape goes through this once and nothing downstream knows
 * a transform existed - which is why a rotated or skewed placement costs the
 * rasteriser nothing that an axis-aligned one does not. */
static void tw2px(const ps_swf_xform *xf, int32_t tx, int32_t ty,
                  float *px, float *py)
{
    float x = (float)tx, y = (float)ty;

    *px = xf->m[0] * x + xf->m[2] * y + xf->m[4];
    *py = xf->m[1] * x + xf->m[3] * y + xf->m[5];
}

/* Chords needed to hold a quadratic within `tol`.
 *
 * A quadratic Bezier has a constant second derivative, B'' = 2(P0 - 2C + P1),
 * so the chord error over a parameter step h is at most |B''|h^2/8. With n
 * equal steps that is |P0 - 2C + P1| / (4n^2), and inverting gives n directly.
 * Worth doing closed-form rather than by recursive subdivision: recursion
 * costs a stack whose depth is also driven by file data, and on SH-4 the
 * whole point of the tolerance knob is to make the cost predictable. */
static int chord_count(float x0, float y0, float cx, float cy,
                       float x1, float y1, float tol)
{
    float dx = x0 - 2.0f * cx + x1;
    float dy = y0 - 2.0f * cy + y1;
    float d  = sqrtf(dx * dx + dy * dy);
    float n;

    if(tol <= 0.0f)
        tol = 0.1f;
    n = sqrtf(d / (4.0f * tol));
    if(!(n > 1.0f))
        return 1;
    if(n >= (float)MAX_CHORDS)
        return MAX_CHORDS;
    return (int)n + 1;
}

/* Sides in the polygon that replaces a round cap or join.
 *
 * A regular n-gon inscribed in a circle of radius r sags r(1 - cos(pi/n))
 * inside the arc, which is r*pi^2/(2n^2) for the angles that matter, so
 * n = pi*sqrt(r/(2*tol)) holds the same tolerance the curve flattener does.
 * Four is the floor because below that the "disc" is a diamond whose area is
 * a third of the circle's, and a cap that visibly wrong is worse than none. */
static int disc_sides(float r, float tol)
{
    float n;

    if(tol <= 0.0f)
        tol = 0.1f;
    n = 3.14159265f * sqrtf(r / (2.0f * tol));
    if(!(n > 4.0f))
        return 4;
    if(n >= (float)MAX_SIDES)
        return MAX_SIDES;
    return (int)n + 1;
}

static long flatten(const ps_swf_shape *sh, const ps_swf_view *v,
                    const ps_swf_xform *xf, ps_chord **out)
{
    uint32_t  i;
    long      total = 0;
    ps_chord *c;
    long      k = 0;

    /* Counted first and allocated exactly. The count is cheap - one square
     * root per curve - and it avoids a growth policy whose overshoot would be
     * the largest single allocation in the renderer. */
    for(i = 0; i < sh->nedge; i++) {
        const ps_swf_edge *e = &sh->edges[i];
        float ax, ay, px, py, bx, by;

        if(!e->curve) {
            total++;
            continue;
        }
        tw2px(xf, e->x0, e->y0, &ax, &ay);
        tw2px(xf, e->cx, e->cy, &px, &py);
        tw2px(xf, e->x1, e->y1, &bx, &by);
        total += chord_count(ax, ay, px, py, bx, by, v->tol);
    }
    if(total == 0) {
        *out = NULL;
        return 0;
    }

    c = ps_swf_alloc((size_t)total * sizeof *c);
    if(!c)
        return -1;

    for(i = 0; i < sh->nedge; i++) {
        const ps_swf_edge *e = &sh->edges[i];
        float ax, ay, bx, by;

        tw2px(xf, e->x0, e->y0, &ax, &ay);
        tw2px(xf, e->x1, e->y1, &bx, &by);

        if(!e->curve) {
            c[k].x0 = ax; c[k].y0 = ay;
            c[k].x1 = bx; c[k].y1 = by;
            c[k].f0 = e->fill0; c[k].f1 = e->fill1;
            c[k].ln = e->line;  c[k].layer = e->layer;
            k++;
        } else {
            float px, py;
            int   n;
            int   s;
            float lx = ax, ly = ay;

            tw2px(xf, e->cx, e->cy, &px, &py);
            n = chord_count(ax, ay, px, py, bx, by, v->tol);

            for(s = 1; s <= n; s++) {
                float t  = (float)s / (float)n;
                float u  = 1.0f - t;
                float qx = u * u * ax + 2.0f * u * t * px + t * t * bx;
                float qy = u * u * ay + 2.0f * u * t * py + t * t * by;

                c[k].x0 = lx; c[k].y0 = ly;
                c[k].x1 = qx; c[k].y1 = qy;
                c[k].f0 = e->fill0; c[k].f1 = e->fill1;
                c[k].ln = e->line;  c[k].layer = e->layer;
                k++;
                lx = qx; ly = qy;
            }
        }
    }
    *out = c;
    return k;
}

/* --- build and scratch --------------------------------------------------- */

float ps_geom_stroke_half(const ps_swf_shape *sh, uint32_t idx,
                          const ps_swf_xform *xf)
{
    float det, scale, w;

    /* Width 0 means a hairline, and even a nominal width can scale below a
     * pixel; either way a stroke that vanishes is worse than one that is
     * slightly too fat, because it is usually the outline that makes a
     * flat-shaded shape readable. */
    if(idx == 0 || idx > sh->nline)
        return 0.5f;

    /* One width for a transform that may not have one. Flash strokes in the
     * shape's own space and transforms the result, so a non-uniform scale
     * gives an elliptical pen - a stroke thicker across than along. The square
     * root of the determinant is the scale factor that preserves area, which
     * is the single number closest to that ellipse, and it is exactly right
     * for the rotations and uniform scales that placements almost always are.
     * Drawing the true ellipse means offsetting in shape space before the
     * transform, which is a different and much larger stroker; worth doing
     * only when a file turns up that needs it. */
    det = xf->m[0] * xf->m[3] - xf->m[1] * xf->m[2];
    if(det < 0.0f)
        det = -det;
    scale = sqrtf(det);
    w = (float)sh->lines[idx - 1].width * scale;
    return w < 1.0f ? 0.5f : w * 0.5f;
}

int ps_geom_build(const ps_swf_shape *sh, const ps_swf_view *v,
                  const ps_swf_xform *xf, ps_geom *g)
{
    long     i;
    uint32_t l, s;
    float    widest = 0.5f;

    memset(g, 0, sizeof *g);
    g->nlayer = sh->nlayer ? sh->nlayer : 1;

    g->nch = flatten(sh, v, xf, &g->ch);
    if(g->nch < 0)
        return -1;
    if(g->nch == 0)
        return 0;

    /* One allocation for both indices: they are built by the same pass over
     * the chords and freed together, and what a character costs the allocator
     * per frame is a fixed cost the hardware timings say matters. */
    g->lrange = ps_swf_alloc((size_t)g->nlayer * 6 * sizeof *g->lrange);
    if(!g->lrange) {
        ps_swf_dealloc(g->ch);
        g->ch = NULL;
        return -1;
    }
    g->lstyle = g->lrange + g->nlayer * 2;
    for(l = 0; l < g->nlayer; l++) {
        g->lrange[l * 2]     = (uint32_t)g->nch;   /* lo, empty until seen */
        g->lrange[l * 2 + 1] = 0;                  /* hi, exclusive */
        g->lstyle[l * 4]     = 0;                  /* fill lo, 0 = none */
        g->lstyle[l * 4 + 1] = 0;                  /* fill hi, inclusive */
        g->lstyle[l * 4 + 2] = 0;                  /* line lo */
        g->lstyle[l * 4 + 3] = 0;                  /* line hi */
    }
    for(i = 0; i < g->nch; i++) {
        const ps_chord *c  = &g->ch[i];
        uint32_t        ly = c->layer;
        uint32_t       *st;

        if(ly >= g->nlayer)
            continue;
        if((uint32_t)i < g->lrange[ly * 2])
            g->lrange[ly * 2] = (uint32_t)i;
        if((uint32_t)i + 1 > g->lrange[ly * 2 + 1])
            g->lrange[ly * 2 + 1] = (uint32_t)i + 1;

        st = &g->lstyle[ly * 4];
        if(c->f0) {
            if(!st[0] || c->f0 < st[0]) st[0] = c->f0;
            if(c->f0 > st[1])           st[1] = c->f0;
        }
        if(c->f1) {
            if(!st[0] || c->f1 < st[0]) st[0] = c->f1;
            if(c->f1 > st[1])           st[1] = c->f1;
        }
        if(c->ln) {
            if(!st[2] || c->ln < st[2]) st[2] = c->ln;
            if(c->ln > st[3])           st[3] = c->ln;
        }
    }

    for(s = 1; s <= sh->nline; s++) {
        float hw = ps_geom_stroke_half(sh, s, xf);
        if(hw > widest)
            widest = hw;
    }
    g->sides = disc_sides(widest, v->tol);

    /* A chord contributes at most two directed edges to a fill - one for each
     * side it bounds - and to a stroke it contributes four for the quad plus a
     * disc at each end. The stroke figure is only paid for when the shape has
     * line styles at all: it is an order of magnitude more than the fill case,
     * and Flash 3 artwork is overwhelmingly flat fills with no outlines - all
     * five shapes in the file this was developed against have none. */
    g->max_edges = g->nch * 2;
    if(sh->nline) {
        long stroke = g->nch * (4 + 2 * g->sides);
        if(stroke > g->max_edges)
            g->max_edges = stroke;
    }
    return 0;
}

void ps_geom_free(ps_geom *g)
{
    ps_swf_dealloc(g->ch);
    ps_swf_dealloc(g->lrange);          /* lstyle points into the same block */
    g->ch = NULL;
    g->lrange = NULL;
    g->lstyle = NULL;
}

int ps_scratch_init(ps_scratch *s, const ps_geom *g, const ps_swf_view *v)
{
    memset(s, 0, sizeof *s);
    s->ae  = ps_swf_alloc((size_t)g->max_edges * sizeof *s->ae);
    s->xs  = ps_swf_alloc((size_t)g->max_edges * sizeof *s->xs);
    s->cov = ps_swf_alloc((size_t)v->w * sizeof *s->cov);
    if(!s->ae || !s->xs || !s->cov) {
        ps_scratch_free(s);
        return -1;
    }
    return 0;
}

void ps_scratch_free(ps_scratch *s)
{
    ps_swf_dealloc(s->ae);
    ps_swf_dealloc(s->xs);
    ps_swf_dealloc(s->cov);
    memset(s, 0, sizeof *s);
}

/* --- stage 2: select ----------------------------------------------------- */

void ps_add_edge(ps_aedge *e, long *n, float x0, float y0, float x1, float y1)
{
    /* Exactly horizontal chords are dropped rather than stored. They can never
     * be crossed by a sample line, they can never bound a trapezoid, and
     * keeping them would only add degenerate divides to both inner loops. */
    if(y0 == y1)
        return;

    if(y0 < y1) {
        e[*n].x0 = x0; e[*n].y0 = y0; e[*n].x1 = x1; e[*n].y1 = y1;
        e[*n].dir = 1;
    } else {
        e[*n].x0 = x1; e[*n].y0 = y1; e[*n].x1 = x0; e[*n].y1 = y0;
        e[*n].dir = -1;
    }
    (*n)++;
}

double ps_geom_area(const ps_aedge *e, long n)
{
    double a = 0.0;
    long   i;

    /* Green's theorem as the integral of x dy, not the usual cross-product
     * shoelace. They agree on a complete polygon, but this list is not one:
     * add_edge threw the horizontal edges away, and the shoelace needs them
     * because each of its terms depends on where the polygon sits relative to
     * the origin. In this form a horizontal edge contributes zero because dy
     * is zero, so dropping it changes nothing.
     *
     * add_edge also stored every edge pointing down and kept the original
     * sense in dir, which has to be put back before contours will cancel. */
    for(i = 0; i < n; i++)
        a += 0.5 * ((double)e[i].x0 + e[i].x1) *
             ((double)e[i].y1 - e[i].y0) * (double)e[i].dir;
    return a;
}

long ps_geom_fill_edges(const ps_geom *g, uint32_t layer, uint32_t style,
                        ps_aedge *out)
{
    uint32_t i, lo, hi;
    long     n = 0;

    if(layer >= g->nlayer)
        return 0;
    lo = g->lrange[layer * 2];
    hi = g->lrange[layer * 2 + 1];

    for(i = lo; i < hi; i++) {
        const ps_chord *c = &g->ch[i];

        if(c->layer != layer)
            continue;
        /* Same style both sides: an interior edge. Adding it twice with
         * opposite winding is not a special case, it is the correct answer,
         * and it costs less than testing for it. */
        if(c->f1 == style)
            ps_add_edge(out, &n, c->x0, c->y0, c->x1, c->y1);
        if(c->f0 == style)
            ps_add_edge(out, &n, c->x1, c->y1, c->x0, c->y0);
    }
    return n;
}

static void quad(ps_aedge *e, long *n, float ax, float ay, float bx, float by,
                 float cx, float cy, float dx, float dy)
{
    ps_add_edge(e, n, ax, ay, bx, by);
    ps_add_edge(e, n, bx, by, cx, cy);
    ps_add_edge(e, n, cx, cy, dx, dy);
    ps_add_edge(e, n, dx, dy, ax, ay);
}

/* Wound clockwise on screen - decreasing angle, because y runs down - to match
 * the segment quads below. That agreement is the whole point: nonzero winding
 * unions two overlapping contours only when they run the same way, and punches
 * a hole through the overlap when they do not. The square join this replaces
 * was wound the other way, so every join it drew was a notch rather than a
 * corner; nothing caught it because no shape in the sample has a stroke. */
static void disc(ps_aedge *e, long *n, float cx, float cy, float r, int sides)
{
    int   i;
    float px = cx + r, py = cy;

    for(i = 1; i <= sides; i++) {
        float a  = -6.28318531f * (float)i / (float)sides;
        float qx = cx + r * cosf(a);
        float qy = cy + r * sinf(a);

        ps_add_edge(e, n, px, py, qx, qy);
        px = qx;
        py = qy;
    }
}

/* A stroke is a quad per chord plus a disc at each end of it.
 *
 * A disc at both ends rather than one at the far end gives round joins and
 * round caps from the same code and without needing to know where a contour
 * starts or ends - which is exactly what SWF does not tell us, since the edges
 * arrive as soup. The redundancy is real (consecutive chords each put a disc
 * on the vertex they share) but it costs edges, not correctness: the union is
 * taken by winding, so a disc drawn twice is the same disc. */
long ps_geom_line_edges(const ps_geom *g, uint32_t layer, uint32_t style,
                        float hw, ps_aedge *out)
{
    uint32_t i, lo, hi;
    long     n = 0;

    if(layer >= g->nlayer)
        return 0;
    lo = g->lrange[layer * 2];
    hi = g->lrange[layer * 2 + 1];

    for(i = lo; i < hi; i++) {
        const ps_chord *c = &g->ch[i];
        float dx, dy, len, nx, ny;

        if(c->layer != layer || c->ln != style)
            continue;
        dx  = c->x1 - c->x0;
        dy  = c->y1 - c->y0;
        len = sqrtf(dx * dx + dy * dy);
        if(len < 1e-6f) {
            /* A zero-length chord still carries a cap. Flash draws a dot for a
             * moveto-lineto onto the same point, and a shape made only of dots
             * is a legal way to draw a dotted line. */
            disc(out, &n, c->x0, c->y0, hw, g->sides);
            continue;
        }
        nx = -dy / len * hw;
        ny =  dx / len * hw;

        /* The normal turns with the segment, so every quad comes out wound the
         * same way and nonzero winding unions them. */
        quad(out, &n,
             c->x0 + nx, c->y0 + ny,
             c->x1 + nx, c->y1 + ny,
             c->x1 - nx, c->y1 - ny,
             c->x0 - nx, c->y0 - ny);

        disc(out, &n, c->x0, c->y0, hw, g->sides);
        disc(out, &n, c->x1, c->y1, hw, g->sides);
    }
    return n;
}

/* --- stage 3: paint ------------------------------------------------------ */

void ps_geom_flat_paint(ps_swf_rgba color, const ps_swf_cxform *cx,
                        ps_swf_paint *p)
{
    memset(p, 0, sizeof *p);
    p->color = cx ? ps_swf_cxform_apply(cx, color) : color;
}

void ps_geom_fill_paint(const ps_swf_shape *sh, const ps_swf_xform *xf,
                        const ps_swf_cxform *cx, uint32_t style,
                        ps_swf_paint *p)
{
    const ps_swf_fill     *f;
    const ps_swf_gradient *g;
    ps_swf_xform           gm, grad2px, px2grad;

    memset(p, 0, sizeof *p);
    if(style == 0 || style > sh->nfill) {
        /* Out of range means the file is malformed and style_index already
         * refused to resolve it; black is the visible answer. */
        p->color.a = 255;
        return;
    }
    f = &sh->fills[style - 1];
    p->color = cx ? ps_swf_cxform_apply(cx, f->color) : f->color;
    if(cx) {
        p->cx     = *cx;
        p->has_cx = 1;
    }

    /* A bitmap fill's matrix maps bitmap space into shape space exactly as a
     * gradient's maps the gradient square, so the composition and the single
     * inversion below are the same work for both and are shared. What differs
     * is only what the result is measured in: the gradient square is in twips
     * and stays that way, while a bitmap texel is twenty of them, so the extra
     * scale is folded into the inverse here and costs the sampler nothing. */
    if(f->bfill && f->bfill <= sh->nbfill) {
        const ps_swf_bitmapfill *bf = &sh->bfills[f->bfill - 1];
        ps_swf_xform             bm, b2px, px2b;

        if(!bf->bmp)
            return;
        memcpy(bm.m, bf->mat, sizeof bm.m);
        ps_swf_xform_mul(&b2px, xf, &bm);
        if(ps_swf_xform_invert(&px2b, &b2px) < 0)
            return;

        p->inv[0] = px2b.m[0] / 20.0f;
        p->inv[1] = px2b.m[2] / 20.0f;
        p->inv[2] = px2b.m[4] / 20.0f;
        p->inv[3] = px2b.m[1] / 20.0f;
        p->inv[4] = px2b.m[3] / 20.0f;
        p->inv[5] = px2b.m[5] / 20.0f;

        /* 0x40 and 0x42 repeat, 0x41 and 0x43 clip; the pair that is not
         * smoothed is the pair with bit 1 set. The four are distinct fill
         * style IDs rather than flags on one, so a file that asks for a hard
         * edged tile and a file that asks for a filtered clip are asking two
         * different questions and get two different answers. */
        p->bitmap   = bf->bmp;
        p->tiled    = (f->type & 0x01) == 0;
        p->smoothed = (f->type & 0x02) == 0;
        return;
    }

    if(!f->grad || f->grad > sh->ngrad)
        return;
    g = &sh->grads[f->grad - 1];
    if(g->nstop == 0)
        return;

    /* The gradient square lands on the stage through its own matrix and then
     * through everything the display list stacked on top, so the map from the
     * square to output pixels is the product - and what the shader wants is
     * the way back. Composing and then inverting once is both fewer operations
     * and better conditioned than inverting each and multiplying.
     *
     * A singular product collapses the square to a line or a point, so there
     * is no ramp to sample. Exporters emit them, usually as a scale of zero on
     * a shape that was resized to nothing. Falling back to the mean draws
     * something plausible instead of dividing by zero. */
    memcpy(gm.m, g->mat, sizeof gm.m);
    ps_swf_xform_mul(&grad2px, xf, &gm);
    if(ps_swf_xform_invert(&px2grad, &grad2px) < 0)
        return;

    p->inv[0] = px2grad.m[0];
    p->inv[1] = px2grad.m[2];
    p->inv[2] = px2grad.m[4];
    p->inv[3] = px2grad.m[1];
    p->inv[4] = px2grad.m[3];
    p->inv[5] = px2grad.m[5];

    p->grad   = g;
    p->radial = (f->type == PS_SWF_FILL_RADIAL);
}

/* The gradient square is 32768 twips on a side, centred on the origin: a
 * linear ramp runs across it in x, a radial one out from its centre to the
 * edge, and everything past either end clamps. */
ps_swf_rgba ps_paint_at(const ps_swf_paint *p, float x, float y)
{
    const ps_swf_gradient *g = p->grad;
    float       gx, gy, t;
    int         r, i;
    ps_swf_rgba out;

    if(p->bitmap) {
        ps_swf_rgba c;

        gx = p->inv[0] * x + p->inv[1] * y + p->inv[2];
        gy = p->inv[3] * x + p->inv[4] * y + p->inv[5];
        c  = ps_swf_bitmap_sample(p->bitmap, gx, gy, p->tiled, p->smoothed);
        /* Recoloured after sampling, not before, for the same reason a
         * gradient stop is: the transform is per pixel because the colour is,
         * and a half-transparent texel under a dimming placement is only right
         * in that order. */
        return p->has_cx ? ps_swf_cxform_apply(&p->cx, c) : c;
    }

    if(!g)
        return p->color;

    gx = p->inv[0] * x + p->inv[1] * y + p->inv[2];
    gy = p->inv[3] * x + p->inv[4] * y + p->inv[5];

    if(p->radial)
        t = sqrtf(gx * gx + gy * gy) / 16384.0f;
    else
        t = (gx + 16384.0f) / 32768.0f;

    if(!(t > 0.0f)) t = 0.0f;            /* also catches a NaN from the map */
    if(t > 1.0f)    t = 1.0f;
    r = (int)(t * 255.0f + 0.5f);

    /* Advance while the *next* stop is still at or before the sample, so a
     * repeated ratio - which is how a designer asks for a hard edge rather
     * than a ramp - resolves to the last stop carrying it. Stops are in
     * nondecreasing ratio order by the format's own rule; a file that breaks
     * it gets a ramp that doubles back, which is what it asked for. */
    i = 0;
    while(i + 1 < g->nstop && (int)g->ratio[i + 1] <= r)
        i++;

    if(i + 1 >= g->nstop || r <= (int)g->ratio[i])
        return p->has_cx ? ps_swf_cxform_apply(&p->cx, g->color[i])
                         : g->color[i];

    {
        int lo = g->ratio[i], hi = g->ratio[i + 1];
        int num = r - lo, den = hi - lo;
        const ps_swf_rgba *ca = &g->color[i];
        const ps_swf_rgba *cb = &g->color[i + 1];

        out.r = (uint8_t)(ca->r + (cb->r - ca->r) * num / den);
        out.g = (uint8_t)(ca->g + (cb->g - ca->g) * num / den);
        out.b = (uint8_t)(ca->b + (cb->b - ca->b) * num / den);
        out.a = (uint8_t)(ca->a + (cb->a - ca->a) * num / den);
    }
    return p->has_cx ? ps_swf_cxform_apply(&p->cx, out) : out;
}
