/* POPSurf graphics backend interface.
 *
 * Core builds quads; a backend puts them on screen. Dreamcast drives the PVR
 * directly. The other consoles get their own backends behind this same vtable
 * later, as a group.
 *
 * Clipping is done geometrically by core, not by the backend: quad corners and
 * UVs are clamped to the clip rect before submission. The PVR's own user clip
 * is tile-granular (32x32) and so cannot express a CSS clip rect exactly.
 * Clamping is exact, costs nothing, and behaves identically on every target,
 * which matters for golden images.
 */
#ifndef PS_GFX_H
#define PS_GFX_H

#include "../core/ps_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque texture handle. 0 is always "no texture" (untextured quad). */
typedef uint32_t ps_texture;
#define PS_TEXTURE_NONE ((ps_texture)0)

typedef enum {
    PS_FMT_ARGB4444 = 0,  /* UI, glyph atlases, anything with alpha */
    PS_FMT_RGB565   = 1,  /* opaque decoded images */

    /* ARGB4444 in linear layout rather than twiddled.
     *
     * Twiddling is a CPU-side bit interleave done once per pixel at upload,
     * and it buys better texture cache locality when sampling. That is the
     * right trade for a page's images and glyph atlases, which are uploaded
     * once and sampled for the life of the page.
     *
     * It is the wrong trade for content rewritten every frame. An animating
     * applet was spending 72 milliseconds a frame twiddling 131,072 pixels to
     * deliver 60,000 - eleven times what interpreting and rasterising the
     * frame cost put together. Linear turns the upload into a store-queue
     * copy. The sampling penalty is real and invisible next to that. */
    PS_FMT_ARGB4444_LINEAR = 2
} ps_pixel_format;

/* One quad corner. Device pixels, already clipped.
 *
 * z encodes paint order, not geometry. The PVR renders the translucent list
 * with autosort disabled so that submission order is preserved, but that makes
 * it behave as a plain Z-buffered target: quads sharing a depth lose the test
 * and the first one submitted wins. Core therefore hands every quad a strictly
 * increasing z, which is the "pre-sort them yourself" the hardware wants. On
 * the PVR larger z is nearer.
 */
typedef struct {
    float    x, y, z;
    float    u, v;
    ps_color argb;
} ps_vert;

/* Quads are 4 verts in TL, TR, BL, BR order, which is the PVR's strip order. */
#define PS_VERTS_PER_QUAD 4

typedef struct ps_gfx_backend {
    int  (*init)(void *self, int width, int height);
    void (*shutdown)(void *self);

    void (*begin_frame)(void *self, ps_color clear);
    void (*end_frame)(void *self);

    ps_texture (*upload_texture)(void *self, const void *pixels,
                                 int w, int h, ps_pixel_format fmt);

    /* Rewrites an existing texture's pixels, same dimensions and format.
     *
     * This exists because of animated content that changes every frame - an
     * applet repainting at 25Hz. Freeing and reallocating for each frame is
     * correct on a host and ruinous here: pvr_mem_malloc is a straightforward
     * allocator over 8MB, and churning a quarter-megabyte texture twenty-five
     * times a second fragments it until an allocation fails. Reusing the
     * allocation costs one upload and no bookkeeping.
     *
     * Returns non-zero on success. A caller that gets zero should fall back to
     * free plus upload. */
    int  (*update_texture)(void *self, ps_texture tex, const void *pixels,
                           int w, int h);

    /* Converts ARGB8888 straight into an existing texture, in one pass.
     *
     * The two-step it replaces - convert into a staging buffer, then copy the
     * buffer into VRAM - touches a quarter of a megabyte twice and blows the
     * cache doing it. Handing the backend the source lets it convert directly
     * into the store queue: one read of the source, one write to video memory,
     * nothing resident in between.
     *
     * The backend owns this because only the backend knows the destination
     * format. src is w x h ARGB8888 with the given stride in pixels; the
     * texture may be wider, and the padding is written as transparent so the
     * layout stays contiguous.
     *
     * Returns non-zero on success. NULL, or zero, means fall back to
     * update_texture with a converted buffer. */
    int  (*update_texture_argb)(void *self, ps_texture tex,
                                const uint32_t *src, int w, int h,
                                int stride);

    void       (*free_texture)(void *self, ps_texture tex);

    /* nverts is a multiple of PS_VERTS_PER_QUAD. tex may be PS_TEXTURE_NONE. */
    void (*draw_quads)(void *self, ps_texture tex,
                       const ps_vert *verts, int nverts);

    void *self;
    int   width, height;
} ps_gfx_backend;

/* Dreamcast PVR backend. */
const ps_gfx_backend *ps_gfx_pvr(void);

#ifdef __cplusplus
}
#endif

#endif /* PS_GFX_H */
