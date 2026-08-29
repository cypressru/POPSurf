/* Stage 4b: filled regions to triangles.
 *
 * The choice of algorithm, and why, because it is the decision in this file.
 *
 * What arrives is what ps_swf_geom.c hands the sweep: a bag of directed
 * segments bounding one style, in no particular order, with the interior
 * defined by the nonzero winding rule. It is not a list of contours. Ear
 * clipping and every other polygon triangulator wants closed simple polygons
 * with holes identified and bridged, so using one means first stitching the
 * soup into contours, deciding which contours are holes, and cutting bridges -
 * three passes of fiddly topology that can fail on self-intersecting artwork,
 * which Flash exporters produce routinely. All of it to recover structure the
 * fill rule never needed.
 *
 * So instead: trapezoidal decomposition by a horizontal sweep. Sort every
 * vertex y, and between two consecutive ones no edge begins or ends, so the
 * set of edges crossing that band is fixed and their left-to-right order is
 * fixed too. Apply the same winding walk the scanline renderer applies, and
 * each interior interval is a trapezoid bounded by two known edges - two
 * triangles. Concavity, holes and disjoint pieces need no special handling
 * because none of them change what a band looks like.
 *
 * Three things recommend it here beyond simplicity. It needs no contour
 * ordering, which is the property the input lacks. Its cost is a sort plus a
 * linear walk per band, with no recursion and no dynamic topology, which is
 * what a 200MHz SH-4 with 16MB wants. And it uses the same winding rule as the
 * span renderer, so when the two agree the agreement is about the geometry
 * rather than about two different interpretations of what "inside" means.
 *
 * The one thing it does need that the scanline renderer does not is edges
 * split where they cross. A scanline recomputes its ordering every sample row,
 * so a crossing costs it nothing; a trapezoid spanning a crossing would be
 * bounded by the wrong edge over half its height. Crossings are therefore
 * found per band, among only the edges active in it, and used as extra band
 * boundaries. That is quadratic in the active count, which is small - it is
 * the number of edges over one horizontal line, not the number in the shape.
 *
 * This wants review. It is the right shape for the hardware and it is
 * verifiable against the sweep, but trapezoids are more triangles than a good
 * triangulator would produce for the same region - roughly two per band per
 * interior interval, where an ear clipper approaches n. Whether that matters
 * is a question about PVR vertex throughput, which is measured on hardware,
 * not decided here.
 *
 * Where the time goes, measured rather than reasoned about.
 *
 * The Crazy Taxi 2 logo's frame 61 is one character of 1418 directed edges
 * over 23 style layers, and it decomposes into 1282 bands and 4357
 * trapezoids. Counting the primitive operations of that frame said the cost
 * was almost entirely one of them: 113,660 evaluations of edge_x, which is
 * the only division in the file and the only expensive thing the SH-4 does
 * here. 87,518 of those - 77% - were the crossing search re-deriving, for
 * every pair of active edges, an x that is a property of one edge and the
 * band it is in. Another 17,428 were emit_trap asking for corners the
 * crossing search had already computed and thrown away.
 *
 * So the two x values an active edge takes at a band's own limits are now
 * computed once per edge per band and kept, and the pair test and the
 * trapezoid corners both read them. That is the same arithmetic on the same
 * inputs in the same order - bitwise the same numbers, so not one output
 * pixel moves - and it takes the count to 26,142 for that frame. The
 * quadratic pair loop survives because with the x values in hand it is four
 * loads and two subtractions, which is not what was costing anything.
 *
 * The standing guess before that count was taken was the pass loop at the
 * bottom of this file: 23 layers times 23 fill styles rescanning the layer's
 * chords, 34,454 iterations for the same frame. It is real and it is fixed
 * here too - see ps_geom.lstyle - but it was 4% of the frame against edge_x's
 * majority, and on the pages the console timings came from it is between 4
 * and 36 iterations, which cannot be any part of 319us. Reading a loop tells
 * you its shape and not its share.
 */
#include "ps_swf_geom.h"
#include "ps_swf_mem.h"

#include <stdlib.h>
#include <string.h>

/* Crossings found inside one band. Beyond this the band is tessellated as if
 * the extras were not there, which shows up as a sliver of wrong coverage
 * rather than as a failure - the alternative, an allocation whose size is a
 * function of how tangled the file chose to be, is worse. Sixty-four crossings
 * over a single horizontal line is far past any real artwork. */
#define MAX_SPLITS 64

typedef struct {
    float x;
    int   dir;
    long  slot;         /* position in the active list, so xa/xb can be read */
} tcross;

typedef struct {
    const ps_aedge *e;
    long            n;

    long   *order;      /* edge indices sorted by y0 */
    long   *act;        /* currently active edge indices */
    float  *ys;         /* band boundaries */

    /* Every active edge's x at the top and at the bottom of the band being
     * worked on, indexed by active slot. See the file header: this is where
     * the frame's time was going. `xu` and `xv` are the same thing for a
     * sub-band, alternating so the boundary between two consecutive sub-bands
     * is evaluated once and read as the bottom of one and the top of the
     * next. */
    float  *xa, *xb, *xu, *xv;

    tcross *xs;
    float   splits[MAX_SPLITS];
} tess;

/* All of it out of one allocation.
 *
 * A character is tessellated from scratch every frame, so what tess_shape
 * asks the allocator for per draw is a fixed cost paid whatever the geometry
 * turns out to be - and the hardware timings say the fixed costs are what a
 * small shape is made of. Nine requests and nine frees became three, of which
 * two are ps_geom_build's. Sixteen-byte granularity because the block holds
 * longs as well as floats and the smallest true requirement differs between
 * the host and SH-4; the waste is at most 15 bytes per buffer. */
static size_t carve_size(size_t n)
{
    return (n + 15u) & ~(size_t)15u;
}

static void *carve(unsigned char **p, size_t n)
{
    void *r = *p;

    *p += carve_size(n);
    return r;
}

static int cmp_float(const void *a, const void *b)
{
    float fa = *(const float *)a, fb = *(const float *)b;

    return fa < fb ? -1 : fa > fb ? 1 : 0;
}

/* qsort's comparator takes no user pointer before C11's qsort_s, and writing a
 * sort by hand to avoid one file-static is the worse trade. Safe here because
 * a shape is tessellated start to finish on one thread; if a later phase
 * tessellates in parallel this is the thing that has to change first. */
static const ps_aedge *g_edges;

static int cmp_by_y0(const void *a, const void *b)
{
    float ya = g_edges[*(const long *)a].y0;
    float yb = g_edges[*(const long *)b].y0;

    return ya < yb ? -1 : ya > yb ? 1 : 0;
}

static float edge_x(const ps_aedge *e, float y)
{
    return e->x0 + (y - e->y0) * (e->x1 - e->x0) / (e->y1 - e->y0);
}

/* One trapezoid, as two triangles sharing the diagonal. Emitted even when it
 * degenerates to a triangle - when the band pinches to a point at a vertex,
 * which is the common case at the top and bottom of every contour - because a
 * zero-area triangle covers nothing and testing for it costs more than the
 * hardware spends discarding it. */
static void emit_trap(const ps_swf_tri_sink *sink, void *user, long *count,
                      float xla, float xra, float ya,
                      float xlb, float xrb, float yb)
{
    ps_swf_vtx v[3];

    v[0].x = xla; v[0].y = ya;
    v[1].x = xra; v[1].y = ya;
    v[2].x = xrb; v[2].y = yb;
    sink->tri(user, v);

    v[1].x = xrb; v[1].y = yb;
    v[2].x = xlb; v[2].y = yb;
    sink->tri(user, v);
    (*count) += 2;
}

/* Every active edge's x on one horizontal line, by active slot. */
static void limit_x(const tess *t, long nact, float y, float *out)
{
    long i;

    for(i = 0; i < nact; i++)
        out[i] = edge_x(&t->e[t->act[i]], y);
}

/* Tessellates one sub-band, which by construction has no edge starting,
 * ending or crossing strictly inside it. `xa` and `xb` are the active edges'
 * x at ya and at yb, already evaluated by the caller. */
static void band(tess *t, long nact, float ya, float yb,
                 const float *xa, const float *xb,
                 const ps_swf_tri_sink *sink, void *user, long *count)
{
    float ym = 0.5f * (ya + yb);
    long  i, nx = 0;
    int   wind = 0;
    long  open = -1;

    for(i = 0; i < nact; i++) {
        const ps_aedge *e = &t->e[t->act[i]];

        /* The midpoint is sampled rather than derived from xa and xb because
         * it decides the left-to-right order, and an order that depended on
         * how the two ends round would not be the order the span renderer
         * arrives at by evaluating the edge itself. */
        t->xs[nx].x    = edge_x(e, ym);
        t->xs[nx].dir  = e->dir;
        t->xs[nx].slot = i;
        nx++;
    }
    for(i = 1; i < nx; i++) {
        tcross tmp = t->xs[i];
        long   j   = i - 1;

        while(j >= 0 && t->xs[j].x > tmp.x) {
            t->xs[j + 1] = t->xs[j];
            j--;
        }
        t->xs[j + 1] = tmp;
    }

    for(i = 0; i < nx; i++) {
        int was = wind;

        wind += t->xs[i].dir;
        if(was == 0 && wind != 0) {
            open = t->xs[i].slot;
        } else if(was != 0 && wind == 0 && open >= 0) {
            long r = t->xs[i].slot;

            /* The trapezoid corners come from the bounding edges evaluated at
             * the band's own limits, not from the midpoint sample. The
             * midpoint only decides which edges bound the interval; using its
             * x values would give a rectangle where the region is a wedge. */
            emit_trap(sink, user, count,
                      xa[open], xa[r], ya,
                      xb[open], xb[r], yb);
            open = -1;
        }
    }
}

static void tess_pass(tess *t, const ps_swf_tri_sink *sink, void *user,
                      long *count)
{
    long i, nys = 0, nact = 0, next = 0;

    for(i = 0; i < t->n; i++) {
        t->ys[nys++] = t->e[i].y0;
        t->ys[nys++] = t->e[i].y1;
        t->order[i]  = i;
    }
    qsort(t->ys, (size_t)nys, sizeof *t->ys, cmp_float);
    g_edges = t->e;
    qsort(t->order, (size_t)t->n, sizeof *t->order, cmp_by_y0);

    for(i = 0; i + 1 < nys; i++) {
        float ya = t->ys[i], yb = t->ys[i + 1];
        long  j, k, nsplit = 0;

        if(!(yb > ya))
            continue;

        /* The active set changes only at band boundaries, so it is carried
         * forward: admit every edge that has started, drop every edge that has
         * finished. Without this each band rescans the whole edge list and the
         * pass is quadratic in a shape's own complexity rather than in how
         * much of it overlaps one line. */
        while(next < t->n && t->e[t->order[next]].y0 <= ya)
            t->act[nact++] = t->order[next++];
        for(j = 0, k = 0; j < nact; j++)
            if(t->e[t->act[j]].y1 > ya)
                t->act[k++] = t->act[j];
        nact = k;
        if(nact == 0)
            continue;

        limit_x(t, nact, ya, t->xa);
        limit_x(t, nact, yb, t->xb);

        /* Where two active edges cross inside this band, split there. Solving
         * for the y of the crossing directly rather than testing for one
         * first: the parametric form gives both answers for the same work. */
        for(j = 0; j < nact && nsplit < MAX_SPLITS; j++) {
            for(k = j + 1; k < nact && nsplit < MAX_SPLITS; k++) {
                float d0 = t->xa[j] - t->xa[k], d1 = t->xb[j] - t->xb[k];
                float f;

                /* Same sign at both ends, or touching at one: no crossing
                 * strictly inside, so no split. */
                if((d0 > 0.0f && d1 > 0.0f) || (d0 < 0.0f && d1 < 0.0f))
                    continue;
                if(d0 == d1)
                    continue;
                f = d0 / (d0 - d1);
                if(f <= 0.0f || f >= 1.0f)
                    continue;
                t->splits[nsplit++] = ya + f * (yb - ya);
            }
        }

        if(nsplit == 0) {
            band(t, nact, ya, yb, t->xa, t->xb, sink, user, count);
        } else {
            float       prev = ya;
            const float *pa  = t->xa;
            float       *cur = t->xu;

            qsort(t->splits, (size_t)nsplit, sizeof *t->splits, cmp_float);
            for(j = 0; j < nsplit; j++) {
                if(t->splits[j] > prev) {
                    /* A split line is the bottom of one sub-band and the top
                     * of the next, so it is evaluated once and handed on. */
                    limit_x(t, nact, t->splits[j], cur);
                    band(t, nact, prev, t->splits[j], pa, cur,
                         sink, user, count);
                    prev = t->splits[j];
                    pa   = cur;
                    cur  = (cur == t->xu) ? t->xv : t->xu;
                }
            }
            if(yb > prev)
                band(t, nact, prev, yb, pa, t->xb, sink, user, count);
        }
    }
}

long ps_swf_tess_shape(const ps_swf_shape *sh, const ps_swf_view *v,
                       const ps_swf_xform *xf, const ps_swf_cxform *cx,
                       const ps_swf_tri_sink *sink, void *user)
{
    ps_geom        g;
    tess           t;
    ps_aedge      *ae;
    unsigned char *block, *cut;
    size_t         ne, bytes;
    uint32_t       layer, s, lo, hi;
    long           count = 0;

    if(ps_geom_build(sh, v, xf, &g) < 0)
        return -1;
    if(g.nch == 0) {
        ps_geom_free(&g);
        return 0;
    }

    ne    = (size_t)g.max_edges;
    bytes = carve_size(ne * sizeof *ae) +
            carve_size(ne * sizeof *t.order) +
            carve_size(ne * sizeof *t.act) +
            carve_size(ne * 2 * sizeof *t.ys) +
            4 * carve_size(ne * sizeof(float)) +
            carve_size(ne * sizeof *t.xs);
    block = ps_swf_alloc(bytes);
    if(!block) {
        ps_geom_free(&g);
        return -1;
    }

    memset(&t, 0, sizeof t);
    cut     = block;
    ae      = carve(&cut, ne * sizeof *ae);
    t.e     = ae;
    t.order = carve(&cut, ne * sizeof *t.order);
    t.act   = carve(&cut, ne * sizeof *t.act);
    t.ys    = carve(&cut, ne * 2 * sizeof *t.ys);
    t.xa    = carve(&cut, ne * sizeof(float));
    t.xb    = carve(&cut, ne * sizeof(float));
    t.xu    = carve(&cut, ne * sizeof(float));
    t.xv    = carve(&cut, ne * sizeof(float));
    t.xs    = carve(&cut, ne * sizeof *t.xs);

    /* Same pass order as the span renderer, because the sinks composite in
     * the order they are called and a difference here would show up as a
     * shape drawn correctly in the wrong z order.
     *
     * The style bounds come from the chords rather than from the style table,
     * for the reason ps_geom.lrange exists: a shape's style table is the union
     * of every layer's, so walking all of it in every layer asks each layer
     * about styles that are somebody else's and rescans the layer's chords to
     * be told so. The Crazy Taxi 2 logo is 23 layers of one style each, which
     * made that 23 scans of every chord for one that mattered. */
    for(layer = 0; layer < g.nlayer; layer++) {
        lo = g.lstyle[layer * 4];
        hi = g.lstyle[layer * 4 + 1];
        /* The parser already refuses a style index past the table it belongs
         * to, so this cannot fire on any file we have seen - but the bound it
         * replaces was the table size itself, and a loop that indexes an array
         * should say why it is inside it rather than inherit the reason. */
        if(hi > sh->nfill)
            hi = sh->nfill;
        for(s = lo; s && s <= hi; s++) {
            ps_swf_paint p;

            t.n = ps_geom_fill_edges(&g, layer, s, ae);
            if(!t.n)
                continue;
            ps_geom_fill_paint(sh, xf, cx, s, &p);
            sink->begin(user, &p);
            tess_pass(&t, sink, user, &count);
            sink->end(user);
        }

        lo = g.lstyle[layer * 4 + 2];
        hi = g.lstyle[layer * 4 + 3];
        if(hi > sh->nline)
            hi = sh->nline;
        for(s = lo; s && s <= hi; s++) {
            float        hw = ps_geom_stroke_half(sh, s, xf);
            ps_swf_paint p;

            t.n = ps_geom_line_edges(&g, layer, s, hw, ae);
            if(!t.n)
                continue;
            ps_geom_flat_paint(sh->lines[s - 1].color, cx, &p);
            sink->begin(user, &p);
            tess_pass(&t, sink, user, &count);
            sink->end(user);
        }
    }

    ps_swf_dealloc(block);
    ps_geom_free(&g);
    return count;
}
