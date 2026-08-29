/* Checks the triangle path against the span path, on the host.
 *
 * The claim being tested is that tessellating a shape and rasterising the
 * triangles covers exactly the region the scanline renderer covers. Both sides
 * are rendered into a canvas with the same view and the same compositing, and
 * the two are compared pixel for pixel with no tolerance, which is possible
 * because the host triangle backend rasterises through the same sweep - see
 * ps_swf_trisoft.c for why that is a fair test and not a circular one.
 *
 * Two classes of pixel are counted separately, because only one of them is
 * allowed to differ at all.
 *
 * An interior pixel - one whose eight neighbours in the span render are all
 * the same colour it is - is fully covered by a single pass. Nothing about
 * float ordering can change a fully covered pixel, so a difference there is a
 * hole, an overlap or a misplaced triangle, and it fails. This is the check
 * that the tessellation covers the region.
 *
 * A boundary pixel holds a coverage fraction, and there the two paths are not
 * bit-identical and cannot be made so. The tessellator evaluates a bounding
 * edge's x at the two band limits and the sweep interpolates along the chord
 * between them; the span renderer interpolates along the original edge. Same
 * line, different order of float operations, so a coverage that lands on a
 * rounding tie in the 0..255 quantiser can fall either way. That is a property
 * of representing a region as triangles, not a defect to be fixed - a triangle
 * carries three vertices and not the line that produced them, so the hardware
 * path could not avoid it either. It is bounded at one part in 255 and
 * anything larger fails.
 *
 * Two numbers are printed beside the pixel comparison, because a picture
 * cannot show either. The first is whether every triangle was wound the same
 * way, which opaque output hides completely. The second is the triangles' own
 * total area against the region's area taken straight from the selected edges
 * by Green's theorem - an exact reference that no rasteriser touches. It is
 * what catches a triangle set that paints the right picture while overlapping
 * itself, which costs the PVR fill rate and stops being invisible the moment
 * a fill is not opaque.
 *
 * That reference is the integral of the winding number, not the area of the
 * region. They are equal only where every part of the region is wound exactly
 * once, and two ordinary things break that: a stroke is a deliberately
 * overlapping union of a quad and two discs per segment, so it is wound twice
 * nearly everywhere; and a contour that crosses itself has lobes of opposite
 * sign that cancel, which is why the winding integral of a bowtie is zero
 * while its filled area is not. There is no cheap independent reference for
 * either - computing the area of a nonzero region means decomposing it, which
 * is the thing under test - so the column reads n/a and the pixel comparison
 * carries those shapes on its own.
 *
 * -m runs the whole comparison again with both sides masked, which is what
 * keeps clipping inside this test rather than beside it. A mask multiplies
 * coverage where geometry becomes pixels, and that is the one stage the two
 * paths share, so the claim being checked is that the multiply does not disturb
 * the agreement - not that the two paths mask differently, which they cannot.
 * The mask is a rectangle on integer pixel boundaries, fed in as spans rather
 * than derived from either renderer: a mask taken from one of the two paths
 * would put that path's own boundary arithmetic on both sides of the
 * comparison, and a mask with a soft edge would square the boundary coverage
 * and turn a one-step difference into two. Everything outside the rectangle is
 * blank on both sides and compares equal, so this run is weaker than the plain
 * one and is an addition to it rather than a replacement.
 *
 *   ./tricmp file.swf [-s scale] [-t tol] [-a samples] [-o prefix] [-m]
 */
#include "ps_swf.h"
#include "ps_swf_clip.h"
#include "ps_swf_morph.h"
#include "ps_swf_trisoft.h"
#include "ps_ppmdiff.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *px;
    int      w, h;
} canvas;

static void put_span(void *user, int y, int x0, int x1, uint8_t cov,
                     ps_swf_rgba c)
{
    canvas  *cv = user;
    int      x;
    unsigned a  = (unsigned)cov * (unsigned)c.a / 255u;

    if(y < 0 || y >= cv->h)
        return;
    if(x0 < 0)      x0 = 0;
    if(x1 > cv->w)  x1 = cv->w;

    for(x = x0; x < x1; x++) {
        uint8_t *p = cv->px + ((size_t)y * cv->w + x) * 3;
        p[0] = (uint8_t)((p[0] * (255u - a) + c.r * a) / 255u);
        p[1] = (uint8_t)((p[1] * (255u - a) + c.g * a) / 255u);
        p[2] = (uint8_t)((p[2] * (255u - a) + c.b * a) / 255u);
    }
}

static int canvas_new(canvas *cv, int w, int h)
{
    cv->w = w;
    cv->h = h;
    cv->px = malloc((size_t)w * h * 3);
    if(!cv->px)
        return -1;
    memset(cv->px, 0x20, (size_t)w * h * 3);
    return 0;
}

static void write_ppm(const char *path, const canvas *cv)
{
    FILE *f = fopen(path, "wb");

    if(!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", cv->w, cv->h);
    fwrite(cv->px, 1, (size_t)cv->w * cv->h * 3, f);
    fclose(f);
}

/* The region's area, re-derived from the shared stages rather than from the
 * tessellator. The pass loop is repeated here rather than borrowed, which is
 * the point: an independent walk of the same geometry is what makes the
 * comparison mean something. */
static double region_area(const ps_swf_shape *sh, const ps_swf_view *v,
                          const ps_swf_xform *xf)
{
    ps_geom    g;
    ps_scratch sc;
    uint32_t   layer, s;
    double     total = 0.0;

    if(ps_geom_build(sh, v, xf, &g) < 0)
        return 0.0;
    if(g.nch == 0) {
        ps_geom_free(&g);
        return 0.0;
    }
    if(ps_scratch_init(&sc, &g, v) < 0) {
        ps_geom_free(&g);
        return 0.0;
    }
    for(layer = 0; layer < g.nlayer; layer++)
        for(s = 1; s <= sh->nfill; s++)
            total += fabs(ps_geom_area(sc.ae,
                          ps_geom_fill_edges(&g, layer, s, sc.ae)));
    ps_scratch_free(&sc);
    ps_geom_free(&g);
    return total;
}

/* The middle half of the canvas, as a mask, delivered the way a real mask is:
 * through ps_swf_clip_span between a begin and an apply. Integer bounds and
 * full coverage, so the mask's own edge contributes nothing either side could
 * round differently. */
static void mask_middle(ps_swf_clip *c, int w, int h)
{
    ps_swf_rgba white = { 255, 255, 255, 255 };
    int         y;

    ps_swf_clip_begin(c);
    for(y = h / 4; y < h - h / 4; y++)
        ps_swf_clip_span(c, y, w / 4, w - w / 4, 255, white);
    ps_swf_clip_apply(c);
}

/* One shape through both paths, and the comparison between them.
 *
 * Split out because there is a second source of shapes that is not in the
 * movie's shape table: a morph, which is a blend computed at a ratio rather
 * than a record read out of a file. That is the case most worth putting
 * through here - a blended coordinate is arithmetic rather than transcribed,
 * so an edge landing exactly on a scanline is far likelier than it is in
 * artwork - and it has to survive the masked pass as well as the plain one,
 * since a mask multiplies coverage at the one stage the two paths share.
 *
 * Returns nonzero if the two paths disagree in a way that is not allowed. */
static int compare_shape(const ps_swf_shape *sh, const char *label,
                         float scale, float tol, int samples,
                         const char *prefix, int masked)
{
    ps_swf_view  v;
    ps_swf_xform xf;
    canvas       ca, cb;
    ps_swf_clip  cla, clb;
    ps_trisoft   ts;
    long         ntri, edge_diff = 0, inner_diff = 0, worse = 0;
    double       ref;
    double       wpx = (sh->xmax - sh->xmin) / 20.0;
    double       hpx = (sh->ymax - sh->ymin) / 20.0;
    size_t       peak;
    int          bad = 0;

    {
        float s = scale / 20.0f;
        ps_swf_xform_scale(&xf, s, s,
                           -((float)sh->xmin * s) + 1.0f,
                           -((float)sh->ymin * s) + 1.0f);
    }
    v.w       = (int)(wpx * scale) + 2;
    v.h       = (int)(hpx * scale) + 2;
    v.tol     = tol;
    v.samples = samples;
    if(v.w < 1 || v.h < 1 || v.w > 8192 || v.h > 8192)
        return 0;

    if(canvas_new(&ca, v.w, v.h) < 0 || canvas_new(&cb, v.w, v.h) < 0) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    if(ps_swf_clip_init(&cla, &v, put_span, &ca) < 0 ||
       ps_swf_clip_init(&clb, &v, put_span, &cb) < 0) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    if(masked) {
        mask_middle(&cla, v.w, v.h);
        mask_middle(&clb, v.w, v.h);
    }
    ps_swf_raster_shape(sh, &v, &xf, NULL, ps_swf_clip_span, &cla);

    ps_swf_mem_reset_peak();
    if(ps_trisoft_init(&ts, &v, ps_swf_clip_span, &clb) < 0) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    ntri = ps_swf_tess_shape(sh, &v, &xf, NULL, ps_trisoft_sink(), &ts);
    peak = ps_swf_mem_peak() - ps_swf_mem_live();
    if(ps_swf_clip_failed(&cla) || ps_swf_clip_failed(&clb)) {
        fprintf(stderr, "out of memory for the mask\n");
        return 1;
    }

    /* The span render is the reference and says which pixels are edges; the
     * rule itself lives in ps_ppmdiff.h, shared with the comparison that puts
     * a frame captured off the console against this same renderer. Exact here,
     * because both sides are 8-bit and computed on the same machine. */
    {
        ps_ppmdiff_result d;

        ps_ppmdiff(ca.px, cb.px, v.w, v.h, 0, NULL, &d);
        inner_diff = d.interior;
        edge_diff  = d.boundary;
        worse      = d.worse;
    }

    ref = sh->nline ? 0.0 : fabs(region_area(sh, &v, &xf));
    /* Below a pixel the ratio is noise, and it is exactly the case a
     * self-crossing contour produces, so it is reported as inapplicable
     * rather than as a suspiciously round number. */
    if(ref < 1.0)
        ref = 0.0;
    printf("%-22s %5s %8ld %8ld %8ld %6ld %6s ", "",
           label, ntri, inner_diff, edge_diff, worse,
           fabs(ts.area_abs - ts.area_signed) < 0.5 ? "same" : "MIXED");
    if(ref > 0.0)
        printf("%8.3f%%", 100.0 * ts.area_abs / ref);
    else
        printf("%8s ", "n/a");
    printf("  %ld scratch bytes\n", (long)peak);

    if(inner_diff || worse || ts.failed || ntri < 0)
        bad = 1;
    if(prefix) {
        char name[256];
        snprintf(name, sizeof name, "%s-%s-span.ppm", prefix, label);
        write_ppm(name, &ca);
        snprintf(name, sizeof name, "%s-%s-tri.ppm", prefix, label);
        write_ppm(name, &cb);
    }
    ps_trisoft_free(&ts);
    ps_swf_clip_free(&cla);
    ps_swf_clip_free(&clb);
    free(ca.px);
    free(cb.px);
    return bad;
}

int main(int argc, char **argv)
{
    const char  *path = NULL, *prefix = NULL;
    float        scale = 1.0f, tol = 0.25f;
    int          samples = 4, i, bad = 0, masked = 0;
    FILE        *f;
    long         len;
    uint8_t     *buf;
    ps_swf_movie m;
    char         err[160] = "";
    uint32_t     si;
    /* Both ends and the middle. The middle is the one that can produce a
     * coordinate no file ever stated, since it is the only ratio where the
     * blend actually divides. */
    static const uint16_t ratios[3] = { 0, 32768, 65535 };

    for(i = 1; i < argc; i++) {
        if(!strcmp(argv[i], "-s") && i + 1 < argc)      scale = (float)atof(argv[++i]);
        else if(!strcmp(argv[i], "-t") && i + 1 < argc) tol = (float)atof(argv[++i]);
        else if(!strcmp(argv[i], "-a") && i + 1 < argc) samples = atoi(argv[++i]);
        else if(!strcmp(argv[i], "-o") && i + 1 < argc) prefix = argv[++i];
        else if(!strcmp(argv[i], "-m"))                 masked = 1;
        else if(argv[i][0] != '-')                      path = argv[i];
    }
    if(!path) {
        fprintf(stderr, "usage: %s <file.swf> [-s scale] [-t tol] [-a samples]"
                        " [-o prefix]\n", argv[0]);
        return 2;
    }

    f = fopen(path, "rb");
    if(!f) {
        fprintf(stderr, "cannot open %s\n", path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)len);
    if(!buf || fread(buf, 1, (size_t)len, f) != (size_t)len) {
        fprintf(stderr, "read failed\n");
        fclose(f);
        free(buf);
        return 1;
    }
    fclose(f);

    if(ps_swf_load(buf, (size_t)len, &m, err, sizeof err) < 0) {
        fprintf(stderr, "%s: %s\n", path, err);
        free(buf);
        return 1;
    }
    free(buf);

    printf("%-22s %5s %8s %8s %8s %6s %6s %9s%s\n", path, "shape", "tris",
           "interior", "boundary", "worst", "wind", "area",
           masked ? "   masked" : "");

    for(si = 0; si < m.nshape; si++) {
        char label[16];

        snprintf(label, sizeof label, "%u", si);
        bad |= compare_shape(&m.shapes[si], label, scale, tol, samples, prefix,
                             masked);
    }

    /* Morphs, at both ends and the middle. They are not in the shape table -
     * a morph is a pair of shapes and a blend, and the thing the renderer sees
     * exists only once a ratio has been chosen - so the only way to put one
     * through this comparison is to compute it here. */
    for(si = 0; si < m.nmorph; si++) {
        int r;

        for(r = 0; r < 3; r++) {
            ps_swf_shape sh;
            char         label[16];

            if(ps_swf_morph_shape_init(&m.morphs[si], &sh) < 0) {
                fprintf(stderr, "out of memory\n");
                return 1;
            }
            ps_swf_morph_at(&m.morphs[si], ratios[r], &sh);
            snprintf(label, sizeof label, "m%u@%d", si, r);
            bad |= compare_shape(&sh, label, scale, tol, samples, prefix,
                                 masked);
            ps_swf_morph_shape_free(&sh);
        }
    }

    ps_swf_free(&m);
    if(bad)
        printf("MISMATCH\n");
    return bad ? 1 : 0;
}
