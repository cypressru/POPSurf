/* The host backend for the triangle sink: rasterise, instead of submitting.
 *
 * On the Dreamcast this is where a pass becomes a PVR polygon context and
 * three vertices per triangle go into the TA's FIFO. There is no Dreamcast
 * code here and there will not be in this file - what it does instead is the
 * thing the hardware makes impossible, which is to check the geometry.
 *
 * It does that by rasterising a whole pass's triangles through ps_sweep - the
 * same sweep, with the same winding rule and the same coverage arithmetic, the
 * span renderer uses on the original edges. Feeding all of a pass's triangles
 * in at once matters: taken one at a time each would antialias its own shared
 * edges and leave seams, whereas the union of a consistently wound set is
 * exactly the region, so the output is bit-identical to the span renderer's
 * whenever the tessellation covers the same ground. That turns "do the
 * triangles look right" into a comparison with no tolerance in it.
 *
 * It is not a tautology. Nothing about the trapezoid corner coordinates comes
 * from the span renderer; they are computed from band limits the span renderer
 * never sees. What is shared is only the definition of coverage, which is what
 * has to be shared for the comparison to be about geometry.
 */
#include "ps_swf_trisoft.h"
#include "ps_swf_geom.h"
#include "ps_swf_mem.h"

#include <string.h>

static void ts_begin(void *user, const ps_swf_paint *paint)
{
    ps_trisoft *t = user;

    t->paint = *paint;
    t->n     = 0;
}

static int ts_reserve(ps_trisoft *t, long want)
{
    ps_aedge *nb;
    long      cap = t->cap ? t->cap : 1024;

    if(want <= t->cap)
        return 0;
    while(cap < want)
        cap *= 2;
    nb = ps_swf_realloc(t->ae, (size_t)cap * sizeof *nb);
    if(!nb) {
        t->failed = 1;
        return -1;
    }
    t->ae  = nb;
    t->cap = cap;
    return 0;
}

static void ts_tri(void *user, const ps_swf_vtx *v)
{
    ps_trisoft *t = user;
    double      a;

    t->tris++;

    /* Twice the signed area, kept as a running total in both forms. The two
     * agreeing says the tessellator wound every triangle the same way; their
     * magnitude matching the region's own area says the triangles neither
     * overlap nor leave gaps. Neither is visible in a picture comparison,
     * because overlapping opaque triangles look exactly like tiling ones. */
    a = ((double)v[1].x - v[0].x) * ((double)v[2].y - v[0].y) -
        ((double)v[2].x - v[0].x) * ((double)v[1].y - v[0].y);
    t->area_signed += a * 0.5;
    t->area_abs    += (a < 0 ? -a : a) * 0.5;

    if(t->failed || ts_reserve(t, t->n + 6) < 0)
        return;
    ps_add_edge(t->ae, &t->n, v[0].x, v[0].y, v[1].x, v[1].y);
    ps_add_edge(t->ae, &t->n, v[1].x, v[1].y, v[2].x, v[2].y);
    ps_add_edge(t->ae, &t->n, v[2].x, v[2].y, v[0].x, v[0].y);
}

static void ts_end(void *user)
{
    ps_trisoft *t = user;

    if(t->failed || t->n == 0)
        return;
    if(t->n > t->xcap) {
        ps_crossing *nx = ps_swf_realloc(t->xs, (size_t)t->n * sizeof *nx);
        if(!nx) {
            t->failed = 1;
            return;
        }
        t->xs   = nx;
        t->xcap = t->n;
    }
    ps_sweep(t->ae, t->n, t->view, &t->paint, t->span, t->user, t->cov, t->xs);
    t->n = 0;
}

const ps_swf_tri_sink *ps_trisoft_sink(void)
{
    static const ps_swf_tri_sink sink = { ts_begin, ts_tri, ts_end };

    return &sink;
}

int ps_trisoft_init(ps_trisoft *t, const ps_swf_view *v,
                    ps_swf_span_fn span, void *user)
{
    memset(t, 0, sizeof *t);
    t->view = v;
    t->span = span;
    t->user = user;
    t->cov  = ps_swf_alloc((size_t)v->w * sizeof *t->cov);
    return t->cov ? 0 : -1;
}

void ps_trisoft_free(ps_trisoft *t)
{
    ps_swf_dealloc(t->ae);
    ps_swf_dealloc(t->xs);
    ps_swf_dealloc(t->cov);
    memset(t, 0, sizeof *t);
}
