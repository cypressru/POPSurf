#include "ps_image.h"
#include "../net/ps_url.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_MAX_DIMENSIONS PS_IMAGE_MAX_DIM
#include "../vendor/stb_image.h"

typedef enum {
    PS_IMG_PENDING = 0,   /* requested, bytes not back yet */
    PS_IMG_READY,
    PS_IMG_FAILED         /* remembered so a broken image is fetched once */
} ps_img_state;

typedef struct {
    char         url[PS_URL_MAX];
    ps_image     img;
    int          used;
    ps_img_state state;
} ps_entry;

struct ps_image_cache {
    const ps_gfx_backend *gfx;
    ps_image_request_fn   request;
    void                 *user;
    ps_entry              entries[PS_IMAGE_CACHE_MAX];
};

ps_image_cache *ps_image_cache_create(const ps_gfx_backend *gfx,
                                      ps_image_request_fn request, void *user)
{
    ps_image_cache *c;

    if(!gfx || !request)
        return NULL;

    c = (ps_image_cache *)calloc(1, sizeof *c);
    if(!c)
        return NULL;

    c->gfx     = gfx;
    c->request = request;
    c->user    = user;
    return c;
}

static void free_entry_textures(ps_image_cache *c, ps_entry *e)
{
    int f;

    for(f = 0; f < e->img.nframes; f++) {
        if(e->img.frames[f].tex != PS_TEXTURE_NONE)
            c->gfx->free_texture(c->gfx->self, e->img.frames[f].tex);
    }
    e->img.nframes = 0;
}

void ps_image_cache_destroy(ps_image_cache *c)
{
    int i;

    if(!c)
        return;

    for(i = 0; i < PS_IMAGE_CACHE_MAX; i++) {
        if(c->entries[i].used)
            free_entry_textures(c, &c->entries[i]);
    }
    free(c);
}

/* The PVR's valid texture dimensions are 8 through 1024. Padding to a power of
 * two is not sufficient on its own: a 1 or 4 pixel wide texture is not
 * representable at all, and uploading one produces garbage rather than an
 * error. One-pixel strips are common - they are how the era drew gradient
 * bars - so this is a real case, not a hypothetical. */
#define PS_TEX_MIN_DIM 8

static int next_pow2(int v)
{
    int p = PS_TEX_MIN_DIM;

    while(p < v)
        p <<= 1;
    return p;
}

static ps_entry *lookup(ps_image_cache *c, const char *url)
{
    int i;

    for(i = 0; i < PS_IMAGE_CACHE_MAX; i++) {
        if(c->entries[i].used && !strcmp(c->entries[i].url, url))
            return &c->entries[i];
    }
    return NULL;
}

/* Converts one RGBA8888 frame into a power-of-two ARGB4444 texture.
 *
 * Padding is transparent, but the UVs never reach it: sampling stops at
 * u1/v1. Bilinear filtering would still bleed the edge, which is why the PVR
 * backend uses PVR_FILTER_NONE. */
static int upload_frame(ps_image_cache *c, ps_image_frame *out,
                        const unsigned char *rgba, int w, int h)
{
    int       pw = next_pow2(w);
    int       ph = next_pow2(h);
    uint16_t *tex;
    int       x, y;

    if(pw > PS_IMAGE_MAX_DIM || ph > PS_IMAGE_MAX_DIM)
        return -1;

    tex = (uint16_t *)calloc((size_t)pw * ph, sizeof(uint16_t));
    if(!tex)
        return -1;

    for(y = 0; y < h; y++) {
        const unsigned char *src = rgba + (size_t)y * w * 4;
        uint16_t            *dst = tex + (size_t)y * pw;

        for(x = 0; x < w; x++) {
            unsigned r = src[0], g = src[1], b = src[2], a = src[3];

            dst[x] = (uint16_t)(((a >> 4) << 12) | ((r >> 4) << 8) |
                                ((g >> 4) << 4) | (b >> 4));
            src += 4;
        }
    }

    out->tex = c->gfx->upload_texture(c->gfx->self, tex, pw, ph,
                                      PS_FMT_ARGB4444);
    free(tex);

    if(out->tex == PS_TEXTURE_NONE)
        return -1;

    out->u1 = (float)w / (float)pw;
    out->v1 = (float)h / (float)ph;
    return 0;
}

static int is_gif(const unsigned char *d, size_t len)
{
    return len >= 6 && !memcmp(d, "GIF8", 4);
}

/* GIF, possibly animated. stb composites disposal for us and returns every
 * frame back to back, already the full canvas size. */
static int decode_gif(ps_image_cache *c, ps_image *img,
                      const unsigned char *data, size_t len)
{
    int           *delays = NULL;
    int            w = 0, h = 0, nframes = 0, comp = 0;
    unsigned char *pixels;
    int            f, ok = 0;

    pixels = stbi_load_gif_from_memory((const stbi_uc *)data, (int)len,
                                       &delays, &w, &h, &nframes, &comp, 4);
    if(!pixels)
        return -1;

    if(w <= 0 || h <= 0 || nframes <= 0)
        goto done;

    if((long)w * h > PS_IMAGE_MAX_PIXELS)
        goto done;

    /* Clamp rather than reject: a long animation should lose its tail, not
     * take the whole page down. */
    if(nframes > PS_IMAGE_MAX_FRAMES)
        nframes = PS_IMAGE_MAX_FRAMES;
    while(nframes > 1 && (long)w * h * nframes > PS_IMAGE_MAX_TOTAL_PIXELS)
        nframes--;

    img->w = w;
    img->h = h;

    for(f = 0; f < nframes; f++) {
        const unsigned char *frame = pixels + (size_t)f * w * h * 4;
        int                  d;

        if(upload_frame(c, &img->frames[f], frame, w, h) != 0) {
            /* Out of VRAM mid-animation: keep what we have and stop. A short
             * loop beats no image. */
            break;
        }

        d = delays ? delays[f] : PS_GIF_DEFAULT_DELAY_MS;
        if(d < PS_GIF_MIN_DELAY_MS)
            d = PS_GIF_DEFAULT_DELAY_MS;
        img->delays_ms[f] = d;

        img->nframes = f + 1;
    }

    ok = img->nframes > 0;

done:
    free(delays);
    stbi_image_free(pixels);
    return ok ? 0 : -1;
}

static int decode_still(ps_image_cache *c, ps_image *img,
                        const unsigned char *data, size_t len)
{
    unsigned char *rgba;
    int            w, h, comp;

    rgba = stbi_load_from_memory((const stbi_uc *)data, (int)len, &w, &h,
                                 &comp, 4);
    if(!rgba)
        return -1;

    if(w <= 0 || h <= 0 || (long)w * h > PS_IMAGE_MAX_PIXELS) {
        stbi_image_free(rgba);
        return -1;
    }

    if(upload_frame(c, &img->frames[0], rgba, w, h) != 0) {
        stbi_image_free(rgba);
        return -1;
    }

    stbi_image_free(rgba);

    img->w            = w;
    img->h            = h;
    img->nframes      = 1;
    img->delays_ms[0] = 0;
    return 0;
}

const ps_image *ps_image_peek(ps_image_cache *c, const char *url)
{
    ps_entry *e;

    if(!c || !url)
        return NULL;

    e = lookup(c, url);
    if(!e || e->state != PS_IMG_READY)
        return NULL;
    return &e->img;
}

const ps_image *ps_image_get(ps_image_cache *c, const char *url)
{
    ps_entry *e;
    int       i;

    if(!c || !url || strlen(url) >= PS_URL_MAX)
        return NULL;

    e = lookup(c, url);
    if(e)
        return e->state == PS_IMG_READY ? &e->img : NULL;

    for(i = 0; i < PS_IMAGE_CACHE_MAX; i++) {
        if(!c->entries[i].used)
            break;
    }
    if(i == PS_IMAGE_CACHE_MAX)
        return NULL;

    /* The slot is claimed before the request goes out, so layout running again
     * while the fetch is in flight sees PENDING and does not re-request. */
    e = &c->entries[i];
    memset(e, 0, sizeof *e);
    strcpy(e->url, url);
    e->used  = 1;
    e->state = PS_IMG_PENDING;

    c->request(c->user, url);
    return NULL;
}

int ps_image_deliver(ps_image_cache *c, const char *url, int ok,
                     const void *data, size_t len)
{
    ps_entry *e;
    int       rc;

    if(!c || !url)
        return 0;

    e = lookup(c, url);
    if(!e || e->state != PS_IMG_PENDING)
        return 0;

    if(!ok || !data || !len) {
        e->state = PS_IMG_FAILED;
        return 0;
    }

    if(is_gif((const unsigned char *)data, len))
        rc = decode_gif(c, &e->img, (const unsigned char *)data, len);
    else
        rc = decode_still(c, &e->img, (const unsigned char *)data, len);

    if(rc != 0) {
        free_entry_textures(c, e);
        e->state = PS_IMG_FAILED;
        return 0;
    }

    e->state = PS_IMG_READY;

    /* An image that arrived with real dimensions changes the layout that was
     * computed while it was still zero-sized. */
    return 1;
}

void ps_image_cache_clear(ps_image_cache *c)
{
    int i;

    if(!c)
        return;

    for(i = 0; i < PS_IMAGE_CACHE_MAX; i++) {
        if(c->entries[i].used) {
            free_entry_textures(c, &c->entries[i]);
            memset(&c->entries[i], 0, sizeof c->entries[i]);
        }
    }
}

int ps_image_cache_tick(ps_image_cache *c, int dt_ms)
{
    int changed = 0;
    int i;

    if(!c)
        return 0;

    for(i = 0; i < PS_IMAGE_CACHE_MAX; i++) {
        ps_image *img = &c->entries[i].img;

        if(!c->entries[i].used || c->entries[i].state != PS_IMG_READY ||
           img->nframes < 2)
            continue;

        img->elapsed_ms += dt_ms;

        /* A while loop, not an if: a long stall must not leave the animation
         * permanently behind, and a fast GIF can owe several frames. */
        while(img->elapsed_ms >= img->delays_ms[img->cur_frame]) {
            img->elapsed_ms -= img->delays_ms[img->cur_frame];
            img->cur_frame   = (img->cur_frame + 1) % img->nframes;
            changed          = 1;
        }
    }

    return changed;
}
