/* Renders a .swf to PPM files, on the host.
 *
 * The reason this exists before any Dreamcast code does: a PPM you can open
 * and look at, produced in well under a second, tells you whether the shape
 * parser is right. The same question asked on hardware costs a twenty-minute
 * upload and answers it worse, because a wrong answer there could be the
 * parser, the tessellator, the PVR list, or the texture format, and you
 * cannot tell which. This project has learned that twice already.
 *
 *   ./swfrender file.swf [-o prefix] [-s scale] [-t tol] [-a samples]
 *                        [-f N | -f all]
 *
 * Without -f it writes prefix-NN.ppm per shape definition, drawn against that
 * shape's own declared bounds. That is the diagnostic view: a shape has no
 * position of its own, so this is the only way to look at one in isolation,
 * and drawing to the declared bounds also checks them against the geometry -
 * art that spills outside the box it declares is a parse error showing itself.
 *
 * With -f it plays the display list and writes prefix-fNNN.ppm per frame, on
 * the stage, which is the picture the file is actually asking for.
 */
#include "ps_swf.h"
#include "ps_swf_clip.h"
#include "ps_swf_edit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *px;      /* RGB, w*h*3 */
    int      w, h;
} canvas;

/* The span sink. Everything above this point works in geometry; this is the
 * only code that knows what a pixel is, which is the whole reason the
 * callback is here rather than the rasteriser writing the buffer itself. On
 * the PVR this becomes a triangle submission and nothing else moves. */
static void put_span(void *user, int y, int x0, int x1, uint8_t cov,
                     ps_swf_rgba c)
{
    canvas *cv = user;
    int     x;
    unsigned a = (unsigned)cov * (unsigned)c.a / 255u;

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

typedef struct {
    canvas            *cv;
    const ps_swf_view *v;
    long               segs;
    ps_swf_clip        clip;
} stagectx;

/* Every shape goes through the clip, mask or not. With no mask in force it
 * forwards the span untouched, so there is no second path to keep in step -
 * which is the same reason the mask itself arrives through this call rather
 * than through one of its own. */
static void draw_shape(void *user, const ps_swf_shape *sh,
                       const ps_swf_xform *xf, const ps_swf_cxform *cx)
{
    stagectx *sc = user;
    long      n  = ps_swf_raster_shape(sh, sc->v, xf, cx, ps_swf_clip_span,
                                       &sc->clip);

    if(n > 0)
        sc->segs += n;
}

static void stage_clip_begin(void *user) { ps_swf_clip_begin(&((stagectx *)user)->clip); }
static void stage_clip_apply(void *user) { ps_swf_clip_apply(&((stagectx *)user)->clip); }
static void stage_clip_end(void *user)   { ps_swf_clip_end(&((stagectx *)user)->clip); }

static const ps_swf_stage_sink stage_sink = {
    draw_shape, stage_clip_begin, stage_clip_apply, stage_clip_end
};

static int write_ppm(const char *path, const canvas *cv)
{
    FILE *f = fopen(path, "wb");

    if(!f)
        return -1;
    fprintf(f, "P6\n%d %d\n255\n", cv->w, cv->h);
    fwrite(cv->px, 1, (size_t)cv->w * cv->h * 3, f);
    fclose(f);
    return 0;
}

static const char *fill_type_name(uint8_t t)
{
    switch(t) {
    case PS_SWF_FILL_SOLID:  return "solid";
    case PS_SWF_FILL_LINEAR: return "linear";
    case PS_SWF_FILL_RADIAL: return "radial";
    default: return (t & 0x40) ? "bitmap" : "?";
    }
}

/* twips -> pixels for a box of twips starting at (x0,y0), at `scale` output
 * pixels per source pixel, with a one-pixel margin so an edge exactly on the
 * boundary is still visible. */
static void fit(ps_swf_xform *xf, int32_t x0, int32_t y0, float scale)
{
    float s = scale / 20.0f;

    ps_swf_xform_scale(xf, s, s, -((float)x0 * s) + 1.0f,
                       -((float)y0 * s) + 1.0f);
}

static uint32_t root_frames(const ps_swf_movie *m)
{
    return m->root.nframe;
}

int main(int argc, char **argv)
{
    const char  *path = NULL, *prefix = "shape", *fsel = NULL;
    float        scale = 1.0f, tol = 0.25f;
    int          samples = 4, i;
    FILE        *f;
    long         len;
    uint8_t     *buf;
    ps_swf_movie m;
    char         err[160] = "";
    uint32_t     si;
    size_t       parse_peak;

    for(i = 1; i < argc; i++) {
        if(!strcmp(argv[i], "-o") && i + 1 < argc)      prefix = argv[++i];
        else if(!strcmp(argv[i], "-s") && i + 1 < argc) scale = (float)atof(argv[++i]);
        else if(!strcmp(argv[i], "-t") && i + 1 < argc) tol = (float)atof(argv[++i]);
        else if(!strcmp(argv[i], "-a") && i + 1 < argc) samples = atoi(argv[++i]);
        else if(!strcmp(argv[i], "-f") && i + 1 < argc) fsel = argv[++i];
        else if(argv[i][0] != '-')                      path = argv[i];
    }
    if(!path) {
        fprintf(stderr,
                "usage: %s <file.swf> [-o prefix] [-s scale] [-t tol_px]"
                " [-a samples] [-f N|all]\n", argv[0]);
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

    ps_swf_mem_reset_peak();
    if(ps_swf_load(buf, (size_t)len, &m, err, sizeof err) < 0) {
        fprintf(stderr, "%s: %s\n", path, err);
        free(buf);
        return 1;
    }
    parse_peak = ps_swf_mem_peak();
    free(buf);

    printf("version %d  stage %.1f x %.1f px  %d frames @ %.2f fps  bg %02x%02x%02x\n",
           m.version,
           (m.xmax - m.xmin) / 20.0, (m.ymax - m.ymin) / 20.0,
           m.frames, m.fps, m.bg.r, m.bg.g, m.bg.b);
    if(err[0])
        printf("note: %s\n", err);
    printf("shapes %u  sprites %u  characters %u  root %u frames, %u ops\n",
           m.nshape, m.nsprite, m.nchar, m.root.nframe, m.root.nop);
    {
        /* Neither of these changes a pixel, which is exactly why they are
         * printed: a label and an instance name are what ActionScript aims a
         * gotoAndPlay or a SetTarget at, and nothing about the picture would
         * show that the parser had skipped them. It did skip them, for as long
         * as the header claimed otherwise. */
        uint32_t k;

        for(k = 0; k < m.root.nlabel; k++) {
            int f = ps_swf_find_label(&m.root, m.root.labels[k].name);

            printf("label \"%s\" -> frame %d\n", m.root.labels[k].name, f);
        }
        for(k = 0; k < m.root.nop; k++)
            if(m.root.ops[k].name)
                printf("instance \"%s\" at depth %u\n", m.root.ops[k].name,
                       m.root.ops[k].depth);
    }
    if(m.nbitmap) {
        /* Printed with their sizes because a bitmap that decoded to the wrong
         * dimensions still draws a picture, just the wrong one, and the tag
         * states them plainly enough that a glance settles it. A bitmap that
         * did not decode at all is not here and is instead the note above. */
        uint32_t bi;

        printf("bitmaps %u:", m.nbitmap);
        for(bi = 0; bi < m.nbitmap && bi < 8; bi++)
            printf(" %u=%ux%u", m.bitmaps[bi].id, m.bitmaps[bi].w,
                   m.bitmaps[bi].h);
        if(m.nbitmap > 8)
            printf(" (%u more)", m.nbitmap - 8);
        putchar('\n');
    }
    printf("parser held %zu bytes, peaked at %zu\n",
           ps_swf_mem_live(), parse_peak);
    if(m.nmorph)
        printf("morphs %u\n", m.nmorph);
    /* A field is printed as what its layout produced rather than as what it
     * declared, because that is the number nothing else can show: a field
     * whose font never arrived, or whose text is all characters that font does
     * not carry, is an empty stage that looks exactly like a placement bug. */
    for(si = 0; si < m.nedit; si++)
        printf("edit text id=%u: %u glyph(s) on %u line(s), font %u at"
               " %.1f px%s\n", m.edits[si].id, m.edits[si].nglyph,
               m.edits[si].nline, m.edits[si].font_id,
               m.edits[si].height / 20.0,
               m.edits[si].html ? ", HTML flag set and not interpreted" : "");

    if(fsel) {
        uint32_t first = 0, last = root_frames(&m);
        uint32_t fr;
        double   wpx = (m.xmax - m.xmin) / 20.0;
        double   hpx = (m.ymax - m.ymin) / 20.0;

        if(last == 0) {
            printf("no frames\n");
            ps_swf_free(&m);
            return 0;
        }
        if(strcmp(fsel, "all")) {
            first = (uint32_t)strtoul(fsel, NULL, 10);
            if(first >= last)
                first = last - 1;
            last = first + 1;
        }

        for(fr = first; fr < last; fr++) {
            ps_swf_view  v;
            ps_swf_xform xf;
            canvas       cv;
            stagectx     sc;
            char         name[256];
            long         drawn;

            v.w = (int)(wpx * scale) + 2;
            v.h = (int)(hpx * scale) + 2;
            v.tol = tol;
            v.samples = samples;
            if(v.w < 1 || v.h < 1 || v.w > 8192 || v.h > 8192) {
                printf("stage unusable at this scale\n");
                break;
            }
            fit(&xf, m.xmin, m.ymin, scale);

            cv.w = v.w;
            cv.h = v.h;
            cv.px = malloc((size_t)cv.w * cv.h * 3);
            if(!cv.px)
                break;
            /* The file's own background, not a debug grey: a frame is meant to
             * be looked at as the page would show it. */
            for(i = 0; i < cv.w * cv.h; i++) {
                cv.px[i * 3 + 0] = m.bg.r;
                cv.px[i * 3 + 1] = m.bg.g;
                cv.px[i * 3 + 2] = m.bg.b;
            }

            sc.cv = &cv;
            sc.v = &v;
            sc.segs = 0;
            /* Inside the reset, so the mask planes are counted as the scratch
             * they are - this is the one number that says what masking costs a
             * frame, and it is the number the hardware path has to beat. */
            ps_swf_mem_reset_peak();
            if(ps_swf_clip_init(&sc.clip, &v, put_span, &cv) < 0) {
                free(cv.px);
                break;
            }
            drawn = ps_swf_render_frame(&m, fr, &xf, &stage_sink, &sc);
            if(ps_swf_clip_failed(&sc.clip))
                printf("  frame %3u: ran out of memory for a mask\n", fr);

            snprintf(name, sizeof name, "%s-f%03u.ppm", prefix, fr);
            if(write_ppm(name, &cv) == 0)
                printf("  frame %3u: %2ld characters, %5ld chords, "
                       "scratch %zu, mask %zu  -> %s\n", fr, drawn, sc.segs,
                       ps_swf_mem_peak() - ps_swf_mem_live(),
                       ps_swf_clip_bytes(&sc.clip), name);
            ps_swf_clip_free(&sc.clip);
            free(cv.px);
        }
        ps_swf_free(&m);
        return 0;
    }

    for(si = 0; si < m.nshape; si++) {
        ps_swf_shape *sh = &m.shapes[si];
        ps_swf_view   v;
        ps_swf_xform  xf;
        canvas        cv;
        char          name[256];
        long          segs;
        uint32_t      k;
        double        wpx = (sh->xmax - sh->xmin) / 20.0;
        double        hpx = (sh->ymax - sh->ymin) / 20.0;

        printf("\nshape %u  id=%u  bounds %.1f,%.1f .. %.1f,%.1f px"
               " (%.0f x %.0f)\n", si, sh->id,
               sh->xmin / 20.0, sh->ymin / 20.0, sh->xmax / 20.0,
               sh->ymax / 20.0, wpx, hpx);
        printf("  %u fills, %u lines, %u edges, %u layer(s)\n",
               sh->nfill, sh->nline, sh->nedge, sh->nlayer);
        for(k = 0; k < sh->nfill && k < 8; k++) {
            const ps_swf_fill *fl = &sh->fills[k];

            printf("    fill %u: %-6s %02x%02x%02x%02x", k + 1,
                   fill_type_name(fl->type),
                   fl->color.r, fl->color.g, fl->color.b, fl->color.a);
            /* The stop ratios, printed because they are the field whose
             * mis-stepping breaks every style after this one, and a ramp that
             * does not start near 0 and end near 255 is the first sign of it. */
            if(fl->grad && fl->grad <= sh->ngrad) {
                const ps_swf_gradient *gr = &sh->grads[fl->grad - 1];
                uint32_t j;

                printf("  %u stops at", gr->nstop);
                for(j = 0; j < gr->nstop; j++)
                    printf(" %u", gr->ratio[j]);
                printf(", matrix %.4g %.4g %.4g %.4g %.0f %.0f",
                       gr->mat[0], gr->mat[1], gr->mat[2], gr->mat[3],
                       gr->mat[4], gr->mat[5]);
            }
            /* Whether the character resolved is the whole question for a
             * bitmap fill: an unresolved one draws a flat grey rectangle,
             * which reads as a renderer that cannot draw bitmaps rather than
             * as a file naming one this build refused. */
            if(fl->bfill && fl->bfill <= sh->nbfill) {
                const ps_swf_bitmapfill *bf = &sh->bfills[fl->bfill - 1];

                printf("  id %u %s, matrix %.4g %.4g %.4g %.4g %.0f %.0f",
                       fl->bitmap_id, bf->bmp ? "resolved" : "MISSING",
                       bf->mat[0], bf->mat[1], bf->mat[2], bf->mat[3],
                       bf->mat[4], bf->mat[5]);
            }
            putchar('\n');
        }
        if(sh->nfill > 8)
            printf("    (%u more)\n", sh->nfill - 8);

        v.w       = (int)(wpx * scale) + 2;
        v.h       = (int)(hpx * scale) + 2;
        v.tol     = tol;
        v.samples = samples;
        if(v.w < 1 || v.h < 1 || v.w > 8192 || v.h > 8192) {
            printf("  (bounds unusable at this scale, skipped)\n");
            continue;
        }
        fit(&xf, sh->xmin, sh->ymin, scale);

        cv.w  = v.w;
        cv.h  = v.h;
        cv.px = malloc((size_t)cv.w * cv.h * 3);
        if(!cv.px)
            continue;
        memset(cv.px, 0x20, (size_t)cv.w * cv.h * 3);

        ps_swf_mem_reset_peak();
        segs = ps_swf_raster_shape(sh, &v, &xf, NULL, put_span, &cv);
        printf("  %ld chords at tol=%.2fpx, raster scratch peaked at %zu bytes\n",
               segs, tol, ps_swf_mem_peak() - ps_swf_mem_live());

        snprintf(name, sizeof name, "%s-%02u.ppm", prefix, si);
        if(write_ppm(name, &cv) == 0)
            printf("  wrote %s (%dx%d)\n", name, cv.w, cv.h);
        free(cv.px);
    }

    ps_swf_free(&m);
    return 0;
}
