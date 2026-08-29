#include "ps_saver.h"
#include "ps_theme.h"
#include "ps_skin.h"

#include <string.h>

/* kos-tool moves two pixels per frame at 60Hz. Expressed as a rate so PAL and
 * NTSC bounce at the same speed rather than one being a sixth slower. */
#define SAVER_PX_PER_MS (PS_SAVER_SPEED * 60.0f / 1000.0f)

/* The KallistiOS mark, 32x32 at one bit per pixel, MSB leftmost. Taken
 * verbatim from kos-tool's screensaver.c, which is where the whole shape of
 * this comes from - the idle timeout, the two-pixel drift and the reflection
 * at the edges are all its behaviour, and it would be strange to copy the
 * mechanism and then bounce something else around with it.
 *
 * What was here before was three bars in a box, which was a mistake for a
 * reason worth writing down: three bars in a box is the toolbar's own menu
 * button. Drifting the same glyph across a blank screen invited the reading
 * that the menu had come loose, and a screensaver's one job is to be
 * unmistakably nothing. */
static const uint32_t g_mark[PS_SAVER_ICON] = {
    0x00000000, 0x01F00000, 0x03FC0600, 0x01FE0F00,
    0x01FF1F00, 0x00FF9E00, 0x007FB800, 0x001FB000,
    0x0007E000, 0x03F9CFC0, 0x07FFFFE0, 0x0FFFFFF0,
    0x1F87E1F8, 0x1F87C1F8, 0x3F8783FC, 0x3F8707FC,
    0x3F860FFC, 0x3F841FFC, 0x3F803FFC, 0x3F807FFC,
    0x3F803FFC, 0x3F841FFC, 0x1F860FFC, 0x1F8707FC,
    0x0F8783F8, 0x0F87C1F8, 0x0787E0F0, 0x0387F0E0,
    0x01FFFFC0, 0x00FFFF80, 0x007FFF00, 0x003E3E00
};

void ps_saver_init(ps_saver *s, int view_w, int view_h)
{
    memset(s, 0, sizeof *s);

    s->view_w = view_w;
    s->view_h = view_h;

    /* Starts at the middle heading down and right, which is where kos-tool
     * starts it and as good a corner-avoiding opening as any. */
    s->x  = (float)(view_w - PS_SAVER_DRAW) / 2.0f;
    s->y  = (float)(view_h - PS_SAVER_DRAW) / 2.0f;
    s->dx = SAVER_PX_PER_MS;
    s->dy = SAVER_PX_PER_MS;
}

void ps_saver_wake(ps_saver *s)
{
    s->active  = 0;
    s->idle_ms = 0;
}

int ps_saver_tick(ps_saver *s, int dt_ms, int input, int busy)
{
    float max_x, max_y;

    if(input) {
        ps_saver_wake(s);
        return 0;
    }

    /* Playing or loading holds the timer at zero rather than merely
     * suppressing the blank. Otherwise a long video would end with the saver
     * appearing the instant it finished, having counted the whole runtime as
     * idle. */
    if(busy) {
        s->idle_ms = 0;
        s->active  = 0;
        return 0;
    }

    if(!s->active) {
        s->idle_ms += dt_ms;
        if(s->idle_ms < PS_SAVER_IDLE_MS)
            return 0;

        s->active = 1;
    }

    max_x = (float)(s->view_w - PS_SAVER_DRAW);
    max_y = (float)(s->view_h - PS_SAVER_DRAW);

    s->x += s->dx * (float)dt_ms;
    s->y += s->dy * (float)dt_ms;

    /* Reflect rather than clamp, and reflect the overshoot too: a long frame -
     * and this browser has plenty, a page parse blocks for hundreds of
     * milliseconds - would otherwise park the mark against the edge for the
     * rest of that frame's travel. */
    if(s->x < 0.0f)      { s->x = -s->x;                 s->dx = -s->dx; }
    if(s->x > max_x)     { s->x = max_x - (s->x - max_x); s->dx = -s->dx; }
    if(s->y < 0.0f)      { s->y = -s->y;                 s->dy = -s->dy; }
    if(s->y > max_y)     { s->y = max_y - (s->y - max_y); s->dy = -s->dy; }

    /* A frame long enough to overshoot both walls at once would still land
     * outside after one reflection. Rare, and cheaper to pin than to solve. */
    if(s->x < 0.0f)  s->x = 0.0f;
    if(s->x > max_x) s->x = max_x;
    if(s->y < 0.0f)  s->y = 0.0f;
    if(s->y > max_y) s->y = max_y;

    return 1;
}

void ps_saver_draw(ps_paint *p, const ps_saver *s)
{
    int x, y, i;

    if(!s->active)
        return;

    x = (int)s->x;
    y = (int)s->y;

    /* A mask, not a mark. The caller has already painted the page; this covers
     * it in black everywhere except where the bitmap is set, so the apple is a
     * hole the page shows through rather than a shape sitting on top of one.
     *
     * There is no stencil in the paint layer - it draws rectangles and nothing
     * else - so the black is assembled as the complement of the mark: four
     * bands around its box, then the clear runs of each row inside it. That is
     * the same span walk as before with the bit test inverted.
     *
     * The K comes out right for free. It is negative space inside the apple in
     * the source bitmap, so it stays black while the body around it opens up,
     * which is exactly how the logo is meant to read. */
    ps_skin_fill(p, 0, 0, s->view_w, y, PS_C_SAVER_MASK);
    ps_skin_fill(p, 0, y + PS_SAVER_DRAW, s->view_w,
                 s->view_h - (y + PS_SAVER_DRAW), PS_C_SAVER_MASK);
    ps_skin_fill(p, 0, y, x, PS_SAVER_DRAW, PS_C_SAVER_MASK);
    ps_skin_fill(p, x + PS_SAVER_DRAW, y,
                 s->view_w - (x + PS_SAVER_DRAW), PS_SAVER_DRAW,
                 PS_C_SAVER_MASK);

    /* Span edges are scaled in integers rather than stepped by a fractional
     * width, so each span starts exactly where the last one ended. At two and
     * a half times, rows come out alternately two and three pixels tall and
     * the seams land nowhere - which matters more here than it did when these
     * were the lit pixels, because a seam in a mask is a one-pixel slit of
     * page showing through the black. */
    for(i = 0; i < PS_SAVER_ICON; i++) {
        uint32_t bits = g_mark[i];
        int      col  = 0;
        int      y0   = y + i * PS_SAVER_DRAW / PS_SAVER_ICON;
        int      y1   = y + (i + 1) * PS_SAVER_DRAW / PS_SAVER_ICON;

        while(col < PS_SAVER_ICON) {
            int run = 0;
            int x0, x1;

            /* Set bits are the window: skipped, so nothing is drawn over the
             * page there. */
            if(bits & (0x80000000u >> col)) {
                col++;
                continue;
            }

            while(col + run < PS_SAVER_ICON &&
                  !(bits & (0x80000000u >> (col + run))))
                run++;

            x0 = x + col * PS_SAVER_DRAW / PS_SAVER_ICON;
            x1 = x + (col + run) * PS_SAVER_DRAW / PS_SAVER_ICON;

            ps_skin_fill(p, x0, y0, x1 - x0, y1 - y0, PS_C_SAVER_MASK);
            col += run;
        }
    }
}
