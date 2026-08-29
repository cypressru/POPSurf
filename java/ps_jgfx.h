/* java.awt.Graphics, rasterised into a plain pixel buffer.
 *
 * An applet paints into an offscreen image and the browser composites that
 * image into the page. Doing the same here is not just convenient, it is what
 * decouples the whole Java subsystem from the renderer: nothing in this file
 * knows about the PVR, ps_paint, or the Dreamcast. It fills an ARGB buffer.
 * The shell uploads that buffer as one texture per damaged frame, and a host
 * build writes it to a PNG, which is why the graphics can be tested without a
 * console in the room.
 *
 * The quirks below are AWT's, not ours, and they are the difference between an
 * applet that looks right and one that is a pixel out everywhere:
 *
 *  - drawRect(x, y, w, h) draws an outline spanning x..x+w *inclusive*, so it
 *    is w+1 pixels wide. fillRect(x, y, w, h) fills exactly w. Applets from
 *    the period lean on this - drawRect(0, 0, size-1, size-1) is the standard
 *    way to outline a component - and getting it wrong shifts every border.
 *  - drawLine includes both endpoints.
 *  - Ovals are bounded by the same box as the rectangle calls, inclusive.
 *  - translate() is cumulative and applies to everything, clip included.
 *  - The clip is intersect-only: clipRect can never widen the region, which is
 *    what lets a component hand a child a Graphics it cannot escape.
 *
 * No antialiasing. Java 1.1 had none, applets were authored against hard
 * edges, and on a 480i composite signal a softened edge is indistinguishable
 * from a blurry one.
 */
#ifndef PS_JGFX_H
#define PS_JGFX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Destination. Non-premultiplied ARGB8888, one word per pixel, because that is
 * what both the PVR upload path and a PNG writer want and neither cares about
 * the other. */
typedef struct {
    uint32_t *px;
    int       w, h;
    int       stride;   /* in pixels, not bytes */
} ps_jsurface;

int  ps_jsurface_init(ps_jsurface *s, int w, int h);
void ps_jsurface_free(ps_jsurface *s);
void ps_jsurface_clear(ps_jsurface *s, uint32_t argb);

/* Text is delegated.
 *
 * The Dreamcast already has a glyph cache with the page's font in it, and the
 * host harness has stb_truetype and a TTF on disc. Neither belongs in here, and
 * embedding a bitmap font so this file could be self-contained would put a
 * third, worse typeface in a browser that already has one. */
typedef struct ps_jtext_ops {
    void *user;
    int (*measure)(void *user, const char *utf8, size_t len, int px_size);
    /* Draws with (x, baseline) as the origin, clipped to the given box. */
    void (*draw)(void *user, ps_jsurface *s, int x, int baseline,
                 const char *utf8, size_t len, uint32_t argb, int px_size,
                 int cx0, int cy0, int cx1, int cy1);
    int (*ascent)(void *user, int px_size);
    int (*descent)(void *user, int px_size);
} ps_jtext_ops;

/* Vector output: the two shapes everything here reduces to. */
struct ps_jvec_ops {
    void (*rect)(void *user, int x0, int y0, int x1, int y1, uint32_t argb);
    void (*text)(void *user, int x, int baseline, const char *utf8,
                 size_t len, uint32_t argb, int px_size);
    int  (*measure)(void *user, const char *utf8, size_t len, int px_size);
};

/* Where the drawing goes.
 *
 * Two backends, and the difference is the whole performance story. The
 * software one fills a pixel buffer that then has to be packed to ARGB4444
 * and pushed to VRAM: a fixed cost proportional to the applet's area, about
 * twelve milliseconds for a 300x200 box, whatever it drew.
 *
 * The vector one emits quads into the browser's paint layer and lets the tile
 * accelerator do the filling. The cost then scales with what the applet drew
 * rather than how big it is, and the pack and the upload disappear entirely.
 * Ripple's nine oval outlines come to about 3,600 quads and Bounce's sprites
 * to 1,300, against a machine that will take twenty thousand without
 * complaint.
 *
 * Software is kept because two things need it: copyArea, which reads pixels
 * back, and any applet reaching into a pixel array directly. */
typedef struct {
    ps_jsurface        *s;
    const ps_jtext_ops *text;

    /* Non-NULL puts this in vector mode. The two callbacks are how geometry
     * leaves: everything this file draws reduces to axis-aligned rectangles
     * and runs of text, so those are the only two shapes it has to hand out.
     *
     * Callbacks rather than a ps_paint pointer on purpose. This file states at
     * the top that it knows nothing about the PVR or the paint layer, and that
     * is not decoration - it is why every test in the Java subsystem runs on a
     * host with no console attached. A direct dependency would end that. */
    const struct ps_jvec_ops *vec;
    void                     *vec_user;
    int                       vx, vy;

    /* Set when the applet asked for something vector mode cannot do -
     * copyArea, which reads pixels back, or an image blit, which needs a
     * texture rather than a rectangle. The caller reads this and moves that
     * applet to the software path for good. */
    int vec_unsupported;

    uint32_t color;
    int      tx, ty;                  /* cumulative translate */
    int      cx0, cy0, cx1, cy1;      /* clip, device space, x1/y1 exclusive */
    int      font_px;
} ps_jgfx;

/* A fresh context: no translate, clip is the whole surface, colour is black,
 * which is what java.awt.Component hands paint(). */
void ps_jgfx_init(ps_jgfx *g, ps_jsurface *s, const ps_jtext_ops *text);

/* Vector mode: quads into p, with the applet's 0,0 at (x, y) on the page and
 * everything clipped to its w by h box. The caller must have pushed a clip and
 * must pop it; nothing here can escape the box regardless, because the clip
 * below is intersected with it. */
void ps_jgfx_init_vector(ps_jgfx *g, const struct ps_jvec_ops *ops, void *user,
                         int x, int y, int w, int h);

/* create() in AWT: a copy that can be translated and clipped without
 * disturbing the original. */
void ps_jgfx_copy(ps_jgfx *dst, const ps_jgfx *src);

void ps_jgfx_set_color(ps_jgfx *g, uint32_t argb);
void ps_jgfx_set_font_size(ps_jgfx *g, int px);
void ps_jgfx_translate(ps_jgfx *g, int dx, int dy);
void ps_jgfx_clip_rect(ps_jgfx *g, int x, int y, int w, int h);
void ps_jgfx_set_clip(ps_jgfx *g, int x, int y, int w, int h);

void ps_jgfx_draw_line(ps_jgfx *g, int x1, int y1, int x2, int y2);
void ps_jgfx_draw_rect(ps_jgfx *g, int x, int y, int w, int h);
void ps_jgfx_fill_rect(ps_jgfx *g, int x, int y, int w, int h);
void ps_jgfx_clear_rect(ps_jgfx *g, int x, int y, int w, int h, uint32_t bg);
void ps_jgfx_draw_round_rect(ps_jgfx *g, int x, int y, int w, int h,
                             int aw, int ah);
void ps_jgfx_fill_round_rect(ps_jgfx *g, int x, int y, int w, int h,
                             int aw, int ah);
void ps_jgfx_draw_oval(ps_jgfx *g, int x, int y, int w, int h);
void ps_jgfx_fill_oval(ps_jgfx *g, int x, int y, int w, int h);

/* Angles in degrees, counterclockwise, zero at three o'clock - AWT's
 * convention, which is neither the screen's y-down sense nor radians. */
void ps_jgfx_draw_arc(ps_jgfx *g, int x, int y, int w, int h,
                      int start_deg, int arc_deg);
void ps_jgfx_fill_arc(ps_jgfx *g, int x, int y, int w, int h,
                      int start_deg, int arc_deg);

void ps_jgfx_draw_polyline(ps_jgfx *g, const int *xs, const int *ys, int n);
void ps_jgfx_draw_polygon(ps_jgfx *g, const int *xs, const int *ys, int n);
void ps_jgfx_fill_polygon(ps_jgfx *g, const int *xs, const int *ys, int n);

void ps_jgfx_draw_string(ps_jgfx *g, const char *utf8, size_t len,
                         int x, int baseline);
int  ps_jgfx_string_width(const ps_jgfx *g, const char *utf8, size_t len);

/* copyArea: scrolling tickers and marquee applets are built out of this. */
void ps_jgfx_copy_area(ps_jgfx *g, int x, int y, int w, int h, int dx, int dy);

/* Blits a decoded image. src is ARGB8888; alpha of 0 is skipped, which covers
 * the transparent-GIF case that every button applet of the era used. */
void ps_jgfx_draw_image(ps_jgfx *g, const uint32_t *src, int sw, int sh,
                        int sstride, int dx, int dy);
void ps_jgfx_draw_image_scaled(ps_jgfx *g, const uint32_t *src, int sw, int sh,
                               int sstride, int dx, int dy, int dw, int dh);

#ifdef __cplusplus
}
#endif

#endif /* PS_JGFX_H */
