/* Host harness for the AWT rasteriser.
 *
 * Renders the java.awt.Graphics calls a period applet actually makes, straight
 * to a PPM, so the graphics can be looked at without a Dreamcast on the desk.
 * The point of ps_jgfx filling a plain pixel buffer rather than talking to the
 * paint layer is that this file is possible at all.
 *
 * Text comes from the same TTF the browser boots with, via stb_truetype, so
 * what is measured here is the real typeface at the real size.
 *
 *   cc -I.. -I../../vendor jgfx_demo.c ../ps_jgfx.c -lm -o jgfx_demo
 */
#include "ps_jgfx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

/* --- text backend -------------------------------------------------------- */

typedef struct {
    unsigned char  *ttf;
    stbtt_fontinfo  info;
    int             ok;
} host_font;

static int hf_scaled(host_font *f, int px, float *scale)
{
    if(!f->ok)
        return 0;
    *scale = stbtt_ScaleForPixelHeight(&f->info, (float)px);
    return 1;
}

static int hf_measure(void *user, const char *s, size_t len, int px)
{
    host_font *f = (host_font *)user;
    float      scale;
    int        w = 0;
    size_t     i;

    if(!hf_scaled(f, px, &scale))
        return 0;

    for(i = 0; i < len; i++) {
        int adv, lsb;

        stbtt_GetCodepointHMetrics(&f->info, (unsigned char)s[i], &adv, &lsb);
        w += (int)(adv * scale);
    }
    return w;
}

static int hf_ascent(void *user, int px)
{
    host_font *f = (host_font *)user;
    float      scale;
    int        a, d, l;

    if(!hf_scaled(f, px, &scale))
        return px;
    stbtt_GetFontVMetrics(&f->info, &a, &d, &l);
    return (int)(a * scale);
}

static int hf_descent(void *user, int px)
{
    host_font *f = (host_font *)user;
    float      scale;
    int        a, d, l;

    if(!hf_scaled(f, px, &scale))
        return 0;
    stbtt_GetFontVMetrics(&f->info, &a, &d, &l);
    return (int)(-d * scale);
}

static void hf_draw(void *user, ps_jsurface *s, int x, int baseline,
                    const char *str, size_t len, uint32_t argb, int px,
                    int cx0, int cy0, int cx1, int cy1)
{
    host_font *f = (host_font *)user;
    float      scale;
    size_t     i;
    int        pen = x;

    if(!hf_scaled(f, px, &scale))
        return;

    for(i = 0; i < len; i++) {
        int            c = (unsigned char)str[i];
        int            gw, gh, gx, gy, adv, lsb, ix, iy;
        unsigned char *bmp;

        bmp = stbtt_GetCodepointBitmap(&f->info, scale, scale, c,
                                       &gw, &gh, &gx, &gy);
        stbtt_GetCodepointHMetrics(&f->info, c, &adv, &lsb);

        if(bmp) {
            for(iy = 0; iy < gh; iy++) {
                int ty = baseline + gy + iy;

                if(ty < cy0 || ty >= cy1 || ty < 0 || ty >= s->h)
                    continue;

                for(ix = 0; ix < gw; ix++) {
                    int      tx = pen + gx + ix;
                    unsigned a  = bmp[iy * gw + ix];
                    uint32_t *d;
                    unsigned  ia;

                    if(!a || tx < cx0 || tx >= cx1 || tx < 0 || tx >= s->w)
                        continue;

                    d  = &s->px[(size_t)ty * s->stride + tx];
                    ia = 255u - a;
                    *d = 0xff000000u
                       | (((((argb >> 16) & 0xff) * a + ((*d >> 16) & 0xff) * ia) / 255u) << 16)
                       | (((((argb >>  8) & 0xff) * a + ((*d >>  8) & 0xff) * ia) / 255u) <<  8)
                       |  ((((  argb        & 0xff) * a + (( *d        & 0xff) * ia)) / 255u));
                }
            }
            stbtt_FreeBitmap(bmp, NULL);
        }
        pen += (int)(adv * scale);
    }
}

/* --- ppm out ------------------------------------------------------------- */

static int write_ppm(const char *path, const ps_jsurface *s)
{
    FILE *f = fopen(path, "wb");
    int   i, n = s->w * s->h;

    if(!f)
        return -1;

    fprintf(f, "P6\n%d %d\n255\n", s->w, s->h);
    for(i = 0; i < n; i++) {
        uint32_t p = s->px[i];
        unsigned char rgb[3];

        rgb[0] = (unsigned char)((p >> 16) & 0xff);
        rgb[1] = (unsigned char)((p >>  8) & 0xff);
        rgb[2] = (unsigned char)( p        & 0xff);
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    return 0;
}

/* --- the scenes ----------------------------------------------------------
 *
 * Each of these is the drawing an actual category of period applet does. They
 * are here to be looked at, so they lean on the calls whose AWT semantics are
 * easy to get subtly wrong - the inclusive outlines, the clip, the even-odd
 * polygon fill, copyArea's overlap.
 */

#define RGB(r, g, b) (0xff000000u | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

/* The inclusive-vs-exclusive rule, drawn so it can be counted.
 * drawRect(x,y,20,20) must be 21 pixels wide; fillRect(x,y,20,20) must be 20.
 * If those are the same width the rasteriser is wrong. */
static void scene_rect_semantics(ps_jgfx *g)
{
    int i;

    ps_jgfx_set_color(g, RGB(255, 255, 255));
    ps_jgfx_fill_rect(g, 0, 0, 300, 200);

    ps_jgfx_set_color(g, RGB(20, 20, 30));
    ps_jgfx_draw_string(g, "drawRect vs fillRect", 20, 14, 22);

    for(i = 0; i < 4; i++) {
        int x = 20 + i * 66, y = 40, sz = 8 + i * 8;

        ps_jgfx_set_color(g, RGB(200, 60, 40));
        ps_jgfx_draw_rect(g, x, y, sz, sz);

        ps_jgfx_set_color(g, RGB(40, 90, 200));
        ps_jgfx_fill_rect(g, x, y + 60, sz, sz);
    }

    ps_jgfx_set_color(g, RGB(120, 120, 130));
    ps_jgfx_draw_line(g, 20, 150, 280, 150);
    ps_jgfx_draw_string(g, "outline is w+1, fill is w", 25, 20, 170);
}

/* Clip and translate, the two things a Graphics handed to a child component
 * carries. The inner box must not leak past the clip no matter what it
 * draws. */
static void scene_clip(ps_jgfx *g)
{
    ps_jgfx inner;
    int     i;

    ps_jgfx_set_color(g, RGB(245, 245, 250));
    ps_jgfx_fill_rect(g, 0, 0, 300, 200);

    ps_jgfx_set_color(g, RGB(20, 20, 30));
    ps_jgfx_draw_string(g, "clip + translate", 16, 14, 22);

    ps_jgfx_copy(&inner, g);
    ps_jgfx_translate(&inner, 40, 40);
    ps_jgfx_clip_rect(&inner, 0, 0, 220, 120);

    ps_jgfx_set_color(&inner, RGB(230, 230, 236));
    ps_jgfx_fill_rect(&inner, 0, 0, 220, 120);

    /* Deliberately way outside the clip. None of it may escape. */
    for(i = -40; i < 300; i += 14) {
        ps_jgfx_set_color(&inner, RGB(60 + i / 2, 120, 200 - i / 3));
        ps_jgfx_draw_line(&inner, i, -60, i - 60, 200);
    }

    ps_jgfx_set_color(g, RGB(200, 40, 40));
    ps_jgfx_draw_rect(g, 40, 40, 220, 120);
}

/* Ovals, arcs and a pie chart. fillArc is what every "stats" applet drew. */
static void scene_curves(ps_jgfx *g)
{
    static const uint32_t slice[] = {
        RGB(220, 80, 60), RGB(240, 170, 60), RGB(90, 170, 90),
        RGB(70, 130, 210), RGB(150, 100, 190)
    };
    static const int pct[] = { 30, 22, 18, 17, 13 };
    int i, at = 0, acc = 0;

    ps_jgfx_set_color(g, RGB(255, 255, 255));
    ps_jgfx_fill_rect(g, 0, 0, 300, 200);

    ps_jgfx_set_color(g, RGB(20, 20, 30));
    ps_jgfx_draw_string(g, "ovals + fillArc", 15, 14, 22);

    /* Boundaries from the running total, not a sum of individually rounded
     * spans. Rounding each slice on its own drops a fraction of a degree per
     * slice and the last one stops short of where the first began - here that
     * was two degrees of white wedge at three o'clock. Accumulating first
     * makes every boundary land on an exact percentage of the circle and the
     * final one land on 360 by construction. */
    for(i = 0; i < 5; i++) {
        int next;

        acc += pct[i];
        next = acc * 360 / 100;

        ps_jgfx_set_color(g, slice[i]);
        ps_jgfx_fill_arc(g, 20, 40, 130, 130, at, next - at);
        at = next;
    }
    ps_jgfx_set_color(g, RGB(30, 30, 40));
    ps_jgfx_draw_oval(g, 20, 40, 129, 129);

    ps_jgfx_set_color(g, RGB(70, 130, 210));
    ps_jgfx_fill_oval(g, 180, 45, 90, 50);
    ps_jgfx_set_color(g, RGB(20, 20, 30));
    ps_jgfx_draw_oval(g, 180, 45, 89, 49);

    /* Degenerate cases an applet will absolutely hand it. */
    ps_jgfx_set_color(g, RGB(200, 60, 40));
    ps_jgfx_draw_oval(g, 180, 110, 80, 1);
    ps_jgfx_draw_oval(g, 180, 120, 1, 40);
    ps_jgfx_fill_oval(g, 200, 120, 3, 40);
    ps_jgfx_set_color(g, RGB(120, 120, 130));
    ps_jgfx_draw_string(g, "1px ovals", 9, 215, 145);
}

/* Even-odd polygon fill. A five-pointed star must come out hollow in the
 * middle - that is what java.awt.Polygon does, and applets that draw stars
 * were drawn against it. */
static void scene_polygon(ps_jgfx *g)
{
    int xs[10], ys[10];
    int i;

    ps_jgfx_set_color(g, RGB(24, 26, 34));
    ps_jgfx_fill_rect(g, 0, 0, 300, 200);

    ps_jgfx_set_color(g, RGB(230, 230, 240));
    ps_jgfx_draw_string(g, "even-odd fillPolygon", 20, 14, 22);

    for(i = 0; i < 5; i++) {
        float a = (float)(-90 + i * 144) * 3.14159265f / 180.0f;

        xs[i] = 90 + (int)(60.0f * cosf(a));
        ys[i] = 110 + (int)(60.0f * sinf(a));
    }
    ps_jgfx_set_color(g, RGB(255, 194, 75));
    ps_jgfx_fill_polygon(g, xs, ys, 5);
    ps_jgfx_set_color(g, RGB(255, 255, 255));
    ps_jgfx_draw_polygon(g, xs, ys, 5);

    /* A convex blob for contrast: this one fills solid. */
    for(i = 0; i < 8; i++) {
        float a = (float)(i * 45) * 3.14159265f / 180.0f;

        xs[i] = 220 + (int)(55.0f * cosf(a));
        ys[i] = 110 + (int)(45.0f * sinf(a));
    }
    ps_jgfx_set_color(g, RGB(90, 170, 200));
    ps_jgfx_fill_polygon(g, xs, ys, 8);
}

/* copyArea, which is how every scrolling-marquee applet was built: draw once,
 * then shift the pixels a couple across per frame. Overlapping source and
 * destination is the normal case, not the edge case. */
static void scene_copyarea(ps_jgfx *g)
{
    int i;

    ps_jgfx_set_color(g, RGB(255, 255, 255));
    ps_jgfx_fill_rect(g, 0, 0, 300, 200);

    ps_jgfx_set_color(g, RGB(20, 20, 30));
    ps_jgfx_draw_string(g, "copyArea scroll", 15, 14, 22);

    ps_jgfx_set_color(g, RGB(40, 90, 200));
    for(i = 0; i < 8; i++)
        ps_jgfx_fill_rect(g, 20 + i * 32, 50, 16, 60);

    /* Shift the band right by 24, twice, the way a ticker steps a frame. */
    ps_jgfx_copy_area(g, 20, 50, 260, 60, 24, 0);
    ps_jgfx_copy_area(g, 20, 50, 260, 60, 24, 0);

    ps_jgfx_set_color(g, RGB(200, 40, 40));
    ps_jgfx_draw_rect(g, 20, 50, 259, 59);

    ps_jgfx_set_color(g, RGB(90, 170, 90));
    ps_jgfx_fill_rect(g, 20, 130, 260, 40);
    ps_jgfx_copy_area(g, 20, 130, 260, 40, 0, 14);
    ps_jgfx_set_color(g, RGB(120, 120, 130));
    ps_jgfx_draw_string(g, "vertical, overlapping", 21, 20, 192);
}

/* Rounded rects and the text baseline, so the typeface is visible at the
 * sizes chrome actually uses. */
static void scene_text(ps_jgfx *g)
{
    static const int sizes[] = { 11, 14, 18, 24, 32 };
    int y = 40, i;

    ps_jgfx_set_color(g, RGB(252, 252, 255));
    ps_jgfx_fill_rect(g, 0, 0, 300, 200);

    ps_jgfx_set_color(g, RGB(255, 194, 75));
    ps_jgfx_fill_round_rect(g, 10, 8, 280, 26, 12, 12);
    ps_jgfx_set_color(g, RGB(30, 24, 8));
    ps_jgfx_draw_string(g, "drawString + roundRect", 22, 22, 27);

    for(i = 0; i < 5; i++) {
        char buf[48];
        int  n;

        ps_jgfx_set_font_size(g, sizes[i]);
        n = snprintf(buf, sizeof buf, "%dpx Hamburgefonstiv", sizes[i]);
        ps_jgfx_set_color(g, RGB(30, 32, 40));
        y += sizes[i] + 6;
        ps_jgfx_draw_string(g, buf, (size_t)n, 16, y);

        /* Measured width, drawn as a rule under the run: if measure and draw
         * disagree the rule will not match the text. */
        ps_jgfx_set_color(g, RGB(220, 100, 80));
        ps_jgfx_draw_line(g, 16, y + 2, 16 + ps_jgfx_string_width(g, buf, (size_t)n),
                          y + 2);
    }
    ps_jgfx_set_font_size(g, 12);
}

/* --- main ---------------------------------------------------------------- */

typedef void (*scene_fn)(ps_jgfx *);

int main(int argc, char **argv)
{
    static const struct { const char *name; scene_fn fn; } scenes[] = {
        { "rects",    scene_rect_semantics },
        { "clip",     scene_clip },
        { "curves",   scene_curves },
        { "polygon",  scene_polygon },
        { "copyarea", scene_copyarea },
        { "text",     scene_text }
    };
    const int    n = (int)(sizeof scenes / sizeof scenes[0]);
    const char  *ttf_path = (argc > 1) ? argv[1] : "../../cd/font.ttf";
    const char  *out_dir  = (argc > 2) ? argv[2] : ".";
    host_font    font;
    ps_jtext_ops ops;
    int          i;

    memset(&font, 0, sizeof font);
    {
        FILE *f = fopen(ttf_path, "rb");

        if(f) {
            long sz;

            fseek(f, 0, SEEK_END);
            sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            font.ttf = (unsigned char *)malloc((size_t)sz);
            if(font.ttf && fread(font.ttf, 1, (size_t)sz, f) == (size_t)sz)
                font.ok = stbtt_InitFont(&font.info, font.ttf, 0);
            fclose(f);
        }
        if(!font.ok)
            fprintf(stderr, "warn: no font at %s, text will be blank\n",
                    ttf_path);
    }

    ops.user    = &font;
    ops.measure = hf_measure;
    ops.draw    = hf_draw;
    ops.ascent  = hf_ascent;
    ops.descent = hf_descent;

    for(i = 0; i < n; i++) {
        ps_jsurface s;
        ps_jgfx     g;
        char        path[256];

        if(ps_jsurface_init(&s, 300, 200) != 0) {
            fprintf(stderr, "surface alloc failed\n");
            return 1;
        }
        ps_jsurface_clear(&s, RGB(255, 0, 255));   /* magenta = never painted */

        ps_jgfx_init(&g, &s, &ops);
        scenes[i].fn(&g);

        snprintf(path, sizeof path, "%s/%s.ppm", out_dir, scenes[i].name);
        if(write_ppm(path, &s) != 0)
            fprintf(stderr, "could not write %s\n", path);
        else
            printf("%s\n", path);

        ps_jsurface_free(&s);
    }

    free(font.ttf);
    return 0;
}
