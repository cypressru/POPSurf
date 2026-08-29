/* Does the hardware shade a gradient the same colour the span renderer does?
 *
 * This is the one question about the PVR gradient path that can be answered
 * without a PVR, and it needed answering: a picture on a television is a poor
 * instrument for it. Two flat blocks of colour is what a correct hard-edged
 * ramp looks like and also what a texture coordinate collapsed to one value
 * would look like, and nothing about the picture distinguishes them.
 *
 * So the two halves are checked separately, because they fail differently.
 *
 * The coordinate. ps_swf_ramp_uv is the map the backend hands the hardware,
 * and the hardware interpolates it linearly across a triangle. Reading it back
 * - undoing the texture's own scale to recover the point of the gradient
 * square it addresses - must reproduce, exactly, the point ps_paint_at
 * computes from the same fill matrix. This is a float comparison with a tight
 * bound and no picture in it, and it is what catches a factor-of-32768, a
 * missing half texel, a swapped axis or a coordinate that does not span the
 * ramp. Every one of those is invisible in the second check on a ramp whose
 * stops happen to be flat.
 *
 * The colour. Build the ramp, sample it as the hardware would - bilinear, from
 * the quantised texels, clamped - and compare against ps_paint_at at the same
 * point. This cannot be exact and is not meant to be: the texture is RGB565 or
 * ARGB4444, so a channel loses two or four bits, and a radial ramp is a
 * finite-resolution picture of a square. What it measures is how much, which
 * is a number worth having rather than a bound worth asserting blindly.
 *
 * Sampled over the fill's own bounding box rather than over its triangles,
 * deliberately: the map is affine in screen position, so it is the same
 * function everywhere and which pixels a tessellation happens to cover is not
 * the thing under test. tricmp already owns that question.
 *
 *   ./ramptest file.swf [file.swf ...] [-s scale] [-v]
 */
#include "ps_swf.h"
#include "ps_swf_geom.h"
#include "ps_swf_ramp.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The coordinate check's bound, in twips of the gradient square, which is
 * 32768 wide. A hundredth of a twip is four decimal digits below anything a
 * file can express and still far above float noise on numbers of this size. */
#define UV_BOUND 0.01f

/* How much of a fill may sit more than one quantiser step from the sampler.
 *
 * Not zero, and the reason is worth stating because it looks like slack and is
 * not. A ramp carrying a hard stop - which every generated gradient here does,
 * because a wash cannot be counted - is a step function, and the hardware
 * filters bilinearly, so the step arrives as a blend one texel wide. That ring
 * is real, it is bounded by the stop's own circumference times the texel
 * spacing, and it is the price of filtering. What it is not is a fraction of
 * the fill: a misplaced coordinate moves every pixel, not a ring, so anything
 * approaching this bound is a different kind of fault. Measured at 0.39% for
 * the one hard-edged radial in the corpus, and at 1.26% before the radial
 * ramp went from 64 texels a side to 128. */
#define RING_BOUND 4.0

typedef struct {
    long   pixels;
    double uv_worst;        /* twips */
    int    chan_worst;      /* 0..255, reported only - see RING_BOUND */
    long   chan_over;       /* pixels past one quantiser step of the format */
    int    gradients;
    int    radial;
} tally;

static int chan_diff(ps_swf_rgba a, ps_swf_rgba b)
{
    int worst = 0, i;

    for(i = 0; i < 4; i++) {
        int d = (int)((const uint8_t *)&a)[i] - (int)((const uint8_t *)&b)[i];

        if(d < 0)
            d = -d;
        if(d > worst)
            worst = d;
    }
    return worst;
}

static void check_fill(const ps_swf_shape *sh, const ps_swf_xform *xf,
                       uint32_t style, tally *t, int verbose,
                       const char *label)
{
    ps_swf_paint p;
    ps_swf_ramp  ramp;
    float        uv[6];
    float        x, y;
    double       uv_worst = 0.0;
    int          chan_worst = 0;
    long         over = 0, n = 0;
    float        x0, x1, y0, y1;

    ps_geom_fill_paint(sh, xf, NULL, style, &p);
    if(!p.grad)
        return;

    t->gradients++;
    t->radial += p.radial ? 1 : 0;

    if(ps_swf_ramp_build(&ramp, p.grad, p.radial) < 0) {
        printf("    %-14s style %u: ramp build failed\n", label,
               (unsigned)style);
        return;
    }
    ps_swf_ramp_uv(&p, uv);

    /* The shape's declared bounds through the view transform. Every pixel the
     * fill could possibly cover is inside it. */
    x0 = xf->m[0] * (float)sh->xmin + xf->m[2] * (float)sh->ymin + xf->m[4];
    y0 = xf->m[1] * (float)sh->xmin + xf->m[3] * (float)sh->ymin + xf->m[5];
    x1 = xf->m[0] * (float)sh->xmax + xf->m[2] * (float)sh->ymax + xf->m[4];
    y1 = xf->m[1] * (float)sh->xmax + xf->m[3] * (float)sh->ymax + xf->m[5];
    if(x1 < x0) { float s = x0; x0 = x1; x1 = s; }
    if(y1 < y0) { float s = y0; y0 = y1; y1 = s; }

    for(y = floorf(y0) + 0.5f; y < y1; y += 1.0f) {
        for(x = floorf(x0) + 0.5f; x < x1; x += 1.0f) {
            float u = uv[0] * x + uv[1] * y + uv[2];
            float w = uv[3] * x + uv[4] * y + uv[5];
            /* Back out of the texture and into the gradient square, which is
             * the space ps_paint_at works in. */
            float gux = (u - 0.5f) * PS_SWF_RAMP_SQUARE;
            float guy = (w - 0.5f) * PS_SWF_RAMP_SQUARE;
            float gx  = p.inv[0] * x + p.inv[1] * y + p.inv[2];
            float gy  = p.inv[3] * x + p.inv[4] * y + p.inv[5];
            double d  = fabs((double)gux - (double)gx);
            int    cd;

            if(p.radial) {
                double dy = fabs((double)guy - (double)gy);

                if(dy > d)
                    d = dy;
            }
            if(d > uv_worst)
                uv_worst = d;

            cd = chan_diff(ps_paint_at(&p, x, y),
                           ps_swf_ramp_sample(&ramp, u, w));
            if(cd > chan_worst)
                chan_worst = cd;
            /* One step of whichever format the ramp landed in. RGB565 keeps
             * five bits of red and blue and six of green, so a channel can be
             * eight low; ARGB4444 keeps four and can be sixteen low. Counting
             * against the format rather than a single number is what stops
             * this reading as "the gradient with alpha is worse", when what is
             * actually true is that it is stored in fewer bits. */
            if(cd > (ramp.opaque ? 8 : 16))
                over++;
            n++;
        }
    }

    ps_swf_ramp_free(&ramp);

    t->pixels += n;
    if(uv_worst > t->uv_worst)   t->uv_worst = uv_worst;
    if(chan_worst > t->chan_worst) t->chan_worst = chan_worst;
    t->chan_over += over;

    if(verbose)
        printf("    %-14s style %u %-6s %7ld px  uv %.5f twips  "
               "worst channel %3d  over step: %ld\n",
               label, (unsigned)style, p.radial ? "radial" : "linear", n,
               uv_worst, chan_worst, over);
}

static int run(const char *path, float scale, int verbose, tally *t)
{
    FILE        *f;
    long         len;
    uint8_t     *buf;
    ps_swf_movie m;
    ps_swf_xform xf;
    char         err[160] = "";
    uint32_t     si, s;

    f = fopen(path, "rb");
    if(!f) {
        fprintf(stderr, "cannot open %s\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)len);
    if(!buf || fread(buf, 1, (size_t)len, f) != (size_t)len) {
        fprintf(stderr, "read failed on %s\n", path);
        fclose(f);
        free(buf);
        return -1;
    }
    fclose(f);

    if(ps_swf_load(buf, (size_t)len, &m, err, sizeof err) < 0) {
        fprintf(stderr, "%s: %s\n", path, err);
        free(buf);
        return -1;
    }
    free(buf);

    /* The same map swfrender uses at this scale: twips to pixels, with the
     * stage origin at the top left. */
    ps_swf_xform_scale(&xf, scale / 20.0f, scale / 20.0f,
                       -(float)m.xmin * scale / 20.0f,
                       -(float)m.ymin * scale / 20.0f);

    for(si = 0; si < m.nshape; si++) {
        char label[24];

        snprintf(label, sizeof label, "shape %u", (unsigned)si);
        for(s = 1; s <= m.shapes[si].nfill; s++)
            check_fill(&m.shapes[si], &xf, s, t, verbose, label);
    }

    ps_swf_free(&m);
    return 0;
}

int main(int argc, char **argv)
{
    float scale = 1.0f;
    int   verbose = 0, i, files = 0, bad = 0;
    tally t;

    memset(&t, 0, sizeof t);

    for(i = 1; i < argc; i++) {
        if(!strcmp(argv[i], "-s") && i + 1 < argc)
            scale = (float)atof(argv[++i]);
        else if(!strcmp(argv[i], "-v"))
            verbose = 1;
    }

    for(i = 1; i < argc; i++) {
        if(argv[i][0] == '-') {
            if(!strcmp(argv[i], "-s"))
                i++;
            continue;
        }
        if(verbose)
            printf("  %s\n", argv[i]);
        if(run(argv[i], scale, verbose, &t) == 0)
            files++;
    }

    if(t.gradients == 0) {
        printf("ramptest: no gradients in %d file(s)\n", files);
        return 0;
    }

    printf("ramptest: %d gradient%s in %d file%s (%d radial), %ld pixels\n",
           t.gradients, t.gradients == 1 ? "" : "s", files,
           files == 1 ? "" : "s", t.radial, t.pixels);

    /* The coordinate has to be right. Nothing about it is approximate: the
     * backend's map and the sampler's are the same affine function written
     * two ways, and if they are not, every conclusion drawn from the colour
     * comparison below is about the wrong pixel. */
    printf("  texture coordinate     worst %.5f twips of 32768 ", t.uv_worst);
    if(t.uv_worst > UV_BOUND) {
        printf("FAIL (bound %.2f)\n", UV_BOUND);
        bad = 1;
    }
    else {
        printf("ok\n");
    }

    /* The colour cannot be, and the number is the point rather than the
     * verdict. A channel is quantised to five or six bits by RGB565 and to
     * four by ARGB4444, so eight levels of difference is the format and not a
     * defect; what would not be the format is a pixel far from its neighbour's
     * error, which is what a misplaced sample looks like once the ramp has any
     * slope at all. */
    {
        double pct = t.pixels ? 100.0 * (double)t.chan_over / (double)t.pixels
                              : 0.0;

        printf("  colour against sampler worst %d of 255, %ld px past one "
               "step (%.2f%%) ", t.chan_worst, t.chan_over, pct);
        if(pct > RING_BOUND) {
            printf("FAIL (bound %.1f%%)\n", RING_BOUND);
            bad = 1;
        }
        else {
            printf("ok\n");
        }
    }

    return bad;
}
