#include "ps_jgfx.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

/* --- surface ------------------------------------------------------------- */

int ps_jsurface_init(ps_jsurface *s, int w, int h)
{
    memset(s, 0, sizeof *s);

    if(w <= 0 || h <= 0)
        return -1;

    s->px = (uint32_t *)calloc((size_t)w * (size_t)h, sizeof(uint32_t));
    if(!s->px)
        return -1;

    s->w = w;
    s->h = h;
    s->stride = w;
    return 0;
}

void ps_jsurface_free(ps_jsurface *s)
{
    free(s->px);
    memset(s, 0, sizeof *s);
}

void ps_jsurface_clear(ps_jsurface *s, uint32_t argb)
{
    int i, n = s->stride * s->h;

    for(i = 0; i < n; i++)
        s->px[i] = argb;
}

/* --- context ------------------------------------------------------------- */

void ps_jgfx_init(ps_jgfx *g, ps_jsurface *s, const ps_jtext_ops *text)
{
    memset(g, 0, sizeof *g);

    g->s       = s;
    g->text    = text;
    g->color   = 0xff000000u;   /* AWT hands paint() a black pen */
    g->cx1     = s->w;
    g->cy1     = s->h;
    g->font_px = 12;
}

void ps_jgfx_init_vector(ps_jgfx *g, const struct ps_jvec_ops *ops, void *user,
                         int x, int y, int w, int h)
{
    memset(g, 0, sizeof *g);

    g->vec      = ops;
    g->vec_user = user;
    g->vx       = x;
    g->vy       = y;
    g->color    = 0xff000000u;
    g->cx0      = 0;
    g->cy0      = 0;
    g->cx1      = w;
    g->cy1      = h;
    g->font_px  = 12;
}

void ps_jgfx_copy(ps_jgfx *dst, const ps_jgfx *src)
{
    *dst = *src;
}

void ps_jgfx_set_color(ps_jgfx *g, uint32_t argb)
{
    g->color = argb;
}

void ps_jgfx_set_font_size(ps_jgfx *g, int px)
{
    if(px > 0)
        g->font_px = px;
}

void ps_jgfx_translate(ps_jgfx *g, int dx, int dy)
{
    g->tx += dx;
    g->ty += dy;
}

void ps_jgfx_clip_rect(ps_jgfx *g, int x, int y, int w, int h)
{
    int x0 = x + g->tx, y0 = y + g->ty;
    int x1 = x0 + w,    y1 = y0 + h;

    /* Intersect only. A clip that could widen would let a component paint
     * outside the box its parent gave it, which is the one thing the clip
     * exists to prevent. */
    if(x0 > g->cx0) g->cx0 = x0;
    if(y0 > g->cy0) g->cy0 = y0;
    if(x1 < g->cx1) g->cx1 = x1;
    if(y1 < g->cy1) g->cy1 = y1;
}

void ps_jgfx_set_clip(ps_jgfx *g, int x, int y, int w, int h)
{
    g->cx0 = x + g->tx;
    g->cy0 = y + g->ty;
    g->cx1 = g->cx0 + w;
    g->cy1 = g->cy0 + h;

    if(g->cx0 < 0) g->cx0 = 0;
    if(g->cy0 < 0) g->cy0 = 0;
    if(g->s) {
        if(g->cx1 > g->s->w) g->cx1 = g->s->w;
        if(g->cy1 > g->s->h) g->cy1 = g->s->h;
    }
}

/* --- pixel plumbing ------------------------------------------------------ */

/* Alpha is honoured even though Java 1.1's Color had none.
 *
 * It costs one compare in the common case - applets of the period draw opaque,
 * and that path is a straight store - and it means the same rasteriser serves
 * the transparent-GIF blits that every rollover-button applet needs. */
static void blend(uint32_t *dst, uint32_t src)
{
    uint32_t a = src >> 24;

    if(a == 0xff) {
        *dst = src;
        return;
    }
    if(a == 0)
        return;

    {
        uint32_t d  = *dst;
        uint32_t ia = 255u - a;
        uint32_t r  = (((src >> 16) & 0xff) * a + ((d >> 16) & 0xff) * ia) / 255u;
        uint32_t gg = (((src >>  8) & 0xff) * a + ((d >>  8) & 0xff) * ia) / 255u;
        uint32_t b  = (( src        & 0xff) * a + ( d        & 0xff) * ia) / 255u;

        *dst = 0xff000000u | (r << 16) | (gg << 8) | b;
    }
}

static void px(ps_jgfx *g, int x, int y)
{
    if(x < g->cx0 || x >= g->cx1 || y < g->cy0 || y >= g->cy1)
        return;

    if(g->vec) {
        g->vec->rect(g->vec_user, g->vx + x, g->vy + y,
                     g->vx + x + 1, g->vy + y + 1, g->color);
        return;
    }

    if(x < 0 || x >= g->s->w || y < 0 || y >= g->s->h)
        return;

    blend(&g->s->px[(size_t)y * g->s->stride + x], g->color);
}

/* Vertical run, y1 exclusive.
 *
 * The counterpart to span(), and the reason outlines are affordable as
 * geometry. An oval's left edge is a nearly vertical line: as single pixels it
 * is two hundred quads, and each quad is thirty-two times the bytes of the
 * pixel write it replaces. As runs it is a handful. */
static void vspan(ps_jgfx *g, int x, int y0, int y1)
{
    int j;

    if(x < g->cx0 || x >= g->cx1)
        return;
    if(y0 < g->cy0) y0 = g->cy0;
    if(y1 > g->cy1) y1 = g->cy1;
    if(y1 <= y0)
        return;

    if(g->vec) {
        g->vec->rect(g->vec_user, g->vx + x, g->vy + y0,
                     g->vx + x + 1, g->vy + y1, g->color);
        return;
    }

    for(j = y0; j < y1; j++)
        px(g, x, j);
}

/* Collects consecutive same-column pixels into one run.
 *
 * Used by the outline paths, which walk an edge a row at a time. Without it
 * every row is its own shape; with it a straight stretch of edge is one. */
typedef struct {
    int active;
    int x, y0, y1;
} ps_run;

static void run_flush(ps_jgfx *g, ps_run *r)
{
    if(r->active) {
        vspan(g, r->x, r->y0, r->y1);
        r->active = 0;
    }
}

static void run_add(ps_jgfx *g, ps_run *r, int x, int y)
{
    if(r->active && r->x == x && r->y1 == y) {
        r->y1 = y + 1;
        return;
    }
    run_flush(g, r);
    r->active = 1;
    r->x = x;
    r->y0 = y;
    r->y1 = y + 1;
}

/* Horizontal run, x1 exclusive. Every fill in this file funnels through here,
 * so the clip is enforced in one place rather than in nine. */
static void span(ps_jgfx *g, int x0, int x1, int y)
{
    uint32_t *row;
    int       i;

    if(y < g->cy0 || y >= g->cy1)
        return;
    if(!g->vec && (y < 0 || y >= g->s->h))
        return;

    if(x0 < g->cx0) x0 = g->cx0;
    if(x1 > g->cx1) x1 = g->cx1;
    if(x1 <= x0)
        return;

    /* One quad for the whole run. This is the funnel every fill in the file
     * goes through, so making it emit geometry is most of what vector mode
     * is. */
    if(g->vec) {
        g->vec->rect(g->vec_user, g->vx + x0, g->vy + y,
                     g->vx + x1, g->vy + y + 1, g->color);
        return;
    }

    if(x0 < 0) x0 = 0;
    if(x1 > g->s->w) x1 = g->s->w;
    if(x1 <= x0)
        return;

    row = &g->s->px[(size_t)y * g->s->stride];

    if((g->color >> 24) == 0xff) {
        for(i = x0; i < x1; i++)
            row[i] = g->color;
    }
    else {
        for(i = x0; i < x1; i++)
            blend(&row[i], g->color);
    }
}

/* --- lines --------------------------------------------------------------- */

void ps_jgfx_draw_line(ps_jgfx *g, int x1, int y1, int x2, int y2)
{
    int dx, dy, sx, sy, err;

    x1 += g->tx; y1 += g->ty;
    x2 += g->tx; y2 += g->ty;

    /* Axis-aligned lines are the overwhelming majority - borders, rules,
     * bar charts - and go through the span path instead of stepping pixel by
     * pixel. Both endpoints are inclusive, as AWT specifies. */
    if(y1 == y2) {
        span(g, x1 < x2 ? x1 : x2, (x1 < x2 ? x2 : x1) + 1, y1);
        return;
    }
    if(x1 == x2) {
        int a = y1 < y2 ? y1 : y2, b = y1 < y2 ? y2 : y1;

        vspan(g, x1, a, b + 1);
        return;
    }

    dx = abs(x2 - x1);
    dy = -abs(y2 - y1);
    sx = x1 < x2 ? 1 : -1;
    sy = y1 < y2 ? 1 : -1;
    err = dx + dy;

    /* Steep lines advance mostly in y, so consecutive pixels share a column
     * and merge into runs. Shallow ones do not, and stay per-pixel - they are
     * bounded by the width rather than the height, and a horizontal run
     * accumulator would need span() to work backwards as well. */
    if(-dy > dx) {
        ps_run r = { 0, 0, 0, 0 };

        for(;;) {
            run_add(g, &r, x1, y1);
            if(x1 == x2 && y1 == y2)
                break;
            {
                int e2 = 2 * err;

                if(e2 >= dy) { err += dy; x1 += sx; }
                if(e2 <= dx) { err += dx; y1 += sy; }
            }
        }
        run_flush(g, &r);
        return;
    }

    for(;;) {
        px(g, x1, y1);
        if(x1 == x2 && y1 == y2)
            break;
        {
            int e2 = 2 * err;

            if(e2 >= dy) { err += dy; x1 += sx; }
            if(e2 <= dx) { err += dx; y1 += sy; }
        }
    }
}

/* --- rectangles ---------------------------------------------------------- */

void ps_jgfx_fill_rect(ps_jgfx *g, int x, int y, int w, int h)
{
    int j;

    x += g->tx;
    y += g->ty;

    /* One quad rather than one per row. Every applet clears its background
     * with this call, so in vector mode it is the difference between a
     * background costing one quad and costing two hundred. */
    if(g->vec) {
        int x0 = x, y0 = y, x1 = x + w, y1 = y + h;

        if(x0 < g->cx0) x0 = g->cx0;
        if(y0 < g->cy0) y0 = g->cy0;
        if(x1 > g->cx1) x1 = g->cx1;
        if(y1 > g->cy1) y1 = g->cy1;
        if(x1 > x0 && y1 > y0)
            g->vec->rect(g->vec_user, g->vx + x0, g->vy + y0,
                         g->vx + x1, g->vy + y1, g->color);
        return;
    }

    for(j = y; j < y + h; j++)
        span(g, x, x + w, j);
}

void ps_jgfx_draw_rect(ps_jgfx *g, int x, int y, int w, int h)
{
    /* w+1 by h+1 pixels. See the header: drawRect is inclusive of its far
     * edge and fillRect is not, and applets depend on the difference. */
    int ax = x + g->tx, ay = y + g->ty;

    if(w < 0 || h < 0)
        return;

    span(g, ax, ax + w + 1, ay);
    span(g, ax, ax + w + 1, ay + h);

    vspan(g, ax,     ay + 1, ay + h);
    vspan(g, ax + w, ay + 1, ay + h);
}

void ps_jgfx_clear_rect(ps_jgfx *g, int x, int y, int w, int h, uint32_t bg)
{
    uint32_t save = g->color;

    /* clearRect paints the component's background, not the current pen -
     * an applet that calls it expects the colour it set with setBackground,
     * which is why the caller supplies it rather than this reading g->color. */
    g->color = bg;
    ps_jgfx_fill_rect(g, x, y, w, h);
    g->color = save;
}

/* --- ovals ---------------------------------------------------------------
 *
 * Extents per scanline from the ellipse equation rather than a midpoint
 * tracer. It is exact at any aspect ratio including the degenerate ones an
 * applet will hand it - a 1-pixel-tall oval, a zero-width one - where an
 * incremental algorithm needs special cases for each. The cost is a sqrt per
 * row, on shapes that are tens of rows tall.
 */

static void oval_extent(float cx, float cy, float rx, float ry, int y,
                        int *x0, int *x1)
{
    float fy, d;

    if(rx <= 0.0f || ry <= 0.0f) {
        *x0 = *x1 = 0;
        return;
    }

    fy = ((float)y + 0.5f - cy) / ry;
    d  = 1.0f - fy * fy;

    if(d <= 0.0f) {
        *x0 = 1;
        *x1 = 0;      /* empty */
        return;
    }

    d = rx * sqrtf(d);
    *x0 = (int)floorf(cx - d + 0.5f);
    *x1 = (int)floorf(cx + d + 0.5f);
}

void ps_jgfx_fill_oval(ps_jgfx *g, int x, int y, int w, int h)
{
    float rx = w * 0.5f, ry = h * 0.5f;
    float cx, cy;
    int   j;

    if(w <= 0 || h <= 0)
        return;

    x += g->tx;
    y += g->ty;
    cx = (float)x + rx;
    cy = (float)y + ry;

    for(j = y; j < y + h; j++) {
        int a, b;

        oval_extent(cx, cy, rx, ry, j, &a, &b);
        if(b >= a)
            span(g, a, b, j);
    }
}

void ps_jgfx_draw_oval(ps_jgfx *g, int x, int y, int w, int h)
{
    /* Inclusive box, like drawRect. */
    float rx = (w + 1) * 0.5f, ry = (h + 1) * 0.5f;
    float cx, cy;
    int   j, i;

    if(w < 0 || h < 0)
        return;

    x += g->tx;
    y += g->ty;
    cx = (float)x + rx;
    cy = (float)y + ry;

    /* Two passes. The row pass leaves gaps where the curve is near-horizontal
     * - the top and bottom caps - and the column pass fills exactly those,
     * which is cheaper and more robust than trying to make one pass step
     * correctly through both regimes.
     *
     * Each edge accumulates into a run, so the long straight stretches down
     * the sides become one shape rather than one per row. */
    {
        ps_run left = { 0, 0, 0, 0 }, right = { 0, 0, 0, 0 };

        for(j = y; j <= y + h; j++) {
            int a, b;

            oval_extent(cx, cy, rx, ry, j, &a, &b);
            if(b < a)
                continue;
            run_add(g, &left, a, j);
            run_add(g, &right, b - 1 < a ? a : b - 1, j);
        }
        run_flush(g, &left);
        run_flush(g, &right);
    }

    for(i = x; i <= x + w; i++) {
        int a, b;

        /* Same equation with the axes swapped. The caps are short, so these
         * stay per-pixel. */
        oval_extent(cy, cx, ry, rx, i, &a, &b);
        if(b < a)
            continue;
        px(g, i, a);
        px(g, i, b - 1 < a ? a : b - 1);
    }
}

void ps_jgfx_draw_round_rect(ps_jgfx *g, int x, int y, int w, int h,
                             int aw, int ah)
{
    int rx = aw / 2, ry = ah / 2;

    if(rx <= 0 || ry <= 0) {
        ps_jgfx_draw_rect(g, x, y, w, h);
        return;
    }
    if(rx * 2 > w) rx = w / 2;
    if(ry * 2 > h) ry = h / 2;

    ps_jgfx_draw_line(g, x + rx, y,     x + w - rx, y);
    ps_jgfx_draw_line(g, x + rx, y + h, x + w - rx, y + h);
    ps_jgfx_draw_line(g, x,     y + ry, x,     y + h - ry);
    ps_jgfx_draw_line(g, x + w, y + ry, x + w, y + h - ry);

    ps_jgfx_draw_arc(g, x,          y,          rx * 2, ry * 2,  90, 90);
    ps_jgfx_draw_arc(g, x + w - rx * 2, y,      rx * 2, ry * 2,   0, 90);
    ps_jgfx_draw_arc(g, x,          y + h - ry * 2, rx * 2, ry * 2, 180, 90);
    ps_jgfx_draw_arc(g, x + w - rx * 2, y + h - ry * 2, rx * 2, ry * 2,
                     270, 90);
}

void ps_jgfx_fill_round_rect(ps_jgfx *g, int x, int y, int w, int h,
                             int aw, int ah)
{
    int rx = aw / 2, ry = ah / 2;
    int j;

    if(rx <= 0 || ry <= 0) {
        ps_jgfx_fill_rect(g, x, y, w, h);
        return;
    }
    if(rx * 2 > w) rx = w / 2;
    if(ry * 2 > h) ry = h / 2;

    x += g->tx;
    y += g->ty;

    for(j = y; j < y + h; j++) {
        int inset = 0;

        if(j < y + ry || j >= y + h - ry) {
            float cy = (j < y + ry) ? (float)(y + ry) : (float)(y + h - ry);
            float fy = ((float)j + 0.5f - cy) / (float)ry;
            float d  = 1.0f - fy * fy;

            if(d <= 0.0f)
                continue;
            inset = rx - (int)(rx * sqrtf(d) + 0.5f);
        }
        span(g, x + inset, x + w - inset, j);
    }
}

/* --- arcs ----------------------------------------------------------------
 *
 * Sampled parametrically. AWT's angles are degrees counterclockwise with zero
 * at three o'clock, which is neither radians nor the screen's y-down sense, so
 * the sine is negated on the way out. Pie charts and clock hands are what
 * these actually get used for, and both are small.
 */

#define ARC_STEP_DEG 2

void ps_jgfx_draw_arc(ps_jgfx *g, int x, int y, int w, int h,
                      int start_deg, int arc_deg)
{
    float rx = (w + 1) * 0.5f, ry = (h + 1) * 0.5f;
    float cx = (float)x + rx, cy = (float)y + ry;
    int   steps, i;
    int   px_ = 0, py_ = 0;

    if(w < 0 || h < 0 || arc_deg == 0)
        return;

    steps = abs(arc_deg) / ARC_STEP_DEG;
    if(steps < 2)
        steps = 2;

    for(i = 0; i <= steps; i++) {
        float a = (float)(start_deg + (float)arc_deg * i / steps)
                  * 3.14159265f / 180.0f;
        int   qx = (int)(cx + rx * cosf(a) - 0.5f);
        int   qy = (int)(cy - ry * sinf(a) - 0.5f);

        if(i)
            ps_jgfx_draw_line(g, px_, py_, qx, qy);
        px_ = qx;
        py_ = qy;
    }
}

void ps_jgfx_fill_arc(ps_jgfx *g, int x, int y, int w, int h,
                      int start_deg, int arc_deg)
{
    float rx = w * 0.5f, ry = h * 0.5f;
    float cx = (float)x + rx, cy = (float)y + ry;
    int   steps, i, n = 0;
    int   xs[184], ys[184];

    if(w <= 0 || h <= 0 || arc_deg == 0)
        return;

    steps = abs(arc_deg) / ARC_STEP_DEG;
    if(steps < 2)   steps = 2;
    if(steps > 180) steps = 180;

    /* Centre first: a pie slice, which is what fillArc means for anything
     * short of a full circle. */
    xs[n] = (int)cx;
    ys[n] = (int)cy;
    n++;

    for(i = 0; i <= steps; i++) {
        float a = (float)(start_deg + (float)arc_deg * i / steps)
                  * 3.14159265f / 180.0f;

        xs[n] = (int)(cx + rx * cosf(a));
        ys[n] = (int)(cy - ry * sinf(a));
        n++;
    }

    ps_jgfx_fill_polygon(g, xs, ys, n);
}

/* --- polygons ------------------------------------------------------------ */

void ps_jgfx_draw_polyline(ps_jgfx *g, const int *xs, const int *ys, int n)
{
    int i;

    for(i = 1; i < n; i++)
        ps_jgfx_draw_line(g, xs[i - 1], ys[i - 1], xs[i], ys[i]);
}

void ps_jgfx_draw_polygon(ps_jgfx *g, const int *xs, const int *ys, int n)
{
    if(n < 2)
        return;

    ps_jgfx_draw_polyline(g, xs, ys, n);
    ps_jgfx_draw_line(g, xs[n - 1], ys[n - 1], xs[0], ys[0]);
}

/* Scanline fill, even-odd. java.awt.Polygon is even-odd, so a self-crossing
 * star leaves its middle hollow - which is exactly what the applets that draw
 * stars expect to see. */
void ps_jgfx_fill_polygon(ps_jgfx *g, const int *xs, const int *ys, int n)
{
    int  ymin, ymax, y, i;
    int  xints[64];

    if(n < 3)
        return;

    ymin = ymax = ys[0];
    for(i = 1; i < n; i++) {
        if(ys[i] < ymin) ymin = ys[i];
        if(ys[i] > ymax) ymax = ys[i];
    }

    ymin += g->ty;
    ymax += g->ty;

    if(ymin < g->cy0) ymin = g->cy0;
    if(ymax > g->cy1) ymax = g->cy1;

    for(y = ymin; y < ymax; y++) {
        int cnt = 0;
        float fy = (float)(y - g->ty) + 0.5f;

        for(i = 0; i < n; i++) {
            int   j  = (i + 1) % n;
            float y0 = (float)ys[i], y1 = (float)ys[j];
            float x0 = (float)xs[i], x1 = (float)xs[j];

            if((y0 <= fy && y1 > fy) || (y1 <= fy && y0 > fy)) {
                if(cnt < (int)(sizeof xints / sizeof xints[0])) {
                    xints[cnt++] =
                        (int)(x0 + (fy - y0) / (y1 - y0) * (x1 - x0) + 0.5f);
                }
            }
        }

        /* Insertion sort: crossing counts here are single digits, and a
         * qsort call per scanline would cost more than the sort. */
        for(i = 1; i < cnt; i++) {
            int v = xints[i], k = i - 1;

            while(k >= 0 && xints[k] > v) {
                xints[k + 1] = xints[k];
                k--;
            }
            xints[k + 1] = v;
        }

        for(i = 0; i + 1 < cnt; i += 2)
            span(g, xints[i] + g->tx, xints[i + 1] + g->tx, y);
    }
}

/* --- text ---------------------------------------------------------------- */

void ps_jgfx_draw_string(ps_jgfx *g, const char *utf8, size_t len,
                         int x, int baseline)
{
    if(!utf8 || !len)
        return;

    /* Straight through the glyph atlas the page's own text uses - textured
     * quads, no CPU rasterisation at all. Applet text is the one thing that
     * gets faster than the browser's own by being in vector mode. */
    if(g->vec) {
        if(g->vec->text)
            g->vec->text(g->vec_user, g->vx + x + g->tx,
                         g->vy + baseline + g->ty, utf8, len, g->color,
                         g->font_px);
        return;
    }

    if(!g->text || !g->text->draw)
        return;

    g->text->draw(g->text->user, g->s, x + g->tx, baseline + g->ty, utf8, len,
                  g->color, g->font_px, g->cx0, g->cy0, g->cx1, g->cy1);
}

int ps_jgfx_string_width(const ps_jgfx *g, const char *utf8, size_t len)
{
    if(!utf8 || !len)
        return 0;

    if(g->vec)
        return g->vec->measure
             ? g->vec->measure(g->vec_user, utf8, len, g->font_px) : 0;

    if(!g->text || !g->text->measure)
        return 0;

    return g->text->measure(g->text->user, utf8, len, g->font_px);
}

/* --- blits --------------------------------------------------------------- */

void ps_jgfx_copy_area(ps_jgfx *g, int x, int y, int w, int h, int dx, int dy)
{
    int j;

    /* Reading pixels back is the one thing geometry cannot express. An applet
     * that scrolls itself this way has to be rasterised, so it says so and the
     * caller moves it to the software path. */
    if(g->vec) {
        g->vec_unsupported = 1;
        return;
    }

    x += g->tx;
    y += g->ty;

    if(w <= 0 || h <= 0 || (dx == 0 && dy == 0))
        return;

    /* Bottom-up when moving down, so a scroll that overlaps itself does not
     * smear the row it is about to read. Tickers scroll by a pixel or two a
     * frame and overlap almost completely. */
    if(dy > 0) {
        for(j = h - 1; j >= 0; j--) {
            int sy = y + j, ty = y + j + dy;

            if(sy < 0 || sy >= g->s->h || ty < g->cy0 || ty >= g->cy1)
                continue;
            memmove(&g->s->px[(size_t)ty * g->s->stride + x + dx],
                    &g->s->px[(size_t)sy * g->s->stride + x],
                    (size_t)w * sizeof(uint32_t));
        }
    }
    else {
        for(j = 0; j < h; j++) {
            int sy = y + j, ty = y + j + dy;

            if(sy < 0 || sy >= g->s->h || ty < g->cy0 || ty >= g->cy1)
                continue;
            memmove(&g->s->px[(size_t)ty * g->s->stride + x + dx],
                    &g->s->px[(size_t)sy * g->s->stride + x],
                    (size_t)w * sizeof(uint32_t));
        }
    }
}

void ps_jgfx_draw_image(ps_jgfx *g, const uint32_t *src, int sw, int sh,
                        int sstride, int dx, int dy)
{
    int i, j;

    /* An image is a texture, not a rectangle. Emitting one would mean the
     * vector path carrying an upload cache of its own, which is the software
     * path with extra steps - so an applet that draws images stays on it. */
    if(g->vec) {
        g->vec_unsupported = 1;
        return;
    }

    dx += g->tx;
    dy += g->ty;

    for(j = 0; j < sh; j++) {
        int ty = dy + j;

        if(ty < g->cy0 || ty >= g->cy1 || ty < 0 || ty >= g->s->h)
            continue;

        for(i = 0; i < sw; i++) {
            int tx = dx + i;

            if(tx < g->cx0 || tx >= g->cx1 || tx < 0 || tx >= g->s->w)
                continue;
            blend(&g->s->px[(size_t)ty * g->s->stride + tx],
                  src[(size_t)j * sstride + i]);
        }
    }
}

void ps_jgfx_draw_image_scaled(ps_jgfx *g, const uint32_t *src, int sw, int sh,
                               int sstride, int dx, int dy, int dw, int dh)
{
    int i, j;

    if(g->vec) {
        g->vec_unsupported = 1;
        return;
    }

    if(sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
        return;

    dx += g->tx;
    dy += g->ty;

    /* Nearest neighbour. Applets scale to fit a fixed panel and the sources
     * are already small; a filtered rescale would cost more than the whole
     * rest of the frame and soften art that was drawn pixel by pixel. */
    for(j = 0; j < dh; j++) {
        int ty = dy + j;
        int sy = j * sh / dh;

        if(ty < g->cy0 || ty >= g->cy1 || ty < 0 || ty >= g->s->h)
            continue;

        for(i = 0; i < dw; i++) {
            int tx = dx + i;
            int sx = i * sw / dw;

            if(tx < g->cx0 || tx >= g->cx1 || tx < 0 || tx >= g->s->w)
                continue;
            blend(&g->s->px[(size_t)ty * g->s->stride + tx],
                  src[(size_t)sy * sstride + sx]);
        }
    }
}
