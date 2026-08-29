/* GIF, PNG, JPEG, and BMP decoding with a bounded texture cache. */
#ifndef PS_IMAGE_H
#define PS_IMAGE_H

#include "ps_types.h"
#include "ps_config.h"
#include "../gfx/ps_gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Refusing an oversized image costs one broken picture; decoding it costs the
 * whole page. */
#define PS_IMAGE_MAX_PIXELS PS_CFG_IMAGE_MAX_PIXELS
#define PS_IMAGE_MAX_DIM    PS_CFG_IMAGE_MAX_DIM

/* Total decoded pixels across every frame of one GIF. A long animation is far
 * more likely to be a memory attack than a real page element. */
#define PS_IMAGE_MAX_FRAMES       PS_CFG_IMAGE_MAX_FRAMES
#define PS_IMAGE_MAX_TOTAL_PIXELS PS_CFG_IMAGE_TOTAL_PIXELS

#define PS_IMAGE_CACHE_MAX PS_CFG_IMAGE_CACHE

/* Browsers clamp implausibly fast GIF delays; 0 and 10ms are both common in
 * the wild and mean "as fast as possible", which pins the CPU for nothing. */
#define PS_GIF_MIN_DELAY_MS     20
#define PS_GIF_DEFAULT_DELAY_MS 100

typedef struct {
    ps_texture tex;
    float      u1, v1;   /* extent of the frame within its padded texture */
} ps_image_frame;

typedef struct {
    ps_image_frame frames[PS_IMAGE_MAX_FRAMES];
    int            delays_ms[PS_IMAGE_MAX_FRAMES];
    int            nframes;
    int            w, h;      /* real image size in pixels */

    /* Animation state, advanced by ps_image_cache_tick. */
    int cur_frame;
    int elapsed_ms;
} ps_image;

static inline ps_texture ps_image_tex(const ps_image *img)
{
    return img->frames[img->cur_frame].tex;
}

typedef struct ps_image_cache ps_image_cache;

/* Asks for url to be fetched. Must return immediately; the bytes come back
 * later through ps_image_deliver. */
typedef void (*ps_image_request_fn)(void *user, const char *url);

ps_image_cache *ps_image_cache_create(const ps_gfx_backend *gfx,
                                      ps_image_request_fn request, void *user);
void            ps_image_cache_destroy(ps_image_cache *c);

/* Looks the image up, requesting it if this is the first sighting. Returns
 * NULL while it is still in flight, or if it will never arrive; callers must
 * treat that as "lay out as empty", never as an error. Layout re-runs when the
 * image lands, which is what makes the load non-blocking. */
const ps_image *ps_image_get(ps_image_cache *c, const char *url);

/* Looks up without requesting. */
const ps_image *ps_image_peek(ps_image_cache *c, const char *url);

/* Hands over fetched bytes. Decodes and uploads on the calling thread, which
 * must be the one that owns the graphics backend. ok == 0 marks the image
 * permanently failed so it is never requested again. Returns non-zero if the
 * cache changed in a way that requires re-layout. */
int ps_image_deliver(ps_image_cache *c, const char *url, int ok,
                     const void *data, size_t len);

/* Forgets everything. Called on navigation so a new page does not inherit the
 * previous one's VRAM. */
void ps_image_cache_clear(ps_image_cache *c);

/* Advances every animation. Returns non-zero if any frame changed, so the
 * caller can skip repainting an idle page. */
int ps_image_cache_tick(ps_image_cache *c, int dt_ms);

#ifdef __cplusplus
}
#endif

#endif /* PS_IMAGE_H */
