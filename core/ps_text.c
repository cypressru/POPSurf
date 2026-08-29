#include "ps_text.h"

#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "../vendor/stb_truetype.h"

struct ps_font {
    stbtt_bakedchar chars[PS_GLYPH_COUNT];
    ps_texture      atlas;
    int             px_size;
    ps_font_metrics metrics;
    int             used;
};

struct ps_text_cache {
    const ps_gfx_backend *gfx;
    stbtt_fontinfo        info;
    const unsigned char  *ttf;
    ps_font               fonts[PS_MAX_FONTS];
};

ps_text_cache *ps_text_create(const ps_gfx_backend *gfx,
                              const void *ttf, size_t ttf_len)
{
    ps_text_cache *tc;

    (void)ttf_len;

    tc = (ps_text_cache *)calloc(1, sizeof *tc);
    if(!tc)
        return NULL;

    tc->gfx = gfx;
    tc->ttf = (const unsigned char *)ttf;

    if(!stbtt_InitFont(&tc->info, tc->ttf,
                       stbtt_GetFontOffsetForIndex(tc->ttf, 0))) {
        free(tc);
        return NULL;
    }

    return tc;
}

void ps_text_destroy(ps_text_cache *tc)
{
    int i;

    if(!tc)
        return;

    for(i = 0; i < PS_MAX_FONTS; i++) {
        if(tc->fonts[i].used)
            tc->gfx->free_texture(tc->gfx->self, tc->fonts[i].atlas);
    }
    free(tc);
}

/* Bake to 8bpp coverage, then widen to ARGB4444 with white RGB. */
static int bake_atlas(ps_text_cache *tc, ps_font *f, int px_size)
{
    unsigned char *gray;
    uint16_t      *rgba;
    int            i, rc;

    gray = (unsigned char *)malloc((size_t)PS_ATLAS_W * PS_ATLAS_H);
    if(!gray)
        return -1;

    rc = stbtt_BakeFontBitmap(tc->ttf, 0, (float)px_size,
                              gray, PS_ATLAS_W, PS_ATLAS_H,
                              PS_GLYPH_FIRST, PS_GLYPH_COUNT, f->chars);
    /* Negative means it ran out of atlas before finishing the range. */
    if(rc <= 0) {
        free(gray);
        return -1;
    }

    rgba = (uint16_t *)malloc((size_t)PS_ATLAS_W * PS_ATLAS_H * 2);
    if(!rgba) {
        free(gray);
        return -1;
    }

    for(i = 0; i < PS_ATLAS_W * PS_ATLAS_H; i++)
        rgba[i] = (uint16_t)(((gray[i] >> 4) << 12) | 0x0fff);

    free(gray);

    f->atlas = tc->gfx->upload_texture(tc->gfx->self, rgba,
                                       PS_ATLAS_W, PS_ATLAS_H,
                                       PS_FMT_ARGB4444);
    free(rgba);

    return f->atlas == PS_TEXTURE_NONE ? -1 : 0;
}

ps_font *ps_text_font(ps_text_cache *tc, int px_size)
{
    ps_font *f;
    int      i, slot = -1;
    int      a, d, gap;
    float    scale;

    if(!tc)
        return NULL;

    for(i = 0; i < PS_MAX_FONTS; i++) {
        if(tc->fonts[i].used && tc->fonts[i].px_size == px_size)
            return &tc->fonts[i];
        if(!tc->fonts[i].used && slot < 0)
            slot = i;
    }
    if(slot < 0)
        return NULL;

    f = &tc->fonts[slot];
    memset(f, 0, sizeof *f);
    f->px_size = px_size;

    if(bake_atlas(tc, f, px_size) < 0)
        return NULL;

    scale = stbtt_ScaleForPixelHeight(&tc->info, (float)px_size);
    stbtt_GetFontVMetrics(&tc->info, &a, &d, &gap);

    f->metrics.ascent   = (int)(a * scale + 0.5f);
    f->metrics.descent  = (int)(-d * scale + 0.5f);
    f->metrics.height   = (int)((a - d + gap) * scale + 0.5f);
    /* Approximation; good enough for the CSS ex unit, which is rare. */
    f->metrics.x_height = (int)(f->metrics.ascent * 0.52f + 0.5f);

    f->used = 1;
    return f;
}

void ps_font_get_metrics(const ps_font *f, ps_font_metrics *out)
{
    if(f && out)
        *out = f->metrics;
}

/* Minimal UTF-8 decode. Anything outside the baked range renders nothing and
 * advances nothing, which is wrong but bounded; real coverage arrives with the
 * encoding work. Malformed input must never walk off the end. */
static uint32_t utf8_next(const char *s, size_t len, size_t *i)
{
    unsigned char c = (unsigned char)s[*i];
    uint32_t      cp;
    int           extra, k;

    if(c < 0x80)      { (*i)++; return c; }
    else if(c < 0xe0) { cp = c & 0x1f; extra = 1; }
    else if(c < 0xf0) { cp = c & 0x0f; extra = 2; }
    else              { cp = c & 0x07; extra = 3; }

    if(*i + (size_t)extra >= len) {
        *i = len;
        return 0;
    }

    for(k = 1; k <= extra; k++)
        cp = (cp << 6) | ((unsigned char)s[*i + (size_t)k] & 0x3f);

    *i += (size_t)extra + 1;
    return cp;
}

static int glyph_index(uint32_t cp)
{
    if(cp < PS_GLYPH_FIRST || cp >= PS_GLYPH_FIRST + PS_GLYPH_COUNT)
        return -1;
    return (int)cp - PS_GLYPH_FIRST;
}

int ps_font_measure(const ps_font *f, const char *utf8, size_t len)
{
    size_t i = 0;
    float  x = 0.0f;

    if(!f || !utf8)
        return 0;

    while(i < len) {
        int g = glyph_index(utf8_next(utf8, len, &i));
        if(g >= 0)
            x += f->chars[g].xadvance;
    }
    return (int)(x + 0.5f);
}

void ps_font_draw(ps_paint *p, const ps_font *f, int x, int y_baseline,
                  const char *utf8, size_t len, ps_color color)
{
    size_t i = 0;
    float  xpos = (float)x;
    float  ypos = (float)y_baseline;

    if(!p || !f || !utf8)
        return;

    while(i < len) {
        int g = glyph_index(utf8_next(utf8, len, &i));
        stbtt_aligned_quad q;
        ps_rect            dst;

        if(g < 0)
            continue;

        stbtt_GetBakedQuad(f->chars, PS_ATLAS_W, PS_ATLAS_H, g,
                           &xpos, &ypos, &q, 1);

        dst.x0 = (int16_t)q.x0;
        dst.y0 = (int16_t)q.y0;
        dst.x1 = (int16_t)q.x1;
        dst.y1 = (int16_t)q.y1;

        /* Whitespace bakes to an empty box; skip rather than emit a quad. */
        if(ps_rect_empty(&dst))
            continue;

        ps_paint_image(p, f->atlas, &dst, q.s0, q.t0, q.s1, q.t1, color);
    }
}

/* --- CPU rasterisation ---------------------------------------------------
 *
 * See the note in the header: this exists so a Java applet's drawString can
 * reach a buffer in main memory, and it reuses the stb_truetype instance
 * already compiled into this file rather than linking a second one.
 */

static float cpu_scale(ps_text_cache *tc, int px_size)
{
    return stbtt_ScaleForPixelHeight(&tc->info, (float)px_size);
}

int ps_text_measure_px(ps_text_cache *tc, const char *utf8, size_t len,
                       int px_size)
{
    float  scale;
    int    w = 0;
    size_t i;

    if(!tc || !utf8 || px_size <= 0)
        return 0;

    scale = cpu_scale(tc, px_size);

    for(i = 0; i < len; i++) {
        int adv, lsb;

        stbtt_GetCodepointHMetrics(&tc->info, (unsigned char)utf8[i],
                                   &adv, &lsb);
        w += (int)(adv * scale);
    }
    return w;
}

int ps_text_ascent_px(ps_text_cache *tc, int px_size)
{
    int a, d, l;

    if(!tc || px_size <= 0)
        return px_size;

    stbtt_GetFontVMetrics(&tc->info, &a, &d, &l);
    return (int)(a * cpu_scale(tc, px_size));
}

int ps_text_descent_px(ps_text_cache *tc, int px_size)
{
    int a, d, l;

    if(!tc || px_size <= 0)
        return 0;

    stbtt_GetFontVMetrics(&tc->info, &a, &d, &l);
    return (int)(-d * cpu_scale(tc, px_size));
}

void ps_text_blit(ps_text_cache *tc, uint32_t *dst, int dst_w, int dst_h,
                  int stride, int x, int baseline, const char *utf8,
                  size_t len, uint32_t argb, int px_size,
                  int cx0, int cy0, int cx1, int cy1)
{
    float  scale;
    size_t i;
    int    pen = x;

    if(!tc || !dst || !utf8 || px_size <= 0)
        return;

    /* The clip is intersected with the buffer here rather than tested per
     * pixel below, so the inner loop only compares against one box. */
    if(cx0 < 0) cx0 = 0;
    if(cy0 < 0) cy0 = 0;
    if(cx1 > dst_w) cx1 = dst_w;
    if(cy1 > dst_h) cy1 = dst_h;
    if(cx1 <= cx0 || cy1 <= cy0)
        return;

    scale = cpu_scale(tc, px_size);

    for(i = 0; i < len; i++) {
        int            ch = (unsigned char)utf8[i];
        int            gw, gh, gx, gy, adv, lsb, ix, iy;
        unsigned char *bmp;

        stbtt_GetCodepointHMetrics(&tc->info, ch, &adv, &lsb);

        /* Skip glyphs that cannot land before rasterising them: an applet is
         * free to draw a string starting a thousand pixels off the left edge,
         * and each glyph costs a malloc and a fill to produce. */
        if(pen > cx1) {
            pen += (int)(adv * scale);
            continue;
        }

        bmp = stbtt_GetCodepointBitmap(&tc->info, scale, scale, ch,
                                       &gw, &gh, &gx, &gy);
        if(bmp) {
            for(iy = 0; iy < gh; iy++) {
                int ty = baseline + gy + iy;

                if(ty < cy0 || ty >= cy1)
                    continue;

                for(ix = 0; ix < gw; ix++) {
                    int       tx = pen + gx + ix;
                    unsigned  a  = bmp[iy * gw + ix];
                    uint32_t *d;
                    unsigned  ia, sr, sg, sb, dr, dg, db;

                    if(!a || tx < cx0 || tx >= cx1)
                        continue;

                    d  = &dst[(size_t)ty * stride + tx];
                    ia = 255u - a;

                    sr = (argb >> 16) & 0xff;
                    sg = (argb >>  8) & 0xff;
                    sb =  argb        & 0xff;
                    dr = (*d   >> 16) & 0xff;
                    dg = (*d   >>  8) & 0xff;
                    db =  *d          & 0xff;

                    *d = 0xff000000u
                       | (((sr * a + dr * ia) / 255u) << 16)
                       | (((sg * a + dg * ia) / 255u) <<  8)
                       |  ((sb * a + db * ia) / 255u);
                }
            }
            stbtt_FreeBitmap(bmp, NULL);
        }
        pen += (int)(adv * scale);
    }
}
