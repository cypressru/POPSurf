#include "ps_applet.h"
#include "ps_jar.h"
#include "ps_joff.h"

#ifdef PS_APPLET_PROFILE
#include <arch/timer.h>

/* Where the time goes, per phase, averaged over a second.
 *
 * Guessing at this has already cost two wrong fixes today, and the four phases
 * have wildly different shapes: the pump is interpreted bytecode, the paint is
 * interpreted bytecode plus rasterising, the conversion is a tight pixel loop,
 * and the upload is a twiddled DMA of a padded texture. Only one of them can
 * be the problem. */
static uint64_t g_prof[4], g_prof_frames, g_prof_at, g_prof_active;
static const char *g_prof_name[4] = { "pump", "paint", "convert", "upload" };
#define PROF_T0()   uint64_t _t0 = timer_us_gettime64()
#define PROF_ADD(n) do { g_prof[n] += timer_us_gettime64() - _t0; } while(0)
#else
#define PROF_T0()   do { } while(0)
#define PROF_ADD(n) do { } while(0)
#endif


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    SLOT_FREE = 0,
    SLOT_PENDING,     /* requested, class file not here yet */
    SLOT_READY,
    SLOT_FAILED
} slot_state;

/* One class the applet needs, and where it is in its life.
 *
 * The bag exists because the JVM's class loader is synchronous and the
 * browser's fetch is not: by the time the interpreter asks for a class it is
 * far too late to go to the network. So every class is resolved up front -
 * the applet's own constant pool names what it references, those are fetched,
 * theirs are scanned in turn, and only when nothing is outstanding does the
 * applet run. Breadth-first over the class graph, done once. */
typedef struct {
    char     name[PS_APPLET_NAME_MAX];
    char     url[PS_URL_MAX];
    uint8_t *bytes;
    size_t   len;
    int      arrived;
    int      failed;
} ps_applet_class;

/* An image the applet asked for by URL.
 *
 * getImage() in AWT returns immediately with an Image that may have no pixels
 * yet, and drawImage draws whatever is there - which is exactly the shape a
 * browser fetch has anyway. So an applet written against real AWT already
 * copes with this, and one that uses MediaTracker to wait simply waits. */
typedef struct {
    char      url[PS_URL_MAX];
    uint32_t *px;
    int       w, h;
    int       requested, arrived, failed;
} ps_applet_img;

/* One <param name= value=> from the page. Fixed fields rather than pointers
 * into the document: the document is reparsed on every layout and an applet
 * holds its parameters for its whole life. */
typedef struct {
    char name[PS_APPLET_PARAM_NAME];
    char value[PS_APPLET_PARAM_VALUE];
} ps_applet_param_kv;

/* A sound the applet asked for by URL. See ps_applet_clip_play for what does
 * and does not happen to it. */
typedef struct {
    char url[PS_URL_MAX];
    int  playing, looping;
} ps_applet_clip;

struct ps_applet_cache;

typedef struct {
    char       url[PS_URL_MAX];      /* the applet's identity: its main class */
    char       fetch[PS_URL_MAX];    /* jar when there is one, else url */
    char       dir[PS_URL_MAX];      /* directory siblings resolve against */
    char       main_name[PS_APPLET_NAME_MAX];
    int        is_jar;
    slot_state state;
    /* Wide enough for the VM's own message verbatim. A truncated reason is
     * exactly the reason you needed to read. */
    char       why[192];

    ps_applet_class cls[PS_APPLET_CLASSES];
    int             ncls;
    int             outstanding;

    ps_applet_img   img[PS_APPLET_IMAGES];
    int             nimg;

    ps_applet_clip  clip[PS_APPLET_CLIPS];
    int             nclip;

    /* What the page configured the applet with, and the page's own URL.
     *
     * params_set is separate from nparam because a page with no <param> at
     * all is still a page that has been asked, and the document calls this
     * every draw - so the flag is what keeps that O(1) and what stops a
     * relayout rewriting the parameters of an applet already running. */
    ps_applet_param_kv param[PS_APPLET_PARAMS];
    int                nparam;
    int                params_set;
    char               doc_base[PS_URL_MAX];

    ps_jsurface  surf;
    ps_jvm       vm;
    int          vm_live;

    /* The Graphics the VM holds, owned here rather than on whatever stack
     * happened to start the applet.
     *
     * vm->gfx is a bare pointer and the VM keeps it for its whole life, so a
     * context built as a local in try_run dangles the moment try_run returns.
     * Every synchronous path got away with it by rebuilding one before use;
     * the animation thread does not, so ps_jvm_pump ran bytecode against a
     * dead frame. `w = size().width` in run() is the opening move of nearly
     * every animated applet of the period, and it read whatever had since
     * been written over that stack. */
    ps_jgfx      gfx;

    /* Scratch for the ARGB8888 -> ARGB4444 conversion, kept rather than
     * allocated per upload. A 300x200 applet pads to 512x256, so this was a
     * quarter-megabyte calloc and free on every animated frame - which on a
     * machine with a plain heap allocator costs more than the conversion. */
    uint16_t *conv;
    int       conv_pw, conv_ph;

    /* Where the applet sits in the document, and whether that intersects the
     * window the viewer has on it. Kept in document space because the page is
     * drawn once and replayed at every scroll position: a screen-space answer
     * would be frozen at whatever scroll the recording was made at. */
    int box_y, box_h, box_known;
    int visible;

    struct ps_applet_cache *owner;

    /* Paint as geometry rather than pixels. Off by default, and measured:
     *
     *   texture path   25 fps with two applets, 61 with none visible
     *   geometry path  16 fps
     *
     * Two reasons, both fixable and neither fixed. A textured applet only
     * repaints when it asks to and blits a cached frame otherwise; geometry
     * has nothing to cache, so paint() runs every frame the applet is on
     * screen - 60 times a second for something that wanted 25. And draw_oval's
     * second pass still walks the caps a pixel at a time, so an outline that
     * should be a few dozen runs is still a few hundred shapes.
     *
     * Kept because the idea is right and the measurement says the
     * implementation is not there yet. PS_APPLET_VECTOR turns it on. */
    int vector;

    /* What the last press was, so a release can be turned into a click.
     *
     * AWT synthesises mouseClicked above the component - the browser is the
     * only thing here that sees a press and a release as one gesture, so it is
     * the only thing that can. Observed on a real JDK: a drag between the two
     * suppresses the click entirely and makes the release report a click count
     * of zero, which is what drag_seen is for. */
    int press_seen, drag_seen;

    ps_applet_view view;
} ps_applet_slot;

struct ps_applet_cache {
    const ps_gfx_backend *gfx;
    ps_applet_request_fn  request;
    void                 *user;
    const ps_jtext_ops   *text;

    /* Which applet the pointer is inside, and which one has the keyboard.
     *
     * Both are held as URLs rather than slot pointers because a slot can be
     * released and reused underneath them - the identity that matters is the
     * applet, not the storage. Empty means none, which is a state that has to
     * be representable: the pointer spends most of its time over the page.
     *
     * Hover is what makes mouseEntered and mouseExited possible at all. The
     * browser has no boundary-crossing notion of its own, so the crossing is
     * derived here by remembering where the pointer was last frame. Deriving
     * it from position alone would refire on every scroll; deriving it from
     * the applet's identity does not, which is the whole reason this is a URL
     * and not a rectangle. */
    char hover[PS_URL_MAX];
    char focus[PS_URL_MAX];

    ps_applet_slot slots[PS_APPLET_MAX];
};

ps_applet_cache *ps_applet_cache_create(const ps_gfx_backend *gfx,
                                        ps_applet_request_fn request,
                                        void *user)
{
    ps_applet_cache *c = (ps_applet_cache *)calloc(1, sizeof *c);

    if(!c)
        return NULL;

    c->gfx     = gfx;
    c->request = request;
    c->user    = user;
    return c;
}

void ps_applet_cache_set_text(ps_applet_cache *c, const ps_jtext_ops *ops)
{
    if(c)
        c->text = ops;

    /* An offscreen image is a pixel buffer whatever mode the applet is
     * painting in, so it needs glyph callbacks even when the applet's own
     * context is a vector one and has none to lend. */
    ps_joff_set_text(ops);
}

static void slot_release(ps_applet_cache *c, ps_applet_slot *s)
{
    int i;

    for(i = 0; i < s->ncls; i++)
        free(s->cls[i].bytes);
    for(i = 0; i < s->nimg; i++)
        free(s->img[i].px);
    free(s->conv);

    if(s->vm_live) {
        /* Before ps_jvm_free, not after: the offscreen surfaces are found by
         * walking this VM's heap, and ps_jvm_free is what takes that away. */
        ps_joff_release(&s->vm);
        ps_jvm_free(&s->vm);
        s->vm_live = 0;
    }
    ps_jsurface_free(&s->surf);

    if(s->view.tex != PS_TEXTURE_NONE) {
        c->gfx->free_texture(c->gfx->self, s->view.tex);
        s->view.tex = PS_TEXTURE_NONE;
    }
    memset(s, 0, sizeof *s);
}

void ps_applet_cache_clear(ps_applet_cache *c)
{
    int i;

    if(!c)
        return;
    for(i = 0; i < PS_APPLET_MAX; i++)
        slot_release(c, &c->slots[i]);
}

void ps_applet_cache_destroy(ps_applet_cache *c)
{
    if(!c)
        return;
    ps_applet_cache_clear(c);
    free(c);
}

static ps_applet_slot *find(ps_applet_cache *c, const char *url)
{
    int i;

    for(i = 0; i < PS_APPLET_MAX; i++) {
        if(c->slots[i].state != SLOT_FREE && !strcmp(c->slots[i].url, url))
            return &c->slots[i];
    }
    return NULL;
}

/* Which slot is waiting on this URL - its own class, its jar, or any of the
 * dependencies it went on to ask for. */
static ps_applet_slot *find_waiting(ps_applet_cache *c, const char *url,
                                    int *which)
{
    int i, k;

    for(i = 0; i < PS_APPLET_MAX; i++) {
        ps_applet_slot *s = &c->slots[i];

        /* A READY applet is still a candidate: images are asked for while it
         * runs, long after the class graph settled. */
        if(s->state != SLOT_PENDING && s->state != SLOT_READY)
            continue;

        if(!strcmp(s->fetch, url)) {
            *which = -1;                 /* the primary fetch */
            return s;
        }
        for(k = 0; k < s->ncls; k++) {
            if(!s->cls[k].arrived && !s->cls[k].failed &&
               !strcmp(s->cls[k].url, url)) {
                *which = k;
                return s;
            }
        }
        for(k = 0; k < s->nimg; k++) {
            if(!s->img[k].arrived && !s->img[k].failed &&
               !strcmp(s->img[k].url, url)) {
                *which = PS_APPLET_CLASSES + k;   /* images are keyed above */
                return s;
            }
        }
    }
    return NULL;
}

/* --- texture upload ------------------------------------------------------
 *
 * The PVR wants power-of-two dimensions, so the frame is padded up and the
 * used fraction reported as u1/v1 - the same arrangement ps_image uses for
 * decoded frames, for the same reason.
 *
 * ARGB4444 rather than RGB565: an applet can leave pixels untouched, and those
 * have to come out transparent so the page shows through rather than black.
 */
static int pot(int v)
{
    int p = 8;

    while(p < v)
        p <<= 1;
    return p;
}

static int upload(ps_applet_cache *c, ps_applet_slot *s)
{
    int       pw = pot(s->surf.w), ph = pot(s->surf.h);
    uint16_t *buf;
    int       x, y, upload_h;
    PROF_T0();

    /* One pass, when the backend can do it: the conversion writes straight
     * into the store queue and never lands in main memory at all. The staging
     * path below is the fallback, and is what a host build uses.
     *
     * PS_APPLET_STAGED forces the fallback, which is the only way to time the
     * pack and the transfer separately - they are interleaved per 32 bytes in
     * the one-pass version and cannot be told apart there. */
#ifndef PS_APPLET_STAGED
    if(s->view.tex != PS_TEXTURE_NONE && c->gfx->update_texture_argb &&
       c->gfx->update_texture_argb(c->gfx->self, s->view.tex, s->surf.px,
                                   s->surf.w, s->surf.h, s->surf.stride)) {
        s->view.u1 = (float)s->surf.w / (float)pw;
        s->view.v1 = (float)s->surf.h / (float)ph;
        /* No convert timer here. This path returns straight into the caller's
         * upload timer, and adding one made the same microseconds appear under
         * both headings - which is why convert and upload read within six
         * microseconds of each other and the total was nonsense. */
        return 0;
    }
#endif

    if(s->conv_pw != pw || s->conv_ph != ph) {
        free(s->conv);
        s->conv = (uint16_t *)calloc((size_t)pw * ph, sizeof(uint16_t));
        s->conv_pw = pw;
        s->conv_ph = ph;
    }
    buf = s->conv;
    if(!buf)
        return -1;

    for(y = 0; y < s->surf.h; y++) {
        const uint32_t *src = &s->surf.px[(size_t)y * s->surf.stride];
        uint16_t       *dst = &buf[(size_t)y * pw];

        /* Each nibble is shifted straight into place rather than extracted
         * and shifted again - four shifts, four masks, three ors instead of
         * fourteen operations. The loop is close to memory bound either way,
         * so this is worth a little and not a lot. */
        for(x = 0; x < s->surf.w; x++) {
            uint32_t p = src[x];

            dst[x] = (uint16_t)(((p >> 16) & 0xf000) |
                                ((p >> 12) & 0x0f00) |
                                ((p >>  8) & 0x00f0) |
                                ((p >>  4) & 0x000f));
        }
    }

    /* Reuse the allocation when there is one. An animated applet repaints
     * about twenty-five times a second, and freeing then reallocating a
     * quarter-megabyte texture at that rate fragments VRAM until an
     * allocation fails - which on a machine with no memory protection is not
     * a failed allocation, it is the browser going down. */
    PROF_ADD(2);

    /* Only the rows the applet occupies.
     *
     * The texture is padded to a power of two - a 300x200 applet becomes
     * 512x256 - but v1 stops the sampler at row 200, so the 56 rows below it
     * are never read. Rows are contiguous in a linear layout, so not sending
     * them is just a shorter copy: a fifth off every upload. The padded
     * columns cannot be skipped the same way, because they are interleaved
     * with real ones. */
    upload_h = s->surf.h;

    if(s->view.tex != PS_TEXTURE_NONE &&
       c->gfx->update_texture &&
       c->gfx->update_texture(c->gfx->self, s->view.tex, buf, pw,
                              upload_h)) {
        s->view.u1 = (float)s->surf.w / (float)pw;
        s->view.v1 = (float)s->surf.h / (float)ph;
        return 0;
    }

    if(s->view.tex != PS_TEXTURE_NONE)
        c->gfx->free_texture(c->gfx->self, s->view.tex);

    /* Linear, not twiddled. An applet's texture is rewritten every frame and
     * twiddling it was costing more than the whole rest of the frame. */
    s->view.tex = c->gfx->upload_texture(c->gfx->self, buf, pw, ph,
                                         PS_FMT_ARGB4444_LINEAR);

    if(s->view.tex == PS_TEXTURE_NONE)
        return -1;

    s->view.u1 = (float)s->surf.w / (float)pw;
    s->view.v1 = (float)s->surf.h / (float)ph;
    return 0;
}

/* --- lookup -------------------------------------------------------------- */

/* --- the class bag -------------------------------------------------------
 *
 * Everything here runs before the applet does. See the note on ps_applet_class
 * for why: the VM's loader cannot go to the network, so the class graph is
 * walked and fetched first, and the loader then only ever answers from memory.
 */

static ps_applet_class *bag_find(ps_applet_slot *s, const char *name)
{
    int i;

    for(i = 0; i < s->ncls; i++) {
        if(!strcmp(s->cls[i].name, name))
            return &s->cls[i];
    }
    return NULL;
}

/* Names the runtime answers itself, which must never be fetched. */
static int is_runtime_class(const char *n)
{
    return !strncmp(n, "java/", 5) || !strncmp(n, "javax/", 6) ||
           !strncmp(n, "sun/", 4) || n[0] == '[';
}

/* Adds a class to the bag, and asks for it unless the caller already has it.
 *
 * `have` is set for the applet's own class, which arrived before anything was
 * scanned - requesting it again would be a second round trip for bytes already
 * in hand, and on a dial-up-era page that is not free. */
static ps_applet_class *want_class(ps_applet_cache *c, ps_applet_slot *s,
                                   const char *name, int have)
{
    ps_applet_class *e;

    if(is_runtime_class(name) || bag_find(s, name))
        return NULL;
    if(s->ncls >= PS_APPLET_CLASSES)
        return NULL;
    if(strlen(name) >= PS_APPLET_NAME_MAX)
        return NULL;

    e = &s->cls[s->ncls++];
    memset(e, 0, sizeof *e);
    snprintf(e->name, sizeof e->name, "%s", name);

    if(have)
        return e;

    /* Inside a jar every class is already in hand, so nothing is requested. */
    if(s->is_jar) {
        e->failed = 1;      /* until the jar turns out to contain it */
        return e;
    }

    /* dir is bounded by PS_URL_MAX and name by PS_APPLET_NAME_MAX, so the
         * join can only overflow on a URL already at the limit - snprintf
         * truncates, and a truncated URL simply 404s. */
    snprintf(e->url, sizeof e->url, "%.*s%s.class",
                 (int)(sizeof e->url - PS_APPLET_NAME_MAX - 8), s->dir, name);
    s->outstanding++;

    if(c->request)
        c->request(c->user, e->url);
    return e;
}

/* Walks a class's constant pool and asks for everything it names.
 *
 * The pool is parsed from a throwaway copy: ps_jclass_parse takes ownership of
 * what it is given and keeps every name pointing into it, and these bytes have
 * to survive to be handed to the real VM later. */
static void scan_refs(ps_applet_cache *c, ps_applet_slot *s,
                      const uint8_t *bytes, size_t len)
{
    uint8_t   *copy = (uint8_t *)malloc(len + 1);
    ps_jclass *k;
    uint16_t   i;

    if(!copy)
        return;
    memcpy(copy, bytes, len);

    k = ps_jclass_parse(copy, len, NULL);
    if(!k) {
        free(copy);
        return;
    }

    for(i = 1; i < k->cp_count; i++) {
        const char *n;

        if(k->cp[i].tag != PS_CP_CLASS)
            continue;
        n = ps_jcp_class_name(k, i);
        if(n)
            want_class(c, s, n, 0);
    }

    ps_jclass_free(k);
}

/* --- lookup -------------------------------------------------------------- */

/* The directory a URL sits in, with the trailing slash kept. Siblings resolve
 * against this, which is what codebase means once it is folded into the URL. */
static void dir_of(const char *url, char *out, size_t n)
{
    const char *slash = strrchr(url, '/');
    size_t      keep;

    if(!slash) {
        snprintf(out, n, "%s", "");
        return;
    }
    keep = (size_t)(slash - url) + 1;
    if(keep >= n)
        keep = n - 1;
    memcpy(out, url, keep);
    out[keep] = '\0';
}

/* The main class's internal name, from its URL: the last path component
 * without the .class suffix. */
static void name_of(const char *url, char *out, size_t n)
{
    const char *slash = strrchr(url, '/');
    const char *base  = slash ? slash + 1 : url;
    size_t      len   = strlen(base);

    if(len > 6 && !strcmp(base + len - 6, ".class"))
        len -= 6;
    if(len >= n)
        len = n - 1;
    memcpy(out, base, len);
    out[len] = '\0';
}

const ps_applet_view *ps_applet_get(ps_applet_cache *c, const char *url,
                                    const char *archive, int w, int h)
{
    ps_applet_slot *s;
    int             i;

    if(!c || !url || !*url || strlen(url) >= PS_URL_MAX)
        return NULL;

    s = find(c, url);
    if(s)
        return s->state == SLOT_READY ? &s->view : NULL;

    /* Refusing an implausible box costs one broken applet; honouring it costs
     * the page. 320x240 of ARGB is 300KB of main memory before the padded
     * texture on top. */
    if(w <= 0 || h <= 0 || w > PS_APPLET_MAX_DIM || h > PS_APPLET_MAX_DIM ||
       w * h > PS_APPLET_MAX_PIXELS)
        return NULL;

    for(i = 0; i < PS_APPLET_MAX; i++) {
        if(c->slots[i].state == SLOT_FREE)
            break;
    }
    if(i == PS_APPLET_MAX)
        return NULL;

    s = &c->slots[i];
    memset(s, 0, sizeof *s);
    snprintf(s->url, sizeof s->url, "%s", url);
    dir_of(url, s->dir, sizeof s->dir);
    name_of(url, s->main_name, sizeof s->main_name);

    s->is_jar = (archive && *archive) ? 1 : 0;
    snprintf(s->fetch, sizeof s->fetch, "%s", s->is_jar ? archive : url);

    s->owner    = c;
#ifdef PS_APPLET_VECTOR
    s->vector   = 1;
#endif
    s->state    = SLOT_PENDING;
    s->view.w   = w;
    s->view.h   = h;
    s->view.tex = PS_TEXTURE_NONE;
    snprintf(s->why, sizeof s->why, "loading");

    if(ps_jsurface_init(&s->surf, w, h) != 0) {
        s->state = SLOT_FAILED;
        snprintf(s->why, sizeof s->why, "out of memory");
        return NULL;
    }

    if(c->request)
        c->request(c->user, s->fetch);

    return NULL;
}

void ps_applet_set_params(ps_applet_cache *c, const char *url,
                          const char *doc_base, const char *const *names,
                          const char *const *values, int n)
{
    ps_applet_slot *s = c ? find(c, url) : NULL;
    int             i;

    /* Once per slot. The document has no idea whether it has said this
     * before, so the answer to being told twice has to be here. */
    if(!s || s->params_set)
        return;

    s->params_set = 1;

    if(doc_base)
        snprintf(s->doc_base, sizeof s->doc_base, "%s", doc_base);

    for(i = 0; i < n && s->nparam < PS_APPLET_PARAMS; i++) {
        ps_applet_param_kv *p;

        /* A <param> with no name cannot be asked for, so it is not worth a
         * slot - and the page is free to write one. */
        if(!names || !names[i] || !*names[i])
            continue;

        p = &s->param[s->nparam++];
        snprintf(p->name, sizeof p->name, "%s", names[i]);
        snprintf(p->value, sizeof p->value, "%s",
                 (values && values[i]) ? values[i] : "");
    }
}

/* Forgets which applets the page contains. Called before a redraw, which is
 * the one thing that can establish it again. */
void ps_applet_cache_page_begin(ps_applet_cache *c)
{
    int i;

    if(!c)
        return;
    for(i = 0; i < PS_APPLET_MAX; i++)
        c->slots[i].box_known = 0;
}

void ps_applet_set_box(ps_applet_cache *c, const char *url, int doc_y, int h)
{
    ps_applet_slot *s = c ? find(c, url) : NULL;

    if(!s)
        return;

    s->box_y     = doc_y;
    s->box_h     = h;
    s->box_known = 1;
}

void ps_applet_set_view(ps_applet_cache *c, int scroll_y, int view_h)
{
    int i;

    if(!c)
        return;

    /* A negative height is the caller saying nothing is on screen at all -
     * the screensaver is up. Applets keep their state and their place in
     * run(), and simply stop being given time, which is what a browser does
     * to a tab nobody is looking at. */
    if(view_h < 0) {
        for(i = 0; i < PS_APPLET_MAX; i++)
            c->slots[i].visible = 0;
        return;
    }

    for(i = 0; i < PS_APPLET_MAX; i++) {
        ps_applet_slot *s = &c->slots[i];

        if(!s->box_known) {
            s->visible = 0;
            continue;
        }

        /* A band either side, so an applet just off the edge is already
         * running by the time it is scrolled to rather than starting from its
         * first frame. */
        s->visible = (s->box_y + s->box_h > scroll_y - 64) &&
                     (s->box_y < scroll_y + view_h + 64);
    }
}

const char *ps_applet_status(ps_applet_cache *c, const char *url)
{
    ps_applet_slot *s = c ? find(c, url) : NULL;

    return s ? s->why : "";
}

/* --- images --------------------------------------------------------------
 *
 * Decoded to ARGB in main memory, because an applet composites them into its
 * own surface with the CPU. A texture handle would be no use to it.
 */

extern unsigned char *stbi_load_from_memory(const unsigned char *buffer,
                                            int len, int *x, int *y,
                                            int *comp, int req_comp);
extern void stbi_image_free(void *retval_from_stbi_load);

/* The slot currently executing, so the runtime's getImage can reach its bag.
 * The interpreter is single-threaded and one applet runs at a time, so this is
 * a parameter that would otherwise have to be threaded through the whole of
 * ps_jre for one call. */
static ps_applet_slot *g_running;

/* Called from ps_jre when an applet asks for an image. Returns the handle it
 * should hold - an index, one-based so zero stays "nothing". */
int ps_applet_want_image(const char *url)
{
    ps_applet_slot *s = g_running;
    int             i;

    if(!s || !url || !*url || strlen(url) >= PS_URL_MAX)
        return 0;

    for(i = 0; i < s->nimg; i++) {
        if(!strcmp(s->img[i].url, url))
            return i + 1;
    }
    if(s->nimg >= PS_APPLET_IMAGES)
        return 0;

    i = s->nimg++;
    memset(&s->img[i], 0, sizeof s->img[i]);
    snprintf(s->img[i].url, sizeof s->img[i].url, "%s", url);
    s->img[i].requested = 1;

    if(s->owner && s->owner->request)
        s->owner->request(s->owner->user, s->img[i].url);

    return i + 1;
}

/* Resolves a possibly-relative image URL against the applet's directory, the
 * way getDocumentBase and getCodeBase do. */
void ps_applet_image_base(char *out, size_t n, const char *rel)
{
    ps_applet_slot *s = g_running;

    if(!s) {
        snprintf(out, n, "%s", rel ? rel : "");
        return;
    }
    if(rel && strstr(rel, "://"))
        snprintf(out, n, "%s", rel);
    else
        snprintf(out, n, "%s%s", s->dir, rel ? rel : "");
}

/* Resolves rel against base, for getImage/getAudioClip. See the header for the
 * rules; they are java.net.URL(URL, String)'s, minus the parts a URL string
 * this runtime produces cannot contain. */
void ps_applet_url_join(char *out, size_t n, const char *base, const char *rel)
{
    const char *slash;
    size_t      keep;

    if(!out || !n)
        return;
    if(!rel)
        rel = "";

    /* No base at all. The applet's own directory is what getCodeBase used to
     * return as an empty string, and what a null context means. */
    if(!base || !*base) {
        ps_applet_image_base(out, n, rel);
        return;
    }
    if(!*rel) {
        snprintf(out, n, "%s", base);
        return;
    }
    if(strstr(rel, "://")) {
        snprintf(out, n, "%s", rel);
        return;
    }

    /* Root-relative: scheme and authority survive, the path does not. */
    if(rel[0] == '/') {
        const char *auth = strstr(base, "://");
        const char *end  = auth ? strchr(auth + 3, '/') : NULL;

        keep = end ? (size_t)(end - base) : (auth ? strlen(base) : 0);
        snprintf(out, n, "%.*s%s", (int)keep, base, rel);
        return;
    }

    /* Otherwise the base's last path component is replaced. That is the one
     * rule that makes both bases work: getDocumentBase() ends in index.html
     * and getCodeBase() ends in a slash, and replacing the last component is
     * right for each. Concatenating would be right for only one of them, and
     * silently fetch ".../index.htmlsplash.gif" for the other. */
    slash = strrchr(base, '/');
    keep  = slash ? (size_t)(slash - base) + 1 : 0;
    snprintf(out, n, "%.*s%s", (int)keep, base, rel);
}

/* --- parameters and bases ------------------------------------------------ */

/* ASCII case folding, written out rather than strcasecmp: a param name comes
 * from a page and only ASCII means anything in one, and this keeps the file
 * off a header whose presence differs between the console and the host. */
static int name_eq(const char *a, const char *b)
{
    for(; *a && *b; a++, b++) {
        int ca = (unsigned char)*a, cb = (unsigned char)*b;

        if(ca >= 'A' && ca <= 'Z') ca += 32;
        if(cb >= 'A' && cb <= 'Z') cb += 32;
        if(ca != cb)
            return 0;
    }
    return *a == *b;
}

const char *ps_applet_param(const char *name)
{
    ps_applet_slot *s = g_running;
    int             i;

    if(!s || !name)
        return NULL;

    for(i = 0; i < s->nparam; i++) {
        if(name_eq(s->param[i].name, name))
            return s->param[i].value;
    }
    return NULL;      /* absent, which is not the same as empty */
}

const char *ps_applet_code_base(void)
{
    return g_running ? g_running->dir : "";
}

const char *ps_applet_doc_base(void)
{
    if(!g_running)
        return "";

    /* Falls back to the code base when the browser did not say which page
     * this is - a host test, or a document with no URL of its own. Wrong only
     * for a page that sets codebase=, and better than empty everywhere. */
    return g_running->doc_base[0] ? g_running->doc_base : g_running->dir;
}

/* --- audio ---------------------------------------------------------------
 *
 * This is a stub and it is worth being precise about which part.
 *
 * The object model, the URL resolution and the lifecycle are real: a clip is
 * registered against the applet, handed back as a handle, and remembers
 * whether it was told to play, loop or stop. What does not happen is sound.
 *
 * Making sound needs two things this browser does not have yet. The audio path
 * here is a MIDI sequencer over a General MIDI soundbank (core/ps_audio.h) and
 * knows nothing about a one-shot sample; the voice layer underneath it does
 * (core/ps_voice.h, upload and key a PCM buffer), but the java subsystem
 * reaches the machine only through the ops tables it is handed - ps_jtext_ops
 * for text, ps_gfx_backend for pixels - and there is no audio one to hand it.
 * On top of that an applet's clip is a Sun .au, 8kHz mu-law, which needs
 * decoding to something the AICA takes.
 *
 * So nothing is fetched either: the bytes would be a dial-up transfer for
 * something that cannot become sound. An applet cannot tell the difference,
 * because getAudioClip in a real AWT also returns an object for a URL that
 * 404s and also silently does nothing when it is played.
 *
 * What this buys is the applet running at all. Before it, getAudioClip stopped
 * the interpreter dead on a class it had never heard of and the box stayed
 * blank - so a missing AudioClip costs the whole applet, and a silent one
 * costs the sound.
 */
int ps_applet_want_clip(const char *url)
{
    ps_applet_slot *s = g_running;
    int             i;

    if(!s || !url || !*url || strlen(url) >= PS_URL_MAX)
        return 0;

    for(i = 0; i < s->nclip; i++) {
        if(!strcmp(s->clip[i].url, url))
            return i + 1;
    }
    if(s->nclip >= PS_APPLET_CLIPS)
        return 0;

    i = s->nclip++;
    memset(&s->clip[i], 0, sizeof s->clip[i]);
    snprintf(s->clip[i].url, sizeof s->clip[i].url, "%s", url);

    printf("popsurf: applet audio clip %s (silent; no decoder)\n",
           s->clip[i].url);
    return i + 1;
}

void ps_applet_clip_play(int handle, int loop)
{
    ps_applet_slot *s = g_running;
    ps_applet_clip *cl;

    if(!s || handle <= 0 || handle > s->nclip)
        return;

    cl = &s->clip[handle - 1];

    /* Only on a change of state. An applet is free to call play() from
     * paint(), and a line a frame would bury the console. */
    if(!cl->playing || cl->looping != loop)
        printf("popsurf: applet audio %s %s\n", cl->url,
               loop ? "loop" : "play");

    cl->playing = 1;
    cl->looping = loop;
}

void ps_applet_clip_stop(int handle)
{
    ps_applet_slot *s = g_running;
    ps_applet_clip *cl;

    if(!s || handle <= 0 || handle > s->nclip)
        return;

    cl = &s->clip[handle - 1];
    cl->playing = 0;
    cl->looping = 0;
}

const uint32_t *ps_applet_image_px(int handle, int *w, int *h)
{
    ps_applet_slot *s = g_running;
    ps_applet_img  *im;

    if(!s || handle <= 0 || handle > s->nimg)
        return NULL;

    im = &s->img[handle - 1];
    if(!im->arrived || !im->px)
        return NULL;

    *w = im->w;
    *h = im->h;
    return im->px;
}

static int image_deliver(ps_applet_slot *s, int idx, int ok,
                         const void *data, size_t len)
{
    ps_applet_img *im = &s->img[idx];
    unsigned char *rgba;
    int            w = 0, h = 0, comp = 0, i, n;

    if(!ok || !data || !len) {
        im->failed = 1;
        printf("popsurf: applet image %s unavailable\n", im->url);
        return 0;
    }

    rgba = stbi_load_from_memory((const unsigned char *)data, (int)len,
                                 &w, &h, &comp, 4);
    if(!rgba) {
        im->failed = 1;
        return 0;
    }
    if(w <= 0 || h <= 0 || w * h > PS_APPLET_IMG_PIXELS) {
        stbi_image_free(rgba);
        im->failed = 1;
        printf("popsurf: applet image %s too large (%dx%d)\n", im->url, w, h);
        return 0;
    }

    n = w * h;
    im->px = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
    if(!im->px) {
        stbi_image_free(rgba);
        im->failed = 1;
        return 0;
    }

    /* stb hands back RGBA; the surface is ARGB. */
    for(i = 0; i < n; i++) {
        im->px[i] = ((uint32_t)rgba[i * 4 + 3] << 24) |
                    ((uint32_t)rgba[i * 4 + 0] << 16) |
                    ((uint32_t)rgba[i * 4 + 1] << 8)  |
                     (uint32_t)rgba[i * 4 + 2];
    }
    stbi_image_free(rgba);

    im->w = w;
    im->h = h;
    im->arrived = 1;
    printf("popsurf: applet image %s, %dx%d\n", im->url, w, h);

    /* An image landing changes what the next paint draws, so the applet is
     * repainted whether or not it asked - it has no way to know the image
     * arrived, which is what imageUpdate exists for in a real AWT. */
    s->vm.repaint = 1;
    return 1;
}

/* --- delivery ------------------------------------------------------------ */

/* The VM's loader, answering only from the bag. Everything it can ever be
 * asked for was fetched before the applet started. */
static uint8_t *bag_loader(void *user, const char *name, size_t *out_len)
{
    ps_applet_slot  *s = (ps_applet_slot *)user;
    ps_applet_class *e = bag_find(s, name);
    uint8_t         *copy;

    if(!e || !e->arrived || !e->bytes)
        return NULL;

    /* The VM takes ownership of what it is handed and keeps names pointing
     * into it, so the bag's copy has to stay the bag's. */
    copy = (uint8_t *)malloc(e->len + 1);
    if(!copy)
        return NULL;
    memcpy(copy, e->bytes, e->len);
    *out_len = e->len;
    return copy;
}

static int jar_entry(void *user, const char *name, uint8_t *data, size_t len)
{
    ps_applet_slot  *s = (ps_applet_slot *)user;
    ps_applet_class *e = bag_find(s, name);

    if(!e) {
        if(s->ncls >= PS_APPLET_CLASSES) {
            free(data);
            return 1;
        }
        e = &s->cls[s->ncls++];
        memset(e, 0, sizeof *e);
        snprintf(e->name, sizeof e->name, "%s", name);
    }

    free(e->bytes);
    e->bytes   = data;
    e->len     = len;
    e->arrived = 1;
    e->failed  = 0;
    return 1;
}

static void slot_fail(ps_applet_slot *s, const char *why)
{
    s->state = SLOT_FAILED;
    snprintf(s->why, sizeof s->why, "%s", why);
}

/* Everything is in hand: build the VM and run the applet. */
static int try_run(ps_applet_cache *c, ps_applet_slot *s)
{
    ps_jclass       *cls;
    ps_applet_class *main_e;
    uint8_t         *copy;

    if(s->outstanding > 0)
        return 0;

    main_e = bag_find(s, s->main_name);
    if(!main_e || !main_e->arrived) {
        slot_fail(s, "main class never arrived");
        return 1;
    }

    copy = (uint8_t *)malloc(main_e->len + 1);
    if(!copy) {
        slot_fail(s, "out of memory");
        return 1;
    }
    memcpy(copy, main_e->bytes, main_e->len);

    ps_jgfx_init(&s->gfx, &s->surf, c->text);

    ps_jvm_init(&s->vm, &s->gfx);
    ps_jvm_set_loader(&s->vm, bag_loader, s);
    s->vm_live = 1;

    g_running = s;
    cls = ps_jvm_define(&s->vm, copy, main_e->len);
    if(!cls) {
        slot_fail(s, s->vm.err);
        printf("popsurf: applet %s: %s\n", s->url, s->vm.err);
        return 1;
    }

    if(ps_jvm_run_applet(&s->vm, cls, &s->gfx) != 0) {
        g_running = NULL;
        slot_fail(s, s->vm.err);
        printf("popsurf: applet %s: %s\n", s->url, s->vm.err);
        return 1;
    }
    g_running = NULL;

    if(upload(c, s) != 0) {
        slot_fail(s, "no VRAM for the frame");
        return 1;
    }

    s->state      = SLOT_READY;
    s->view.ready = 1;
    snprintf(s->why, sizeof s->why, "ok");

    printf("popsurf: applet %s ran, %ld instructions, %d class%s, %dx%d%s\n",
           s->url, 20L * 1000 * 1000 - s->vm.budget, s->ncls,
           s->ncls == 1 ? "" : "es", s->view.w, s->view.h,
           ps_jvm_has_thread(&s->vm) ? ", animating" : "");
    return 1;
}

int ps_applet_deliver(ps_applet_cache *c, const char *url, int ok,
                      const void *data, size_t len)
{
    int             which = 0;
    ps_applet_slot *s = c ? find_waiting(c, url, &which) : NULL;

    if(!s)
        return 0;

    if(which >= PS_APPLET_CLASSES) {
        /* An image the running applet asked for. */
        image_deliver(s, which - PS_APPLET_CLASSES, ok, data, len);
        return 0;      /* pixels change, layout does not */
    }

    if(which >= 0) {
        /* A dependency. Its own references are scanned in turn, which is what
         * makes this breadth-first rather than one level deep. */
        ps_applet_class *e = &s->cls[which];

        s->outstanding--;

        if(!ok || !data || !len) {
            e->failed = 1;
            printf("popsurf: applet class %s unavailable\n", e->name);
        }
        else {
            e->bytes = (uint8_t *)malloc(len + 1);
            if(e->bytes) {
                memcpy(e->bytes, data, len);
                e->len     = len;
                e->arrived = 1;
                scan_refs(c, s, e->bytes, e->len);
            }
            else {
                e->failed = 1;
            }
        }
        return try_run(c, s);
    }

    /* The primary fetch: the jar, or the applet's own class. */
    if(!ok || !data || !len) {
        slot_fail(s, s->is_jar ? "jar could not be fetched"
                               : "class file could not be fetched");
        return 1;
    }

    if(s->is_jar) {
        int n = ps_jar_read((const uint8_t *)data, len, jar_entry, s);

        if(n <= 0) {
            slot_fail(s, "jar is not readable");
            return 1;
        }
        printf("popsurf: applet jar %s, %d classes\n", url, n);

        /* A jar is normally self-contained, but an applet is free to name a
         * class that is not in it - so the graph is still walked, and anything
         * missing is fetched from the jar's own directory. */
        {
            int i;

            s->is_jar = 0;   /* misses now resolve over the network */
            for(i = 0; i < s->ncls; i++) {
                if(s->cls[i].arrived)
                    scan_refs(c, s, s->cls[i].bytes, s->cls[i].len);
            }
        }
        return try_run(c, s);
    }

    {
        ps_applet_class *e = want_class(c, s, s->main_name, 1);

        if(!e)
            e = bag_find(s, s->main_name);
        if(!e) {
            slot_fail(s, "out of class slots");
            return 1;
        }

        e->bytes = (uint8_t *)malloc(len + 1);
        if(!e->bytes) {
            slot_fail(s, "out of memory");
            return 1;
        }
        memcpy(e->bytes, data, len);
        e->len     = len;
        e->arrived = 1;
        e->failed  = 0;

        scan_refs(c, s, e->bytes, e->len);
    }

    return try_run(c, s);
}

/* Instructions one applet may execute per frame.
 *
 * This is the whole scheduling policy. Too low and animation crawls; too high
 * and a badly written applet eats the browser's frame. Fifty thousand is
 * roughly six times what the busiest test applet needs for a full repaint, so
 * an ordinary animation loop finishes its step and sleeps well inside it,
 * while a runaway loop is simply cut off and resumed next frame - it can waste
 * its own animation, never the page's responsiveness. */
#define APPLET_SLICE 50000

/* Instructions one repaint may take.
 *
 * The first run of an applet gets the full budget - it has a constructor,
 * init() and start() to get through. A repaint is just paint(), and the
 * heaviest test applet needs 7,495 instructions, so this is two hundred times
 * what any of them use and still bounds a runaway paint to something the
 * browser survives. At the previous twenty million, one applet in a loop
 * stalled the whole browser for seconds, and eight of them for most of a
 * minute. */
#define APPLET_PAINT_BUDGET 2000000

int ps_applet_cache_tick(ps_applet_cache *c, int dt_ms)
{
    int i, changed = 0;

    if(!c)
        return 0;

    for(i = 0; i < PS_APPLET_MAX; i++) {
        ps_applet_slot *s = &c->slots[i];

        if(s->state != SLOT_READY || !s->vm_live)
            continue;

        /* Off screen: leave it exactly as it is. Resuming where it left off
         * when it scrolls back into view is also what a browser tab does.
         *
         * Read but not cleared. Clearing here was right when every frame drew
         * the page, and wrong the moment an unchanged page started being
         * replayed instead: visibility is established by the element's draw,
         * and on a replayed frame that draw never runs. The flag went to zero
         * and never came back, so applets only advanced while the page was
         * being scrolled.
         *
         * It is cleared by ps_applet_cache_page_begin, which runs only before
         * a real redraw - the same event that will re-establish it. Between
         * redraws nothing has moved, so the last answer is still the right
         * one. */
        if(!s->visible)
            continue;

        g_running = s;

        /* Run a slice of the applet's animation thread. */
        if(ps_jvm_has_thread(&s->vm)) {
            ps_jrun r;

            PROF_T0();
            r = ps_jvm_pump(&s->vm, dt_ms, APPLET_SLICE);
            PROF_ADD(0);

            /* A thread that finishes never restarts, so the one thing worth
             * saying out loud is which one and why. Without this an applet
             * that stops is indistinguishable from one that is merely slow. */
            if(!ps_jvm_has_thread(&s->vm)) {
                printf("popsurf: applet %s thread ended (%s)\n", s->url,
                       r == PS_RUN_ERROR ? (s->vm.err[0] ? s->vm.err
                                                         : "error")
                                         : "run() returned");
            }
        }

        /* A vector applet paints during the page's draw, so there is nothing
         * further to do for it here - no surface to fill, no texture to
         * push. */
        if(s->vector) {
            g_running = NULL;
            continue;
        }

        /* Only repaint when the applet asked. An animation that has not
         * called repaint() must not cost a paint() and a texture upload - and
         * on this machine an unnecessary VRAM write is not merely wasteful,
         * it is the thing that collides with the network adapter. */
        if(!ps_jvm_take_repaint(&s->vm)) {
            g_running = NULL;
            continue;
        }

        ps_jgfx_init(&s->gfx, &s->surf, c->text);
        s->vm.budget = APPLET_PAINT_BUDGET;

        { PROF_T0();
        if(ps_jvm_paint(&s->vm, &s->gfx) != 0) {
            /* A repaint that fails stops the animation and leaves the last
             * good frame on screen, which is better than a box that goes
             * blank halfway through a page. */
            snprintf(s->why, sizeof s->why, "%s", s->vm.err);
            printf("popsurf: applet %s stopped: %s\n", s->url, s->vm.err);
            s->vm.failed = 0;
            s->vm.thread_done = 1;
            g_running = NULL;
            continue;
        }
        PROF_ADD(1); }

        { PROF_T0();
        if(upload(c, s) == 0)
            changed = 1;
        PROF_ADD(3); }
#ifdef PS_APPLET_PROFILE
        g_prof_active++;
#endif

        g_running = NULL;
    }

#ifdef PS_APPLET_PROFILE
    g_prof_frames++;
    if(timer_us_gettime64() - g_prof_at > 1000000ull) {
        uint64_t tot = g_prof[0] + g_prof[1] + g_prof[2] + g_prof[3];

        printf("popsurf: applet %llu fps, %.1f painting:",
               (unsigned long long)g_prof_frames,
               g_prof_frames ? (double)g_prof_active / (double)g_prof_frames
                             : 0.0);
        for(i = 0; i < 4; i++) {
            printf("  %s %lu us/f", g_prof_name[i],
                   (unsigned long)(g_prof_frames ? g_prof[i] / g_prof_frames : 0));
        }
        printf("  total %lu us/f\n",
               (unsigned long)(g_prof_frames ? tot / g_prof_frames : 0));

        for(i = 0; i < 4; i++)
            g_prof[i] = 0;
        g_prof_active = 0;
        g_prof_frames = 0;
        g_prof_at = timer_us_gettime64();
    }
#endif

    return changed;
}

/* --- painting as geometry ------------------------------------------------ */

int ps_applet_draw(ps_applet_cache *c, const char *url,
                   const struct ps_jvec_ops *ops, void *user,
                   int x, int y, int w, int h)
{
    ps_applet_slot *s = c ? find(c, url) : NULL;
    int             rc;

    if(!s || s->state != SLOT_READY || !s->vm_live || !s->vector)
        return 0;

    ps_jgfx_init_vector(&s->gfx, ops, user, x, y, w, h);

    /* Every frame, not only on repaint(). There is no cached texture to reuse
     * in this mode - the drawing goes straight into the page's display list,
     * which is rebuilt from nothing each frame. */
    s->vm.budget = APPLET_PAINT_BUDGET;

    g_running = s;
    rc = ps_jvm_paint(&s->vm, &s->gfx);
    g_running = NULL;

    if(rc != 0) {
        snprintf(s->why, sizeof s->why, "%s", s->vm.err);
        printf("popsurf: applet %s stopped: %s\n", s->url, s->vm.err);
        s->vm.failed = 0;
        s->vm.thread_done = 1;
        return 1;              /* it drew something; the failure is reported */
    }

    if(s->gfx.vec_unsupported) {
        /* It wants pixels. Fall back for good and let this frame come from
         * the software path, which has to run once to have anything to
         * show. */
        printf("popsurf: applet %s needs the software path\n", s->url);
        s->vector = 0;
        s->vm.repaint = 1;
        return 0;
    }

    /* Consumed here so the software tick does not also paint it. */
    ps_jvm_take_repaint(&s->vm);
    return 1;
}

/* --- input ---------------------------------------------------------------
 *
 * Two event models arrive here and exactly one of them runs, chosen by whether
 * the applet ever registered a listener. See the note above PS_LSN_MOUSE in
 * ps_jvm.h for why it is a choice and not a sum: real AWT retires the 1.0
 * methods the moment anything is registered, and an applet handed one click
 * twice is as broken as one handed none.
 *
 * Coordinates are the applet's own box, origin at its top left. The page hands
 * ps_document_applet_input a screen point, that adds the scroll to reach
 * document space, the element's box is document space too, and the box origin
 * comes off before the call gets here - so what x and y hold below is neither
 * screen nor document space but component space, which is what an applet
 * written against a Component starting at 0,0 expects.
 */

/* Puts the VM in a state where a callback can run, and hands back the Graphics
 * it is allowed to draw through.
 *
 * A handler may draw and may call repaint(). Giving it a context costs nothing
 * and means an applet that paints straight out of a click works rather than
 * failing on a null surface. */
static void event_prologue(ps_applet_cache *c, ps_applet_slot *s)
{
    ps_jgfx_init(&s->gfx, &s->surf, c->text);
    s->vm.gfx    = &s->gfx;
    s->vm.budget = 2L * 1000 * 1000;
    g_running    = s;
}

/* Calls one of the Java 1.0 event methods if the applet defines it.
 *
 * These are overrides on Component, so an applet that does not care simply
 * does not declare one and the lookup misses - which is why nothing has to be
 * registered and there is no listener list. */
static int call_event(ps_applet_cache *c, ps_applet_slot *s, const char *name,
                      const char *desc, int id, int a, int b)
{
    ps_jmethod *m;
    ps_jobj    *ev;
    ps_jslot    argv[4], r;
    int         rc;

    if(s->state != SLOT_READY || !s->vm_live || !s->vm.applet)
        return 0;

    m = ps_jclass_find_method(s->vm.applet->cls, name, desc);
    if(!m || !m->code)
        return 0;

    /* An Event object has to exist for the signature to be satisfied. Period
     * applets read the coordinates from the arguments, but they read the
     * modifiers and the click count off the Event, so it is filled in rather
     * than left as the blank it used to be. */
    ev = ps_jvm_new(&s->vm, ps_jvm_class(&s->vm, "java/awt/Event"));
    if(ev && ev->fields) {
        ev->fields[PS_EVF_SOURCE].o = s->vm.applet;
        ev->fields[PS_EVF_ID].i     = id;
        ev->fields[PS_EVF_MODS].i   = PS_MOD_BUTTON1;
        ev->fields[PS_EVF_CLICKS].i = 1;
        if(strstr(desc, ";II)")) {          /* two ints is a mouse position */
            ev->fields[PS_EVF_X].i = a;
            ev->fields[PS_EVF_Y].i = b;
        }
        else {
            ev->fields[PS_EVF_KEY].i  = a;
            ev->fields[PS_EVF_CHAR].i = a;
            ev->fields[PS_EVF_MODS].i = 0;
        }
    }

    memset(argv, 0, sizeof argv);
    argv[0].o = s->vm.applet;
    argv[1].o = ev;
    argv[2].i = a;
    argv[3].i = b;

    event_prologue(c, s);
    memset(&r, 0, sizeof r);
    rc = ps_jvm_call(&s->vm, s->vm.applet->cls, m, argv, 4, &r);
    g_running = NULL;

    if(rc != 0) {
        printf("popsurf: applet %s in %s: %s\n", s->url, name, s->vm.err);
        s->vm.failed = 0;
        return 0;
    }

    /* A 1.0 handler returns true when it consumed the event. Reaching a
     * handler at all is what the page needs to know, though: an applet that
     * returns false has still had the click, and giving the press to the page
     * as well would follow a link out from under it. */
    return 1;
}

/* The 1.1 key code for a key the browser reported in Java 1.0 terms.
 *
 * The browser speaks 1.0 because that is the model that was already wired up:
 * a character for anything that has one, and Event.UP through Event.F12 for
 * the keys that do not. 1.1 renumbered exactly those - it kept the characters
 * and gave the rest new codes - so this is the whole of the translation. */
static int vk_of(int key)
{
    /* Event.HOME(1000) through Event.PGDN(1003), then the arrows. */
    static const int nav[] = { 36, 35, 33, 34, 38, 40, 37, 39 };

    if(key >= 1000 && key <= 1007)
        return nav[key - 1000];
    if(key >= 1008 && key <= 1019)          /* Event.F1..F12 -> VK_F1..VK_F12 */
        return 112 + (key - 1008);
    if(key >= 'a' && key <= 'z')            /* VK_A is 'A', not 'a' */
        return key - 'a' + 'A';
    return key;
}

/* The 1.1 half: one browser gesture, expanded into the events AWT would have
 * generated for it, delivered to whatever registered. */
static int post_11_mouse(ps_applet_cache *c, ps_applet_slot *s, int x, int y,
                         int down, int dragging)
{
    int     ran = 0;

    if(s->state != SLOT_READY || !s->vm_live || !s->vm.applet)
        return 0;

    event_prologue(c, s);

    if(dragging) {
        s->drag_seen = 1;
        ran += ps_jvm_post_mouse(&s->vm, PS_EV_MOUSE_DRAGGED, x, y, 0,
                                 PS_MOD_BUTTON1);
    }
    else if(down > 0) {
        s->press_seen = 1;
        s->drag_seen  = 0;
        ran += ps_jvm_post_mouse(&s->vm, PS_EV_MOUSE_PRESSED, x, y, 1,
                                 PS_MOD_BUTTON1);
    }
    else if(down == 0) {
        /* Release, then the click AWT would have synthesised from it. Both
         * orderings and both click counts here are observed, not guessed: a
         * clean press/release gives released(1) then clicked(1), and one with
         * a drag in it gives released(0) and no click at all. */
        int clean = s->press_seen && !s->drag_seen;

        ran += ps_jvm_post_mouse(&s->vm, PS_EV_MOUSE_RELEASED, x, y,
                                 clean ? 1 : 0, PS_MOD_BUTTON1);
        if(clean)
            ran += ps_jvm_post_mouse(&s->vm, PS_EV_MOUSE_CLICKED, x, y, 1,
                                     PS_MOD_BUTTON1);
        s->press_seen = 0;
        s->drag_seen  = 0;
    }
    else {
        ran += ps_jvm_post_mouse(&s->vm, PS_EV_MOUSE_MOVED, x, y, 0, 0);
    }

    g_running = NULL;
    return ran > 0;
}

/* Delivers one crossing to whichever model the applet chose.
 *
 * Java 1.0 spells these mouseEnter and mouseExit and hands them a position;
 * 1.1 spells them mouseEntered and mouseExited and routes them through the
 * listener table. The position is the pointer's, which for an exit is the
 * point it left from - AWT reports the last position inside the component,
 * not the first outside it. */
static void post_crossing(ps_applet_cache *c, const char *url, int id,
                          int x, int y)
{
    ps_applet_slot *s = c ? find(c, url) : NULL;

    if(!s || s->state != SLOT_READY || !s->vm_live)
        return;

    if(ps_jvm_new_events_only(&s->vm)) {

        /* Same shape as every other event entry point: the listener may
         * repaint, and it needs a Graphics to repaint into. */
        event_prologue(c, s);
        ps_jvm_post_mouse(&s->vm, id, x, y, 0, 0);
        g_running = NULL;
        return;
    }

    call_event(c, s, id == PS_EV_MOUSE_ENTERED ? "mouseEnter" : "mouseExit",
               "(Ljava/awt/Event;II)Z", id, x, y);
}

void ps_applet_set_hover(ps_applet_cache *c, const char *url, int x, int y)
{
    char prev[PS_URL_MAX];

    if(!c)
        return;

    /* Nothing crossed anything. This is the common case by a wide margin -
     * it runs every frame - so it costs one compare and returns. */
    if(!strcmp(c->hover, url ? url : ""))
        return;

    memcpy(prev, c->hover, sizeof prev);
    snprintf(c->hover, sizeof c->hover, "%s", url ? url : "");

    /* Exit first, then enter. An applet that repaints on either must not see
     * itself entered before it has been told it was left. */
    if(prev[0])
        post_crossing(c, prev, PS_EV_MOUSE_EXITED, x, y);
    if(c->hover[0])
        post_crossing(c, c->hover, PS_EV_MOUSE_ENTERED, x, y);
}

void ps_applet_set_focus(ps_applet_cache *c, const char *url)
{
    if(c)
        snprintf(c->focus, sizeof c->focus, "%s", url ? url : "");
}

const char *ps_applet_focus(const ps_applet_cache *c)
{
    return (c && c->focus[0]) ? c->focus : NULL;
}

int ps_applet_mouse(ps_applet_cache *c, const char *url, int x, int y,
                    int down, int dragging)
{
    ps_applet_slot *s = c ? find(c, url) : NULL;
    const char     *name;
    int             id;

    if(!s)
        return 0;

    /* A press inside an applet gives it the keyboard, which is the only way
     * an applet ever gets one: there is no tab order and nothing else to
     * focus. Release does not, or clicking through an applet on the way to
     * somewhere else would leave the keys behind. */
    if(down > 0)
        ps_applet_set_focus(c, url);

    if(s->vm_live && ps_jvm_new_events_only(&s->vm))
        return post_11_mouse(c, s, x, y, down, dragging);

    if(dragging) {
        name = "mouseDrag";
        id   = PS_EV_MOUSE_DRAGGED;
    }
    else if(down > 0) {
        name = "mouseDown";
        id   = PS_EV_MOUSE_PRESSED;
    }
    else if(down == 0) {
        name = "mouseUp";
        id   = PS_EV_MOUSE_RELEASED;
    }
    else {
        name = "mouseMove";
        id   = PS_EV_MOUSE_MOVED;
    }

    return call_event(c, s, name, "(Ljava/awt/Event;II)Z", id, x, y);
}

int ps_applet_key(ps_applet_cache *c, const char *url, int key, int down)
{
    ps_applet_slot *s = c ? find(c, url) : NULL;
    int             ran;

    if(!s)
        return 0;

    if(!(s->vm_live && ps_jvm_new_events_only(&s->vm)))
        return call_event(c, s, down ? "keyDown" : "keyUp",
                          "(Ljava/awt/Event;I)Z",
                          down ? PS_EV_KEY_PRESSED : PS_EV_KEY_RELEASED,
                          key, 0);

    if(s->state != SLOT_READY || !s->vm_live || !s->vm.applet)
        return 0;

    {
        int has_char = key < 1000;
        int code     = vk_of(key);
        int ch       = has_char ? key : PS_CHAR_UNDEFINED;

        event_prologue(c, s);

        /* Three events for one key, which is the 1.1 model's whole reason for
         * existing: keyPressed says which key, keyTyped says which character,
         * and a key with no character - an arrow - produces no keyTyped at
         * all. Observed on a real JDK, including that Enter, Tab, Backspace
         * and Escape do get one, with the character equal to the code. */
        if(down) {
            ran = ps_jvm_post_key(&s->vm, PS_EV_KEY_PRESSED, code, ch, 0);
            if(has_char)
                ran += ps_jvm_post_key(&s->vm, PS_EV_KEY_TYPED,
                                       PS_VK_UNDEFINED, ch, 0);
        }
        else {
            ran = ps_jvm_post_key(&s->vm, PS_EV_KEY_RELEASED, code, ch, 0);
        }

        g_running = NULL;
    }
    return ran > 0;
}

/* --- test affordance ----------------------------------------------------- */

int ps_applet_dump_ppm(ps_applet_cache *c, const char *url, const char *path)
{
    ps_applet_slot *s = c ? find(c, url) : NULL;
    FILE           *f;
    int             i, n;

    if(!s || !s->surf.px)
        return -1;

    f = fopen(path, "wb");
    if(!f)
        return -1;

    n = s->surf.w * s->surf.h;
    fprintf(f, "P6\n%d %d\n255\n", s->surf.w, s->surf.h);
    for(i = 0; i < n; i++) {
        uint32_t      p = s->surf.px[i];
        unsigned char rgb[3];

        rgb[0] = (unsigned char)((p >> 16) & 0xff);
        rgb[1] = (unsigned char)((p >>  8) & 0xff);
        rgb[2] = (unsigned char)( p        & 0xff);
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    return 0;
}

void ps_applet_heap_report(ps_applet_cache *c, const char *url)
{
    ps_applet_slot *s = c ? find(c, url) : NULL;

    if(!s || !s->vm_live)
        return;

    printf("heap: %ld objects, %ld bytes live, %ld collections\n",
           s->vm.objects, s->vm.bytes, s->vm.gc_runs);
}
