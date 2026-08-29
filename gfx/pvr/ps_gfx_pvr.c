/* Dreamcast PVR backend.
 *
 * Everything goes through the translucent list, and paint order is carried in
 * the z of every quad rather than in the order they were submitted. Those two
 * are the same picture here - ps_paint hands out one depth step per quad in
 * the order it is asked - so the difference is invisible in the output and
 * decisive underneath it.
 *
 * It was the other way round until the SWF player needed a modifier volume.
 * With autosort disabled the hardware renders translucent polygons in strip
 * order instead of through the per-pixel sort, and a modifier volume submitted
 * in that mode does not do what the headers say: it corrupted the scene in
 * bands locked to the 32-row tile grid, whether or not any polygon in the
 * frame read the volume flag. Turning the sort on made the same page exact.
 * That is measured, not read - see ps_swf_clip.c for the four captures that
 * cornered it - and it is why this reads as a fact about depth now.
 *
 * The cost is the per-pixel sort, which is real and paid on every page. The
 * saving was never correctness: the z was always there and always right, which
 * is why the identical picture came back to the pixel when the sort went on.
 */
#include "../ps_gfx.h"
#include "ps_gfx_pvr.h"
#include "../../core/ps_config.h"

#include <dc/pvr.h>
/* Not reached through dc/pvr.h: PVR_GET and the TA register numbers live here,
 * and the vertex buffer's write pointer is the only honest answer to "how much
 * of it is gone". */
#include <dc/pvr/pvr_regs.h>
#include <dc/sq.h>
/* vid_set_dithering only; the video mode itself is KOS's to set. */
#include <dc/video.h>
#include <string.h>
#include <stdio.h>

/* Sized for animated GIFs: every frame of every animation on a page is its own
 * texture, and a single decorative GIF can be dozens of frames. The table is
 * handles only; VRAM is what actually runs out, and exhaustion is a page-level
 * failure, never fatal. */
#define PS_MAX_TEXTURES PS_CFG_MAX_TEXTURES

/* The TA vertex buffer, as asked for at pvr_init.
 *
 * 512KB is what a page needs with room to spare and what a movie can exhaust
 * on its own: the densest page measured submits 2,433 quads, which is 2,433
 * headers and 9,732 vertices at thirty-two bytes each - 380KB, three quarters
 * of it. Raising the figure for the movies that want more would take the
 * difference out of texture RAM on every page, whether or not it carries one,
 * so the number stays and the submitter that can overrun it is given a budget
 * instead. See ps_gfx_pvr.h and ps_swf_pvr.c's own budget. */
#define PS_PVR_VTX_BYTES (512 * 1024)

typedef struct {
    pvr_ptr_t       ptr;
    int             w, h;
    ps_pixel_format fmt;
    int             used;
} ps_pvr_texture;

typedef struct {
    ps_pvr_texture textures[PS_MAX_TEXTURES];
    int            width, height;
    int            in_frame;

    /* Store queue cursor for direct submission. A no-op token in this KOS -
     * pvr_dr_target toggles a global - but carried here so the dependency is
     * visible rather than implied. */
    pvr_dr_state_t dr;

    size_t   vtx_counted;          /* bytes the submitters say they wrote */
    uint32_t vtx_mark;             /* PVR_TA_VERTBUF_POS when the scene opened */
} ps_pvr_state;

static ps_pvr_state g_pvr;

static void (*g_list_hook)(void *);
static void  *g_list_hook_user;

size_t ps_pvr_vtx_capacity(void) { return PS_PVR_VTX_BYTES; }
size_t ps_pvr_vtx_counted(void)  { return g_pvr.vtx_counted; }

/* The TA's own write pointer, less where it stood when the scene opened.
 *
 * Clamped rather than trusted blind: the register is a VRAM address and a read
 * that lands before the mark - a buffer flipped under us, a scene that never
 * opened - would wrap the subtraction into a number that says the buffer is
 * full. Reading zero is the safe direction, because the caller's floor keeps it
 * drawing either way. */
size_t ps_pvr_vtx_used(void)
{
    uint32_t pos = PVR_GET(PVR_TA_VERTBUF_POS);

    if(pos < g_pvr.vtx_mark || pos - g_pvr.vtx_mark > PS_PVR_VTX_BYTES)
        return 0;
    return pos - g_pvr.vtx_mark;
}

void ps_pvr_vtx_charge(size_t bytes)
{
    g_pvr.vtx_counted += bytes;
}

void ps_pvr_set_list_hook(void (*fn)(void *), void *user)
{
    g_list_hook      = fn;
    g_list_hook_user = user;
}

static int is_pow2(int v)
{
    return v > 0 && (v & (v - 1)) == 0;
}

static int g_profile = PS_PVR_CFG_BROWSER;

/* One name per profile, spelled the same way the bootargs file spells it, so
 * the log line and the request can be compared by a machine. */
const char *ps_pvr_profile_name(int profile)
{
    static const char *const name[PS_PVR_CFG_PROFILES] = {
        "browser", "strip"
    };

    if(profile < 0 || profile >= PS_PVR_CFG_PROFILES)
        return "?";
    return name[profile];
}

void ps_pvr_set_init_profile(int profile)
{
    if(profile >= 0 && profile < PS_PVR_CFG_PROFILES)
        g_profile = profile;
}

/* After the mode is set, never before: vid_set_dithering rebuilds the frame
 * buffer configuration word from the current video mode's own entry, so a call
 * made before there is a mode writes the wrong thing to the register. */
void ps_pvr_set_dither(int enable)
{
    vid_set_dithering(enable ? true : false);
    printf("popsurf: dither %s\n", enable ? "on" : "off");
}

void ps_pvr_disable_vsmooth(void)
{
    /* One to one, so a rendered row is an output row and nothing is averaged
     * with its neighbour. pvr_init chose 0.999 for us if the console is on
     * anything but a VGA cable; on VGA this is already the case and the call
     * costs a register write that changes nothing. */
    if(pvr_set_vertical_scale(1.0f) < 0) {
        printf("popsurf: vsmooth off refused\n");
        return;
    }
    printf("popsurf: vsmooth off\n");
}

static int ps_pvr_init(void *self, int width, int height)
{
    ps_pvr_state *st = (ps_pvr_state *)self;

    /* The translucent list, and the translucent modifier list beside it.
     *
     * The sort is on. Paint order is the z ps_paint puts on every quad, which
     * was always true and is now what the picture rests on; leaving it off
     * bought strip-order rendering and cost the modifier volume, which is not
     * a trade worth making for a browser that has to draw masked Flash. The
     * file header has the measurement.
     *
     * The modifier bin is what a SWF mask that is not a rectangle costs, and it
     * is charged once here rather than per mask: an object pointer buffer is
     * sized per tile for the whole frame, so sixteen words across 20x15 tiles
     * is 19KB of video memory whether the page has one mask, forty, or none.
     * That is the entire storage cost of hardware masking - there is no
     * full-screen coverage plane anywhere, which is the thing ps_swf_clip.c
     * says a tile accelerator must not need. */
    pvr_init_params_t params = {
        { PVR_BINSIZE_0, PVR_BINSIZE_0, PVR_BINSIZE_16, PVR_BINSIZE_16, PVR_BINSIZE_0 },
        PS_PVR_VTX_BYTES,
        0,            /* dma_enabled */
        0,            /* fsaa_enabled */
        0,            /* autosort_disabled: off, so volumes are evaluated */
        3,            /* opb_overflow_count */
        0             /* vbuf_doublebuf_disabled */
    };

    /* The configuration that breaks modifier volumes, kept so that one command
     * demonstrates it rather than one paragraph asserting it. */
    if(g_profile == PS_PVR_CFG_STRIP)
        params.autosort_disabled = 1;

    /* Named in the log because a run whose configuration is not the one it was
     * asked for is not evidence about that configuration, and the host checks
     * this line to make sure. */
    printf("popsurf: pvr config %s: opb %d/%d/%d/%d/%d, autosort %s, "
           "overflow %d\n",
           ps_pvr_profile_name(g_profile),
           params.opb_sizes[0], params.opb_sizes[1], params.opb_sizes[2],
           params.opb_sizes[3], params.opb_sizes[4],
           params.autosort_disabled ? "off" : "on",
           params.opb_overflow_count);

    if(pvr_init(&params) < 0)
        return -1;

    memset(st->textures, 0, sizeof st->textures);
    st->width    = width;
    st->height   = height;
    st->in_frame = 0;
    return 0;
}

static void ps_pvr_shutdown(void *self)
{
    ps_pvr_state *st = (ps_pvr_state *)self;
    int i;

    for(i = 0; i < PS_MAX_TEXTURES; i++) {
        if(st->textures[i].used) {
            pvr_mem_free(st->textures[i].ptr);
            st->textures[i].used = 0;
        }
    }
    pvr_shutdown();
}

static void ps_pvr_begin_frame(void *self, ps_color clear)
{
    ps_pvr_state *st = (ps_pvr_state *)self;

    pvr_set_bg_color(PS_COLOR_R(clear) / 255.0f,
                     PS_COLOR_G(clear) / 255.0f,
                     PS_COLOR_B(clear) / 255.0f);

    pvr_wait_ready();
    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_TR_POLY);
    st->in_frame = 1;

    /* Both marks together: the TA's write pointer as this scene starts, and
     * the counter that is compared against it. */
    st->vtx_counted = 0;
    st->vtx_mark    = PVR_GET(PVR_TA_VERTBUF_POS);
}

static void ps_pvr_end_frame(void *self)
{
    ps_pvr_state *st = (ps_pvr_state *)self;

    if(!st->in_frame)
        return;

    pvr_list_finish();
    if(g_list_hook)
        g_list_hook(g_list_hook_user);
    pvr_scene_finish();
    st->in_frame = 0;
}

static ps_texture ps_pvr_upload_texture(void *self, const void *pixels,
                                        int w, int h, ps_pixel_format fmt)
{
    ps_pvr_state *st = (ps_pvr_state *)self;
    int i;

    /* Twiddled loads need power-of-two dimensions. Callers pad; a non-POT
     * request is a bug in the caller, not a runtime condition. */
    if(!is_pow2(w) || !is_pow2(h))
        return PS_TEXTURE_NONE;

    for(i = 0; i < PS_MAX_TEXTURES; i++) {
        if(!st->textures[i].used)
            break;
    }
    if(i == PS_MAX_TEXTURES)
        return PS_TEXTURE_NONE;

    pvr_ptr_t p = pvr_mem_malloc((size_t)w * h * 2);
    if(!p)
        return PS_TEXTURE_NONE;

    if(fmt == PS_FMT_ARGB4444_LINEAR)
        pvr_txr_load((void *)pixels, p, (size_t)w * h * 2);
    else
        pvr_txr_load_ex((void *)pixels, p, w, h, PVR_TXRLOAD_16BPP);

    st->textures[i].ptr  = p;
    st->textures[i].w    = w;
    st->textures[i].h    = h;
    st->textures[i].fmt  = fmt;
    st->textures[i].used = 1;

    /* Handles are 1-based so 0 stays "no texture". */
    return (ps_texture)(i + 1);
}

/* Same allocation, new pixels. See the note in ps_gfx.h: this is what stops
 * an animated applet fragmenting VRAM to death. */
static int ps_pvr_update_texture(void *self, ps_texture tex,
                                 const void *pixels, int w, int h)
{
    ps_pvr_state *st = (ps_pvr_state *)self;
    int i = (int)tex - 1;

    if(i < 0 || i >= PS_MAX_TEXTURES || !st->textures[i].used)
        return 0;

    /* Height may be short: a caller that only rewrote the rows its content
     * occupies passes those, and the rest of the allocation keeps whatever
     * was there. The sampler never reads it. Width must still match, because
     * a linear row is only contiguous at the allocated stride. */
    if(st->textures[i].w != w || h > st->textures[i].h)
        return 0;

    /* The point of a linear applet texture: this is a store-queue copy rather
     * than a per-pixel twiddle, and it runs on every animated frame. */
    if(st->textures[i].fmt == PS_FMT_ARGB4444_LINEAR)
        pvr_txr_load((void *)pixels, st->textures[i].ptr, (size_t)w * h * 2);
    else
        pvr_txr_load_ex((void *)pixels, st->textures[i].ptr, w, h,
                        PVR_TXRLOAD_16BPP);
    return 1;
}

/* ARGB8888 to ARGB4444, converted directly into the store queue.
 *
 * The staging buffer this replaces was read and written a second time on every
 * animated frame - a quarter of a megabyte through the cache twice, to move
 * data that is only looked at once. Converting into the queue itself makes it
 * one pass: read a source pixel, pack it, and it leaves for video memory
 * thirty-two bytes at a time without ever being resident.
 *
 * Only valid for a linear texture. A twiddled one has no contiguous rows to
 * stream into. */
static int ps_pvr_update_texture_argb(void *self, ps_texture tex,
                                      const uint32_t *src, int w, int h,
                                      int stride)
{
    ps_pvr_state *st = (ps_pvr_state *)self;
    int           i  = (int)tex - 1;
    int           tw, x, y, full;
    uint32_t     *sq;

    if(i < 0 || i >= PS_MAX_TEXTURES || !st->textures[i].used)
        return 0;
    if(st->textures[i].fmt != PS_FMT_ARGB4444_LINEAR)
        return 0;

    tw = st->textures[i].w;
    if(w > tw || h > st->textures[i].h || (tw & 15))
        return 0;

    /* Three regions per row, so the inner loop carries no bounds check.
     *
     * A 300 pixel applet in a 512 pixel row is 18 whole 16-pixel groups, one
     * partial group, and 13 groups of padding. Testing every pixel against the
     * width instead was 512 branches a row and 102,400 an applet per frame,
     * which on a 200MHz in-order core is most of why this was running at a
     * third of the bandwidth the store queues can do. */
    full = (w / 16) * 16;

    /* Content columns only, rounded up to a whole transfer.
     *
     * A 300 pixel applet in a 512 pixel row leaves 212 columns of padding that
     * are transparent and stay transparent - they were zeroed when the texture
     * was created and nothing ever draws there. Rewriting them every frame was
     * two fifths of the store queue traffic to reproduce a constant.
     *
     * The cost is a lock per row instead of one for the whole texture, because
     * the queue address advances with the data and skipping the padding means
     * jumping. That is 200 register writes against 20,000 stores avoided. */
    for(y = 0; y < h; y++) {
        const uint32_t *row = &src[(size_t)y * stride];

        sq = sq_lock((uint8_t *)st->textures[i].ptr +
                     (size_t)y * (size_t)tw * 2);

        for(x = 0; x < full; x += 16) {
            const uint32_t *p = &row[x];
            int             k;

            for(k = 0; k < 8; k++) {
                uint32_t v0 = p[k * 2], v1 = p[k * 2 + 1];

                sq[k] = (((v0 >> 16) & 0xf000) | ((v0 >> 12) & 0x0f00) |
                         ((v0 >>  8) & 0x00f0) | ((v0 >>  4) & 0x000f)) |
                        ((((v1 >> 16) & 0xf000) | ((v1 >> 12) & 0x0f00) |
                          ((v1 >>  8) & 0x00f0) | ((v1 >>  4) & 0x000f)) << 16);
            }
            sq_flush(sq);
            sq += 8;
        }

        /* The one group straddling the edge of the content. */
        if(x < tw) {
            int k;

            for(k = 0; k < 8; k++) {
                int      p0 = x + k * 2, p1 = p0 + 1;
                uint32_t a = 0, b = 0;

                if(p0 < w) {
                    uint32_t v = row[p0];

                    a = ((v >> 16) & 0xf000) | ((v >> 12) & 0x0f00) |
                        ((v >>  8) & 0x00f0) | ((v >>  4) & 0x000f);
                }
                if(p1 < w) {
                    uint32_t v = row[p1];

                    b = ((v >> 16) & 0xf000) | ((v >> 12) & 0x0f00) |
                        ((v >>  8) & 0x00f0) | ((v >>  4) & 0x000f);
                }
                sq[k] = a | (b << 16);
            }
            sq_flush(sq);
            sq += 8;
            x += 16;
        }

        /* Padding is left alone: it was zeroed at creation and no applet can
         * draw into it. */
        sq_unlock();
    }

    return 1;
}

static void ps_pvr_free_texture(void *self, ps_texture tex)
{
    ps_pvr_state *st = (ps_pvr_state *)self;
    int i = (int)tex - 1;

    if(i < 0 || i >= PS_MAX_TEXTURES || !st->textures[i].used)
        return;

    pvr_mem_free(st->textures[i].ptr);
    st->textures[i].used = 0;
}

/* Vertices go out through a pre-armed store queue, the way DCA3's renderer
 * does it, rather than one pvr_prim per vertex arming a fresh one each time.
 *
 * The note that used to sit here said to count quads first, and that turned
 * out to matter more than expected: at 240 quads a frame submission was 454
 * microseconds of a 10,600 microsecond page draw - four percent, not worth
 * touching - while a dense page under scroll submits 2,433 quads at 4,519
 * microseconds against 7,127 of traversal, which is thirty-nine percent. The
 * same code, the same instrumentation, a different page. */
static void ps_pvr_draw_quads(void *self, ps_texture tex,
                              const ps_vert *verts, int nverts)
{
    ps_pvr_state  *st = (ps_pvr_state *)self;
    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr;
    pvr_vertex_t   v;
    int            i, c;

    if(!st->in_frame || nverts <= 0)
        return;

    if(tex == PS_TEXTURE_NONE) {
        pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
    }

    else {
        int i_tex = (int)tex - 1;
        ps_pvr_texture *t;

        if(i_tex < 0 || i_tex >= PS_MAX_TEXTURES || !st->textures[i_tex].used)
            return;
        t = &st->textures[i_tex];

        pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY,
                         (t->fmt == PS_FMT_RGB565 ? PVR_TXRFMT_RGB565
                                                  : PVR_TXRFMT_ARGB4444)
                             | (t->fmt == PS_FMT_ARGB4444_LINEAR
                                    ? PVR_TXRFMT_NONTWIDDLED
                                    : PVR_TXRFMT_TWIDDLED),
                         t->w, t->h, t->ptr, PVR_FILTER_NONE);

        /* Glyph atlases are white coverage tinted by the vertex color, so the
         * texture must modulate both color and alpha. */
        cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
    }

    /* Gouraud, so a quad given different top and bottom vertex colours is
     * interpolated per pixel. That is how the UI gets metallic shading
     * without a texture and without ARGB4444 banding. */
    cxt.gen.shading = PVR_SHADE_GOURAUD;

    pvr_poly_compile(&hdr, &cxt);

    /* Straight into a pre-armed store queue rather than pvr_prim per vertex,
     * which arms one each time.
     *
     * Worth the measurement that justified it. At 240 quads a frame - an
     * ordinary page - submission was 454 microseconds against 10,157 spent
     * deciding what to draw, so this would have bought four percent and the
     * retained list was the answer. On a dense page being scrolled it is 4,519
     * against 7,127, and then it is thirty-nine percent. Both numbers came
     * from the same instrumentation; only the page changed.
     *
     * A header and a vertex are both thirty-two bytes, which is exactly one
     * store queue transfer, so both go the same way. This is what DCA3's
     * renderer does throughout. */
    {
        pvr_poly_hdr_t *dh = (pvr_poly_hdr_t *)pvr_dr_target(st->dr);

        *dh = hdr;
        pvr_dr_commit(dh);
    }

    /* Charged whole here rather than per vertex: the loop below writes exactly
     * this much and a counter inside it would be an add per store queue
     * transfer to reach the same total. */
    ps_pvr_vtx_charge(32u + (size_t)nverts * 32u);

    (void)v;

    for(i = 0; i < nverts; i++) {
        pvr_vertex_t *dv = (pvr_vertex_t *)pvr_dr_target(st->dr);

        c = i % PS_VERTS_PER_QUAD;

        dv->flags = (c == PS_VERTS_PER_QUAD - 1) ? PVR_CMD_VERTEX_EOL
                                                 : PVR_CMD_VERTEX;
        dv->x    = verts[i].x;
        dv->y    = verts[i].y;
        dv->z    = verts[i].z;
        dv->u    = verts[i].u;
        dv->v    = verts[i].v;
        dv->argb = verts[i].argb;
        dv->oargb = 0;

        pvr_dr_commit(dv);
    }
}

static const ps_gfx_backend g_pvr_backend = {
    ps_pvr_init,
    ps_pvr_shutdown,
    ps_pvr_begin_frame,
    ps_pvr_end_frame,
    ps_pvr_upload_texture,
    ps_pvr_update_texture,
    ps_pvr_update_texture_argb,
    ps_pvr_free_texture,
    ps_pvr_draw_quads,
    &g_pvr,
    640, 480
};

const ps_gfx_backend *ps_gfx_pvr(void)
{
    return &g_pvr_backend;
}
