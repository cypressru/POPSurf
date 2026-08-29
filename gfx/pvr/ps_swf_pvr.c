/* Triangles to the tile accelerator.
 *
 * The five decisions worth stating, because the code reads as obvious once
 * they are made and arbitrary until then.
 *
 * Culling is off. pvr_poly_cxt_col leaves it at CCW, and a SWF's winding is
 * not a fact about the artwork: the tessellator winds a hole the opposite way
 * from the contour around it, and a placement matrix with a negative
 * determinant - a mirrored sprite, which is how half of Flash's animation is
 * authored - flips every triangle in the character. Culling anything here
 * would delete whichever half of the picture the file happened to wind the
 * wrong way, and the failure would look like a tessellation bug.
 *
 * A pass takes one depth step. The translucent list compares with GREATER and
 * writes depth, so two passes submitted at the same z lose the second - see
 * ps_gfx.h, which pays for the same fact one quad at a time. Within a pass the
 * tessellator guarantees the triangles do not overlap, so they can share one,
 * and that is the granularity: passes climb, triangles do not.
 *
 * Trapezoids arrive as two triangles and leave as one four-vertex strip. That
 * is not an optimisation looking for a problem. The tessellator's cost model is
 * "roughly two triangles per band per interior interval", so a shape of any
 * size is mostly trapezoids, and the arithmetic decides whether the thing runs
 * at all: at three vertices a triangle, thirty-two bytes a vertex, an eleven
 * thousand triangle shape is 1.08MB of vertex data into a TA buffer that
 * pvr_init sized at 512KB. As strips the same shape is 720KB. Neither fits
 * today, which is a finding rather than a fix, but the strip is free and the
 * pairing is recognised from the geometry rather than assumed from the
 * tessellator - two consecutive triangles (a,b,c) and (a,c,d) are a
 * quadrilateral a,b,c,d whatever produced them, and a,b,d,c is that
 * quadrilateral as a strip.
 *
 * A shape too big for the vertex buffer is refused rather than allowed to
 * overrun it, and refused against what the page has already spent rather than
 * against a share fixed in advance. The three candidate answers were a bigger
 * buffer, growth on demand, and a budget; the first two are not available and
 * the third is. The buffer is laid out at pvr_init below texture memory, so
 * enlarging it takes VRAM from every page to serve the movies that need it -
 * and the measured page already spends 380KB of the 512KB, so the increment
 * would have to be large. Growth on demand means pvr_shutdown and pvr_init
 * mid-page, which frees every texture on it. What is left is to know how much
 * room remains and stop, which is the one answer that never produces a torn
 * frame: the TA does not fail an overrun cleanly, it stops accepting a scene
 * that is half submitted. So the movie takes what the page left minus a
 * reserve for the chrome drawn after it, stops when that runs out, and says
 * so - a movie missing its topmost characters, reported, rather than a page
 * that tears.
 *
 * A mask that is not a rectangle is a modifier volume. ps_swf_clip.c argues
 * the mechanism; what is settled here is the shape of it. The volume list is
 * a list of its own and a list may be submitted only once per scene, so the
 * mask's triangles cannot be written where they are discovered - they are held
 * in RAM and submitted in one run from ps_pvr_set_list_hook, after the
 * translucent list is closed and before the scene is finished. That is also
 * what bounds the feature: there is one volume flag per pixel per scene, every
 * volume in the list contributes to the same flag, and a polygon carrying two
 * parameter sets picks between them by reading it. One flag expresses one
 * mask, so the first mask in a frame that needs a volume gets it and any
 * further one falls back to its bounding box. A rectangular mask needs no
 * volume at all and does not consume it, and a rectangle nested inside a
 * volume mask still works, because the two mechanisms are independent: the
 * rectangle clips vertices on the CPU and the volume marks pixels on the chip.
 *
 * All of which is now measured rather than argued: a mask with a hole in it
 * comes out exact on the console, zero differing interior pixels. It took the
 * per-pixel sort being turned on in pvr_init to get there - a modifier volume
 * submitted while the hardware is rendering translucent polygons in strip
 * order corrupts the scene in bands locked to the tile grid, whether or not
 * anything reads the volume - and ps_swf_clip.c has the four captures that
 * cornered that. The remaining approximations are named where they are made:
 * a second general mask in a frame, and a textured fill under one.
 *
 * Two counts of differing pixels that survived all of this were not rendering
 * faults and are worth recording as such, because the shape of them will come
 * back. t_hole differed on fifteen pixels, all on row 98, every fourth column;
 * t_stroke on thirty-nine, all on row 178, every second column. Single pixels,
 * one scanline, regular stride, and in t_hole's case inside the hole - where
 * this file submits no geometry at all.
 *
 * It was the display. Five bits of red and blue step by eight, so a page
 * painted 0x242424 is a colour the framebuffer cannot hold, and the hardware
 * alternates 0x20 and 0x28 between neighbours to average out to it. Dumping
 * the console's own pixels along one row showed the alternation running
 * through open background far from any edge, which is what settled it: the
 * comparison's one-step tolerance covers most of the pattern and a minority of
 * columns fall outside, and which minority depends on the colour, which is why
 * one page struck every fourth column and the other every second.
 *
 * Nothing here changed. An unattended capture now turns dithering off - see
 * ps_gfx_pvr.h - because a measurement against a renderer that stores colours
 * exactly should not be arguing with the display's own approximation. The
 * lesson worth keeping is that "one scanline at a regular stride" describes a
 * sampling pattern rather than a shape, and the first thing to do with one is
 * read the pixels rather than reason about the geometry.
 *
 * t_hole went to zero when the dithering did. t_stroke did not, and what was
 * left is a real difference that the pattern had been hiding: the bottom two
 * rows of the stroked square's lower bar come out blended with the page
 * behind, about seven percent of it on one row and thirty-five on the next,
 * where the software renderer holds the stroke solid to its last row. Both
 * renderers agree everywhere else on that shape.
 *
 * Which was stranger than it sounded, because neither side should be producing
 * a partial pixel there. The stroke is twenty pixels wide about a path at
 * y=190, so its edges land on y=180 and y=200 exactly - the reference has no
 * antialiasing to do and does none, and the tile accelerator has none to
 * offer: FSAA is off, every vertex of the pass carries the same colour, and
 * that colour's alpha is 255. An opaque polygon cannot blend. The tessellation
 * was never in question either - the same sixty-six triangles measure zero
 * interior and zero boundary against the sweep on the host.
 *
 * It was the display path again, and the second time it wore a better
 * disguise. KOS renders the scene to the framebuffer through a vertical scale
 * of 0.999 on any cable that is not VGA - one output row per 1024/1025
 * rendered rows, so every row is a blend of two - and its own header says what
 * for: "having a value slightly below 1.0f gives the image a pleasant
 * smoothing". Walking down through the edge rather than along it is what named
 * it: the ramp is symmetric, appears at the top of a bar as well as the
 * bottom, sits at a different phase on a shape at a different height, and
 * vertical edges are hard to the pixel. Nothing that renders geometry does
 * that; a vertical resample does exactly that.
 *
 * Every other page hid it because a one-row ramp lands inside the comparison's
 * boundary band. A twenty-pixel stroke has edges sharp enough that the middle
 * row of a three-row ramp falls in the interior instead, which is why the only
 * page with strokes on it was the only page that ever showed this.
 *
 * Turned off for an unattended capture beside the dithering, for the same
 * reason and in the same shape - see ps_gfx_pvr.h. Both are the display making
 * a picture pleasant, both are invisible to the eye doing it, and neither
 * belongs in a measurement.
 */
#include "ps_swf_pvr.h"
#include "ps_gfx_pvr.h"
/* The ramp and the map into it are the player's, not this file's: they are the
 * half of a hardware gradient that can be checked on a host, and
 * tests/swf-host/ramptest.c checks exactly these two functions against the
 * span renderer. Anything computed here instead would be untested by
 * construction. */
#include "../../swf/ps_swf_ramp.h"

#include <arch/timer.h>
#include <dc/pvr.h>
#include <stdlib.h>
#include <string.h>

/* Curve flattening tolerance, in screen pixels.
 *
 * This is the one knob trading picture against cost in the whole player, and
 * ps_swf.h is explicit that the right value on a 200MHz SH-4 is not the right
 * value on a workstation and wants measuring on hardware. Half a pixel is the
 * starting point, not an answer: halving it roughly doubles the chord count,
 * and the trapezoid count grows faster than that because chords add band
 * boundaries as well as edges. */
#define PS_SWF_PVR_TOL 0.5f

/* Textures one movie may hold, and what they may cost.
 *
 * VRAM is eight megabytes shared with every page texture, and a movie arrives
 * on a page that has already spent some. The budget is what a movie may take
 * before its remaining fills fall back to their own mean colour, which is the
 * use ps_swf_fill's `color` field was put there for. */
#define PS_SWF_PVR_MAX_TEX     32
#define PS_SWF_PVR_VRAM_BUDGET (512 * 1024)

/* What the chrome drawn after the movie needs of the vertex buffer.
 *
 * The toolbar, the menu, the on-screen keyboard and the cursor are all quads
 * through ps_paint, and the whole of them is a few hundred: 64KB is two
 * thousand vertices, which is more than any of those has ever submitted. The
 * movie may have everything else that is left. Reserved rather than shared
 * because the toolbar disappearing is a browser that looks broken, while a
 * movie missing its top layer is a movie that looks wrong - and the browser
 * has to survive the page. */
#define PS_SWF_PVR_VTX_TAIL (64 * 1024)

/* What a movie is given even when the page appears to have spent everything.
 *
 * The first version of this had no floor, and on the console it computed a
 * budget of zero on a page that had drawn a few hundred quads: the movie drew
 * nothing at all, and a testbed that draws nothing looks exactly like a
 * renderer that clips everything away. That is the wrong failure. An
 * overspent buffer tears one frame and is loud; a movie deleted by its own
 * accounting is silent and reads as every other bug at once.
 *
 * 128KB with the 64KB tail leaves 320KB for a page, which is more than any
 * page carrying a movie has ever wanted - the 380KB measurement is a dense
 * page of text with no movie on it at all. */
#define PS_SWF_PVR_VTX_FLOOR (128 * 1024)

/* Triangles one modifier volume may hold.
 *
 * A mask is a window - a rectangle, a rounded panel, a title cut out of a
 * shape - and 512 trapezoid halves is a far more complicated window than
 * Flash authoring produces for one. Past it the mask falls back to its
 * bounding box, which is what it would have done with no volume support at
 * all, so the ceiling costs picture rather than correctness. 14KB resident,
 * once, in a structure the shell holds for the life of the browser. */
#define PS_SWF_PVR_VOL_TRIS 512

typedef struct {
    const void *key;        /* the gradient or bitmap it was built from */
    int         radial;     /* one gradient can be used both ways */
    int         tiled;
    pvr_ptr_t   ptr;
    int         w, h;       /* texels, power of two */
    uint32_t    fmt;
    /* Source units per texture edge. A gradient's source is the 32768-twip
     * square; a bitmap's is its own texels, and the two differ because a
     * bitmap is padded to a power of two rather than resampled to one. */
    float       su, sv;
    size_t      bytes;
} swf_tex;

struct ps_swf_pvr {
    ps_swf_view    view;
    pvr_dr_state_t dr;

    const ps_swf_movie *movie;
    swf_tex             tex[PS_SWF_PVR_MAX_TEX];
    int                 ntex;
    size_t              vram;

    /* --- the open scope ------------------------------------------------- */
    int   open;
    float z;                    /* the depth the open pass is submitting at */
    float zbase, zstep;
    int   zi, zsteps;

    ps_rect box;                /* what the movie may paint in */
    float   cx0, cy0, cx1, cy1; /* that, intersected with every live mask */

    /* --- the vertex buffer ------------------------------------------------ */
    size_t vtx_used;            /* bytes this scope has put into the TA */
    size_t vtx_page;            /* what the page had spent before it started */
    size_t vtx_budget;          /* what the page left it, less the chrome's */
    int    vtx_full;            /* it ran out, and stopped submitting */

    /* Which of the mask paths to take. Selectable because the hardware
     * behaviour of a modifier volume is not settled, and four captures of one
     * page with the same geometry are what settles it - see ps_swf_pvr.h. */
    int    mask_mode;

    /* --- the open pass --------------------------------------------------- */
    int      hdr_pending;
    pvr_poly_hdr_t hdr;
    uint32_t argb;
    int      pass_mod;          /* vertices carry two colours, not a texture */
    float    ua, ub, uc, va, vb, vc;

    /* One triangle held back, waiting to see whether the next one completes a
     * trapezoid with it. */
    int   held;
    float hx[3], hy[3];

    /* --- masks ------------------------------------------------------------ */
    struct {
        float  x0, y0, x1, y1;    /* the mask's bounding box, screen pixels */
        double area;              /* what its triangles actually cover */
        float  sx0, sy0, sx1, sy1;/* the clip in force when it was pushed */
        int    applied;
    } mask[PS_SWF_CLIP_DEPTH];
    int nmask;
    int mask_inexact;             /* masks left approximated by their box */

    /* The one modifier volume a scene has room to express, held until the
     * translucent list is closed. `last` closes a volume group: the
     * tessellator promises the triangles of one pass do not overlap, and a
     * volume decides its interior by parity, so each pass has to be its own
     * group or two overlapping passes of one mask would cancel where they
     * meet and leave a hole nothing in the file asked for. */
    struct {
        float   x[3], y[3];
        uint8_t last;
    } vol[PS_SWF_PVR_VOL_TRIS];
    int nvol;
    int vol_open;                 /* the mask being collected may claim it */
    int vol_live;                 /* content is being marked against it */
    int vol_owner;                /* which mask level took it */
    int vol_spent;                /* the scene's one volume is claimed */
    int vol_txr;                  /* textured passes it could not mark */

    /* --- what a frame cost, and where -------------------------------------
     *
     * Split this way because the two candidate explanations for a slow frame
     * predict different columns, and a total predicts neither. A cost that
     * scales with `passes` is the state change - a polygon context built and
     * compiled, and a header down the queue. A cost that scales with `draws`
     * is ps_swf_tess_shape's own setup, which flattens every edge of a
     * character and takes six allocations to do it, once per character per
     * frame whatever comes out. A cost that scales with `tris` is the work
     * itself. Those are three different fixes and the numbers tell them apart.
     *
     * Texture uploads are deliberately absent, and their absence is the
     * finding: pvr_txr_load_ex is reachable only from ps_swf_pvr_bind, which
     * runs once when a movie loads. Nothing uploads during a frame, so a pass
     * carrying a texture differs from a flat one by pvr_poly_cxt_txr and a
     * wider compile - CPU struct work, counted in us_state like any other. */
    long     tris;          /* emitted by the tessellator */
    long     mask_tris;     /* of those, measured for a mask and not submitted */
    long     vol_tris;      /* of those, submitted again as a volume */
    long     flat_tris;     /* dropped: zero area, so no pixel of any of it */
    long     strips;        /* primitives that reached the TA */
    long     passes;
    long     draws;         /* characters tessellated */
    uint64_t us_state;      /* inside tri_begin */
    uint64_t us_draw;       /* inside ps_swf_tess_shape, state included */
};

/* --- textures ------------------------------------------------------------ */

static int pot_up(int v)
{
    int p = 8;                  /* the PVR's smallest twiddled dimension */

    while(p < v && p < 1024)
        p <<= 1;
    return p;
}

static uint16_t pack4444(ps_swf_rgba c)
{
    return (uint16_t)(((c.a >> 4) << 12) | ((c.r >> 4) << 8) |
                      ((c.g >> 4) << 4) | (c.b >> 4));
}

static swf_tex *tex_find(ps_swf_pvr *p, const void *key, int radial, int tiled)
{
    int i;

    for(i = 0; i < p->ntex; i++)
        if(p->tex[i].key == key && p->tex[i].radial == radial &&
           p->tex[i].tiled == tiled)
            return &p->tex[i];
    return NULL;
}

static swf_tex *tex_new(ps_swf_pvr *p, const uint16_t *px, int w, int h,
                        uint32_t fmt)
{
    size_t    bytes = (size_t)w * (size_t)h * 2u;
    swf_tex  *t;
    pvr_ptr_t mem;

    if(p->ntex >= PS_SWF_PVR_MAX_TEX || p->vram + bytes > PS_SWF_PVR_VRAM_BUDGET)
        return NULL;
    mem = pvr_mem_malloc(bytes);
    if(!mem)
        return NULL;

    pvr_txr_load_ex((void *)px, mem, w, h, PVR_TXRLOAD_16BPP);

    t = &p->tex[p->ntex++];
    memset(t, 0, sizeof *t);
    t->ptr   = mem;
    t->w     = w;
    t->h     = h;
    t->fmt   = fmt;
    t->bytes = bytes;
    p->vram += bytes;
    return t;
}

/* Built by ps_swf_ramp_build and uploaded. Nothing about the ramp's contents
 * is decided here, which is what lets a host test assert them. */
static swf_tex *grad_texture(ps_swf_pvr *p, const ps_swf_gradient *g,
                             int radial)
{
    ps_swf_ramp ramp;
    swf_tex    *t;

    if(ps_swf_ramp_build(&ramp, g, radial) < 0)
        return NULL;

    t = tex_new(p, ramp.px, ramp.w, ramp.h,
                ramp.opaque ? PVR_TXRFMT_RGB565 : PVR_TXRFMT_ARGB4444);
    ps_swf_ramp_free(&ramp);
    if(t) {
        t->key    = g;
        t->radial = radial;
    }
    return t;
}

/* A bitmap fill is either clipped or tiled and the two want different
 * textures, which is why the mode is part of the key.
 *
 * Clipped means nothing outside the bitmap, so it is padded into the next
 * power of two with transparent texels and sampled with clamping: the clamp
 * repeats the padding, which is transparent, so the fill stops at the edge as
 * the format says it must. Tiled means the bitmap repeats seamlessly, and
 * padding would put a transparent gutter between the tiles - so that one is
 * resampled to the power of two instead and the stretch is folded into the
 * texture coordinate, where it costs nothing. */
static swf_tex *bitmap_texture(ps_swf_pvr *p, const ps_swf_bitmap *b,
                               int tiled)
{
    int       w = pot_up(b->w), h = pot_up(b->h);
    uint16_t *px;
    swf_tex  *t;
    int       x, y;

    px = malloc((size_t)w * (size_t)h * 2u);
    if(!px)
        return NULL;

    if(tiled) {
        for(y = 0; y < h; y++) {
            int sy = (int)((long)y * b->h / h);

            for(x = 0; x < w; x++) {
                int sx = (int)((long)x * b->w / w);

                px[y * w + x] = pack4444(b->px[(size_t)sy * b->w + sx]);
            }
        }
    }
    else {
        memset(px, 0, (size_t)w * (size_t)h * 2u);
        for(y = 0; y < b->h; y++)
            for(x = 0; x < b->w; x++)
                px[y * w + x] = pack4444(b->px[(size_t)y * b->w + x]);
    }

    t = tex_new(p, px, w, h, PVR_TXRFMT_ARGB4444);
    free(px);
    if(t) {
        t->key   = b;
        t->tiled = tiled;
        /* Texels per texture edge: the padded form addresses the whole
         * allocation, the resampled form addresses the original extent. */
        t->su = tiled ? (float)b->w : (float)w;
        t->sv = tiled ? (float)b->h : (float)h;
    }
    return t;
}

/* --- lifetime ------------------------------------------------------------ */

static void flush_mods(void *user);

ps_swf_pvr *ps_swf_pvr_create(void)
{
    ps_swf_pvr *p = calloc(1, sizeof *p);

    if(!p)
        return NULL;
    p->view.tol     = PS_SWF_PVR_TOL;
    p->view.samples = 1;
    p->mask_mode    = PS_SWF_MASK_VOL;
    /* Registered for the life of the object rather than per frame: the hook
     * runs on every frame the page draws, movie or not, and a frame with no
     * volume to submit costs it one test. */
    ps_pvr_set_list_hook(flush_mods, p);
    return p;
}

void ps_swf_pvr_set_mask_mode(ps_swf_pvr *p, int mode)
{
    if(mode >= 0 && mode < PS_SWF_MASK_MODES)
        p->mask_mode = mode;
}

void ps_swf_pvr_destroy(ps_swf_pvr *p)
{
    if(!p)
        return;
    ps_pvr_set_list_hook(NULL, NULL);
    ps_swf_pvr_unbind(p);
    free(p);
}

void ps_swf_pvr_unbind(ps_swf_pvr *p)
{
    int i;

    for(i = 0; i < p->ntex; i++)
        pvr_mem_free(p->tex[i].ptr);
    p->ntex  = 0;
    p->vram  = 0;
    p->movie = NULL;
}

/* Only the movie's own shapes and glyphs are walked. A morph produces a fresh
 * shape with a fresh gradient every frame it is blended, so its gradients have
 * no stable identity to key a texture on and no lifetime worth uploading
 * against; a morphed gradient therefore draws as the mean of its stops, which
 * is what ps_swf_fill's `color` exists to be. Its bitmap fills are unaffected,
 * because a morph does not interpolate those - see ps_swf_morph.c. */
int ps_swf_pvr_bind(ps_swf_pvr *p, const ps_swf_movie *m)
{
    uint32_t i, j;

    ps_swf_pvr_unbind(p);
    if(!m)
        return -1;
    p->movie = m;

    for(i = 0; i < m->nshape; i++) {
        const ps_swf_shape *sh = &m->shapes[i];

        for(j = 0; j < sh->nfill; j++) {
            const ps_swf_fill *f = &sh->fills[j];

            if(f->grad && f->grad <= sh->ngrad) {
                int radial = (f->type == PS_SWF_FILL_RADIAL);

                if(!tex_find(p, &sh->grads[f->grad - 1], radial, 0))
                    (void)grad_texture(p, &sh->grads[f->grad - 1], radial);
            }
            else if(f->bfill && f->bfill <= sh->nbfill) {
                const ps_swf_bitmap *b = sh->bfills[f->bfill - 1].bmp;
                int                  tiled = (f->type & 0x01) == 0;

                if(b && !tex_find(p, b, 0, tiled))
                    (void)bitmap_texture(p, b, tiled);
            }
        }
    }

    /* Glyph outlines carry a synthetic solid fill and never a gradient, so
     * fonts are skipped deliberately rather than forgotten. */
    return 0;
}

/* --- the scope ------------------------------------------------------------ */

void ps_swf_pvr_begin(ps_swf_pvr *p, const ps_rect *clip, float z, float zstep,
                      int steps)
{
    p->open    = 1;
    p->box     = *clip;
    p->cx0     = (float)clip->x0;
    p->cy0     = (float)clip->y0;
    p->cx1     = (float)clip->x1;
    p->cy1     = (float)clip->y1;
    p->z       = z;
    p->zbase   = z;
    p->zstep   = zstep;
    p->zi      = 0;
    p->zsteps  = steps > 0 ? steps : 1;
    p->nmask   = 0;
    p->held    = 0;
    p->hdr_pending = 0;
    p->tris      = 0;
    p->mask_tris = 0;
    p->vol_tris  = 0;
    p->flat_tris = 0;
    p->strips    = 0;
    p->passes    = 0;
    p->draws     = 0;
    p->us_state  = 0;
    p->us_draw   = 0;
    p->mask_inexact = 0;
    p->pass_mod  = 0;

    /* The volume is submitted after this scope closes, so its triangles are
     * dropped here, at the start of the frame that will refill them, rather
     * than at the end of the one that used them. */
    p->nvol      = 0;
    p->vol_open  = 0;
    p->vol_live  = 0;
    p->vol_spent = 0;
    p->vol_owner = -1;
    p->vol_txr   = 0;

    /* What the page has not already spent, less what the chrome will, and
     * never less than the floor. Read once per scope rather than per strip:
     * the page's own submission is finished by the time a movie draws -
     * ps_paint is flushed immediately before - so the figure cannot move under
     * this. */
    {
        size_t cap  = ps_pvr_vtx_capacity();
        size_t used = ps_pvr_vtx_used() + PS_SWF_PVR_VTX_TAIL;
        size_t left = cap > used ? cap - used : 0;

        p->vtx_page   = ps_pvr_vtx_used();
        p->vtx_used   = 0;
        p->vtx_full   = 0;
        p->vtx_budget = left > PS_SWF_PVR_VTX_FLOOR ? left
                                                    : PS_SWF_PVR_VTX_FLOOR;
    }

    /* The view is the screen and not the movie's box, because the tessellator
     * works in the same pixel space the vertices are submitted in - the fit is
     * already folded into the root transform. Only the tolerance and the
     * dimensions are read, and the dimensions only bound a scratch buffer the
     * triangle path does not use. */
    p->view.w = clip->x1 - clip->x0;
    p->view.h = clip->y1 - clip->y0;
    if(p->view.w < 1) p->view.w = 1;
    if(p->view.h < 1) p->view.h = 1;
}

void ps_swf_pvr_end(ps_swf_pvr *p)
{
    p->open = 0;
}

long   ps_swf_pvr_tris(const ps_swf_pvr *p)      { return p->tris; }
long   ps_swf_pvr_mask_tris(const ps_swf_pvr *p) { return p->mask_tris; }
long   ps_swf_pvr_vol_tris(const ps_swf_pvr *p)  { return p->vol_tris; }
long   ps_swf_pvr_flat_tris(const ps_swf_pvr *p) { return p->flat_tris; }
long   ps_swf_pvr_strips(const ps_swf_pvr *p)    { return p->strips; }
long   ps_swf_pvr_passes(const ps_swf_pvr *p)    { return p->passes; }
long   ps_swf_pvr_draws(const ps_swf_pvr *p)     { return p->draws; }
size_t ps_swf_pvr_vram(const ps_swf_pvr *p)      { return p->vram; }
size_t ps_swf_pvr_vtx(const ps_swf_pvr *p)       { return p->vtx_used; }
size_t ps_swf_pvr_vtx_budget(const ps_swf_pvr *p) { return p->vtx_budget; }
size_t ps_swf_pvr_vtx_page(const ps_swf_pvr *p)  { return p->vtx_page; }
int    ps_swf_pvr_mask_mode(const ps_swf_pvr *p) { return p->mask_mode; }
int    ps_swf_pvr_vtx_full(const ps_swf_pvr *p)  { return p->vtx_full; }
int    ps_swf_pvr_mask_inexact(const ps_swf_pvr *p) { return p->mask_inexact; }
int    ps_swf_pvr_mask_untextured(const ps_swf_pvr *p) { return p->vol_txr; }

uint64_t ps_swf_pvr_us_state(const ps_swf_pvr *p) { return p->us_state; }
uint64_t ps_swf_pvr_us_draw(const ps_swf_pvr *p)  { return p->us_draw; }

/* --- submission ----------------------------------------------------------- */

static int collecting(const ps_swf_pvr *p)
{
    return p->nmask > 0 && !p->mask[p->nmask - 1].applied;
}

static void put_vertex(ps_swf_pvr *p, float x, float y, uint32_t flags)
{
    if(p->hdr_pending) {
        pvr_poly_hdr_t *dh = (pvr_poly_hdr_t *)pvr_dr_target(p->dr);

        *dh = p->hdr;
        pvr_dr_commit(dh);
        p->hdr_pending = 0;
    }

    if(p->pass_mod) {
        /* Two colours, and the volume flag picks between them per pixel: the
         * fill inside the mask, and an alpha of zero outside it. The polygon
         * still rasterises everywhere it covers - it is the blend that makes
         * the outside contribute nothing - which is why masked content has to
         * be in the translucent list and could not be in the opaque one.
         *
         * argb0 outside and argb1 inside is what dc/pvr.h says and what the
         * console does: the mask comes out the right way round, which is the
         * one thing about the flag that a picture can confirm outright. */
        pvr_vertex_pcm_t *v = (pvr_vertex_pcm_t *)pvr_dr_target(p->dr);

        v->flags = flags;
        v->x     = x;
        v->y     = y;
        v->z     = p->z;
        v->argb0 = 0;
        v->argb1 = p->argb;
        v->d1    = 0;
        v->d2    = 0;
        pvr_dr_commit(v);
        return;
    }

    {
        pvr_vertex_t *v = (pvr_vertex_t *)pvr_dr_target(p->dr);

        v->flags = flags;
        v->x     = x;
        v->y     = y;
        v->z     = p->z;
        v->u     = p->ua * x + p->ub * y + p->uc;
        v->v     = p->va * x + p->vb * y + p->vc;
        v->argb  = p->argb;
        v->oargb = 0;
        pvr_dr_commit(v);
    }
}

/* Every primitive this file submits passes through here, which is what makes
 * the budget a single test rather than a rule each caller has to remember.
 *
 * Nothing is submitted once the budget is gone, not even a primitive small
 * enough to fit in what is left. Letting the small ones through would fill the
 * remainder with whichever triangles happened to be small, which is a picture
 * assembled out of the wrong half of the display list; stopping keeps it a
 * prefix of the painter's order, which is the movie with its topmost layers
 * missing and is at least explicable. */
static void emit_strip(ps_swf_pvr *p, const float *xs, const float *ys, int n)
{
    size_t need = (size_t)n * 32u + (p->hdr_pending ? 32u : 0u);
    int    i;

    if(p->vtx_full)
        return;
    if(p->vtx_used + need > p->vtx_budget) {
        p->vtx_full = 1;
        return;
    }
    p->vtx_used += need;
    ps_pvr_vtx_charge(need);

    for(i = 0; i < n; i++)
        put_vertex(p, xs[i], ys[i],
                   i == n - 1 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX);
    p->strips++;
}

/* Sutherland-Hodgman against the four edges of the live clip rectangle.
 *
 * Exact, and the same arithmetic core/ps_paint.c applies to a quad's corners,
 * for the reason ps_gfx.h gives: the PVR's own user clip is stated in 32-pixel
 * tiles, so it can reject coarsely but cannot express the edge of an element's
 * box. Reached only when a triangle actually straddles the boundary, which
 * inside a movie fitted to its own stage is nearly never. */
static int clip_poly(const ps_swf_pvr *p, float *xs, float *ys, int n)
{
    float bx[10], by[10];
    int   e;

    for(e = 0; e < 4; e++) {
        int   m = 0, i;
        float lim = e == 0 ? p->cx0 : e == 1 ? p->cx1 :
                    e == 2 ? p->cy0 : p->cy1;

        for(i = 0; i < n; i++) {
            int   j  = (i + 1) % n;
            float ai = e < 2 ? xs[i] : ys[i];
            float aj = e < 2 ? xs[j] : ys[j];
            int   in_i = (e == 0 || e == 2) ? ai >= lim : ai <= lim;
            int   in_j = (e == 0 || e == 2) ? aj >= lim : aj <= lim;

            if(in_i && m < 10) {
                bx[m]   = xs[i];
                by[m++] = ys[i];
            }
            if(in_i != in_j && m < 10 && aj != ai) {
                float t = (lim - ai) / (aj - ai);

                bx[m]   = xs[i] + (xs[j] - xs[i]) * t;
                by[m++] = ys[i] + (ys[j] - ys[i]) * t;
            }
        }
        n = m;
        for(i = 0; i < n; i++) {
            xs[i] = bx[i];
            ys[i] = by[i];
        }
        if(n < 3)
            return 0;
    }
    return n;
}

/* One mask triangle into the volume, clipped to whatever confines the mask
 * itself.
 *
 * That clip is what makes a general mask nested inside a rectangular one exact
 * rather than approximate - route 2 in ps_swf_clip.c's list, and it costs
 * nothing extra here because the rectangle is already in force and the
 * Sutherland-Hodgman is already written. A clipped triangle can come back as
 * up to a heptagon, which fans into five, so the room left is checked against
 * the worst case rather than against one.
 *
 * Overflow abandons the whole volume rather than truncating it: half a mask
 * marks the wrong pixels and looks like a rendering fault, where the bounding
 * box it falls back to is merely the approximation this file made before
 * volumes existed. */
static void keep_vol_tri(ps_swf_pvr *p, const float *xs, const float *ys)
{
    float cx[10], cy[10];
    int   n, i;

    /* A pinch marks nothing, and a volume triangle is sixty-four bytes and one
     * of five hundred and twelve slots. Same test and same reason as
     * submit_tri's. */
    if((xs[1] - xs[0]) * (ys[2] - ys[0]) ==
       (xs[2] - xs[0]) * (ys[1] - ys[0]))
        return;

    if(p->nvol + 8 > PS_SWF_PVR_VOL_TRIS) {
        p->nvol     = 0;
        p->vol_open = 0;
        return;
    }

    memcpy(cx, xs, 3 * sizeof *cx);
    memcpy(cy, ys, 3 * sizeof *cy);
    n = clip_poly(p, cx, cy, 3);

    for(i = 2; i < n; i++) {
        p->vol[p->nvol].x[0] = cx[0];
        p->vol[p->nvol].y[0] = cy[0];
        p->vol[p->nvol].x[1] = cx[i - 1];
        p->vol[p->nvol].y[1] = cy[i - 1];
        p->vol[p->nvol].x[2] = cx[i];
        p->vol[p->nvol].y[2] = cy[i];
        p->vol[p->nvol].last = 0;
        p->nvol++;
    }
}

/* Room for the volume, taken now rather than found later.
 *
 * The volume is submitted after everything else and it is the one primitive
 * whose absence is not a smaller picture but a wrong one: masked polygons with
 * no volume to read are marked nowhere, so every one of them draws its outside
 * colour, which is nothing at all. Charging it here means a mask that will not
 * fit falls back to its bounding box, which is merely approximate, instead of
 * deleting the content it was masking.
 *
 * The group still being collected has no `last` yet, so it is counted here
 * rather than found in the flags. */
static int vol_afford(ps_swf_pvr *p)
{
    size_t need;
    int    i, groups = 1;

    for(i = 0; i < p->nvol; i++)
        if(p->vol[i].last)
            groups++;

    need = (size_t)p->nvol * sizeof(pvr_modifier_vol_t) +
           (size_t)groups * 2u * sizeof(pvr_mod_hdr_t);
    if(p->vtx_used + need > p->vtx_budget)
        return 0;

    p->vtx_used += need;
    ps_pvr_vtx_charge(need);
    return 1;
}

/* The held volume, into the modifier list, once the polygons that read it are
 * already on their way.
 *
 * The z is one step past every depth the movie reserved, which is in front of
 * all of it: a volume's interior is decided by counting the surfaces of it
 * that lie in front of the pixel being shaded, and a mask that is one flat
 * sheet marks what it covers only if that sheet is in front. Behind the
 * content it would count zero surfaces and mark nothing. The volume writes no
 * depth of its own, so being in front of the chrome as well costs nothing. */
/* A modifier volume header with nothing undefined in it.
 *
 * pvr_mod_compile writes two of the eight words and gets its storage from
 * dcache_alloc_line_with_value, which claims a cache line without reading what
 * was at that address - so the other six words of the thirty-two byte global
 * parameter handed to the tile accelerator are whatever the stack happened to
 * hold. The specification calls them ignored and they may well be, but a
 * hardware descriptor built partly out of undefined bytes is not something to
 * leave in place while hunting a fault that looks like the hardware being
 * handed nonsense. Zeroing them costs six stores per frame. */
static void mod_header(pvr_mod_hdr_t *h, uint32_t mode)
{
    pvr_mod_compile(h, PVR_LIST_TR_MOD, mode, PVR_CULLING_NONE);
    h->mode2 = 0;
    h->mode3 = 0;
    h->a = h->r = h->g = h->b = 0.0f;
}

static void flush_mods(void *user)
{
    ps_swf_pvr        *p = user;
    pvr_mod_hdr_t      other, last;
    pvr_modifier_vol_t mv;
    float              z;
    int                i;

    if(p->nvol <= 0)
        return;

    /* The volume is collected and then thrown away, so the polygons that were
     * built to read it have nothing to read. That is the point: it is the one
     * arrangement that shows what the two-colour polygons do on their own. */
    if(p->mask_mode == PS_SWF_MASK_MODONLY) {
        p->nvol     = 0;
        p->vol_live = 0;
        return;
    }

    mod_header(&other, PVR_MODIFIER_OTHER_POLY);
    mod_header(&last, PVR_MODIFIER_INCLUDE_LAST_POLY);

    z = p->zbase + (float)p->zsteps * p->zstep;
    memset(&mv, 0, sizeof mv);
    mv.flags = PVR_CMD_VERTEX_EOL;
    mv.az = mv.bz = mv.cz = z;

    pvr_list_begin(PVR_LIST_TR_MOD);
    for(i = 0; i < p->nvol; i++) {
        if(i == 0 || p->vol[i - 1].last)
            pvr_prim(p->vol[i].last ? &last : &other, sizeof other);
        else if(p->vol[i].last)
            pvr_prim(&last, sizeof last);

        mv.ax = p->vol[i].x[0]; mv.ay = p->vol[i].y[0];
        mv.bx = p->vol[i].x[1]; mv.by = p->vol[i].y[1];
        mv.cx = p->vol[i].x[2]; mv.cy = p->vol[i].y[2];
        pvr_prim(&mv, sizeof mv);
    }
    pvr_list_finish();

    p->nvol     = 0;
    p->vol_live = 0;
}

static void flush_held(ps_swf_pvr *p)
{
    if(!p->held)
        return;
    emit_strip(p, p->hx, p->hy, 3);
    p->held = 0;
}

static void submit_tri(ps_swf_pvr *p, const float *xs, const float *ys)
{
    float bx0 = xs[0], bx1 = xs[0], by0 = ys[0], by1 = ys[0];
    int   i;

    /* Zero area, so no pixel of it exists on any rasteriser.
     *
     * The tessellator emits one wherever a band pinches to a point, which is
     * at the top and the bottom of every contour and at every vertex a
     * trapezoid degenerates on - and it is right not to test there, where the
     * cost is a branch against a triangle the hardware discards. Here the cost
     * is ninety-six bytes of a vertex buffer the same file's arithmetic says
     * is the binding constraint, so the same branch buys something.
     *
     * Exactly zero rather than nearly: a pinch is the same float written
     * twice, so the cross product is zero by construction, while a sliver a
     * thousandth of a pixel wide is real geometry that the sweep antialiases
     * and dropping it would be a difference this backend invented. */
    if((xs[1] - xs[0]) * (ys[2] - ys[0]) ==
       (xs[2] - xs[0]) * (ys[1] - ys[0])) {
        flush_held(p);
        p->flat_tris++;
        return;
    }

    for(i = 1; i < 3; i++) {
        if(xs[i] < bx0) bx0 = xs[i];
        if(xs[i] > bx1) bx1 = xs[i];
        if(ys[i] < by0) by0 = ys[i];
        if(ys[i] > by1) by1 = ys[i];
    }

    if(bx1 <= p->cx0 || bx0 >= p->cx1 || by1 <= p->cy0 || by0 >= p->cy1) {
        flush_held(p);
        return;
    }

    if(bx0 < p->cx0 || bx1 > p->cx1 || by0 < p->cy0 || by1 > p->cy1) {
        float cx[10], cy[10];
        int   n;

        flush_held(p);
        memcpy(cx, xs, 3 * sizeof *cx);
        memcpy(cy, ys, 3 * sizeof *cy);
        n = clip_poly(p, cx, cy, 3);
        for(i = 2; i < n; i++) {
            float tx[3] = { cx[0], cx[i - 1], cx[i] };
            float ty[3] = { cy[0], cy[i - 1], cy[i] };

            emit_strip(p, tx, ty, 3);
        }
        return;
    }

    /* (a,b,c) then (a,c,d) is a quadrilateral a,b,c,d split along a-c, and
     * a,b,d,c is that same quadrilateral as a strip - which the hardware
     * splits along b-d instead. The two splits cover the same ground only if
     * the quadrilateral is convex, and this one always is: it is a trapezoid
     * with two horizontal sides, because that is the only thing the
     * tessellator emits.
     *
     * It cannot fire across two trapezoids by accident, which is the failure
     * that would matter. Matching needs the two triangles to share their first
     * vertex, and a trapezoid's second triangle starts at its top-left corner
     * while the next band's first triangle starts on that band's top edge -
     * which is the previous band's bottom. Two distinct bands do not share a
     * top, so the y coordinates cannot agree unless the band was empty, and an
     * empty band's triangles cover nothing either way.
     *
     * Compared exactly rather than within a tolerance for the same reason:
     * both triangles came out of one emit_trap with the same float written
     * twice, so equality is what the producer actually guarantees, and a
     * tolerance would only manufacture strips from two shapes that touch. */
    if(p->held && p->hx[0] == xs[0] && p->hy[0] == ys[0] &&
       p->hx[2] == xs[1] && p->hy[2] == ys[1]) {
        float sx[4] = { p->hx[0], p->hx[1], xs[2], p->hx[2] };
        float sy[4] = { p->hy[0], p->hy[1], ys[2], p->hy[2] };

        p->held = 0;
        emit_strip(p, sx, sy, 4);
        return;
    }

    flush_held(p);
    p->hx[0] = xs[0]; p->hy[0] = ys[0];
    p->hx[1] = xs[1]; p->hy[1] = ys[1];
    p->hx[2] = xs[2]; p->hy[2] = ys[2];
    p->held  = 1;
}

/* --- the triangle sink ---------------------------------------------------- */

static uint32_t argb_of(ps_swf_rgba c)
{
    return ((uint32_t)c.a << 24) | ((uint32_t)c.r << 16) |
           ((uint32_t)c.g << 8) | c.b;
}

/* A colour transform under a texture, as far as a vertex colour can carry it.
 *
 * The multiplier is 8.8 with 256 meaning unchanged, and a vertex colour tops
 * out at 255, so a file asking to brighten past unity gets unity. The additive
 * term is dropped outright: PVR_TXRENV_MODULATEALPHA is a multiply and there is
 * no add anywhere in the pipeline to put it in. Flat fills do not come through
 * here at all - ps_geom_fill_paint has already applied both halves exactly to
 * the colour it hands over - so this only ever costs a gradient or a bitmap
 * under a transform that adds, which is rare next to one that dims. */
static uint32_t cxform_tint(const ps_swf_cxform *c)
{
    int v[4], i;

    for(i = 0; i < 4; i++) {
        v[i] = c->mult[i];
        if(v[i] < 0)   v[i] = 0;
        if(v[i] > 255) v[i] = 255;
    }
    return ((uint32_t)v[3] << 24) | ((uint32_t)v[0] << 16) |
           ((uint32_t)v[1] << 8) | (uint32_t)v[2];
}

static void tri_begin(void *user, const ps_swf_paint *paint)
{
    ps_swf_pvr    *p = user;
    pvr_poly_cxt_t cxt;
    const swf_tex *t = NULL;
    uint64_t       t0 = timer_us_gettime64();

    flush_held(p);
    p->passes++;

    if(collecting(p)) {
        /* One volume group per pass, closed on the pass that follows it. See
         * the `last` field: parity is what decides a volume's interior, and
         * the non-overlap guarantee that makes a volume safe holds within a
         * pass and not across two. */
        if(p->vol_open && p->nvol > 0)
            p->vol[p->nvol - 1].last = 1;
        p->us_state += timer_us_gettime64() - t0;
        return;                 /* mask geometry is measured, not drawn */
    }

    /* The first pass takes the depth the caller reserved and each after it one
     * step more. Past the reservation they share the last one, which loses the
     * later passes where they overlap - the wrong picture, but confined to the
     * movie, where climbing on would put the toolbar behind the page. */
    p->z = p->zbase + (float)p->zi * p->zstep;
    if(p->zi + 1 < p->zsteps)
        p->zi++;

    if(paint->grad)
        t = tex_find(p, paint->grad, paint->radial, 0);
    else if(paint->bitmap)
        t = tex_find(p, paint->bitmap, 0, paint->tiled);

    /* A volume marks pixels, and a vertex that carries two colours is what
     * reads the mark - but the vertex that carries two colours carries no
     * texture coordinates. The textured form exists (pvr_vertex_tpcm_t) and is
     * sixty-four bytes with two of everything, which doubles the cost of
     * exactly the fills that are already the expensive ones; until a testbed
     * puts a gradient or a bitmap under a mask, a textured pass under a volume
     * takes the bounding box it would have taken before and is counted. */
    p->pass_mod = p->vol_live && !t &&
                  (p->mask_mode == PS_SWF_MASK_VOL ||
                   p->mask_mode == PS_SWF_MASK_MODONLY);
    if(p->vol_live && t)
        p->vol_txr++;

    if(!t) {
        /* Flat, and that includes every fill whose texture did not fit: the
         * mean of a gradient's stops is a worse picture than the ramp and a
         * far better one than nothing, which is the trade ps_swf_fill's
         * `color` field was added for. */
        if(p->pass_mod)
            pvr_poly_cxt_col_mod(&cxt, PVR_LIST_TR_POLY);
        else
            pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
        p->argb = argb_of(paint->color);
        p->ua = p->ub = p->uc = 0.0f;
        p->va = p->vb = p->vc = 0.0f;
    }
    else {
        pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, t->fmt | PVR_TXRFMT_TWIDDLED,
                         t->w, t->h, t->ptr,
                         paint->bitmap && !paint->smoothed ? PVR_FILTER_NONE
                                                           : PVR_FILTER_BILINEAR);
        cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
        cxt.txr.uv_clamp = (paint->bitmap && paint->tiled) ? PVR_UVCLAMP_NONE
                                                           : PVR_UVCLAMP_UV;
        p->argb = paint->has_cx ? cxform_tint(&paint->cx) : 0xffffffffu;

        /* inv maps a screen pixel into the fill's own space - the gradient
         * square in twips, or the bitmap's texels - and the texture covers a
         * known extent of that space, so the texture coordinate is inv scaled
         * by the extent. Affine in x and y either way, which is what makes a
         * linear ramp exact: the hardware interpolates the same function the
         * sampler would have evaluated per pixel. */
        if(paint->grad) {
            float uv[6];

            ps_swf_ramp_uv(paint, uv);
            p->ua = uv[0]; p->ub = uv[1]; p->uc = uv[2];
            p->va = uv[3]; p->vb = uv[4]; p->vc = uv[5];
        }
        else {
            p->ua = paint->inv[0] / t->su;
            p->ub = paint->inv[1] / t->su;
            p->uc = paint->inv[2] / t->su;
            p->va = paint->inv[3] / t->sv;
            p->vb = paint->inv[4] / t->sv;
            p->vc = paint->inv[5] / t->sv;
        }
    }

    cxt.gen.culling  = PVR_CULLING_NONE;
    cxt.gen.shading  = PVR_SHADE_GOURAUD;
    if(p->pass_mod)
        pvr_poly_mod_compile(&p->hdr, &cxt);
    else
        pvr_poly_compile(&p->hdr, &cxt);
    p->hdr_pending = 1;

    p->us_state += timer_us_gettime64() - t0;
}

static void tri_tri(void *user, const ps_swf_vtx *v)
{
    ps_swf_pvr *p = user;
    float       xs[3], ys[3];
    int         i;

    p->tris++;
    for(i = 0; i < 3; i++) {
        xs[i] = v[i].x;
        ys[i] = v[i].y;
    }

    if(collecting(p)) {
        /* A mask is measured, not drawn: its bounding box, and how much of
         * that box its triangles actually cover. The two agreeing is what says
         * the mask is a rectangle, and a rectangle is a clip this backend can
         * apply exactly. See ps_swf_clip.c for why that case is the one worth
         * detecting and what the other cases would cost. */
        int k = p->nmask - 1;
        double a = ((double)xs[1] - xs[0]) * ((double)ys[2] - ys[0]) -
                   ((double)xs[2] - xs[0]) * ((double)ys[1] - ys[0]);

        /* Counted apart from the submitted ones. A mask costs the tessellator
         * everything a visible character costs and the tile accelerator
         * nothing, so a frame reading "sixty triangles, four strips" is a
         * masked frame and not a broken strip builder - which is exactly how
         * it read before this line existed. */
        p->mask_tris++;
        p->mask[k].area += (a < 0 ? -a : a) * 0.5;
        for(i = 0; i < 3; i++) {
            if(xs[i] < p->mask[k].x0) p->mask[k].x0 = xs[i];
            if(xs[i] > p->mask[k].x1) p->mask[k].x1 = xs[i];
            if(ys[i] < p->mask[k].y0) p->mask[k].y0 = ys[i];
            if(ys[i] > p->mask[k].y1) p->mask[k].y1 = ys[i];
        }
        if(p->vol_open)
            keep_vol_tri(p, xs, ys);
        return;
    }

    submit_tri(p, xs, ys);
}

static void tri_end(void *user)
{
    flush_held((ps_swf_pvr *)user);
}

static const ps_swf_tri_sink g_tri_sink = { tri_begin, tri_tri, tri_end };

/* --- the stage sink -------------------------------------------------------- */

static void st_draw(void *user, const ps_swf_shape *sh, const ps_swf_xform *xf,
                    const ps_swf_cxform *cx)
{
    ps_swf_pvr *p = user;
    uint64_t    t0;

    if(!p->open)
        return;

    t0 = timer_us_gettime64();
    p->draws++;
    (void)ps_swf_tess_shape(sh, &p->view, xf, cx, &g_tri_sink, p);
    p->us_draw += timer_us_gettime64() - t0;
}

static void st_clip_begin(void *user)
{
    ps_swf_pvr *p = user;
    int         k;

    if(p->nmask >= PS_SWF_CLIP_DEPTH)
        return;                 /* the walker checks the same limit first */
    k = p->nmask++;

    p->mask[k].x0 = 1e30f;
    p->mask[k].y0 = 1e30f;
    p->mask[k].x1 = -1e30f;
    p->mask[k].y1 = -1e30f;
    p->mask[k].area    = 0.0;
    p->mask[k].applied = 0;
    p->mask[k].sx0 = p->cx0;
    p->mask[k].sy0 = p->cy0;
    p->mask[k].sx1 = p->cx1;
    p->mask[k].sy1 = p->cy1;

    /* Collected on the chance it is not a rectangle, which is not known until
     * every triangle has been seen. A mask that turns out to be one drops what
     * was collected and costs the copy; the alternative is tessellating the
     * mask twice, which costs the whole of ps_swf_tess_shape. */
    if(!p->vol_spent && p->mask_mode != PS_SWF_MASK_BOX) {
        p->vol_open = 1;
        p->nvol     = 0;
    }
}

static void st_clip_apply(void *user)
{
    ps_swf_pvr *p = user;
    int         k = p->nmask - 1;
    double      box;
    /* Whether what is in the volume buffer is this mask's own geometry. It is
     * not, when an outer mask has already claimed the scene's one volume and
     * this one was never collected - and that difference has to be read before
     * anything is decided, because every path below either keeps the buffer or
     * empties it, and emptying somebody else's is a mask that silently stops
     * masking. */
    int         mine = p->vol_open;

    if(k < 0)
        return;
    p->mask[k].applied = 1;
    p->vol_open        = 0;

    if(p->mask[k].x1 <= p->mask[k].x0 || p->mask[k].y1 <= p->mask[k].y0) {
        /* An empty mask hides everything in its range, which is the reading
         * ps_swf_stage.c argues for: a broken file is not an instruction to
         * show what the author hid. */
        if(mine)
            p->nvol = 0;
        p->cx1 = p->cx0;
        p->cy1 = p->cy0;
        return;
    }

    box = (double)(p->mask[k].x1 - p->mask[k].x0) *
          (double)(p->mask[k].y1 - p->mask[k].y0);

    /* A mask that fills its own bounding box is that rectangle, and a
     * rectangle is exact on the CPU, composes under nesting and leaves the
     * scene's one volume for a mask that needs it.
     *
     * Anything else takes the volume if it is still there. If it is not -
     * a second general mask in one frame, or one too big to hold - the mask
     * is clipped to its bounding box, which leaves visible what it would have
     * hidden between its outline and that box. That is the same direction the
     * walker fails in when it cannot use a mask at all, and the opposite of
     * putting the mask's own artwork on the stage. */
    if(p->mask[k].area >= box * 0.995) {
        if(mine)
            p->nvol = 0;
    }
    else if(mine && p->nvol > 0 && vol_afford(p)) {
        p->vol[p->nvol - 1].last = 1;
        p->vol_tris  = p->nvol;
        p->vol_spent = 1;
        p->vol_live  = 1;
        p->vol_owner = k;
    }
    else {
        if(mine)
            p->nvol = 0;
        p->mask_inexact++;
    }

    if(p->mask[k].x0 > p->cx0) p->cx0 = p->mask[k].x0;
    if(p->mask[k].y0 > p->cy0) p->cy0 = p->mask[k].y0;
    if(p->mask[k].x1 < p->cx1) p->cx1 = p->mask[k].x1;
    if(p->mask[k].y1 < p->cy1) p->cy1 = p->mask[k].y1;
    if(p->cx1 < p->cx0) p->cx1 = p->cx0;
    if(p->cy1 < p->cy0) p->cy1 = p->cy0;
}

static void st_clip_end(void *user)
{
    ps_swf_pvr *p = user;
    int         k;

    if(p->nmask == 0)
        return;
    k = --p->nmask;

    /* The volume's triangles stay - they are not submitted until the list is
     * closed - but they stop applying to what is drawn from here, which is
     * what makes a mask a scope rather than a mode. */
    if(k == p->vol_owner) {
        p->vol_live  = 0;
        p->vol_owner = -1;
    }

    p->cx0 = p->mask[k].sx0;
    p->cy0 = p->mask[k].sy0;
    p->cx1 = p->mask[k].sx1;
    p->cy1 = p->mask[k].sy1;
}

const ps_swf_stage_sink *ps_swf_pvr_stage_sink(void)
{
    static const ps_swf_stage_sink sink = {
        st_draw, st_clip_begin, st_clip_apply, st_clip_end
    };

    return &sink;
}
