/* Stage 4a: the scanline sweep, and the span renderer built on it.
 *
 * This is the stage that does not survive the move to hardware. The PVR
 * computes coverage itself, from triangles, so a sweep that computes coverage
 * in software is not a step towards it - ps_swf_tess.c is the sibling that is,
 * and it consumes exactly what ps_swf_geom.c produces, with nothing above this
 * file's stage changing.
 *
 * What this stage keeps earning after that is the answer to "is the geometry
 * right". Run the tessellator's triangles back through ps_sweep and the
 * picture has to come out the same as the span renderer's, pixel for pixel,
 * because both are then the same rasteriser over what should be the same
 * region. That comparison is the only way to test a tile accelerator's input
 * without a tile accelerator.
 */
#include "ps_swf_geom.h"
#include "ps_swf_mem.h"

#include <math.h>
#include <string.h>

static void span_accum(float *cov, int w, float xa, float xb, float weight)
{
    int ia, ib, i;

    if(xb <= xa)
        return;
    if(xa < 0.0f)
        xa = 0.0f;
    if(xb > (float)w)
        xb = (float)w;
    if(xb <= xa)
        return;

    ia = (int)xa;
    ib = (int)xb;
    if(ia >= w)
        return;

    if(ia == ib) {
        cov[ia] += (xb - xa) * weight;
        return;
    }
    /* Partial first pixel, whole pixels, partial last. Doing the ends exactly
     * is what makes a near-vertical edge look like an edge rather than a
     * staircase, and it costs nothing next to the sample loop. */
    cov[ia] += ((float)(ia + 1) - xa) * weight;
    for(i = ia + 1; i < ib && i < w; i++)
        cov[i] += weight;
    if(ib < w)
        cov[ib] += (xb - (float)ib) * weight;
}

void ps_sweep(const ps_aedge *e, long n, const ps_swf_view *v,
              const ps_swf_paint *p, ps_swf_span_fn span, void *user,
              float *cov, ps_crossing *xs)
{
    int   y, ylo, yhi;
    long  i;
    float fymin, fymax;
    int   samples = v->samples > 0 ? v->samples : 1;
    float weight  = 1.0f / (float)samples;

    if(n == 0)
        return;

    fymin = e[0].y0;
    fymax = e[0].y1;
    for(i = 1; i < n; i++) {
        if(e[i].y0 < fymin) fymin = e[i].y0;
        if(e[i].y1 > fymax) fymax = e[i].y1;
    }
    ylo = (int)floorf(fymin);
    yhi = (int)ceilf(fymax);
    if(ylo < 0)   ylo = 0;
    if(yhi > v->h) yhi = v->h;

    for(y = ylo; y < yhi; y++) {
        int s, x, run_start = -1;
        int run_val = 0;

        memset(cov, 0, (size_t)v->w * sizeof *cov);

        for(s = 0; s < samples; s++) {
            float sy = (float)y + ((float)s + 0.5f) * weight;
            long  nx = 0;
            int   wind = 0;
            float open = 0.0f;

            for(i = 0; i < n; i++) {
                /* Half-open in y: a vertex shared by two chords is counted
                 * once, which is what stops a seam of unfilled pixels
                 * appearing along every horizontal join. */
                if(sy < e[i].y0 || sy >= e[i].y1)
                    continue;
                xs[nx].x = e[i].x0 + (sy - e[i].y0) *
                           (e[i].x1 - e[i].x0) / (e[i].y1 - e[i].y0);
                xs[nx].dir = e[i].dir;
                nx++;
            }
            /* Insertion sort: crossing counts are small - a handful for the
             * shapes this has to draw - and the list is close to sorted from
             * one sample line to the next. */
            for(i = 1; i < nx; i++) {
                ps_crossing t = xs[i];
                long        j = i - 1;
                while(j >= 0 && xs[j].x > t.x) {
                    xs[j + 1] = xs[j];
                    j--;
                }
                xs[j + 1] = t;
            }

            for(i = 0; i < nx; i++) {
                int was = wind;
                wind += xs[i].dir;
                if(was == 0 && wind != 0)
                    open = xs[i].x;
                else if(was != 0 && wind == 0)
                    span_accum(cov, v->w, open, xs[i].x, weight);
            }
        }

        if(p->grad || p->bitmap) {
            /* A gradient's or a bitmap's colour changes every pixel, so runs
             * cannot be coalesced and the sink is called once per covered
             * pixel. That cost is only paid by those two, and it is what a
             * span interface costs: on the PVR either one is a textured
             * triangle pair and the hardware interpolates. */
            for(x = 0; x < v->w; x++) {
                float c = cov[x];
                int   val;

                if(!(c > 0.0f))
                    continue;
                val = (int)(c * 255.0f + 0.5f);
                if(val > 255) val = 255;
                if(val > 0)
                    span(user, y, x, x + 1, (uint8_t)val,
                         ps_paint_at(p, (float)x + 0.5f, (float)y + 0.5f));
            }
            continue;
        }

        /* Runs of equal quantised coverage, so a solid interior costs one
         * callback per row rather than one per pixel. */
        for(x = 0; x <= v->w; x++) {
            int val = 0;

            if(x < v->w) {
                float c = cov[x];
                if(c > 0.0f) {
                    val = (int)(c * 255.0f + 0.5f);
                    if(val > 255) val = 255;
                }
            }
            if(val != run_val) {
                if(run_val > 0 && run_start >= 0)
                    span(user, y, run_start, x, (uint8_t)run_val, p->color);
                run_val   = val;
                run_start = x;
            }
        }
    }
}

long ps_swf_raster_shape(const ps_swf_shape *sh, const ps_swf_view *v,
                         const ps_swf_xform *xf, const ps_swf_cxform *cx,
                         ps_swf_span_fn span, void *user)
{
    ps_geom    g;
    ps_scratch sc;
    uint32_t   layer, s;
    long       nch;

    if(ps_geom_build(sh, v, xf, &g) < 0)
        return -1;
    nch = g.nch;
    if(nch == 0) {
        ps_geom_free(&g);
        return 0;
    }
    if(ps_scratch_init(&sc, &g, v) < 0) {
        ps_geom_free(&g);
        return -1;
    }

    /* Layer by layer, fills before strokes within a layer. A NewStyles record
     * starts a new layer that paints over everything before it, and inside a
     * layer an outline is meant to sit on top of the fill it outlines.
     *
     * Within one layer the fills are drawn in style order. Flash does not
     * define an order there because a well-formed layer's fills do not
     * overlap; when they do - and exporters do emit that - style order is a
     * guess, and one of the places this may need revisiting against Ruffle's
     * output. */
    for(layer = 0; layer < g.nlayer; layer++) {
        for(s = 1; s <= sh->nfill; s++) {
            long         n = ps_geom_fill_edges(&g, layer, s, sc.ae);
            ps_swf_paint p;

            if(!n)
                continue;
            ps_geom_fill_paint(sh, xf, cx, s, &p);
            ps_sweep(sc.ae, n, v, &p, span, user, sc.cov, sc.xs);
        }

        for(s = 1; s <= sh->nline; s++) {
            float        hw = ps_geom_stroke_half(sh, s, xf);
            long         n  = ps_geom_line_edges(&g, layer, s, hw, sc.ae);
            ps_swf_paint p;

            if(!n)
                continue;
            ps_geom_flat_paint(sh->lines[s - 1].color, cx, &p);
            ps_sweep(sc.ae, n, v, &p, span, user, sc.cov, sc.xs);
        }
    }

    ps_scratch_free(&sc);
    ps_geom_free(&g);
    return nch;
}
