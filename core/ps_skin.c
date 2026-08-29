#include "ps_skin.h"
#include "ps_theme.h"

#include <string.h>
#include <stdio.h>

void ps_skin_fill(ps_paint *p, int x, int y, int w, int h, ps_color c)
{
    ps_rect r;

    if(w <= 0 || h <= 0)
        return;

    r.x0 = (int16_t)x;
    r.y0 = (int16_t)y;
    r.x1 = (int16_t)(x + w);
    r.y1 = (int16_t)(y + h);
    ps_paint_rect(p, &r, c);
}

void ps_skin_grad(ps_paint *p, int x, int y, int w, int h, ps_color top,
                  ps_color bottom)
{
    ps_rect r;

    if(w <= 0 || h <= 0)
        return;

    r.x0 = (int16_t)x;
    r.y0 = (int16_t)y;
    r.x1 = (int16_t)(x + w);
    r.y1 = (int16_t)(y + h);
    ps_paint_rect_v(p, &r, top, bottom);
}

void ps_skin_frame(ps_paint *p, int x, int y, int w, int h, ps_color c)
{
    ps_skin_fill(p, x, y, w, PS_STROKE, c);
    ps_skin_fill(p, x, y + h - PS_STROKE, w, PS_STROKE, c);
    ps_skin_fill(p, x, y, PS_STROKE, h, c);
    ps_skin_fill(p, x + w - PS_STROKE, y, PS_STROKE, h, c);
}

void ps_skin_band(ps_paint *p, int x, int y, int w, int h)
{
    ps_skin_fill(p, x, y, w, h, PS_C_PANEL_RIM);
    ps_skin_grad(p, x + 2, y + 2, w - 4, h - 4, PS_C_PANEL_HI, PS_C_PANEL_LO);
}

void ps_skin_panel(ps_paint *p, int x, int y, int w, int h)
{
    ps_skin_band(p, x, y, w, h);
    ps_skin_fill(p, x + 3, y + 2, w - 6, 2, PS_ARGB(150, 255, 255, 255));
}

void ps_skin_burger(ps_paint *p, const ps_rect *r, ps_color c)
{
    int w = r->x1 - r->x0, h = r->y1 - r->y0;
    int bar_w = w - 24;
    int bar_h = 4;
    int gap   = 5;
    int total = bar_h * 3 + gap * 2;
    int y     = r->y0 + (h - total) / 2;
    int i;

    if(bar_w <= 0)
        return;

    for(i = 0; i < 3; i++)
        ps_skin_fill(p, r->x0 + 12, y + i * (bar_h + gap), bar_w, bar_h, c);
}

void ps_skin_well(ps_paint *p, int x, int y, int w, int h)
{
    ps_skin_fill(p, x, y, w, h, PS_C_WELL_RIM);
    ps_skin_grad(p, x + 2, y + 2, w - 4, h - 4, PS_C_WELL_HI, PS_C_WELL_LO);
}

void ps_skin_key(ps_paint *p, const ps_rect *r, int pressed, int active,
                 int off)
{
    int x = r->x0, y = r->y0;
    int w = r->x1 - r->x0, h = r->y1 - r->y0;
    int half = h / 2;

    ps_color hi_a = PS_C_METAL_HI_A;
    ps_color hi_b = PS_C_METAL_HI_B;
    ps_color lo_a = PS_C_METAL_LO_A;
    ps_color lo_b = PS_C_METAL_LO_B;

    if(w <= 4 || h <= 4)
        return;

    if(off) {
        /* Flat, and deliberately so: the shine is what says "press me". */
        ps_skin_fill(p, x, y, w, h, PS_C_METAL_RIM);
        ps_skin_grad(p, x + 2, y + 2, w - 4, h - 4, PS_C_METAL_OFF_A,
                     PS_C_METAL_OFF_B);
        return;
    }

    if(active) {
        hi_a = PS_C_METAL_ON_HI_A;
        hi_b = PS_C_METAL_ON_HI_B;
        lo_a = PS_C_METAL_ON_LO_A;
        lo_b = PS_C_METAL_ON_LO_B;
    }

    if(pressed) {
        /* Pressed flips the light source: dark on top, bright below. */
        ps_color t;

        t = hi_a; hi_a = lo_a; lo_a = t;
        t = hi_b; hi_b = lo_b; lo_b = t;
    }

    ps_skin_fill(p, x, y, w, h, PS_C_METAL_RIM);
    ps_skin_grad(p, x + 2, y + 2, w - 4, half - 2, hi_a, hi_b);
    ps_skin_grad(p, x + 2, y + half, w - 4, h - half - 2, lo_a, lo_b);

    /* Specular line, inset so the rim still reads as a rim. */
    if(!pressed)
        ps_skin_fill(p, x + 3, y + 2, w - 6, 2, PS_C_SPECULAR);
}

ps_color ps_skin_ink(int active, int off)
{
    if(off)
        return PS_C_METAL_INK_OFF;
    return active ? PS_C_METAL_INK_ON : PS_C_METAL_INK;
}

void ps_skin_ring(ps_paint *p, const ps_rect *r, ps_color c)
{
    int x = r->x0 - PS_STROKE, y = r->y0 - PS_STROKE;
    int w = (r->x1 - r->x0) + PS_STROKE * 2;
    int h = (r->y1 - r->y0) + PS_STROKE * 2;

    ps_skin_frame(p, x, y, w, h, c);
}

void ps_skin_tri(ps_paint *p, int x, int y, int w, int h, int dir, ps_color c)
{
    int d;

    if(w <= 0 || h <= 0)
        return;

    for(d = 0; d < w; d += 2) {
        /* Half-extent at this strip, measured from the apex. The +2 samples
         * the far edge of the strip, so the widest strip reaches the full
         * base instead of stopping one step short of it. */
        int ext = (d + 2) * (h / 2) / w;
        int sx  = dir < 0 ? x + d : x + w - d - 2;

        if(ext < 1)
            ext = 1;
        ps_skin_fill(p, sx, y + h / 2 - ext, 2, ext * 2, c);
    }
}

int ps_skin_text_w(ps_text_cache *tc, int size, const char *s)
{
    ps_font *f;

    if(!s || !*s)
        return 0;

    f = ps_text_font(tc, size);
    return f ? ps_font_measure(f, s, strlen(s)) : 0;
}

int ps_skin_text(ps_paint *p, ps_text_cache *tc, int size, int x, int baseline,
                 const char *s, ps_color c)
{
    ps_font *f;
    size_t   len;

    if(!s || !*s)
        return 0;

    f = ps_text_font(tc, size);
    if(!f)
        return 0;

    len = strlen(s);
    ps_font_draw(p, f, x, baseline, s, len, c);
    return ps_font_measure(f, s, len);
}

void ps_skin_text_center(ps_paint *p, ps_text_cache *tc, int size, int cx,
                         int baseline, const char *s, ps_color c)
{
    int w = ps_skin_text_w(tc, size, s);

    ps_skin_text(p, tc, size, cx - w / 2, baseline, s, c);
}

void ps_skin_text_elide(ps_text_cache *tc, int size, const char *s, int max_w,
                        char *out, size_t out_len)
{
    ps_font *f;
    size_t   n;
    int      dots;

    if(!out || !out_len)
        return;

    out[0] = '\0';
    if(!s || !*s || max_w <= 0)
        return;

    snprintf(out, out_len, "%s", s);

    f = ps_text_font(tc, size);
    if(!f)
        return;

    n = strlen(out);
    if(ps_font_measure(f, out, n) <= max_w)
        return;

    /* Trim from the end rather than the middle. For an address the scheme and
     * host are the part that tells you where you are; the tail of a query
     * string is not worth the characters it would cost to keep. */
    dots = ps_font_measure(f, "...", 3);
    while(n > 0 && ps_font_measure(f, out, n) + dots > max_w)
        n--;

    if(n + 4 > out_len)
        n = out_len > 4 ? out_len - 4 : 0;

    out[n] = '\0';
    if(n)
        strcat(out, "...");
}
