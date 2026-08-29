/* Offscreen images. See ps_joff.h for what an applet uses them for.
 *
 * --- who owns the pixels -------------------------------------------------
 *
 * An applet allocates its back buffer in init() and holds it for the rest of
 * its life, so the buffer outlives every call that touches it and the question
 * is only who frees it and when.
 *
 * The collector cannot: it has no finaliser hook, and ps_jobj::owns_native is
 * a plain free() of one pointer. That is enough for a String, whose bytes are
 * one allocation nobody else points at, and it is not enough here, because
 * Image.getGraphics() hands out contexts that point *into* the surface. If the
 * Image were swept while one of those contexts was still live, the next
 * drawLine would write through a freed pointer. This codebase has already paid
 * for one native pointer outliving its owner - see the note on owns_native in
 * ps_jvm.h - and this is the same trap one level up.
 *
 * So the surfaces live in a table this file owns, and nothing else ever frees
 * them. What the collector still gives us is a way to find out when one has
 * become garbage, without any hook at all: vm->gc_head is the list of every
 * object that still exists, because the sweep unlinks an object before it
 * frees it. Walking it answers "is anything still holding this surface"
 * exactly. That is reap(), and it runs when a slot is wanted and none is free,
 * which is precisely the resize case -
 *
 *     if (off == null || w != size().width) off = createImage(w, h);
 *
 * - where the buffer being replaced must not be leaked.
 */
#include "ps_joff.h"

#include <stdlib.h>
#include <string.h>

/* Eight buffers, four megabytes of pixels between them. An applet uses one;
 * two is a lot; eight is already generous for a machine with twelve megabytes
 * of main memory, and a bound is what stops a hostile page allocating until
 * the browser dies. */
#define JOFF_SLOTS     8
#define JOFF_MAX_PX    (512L * 1024L)          /* one image, 2 MB of ARGB */
#define JOFF_TOTAL_PX  (1024L * 1024L)         /* the table, 4 MB */

/* An offscreen image, and the applet that asked for it. */
typedef struct {
    ps_jvm     *vm;        /* owner; NULL when the slot is free */
    ps_jsurface s;
} joff_slot;

static joff_slot           g_slots[JOFF_SLOTS];
static long                g_px_used;
static const ps_jtext_ops *g_text;

/* A Graphics this file made.
 *
 * ps_jre.c reaches a Graphics context by casting ps_jobj::native straight to
 * ps_jgfx *, so the ps_jgfx has to be the first member and has to stay there;
 * everything after it is invisible to that cast, which is exactly what is
 * wanted. */
#define JOFF_GFX_MAGIC 0x6a4f4647u             /* 'jOFG' */

typedef struct {
    ps_jgfx    g;
    uint32_t   magic;
    joff_slot *slot;       /* NULL for a Component.getGraphics() context */
} joff_gfx;

void ps_joff_set_text(const ps_jtext_ops *ops)
{
    g_text = ops;
}

/* --- matching ------------------------------------------------------------ */

static int is(const char *name, const char *desc, const char *n,
              const char *d)
{
    if(strcmp(name, n))
        return 0;
    return d ? !strcmp(desc, d) : 1;
}

/* Counts the plain-int parameters of a descriptor.
 *
 * Not a scan for 'I': "Ljava/awt/Image;" has one in it, and the difference
 * between drawImage(img, x, y, obs) and drawImage(img, x, y, w, h, obs) is the
 * only thing telling a blit from a scaled blit. */
static int int_params(const char *d)
{
    int n = 0;

    if(!d)
        return 0;
    if(*d == '(')
        d++;
    while(*d && *d != ')') {
        while(*d == '[')
            d++;
        if(*d == 'L') {
            while(*d && *d != ';')
                d++;
            if(*d)
                d++;
            continue;
        }
        if(*d == 'I')
            n++;
        d++;
    }
    return n;
}

/* --- the table ----------------------------------------------------------- */

static joff_slot *slot_of_image(const ps_jobj *o)
{
    int i;

    /* An offscreen Image is exactly an Image whose native pointer is one of
     * our slots. A network image carries a small integer handle in ->len and
     * leaves ->native NULL, which is what keeps the two apart everywhere
     * below. */
    if(!o || !o->native)
        return NULL;
    for(i = 0; i < JOFF_SLOTS; i++) {
        if((const void *)&g_slots[i] == o->native)
            return &g_slots[i];
    }
    return NULL;
}

static joff_gfx *own_gfx(const ps_jobj *o)
{
    /* This file is the only thing that ever sets owns_native on a
     * java.awt.Graphics - the context the browser hands paint() is borrowed
     * and says so - and that combination is what makes the block safe to look
     * inside. Probing any owns_native block would read off the end of, say, a
     * one-byte String. */
    if(!o || o->kind != PS_OBJ_INSTANCE || !o->native || !o->owns_native)
        return NULL;
    if(!o->cls || !o->cls->name || strcmp(o->cls->name, "java/awt/Graphics"))
        return NULL;
    if(((const joff_gfx *)o->native)->magic != JOFF_GFX_MAGIC)
        return NULL;
    return (joff_gfx *)o->native;
}

static void slot_free(joff_slot *s)
{
    g_px_used -= (long)s->s.w * s->s.h;
    if(g_px_used < 0)
        g_px_used = 0;
    ps_jsurface_free(&s->s);
    s->vm = NULL;
}

/* Frees the slots of vm that nothing on its heap refers to any more. */
static void reap(ps_jvm *vm)
{
    unsigned char used[JOFF_SLOTS];
    const ps_jobj *o;
    int            i;

    memset(used, 0, sizeof used);

    for(o = vm->gc_head; o; o = o->gc_next) {
        joff_slot *s = slot_of_image(o);

        /* A Graphics counts too. An applet that dropped the Image but kept the
         * context it drew with has made a buffer it can never blit, but that
         * is a wasted picture, not a licence to free the memory underneath a
         * live pointer. */
        if(!s) {
            joff_gfx *jg = own_gfx(o);

            if(jg)
                s = jg->slot;
        }
        if(s && s->vm == vm)
            used[s - g_slots] = 1;
    }

    for(i = 0; i < JOFF_SLOTS; i++) {
        if(g_slots[i].vm == vm && !used[i])
            slot_free(&g_slots[i]);
    }
}

void ps_joff_release(ps_jvm *vm)
{
    int i;

    for(i = 0; i < JOFF_SLOTS; i++) {
        if(g_slots[i].vm == vm)
            slot_free(&g_slots[i]);
    }
}

static joff_slot *slot_take(ps_jvm *vm, int w, int h)
{
    long want;
    int  i, pass;

    /* Each dimension is bounded before they are multiplied. long is 32 bits on
     * SH-4, so createImage(100000, 100000) - which an applet can name and a
     * hostile page will - overflows the product and passes a budget check it
     * should fail. */
    if(w <= 0 || h <= 0 || w > 4096 || h > 4096)
        return NULL;

    want = (long)w * h;
    if(want > JOFF_MAX_PX)
        return NULL;

    for(pass = 0; pass < 3; pass++) {
        if(pass == 1) {
            /* Nothing free. Whatever the applet dropped may already have been
             * swept, in which case walking the heap alone recovers it. */
            reap(vm);
        }
        else if(pass == 2) {
            /* Still nothing, so the dead Images are dead but not yet swept.
             *
             * Forcing a collection here is a last resort rather than the first
             * move because ps_jvm.c's interpreter keeps the running frame's
             * stack pointer in a C local and only writes it back to the frame
             * when it suspends: a collection started from inside a native call
             * therefore under-scans the top frame's operand stack. That is a
             * pre-existing hazard on every path that allocates from a native -
             * new Color() reaches it too - and the way not to make it worse is
             * to arrive here as rarely as possible. */
            ps_jvm_gc(vm);
            reap(vm);
        }

        for(i = 0; i < JOFF_SLOTS; i++) {
            if(g_slots[i].vm)
                continue;
            if(g_px_used + want > JOFF_TOTAL_PX)
                break;                 /* a free slot is no use without room */
            if(ps_jsurface_init(&g_slots[i].s, w, h) != 0)
                return NULL;

            /* AWT fills a new offscreen image with the component's background
             * colour, and leaves it opaque. Both halves matter: an applet that
             * draws sprites without clearing first shows the background
             * through the gaps, and a buffer left at alpha zero would blit as
             * nothing at all, because ps_jgfx skips transparent source pixels.
             * White is the applet background this runtime already assumes -
             * see clearRect in ps_jre.c. */
            ps_jsurface_clear(&g_slots[i].s, 0xffffffffu);

            g_slots[i].vm = vm;
            g_px_used += want;
            return &g_slots[i];
        }
    }
    return NULL;
}

const uint32_t *ps_joff_image_px(const ps_jobj *img, int *w, int *h,
                                 int *stride)
{
    joff_slot *s = slot_of_image(img);

    if(!s || !s->s.px)
        return NULL;

    *w      = s->s.w;
    *h      = s->s.h;
    *stride = s->s.stride;
    return s->s.px;
}

/* --- contexts ------------------------------------------------------------ */

static ps_jgfx *gfx_of(ps_jvm *vm, ps_jobj *o)
{
    /* The same rule ps_jre.c uses, and it has to stay the same rule: a
     * Graphics with a context of its own uses it, and the bare one the browser
     * handed paint() falls back to the VM's. */
    if(o && o->native)
        return (ps_jgfx *)o->native;
    return vm->gfx;
}

/* Wraps a fresh context in a java.awt.Graphics the applet can hold.
 *
 * The block is owned by the object, so the heap sweep reclaims it - which is
 * right here and wrong for the surface, because a ps_jgfx is self-contained
 * and points at a surface it does not own. Repeated getGraphics() calls
 * therefore cost one small allocation each and are independent of one another,
 * which is what the real API promises. */
static ps_jobj *wrap_gfx(ps_jvm *vm, const ps_jgfx *src, joff_slot *slot)
{
    ps_jobj  *o;
    joff_gfx *jg;

    o = ps_jvm_new(vm, ps_jvm_class(vm, "java/awt/Graphics"));
    if(!o)
        return NULL;

    jg = (joff_gfx *)malloc(sizeof *jg);
    if(!jg)
        return NULL;

    jg->g     = *src;
    jg->magic = JOFF_GFX_MAGIC;
    jg->slot  = slot;

    o->native      = jg;
    o->owns_native = 1;

    /* The sweep charges an owned native block as ->len + 1 bytes, because the
     * only one it was written for is a String's. Sizing ->len to match keeps
     * the collector's byte count honest; without it every per-frame Graphics
     * an animated applet allocates is invisible to the threshold that decides
     * when to collect. Nothing reads ->len on a Graphics. */
    o->len = (int32_t)(sizeof *jg - 1);
    vm->bytes += (long)sizeof *jg;

    return o;
}

/* --- java.awt.Image ------------------------------------------------------ */

static int image(ps_jvm *vm, const char *n, const char *d, ps_jslot *args,
                 int nargs, ps_jslot *ret, int *handled)
{
    ps_jobj   *self = nargs >= 1 ? args[0].o : NULL;
    joff_slot *s    = slot_of_image(self);

    if(!s)
        return 0;                      /* a network image; ps_jre.c has it */

    if(is(n, d, "getGraphics", NULL)) {
        ps_jgfx g;

        /* A fresh context every time: identity translate, clip the whole
         * image, black pen. */
        ps_jgfx_init(&g, &s->s, vm->gfx && vm->gfx->text ? vm->gfx->text
                                                         : g_text);
        ret->o = wrap_gfx(vm, &g, s);
        *handled = 1;
        return 0;
    }
    if(is(n, d, "getWidth", NULL)) {
        ret->i = s->s.w;
        *handled = 1;
        return 0;
    }
    if(is(n, d, "getHeight", NULL)) {
        ret->i = s->s.h;
        *handled = 1;
        return 0;
    }
    if(is(n, d, "flush", NULL)) {
        /* flush() discards a *decoded* image so it will be fetched again. An
         * offscreen one has no source to be fetched from - the applet's own
         * drawing is all there is - so throwing the pixels away here would
         * only lose them. Applets call it before dropping the reference; the
         * reap above is what actually reclaims the memory. */
        *handled = 1;
        return 0;
    }
    return 0;
}

/* --- Graphics.drawImage, for an offscreen source ------------------------- */

static int draw_image(ps_jvm *vm, const char *d, ps_jslot *args, int nargs,
                      ps_jslot *ret, int *handled)
{
    ps_jobj   *src = nargs >= 2 ? args[1].o : NULL;
    joff_slot *s   = slot_of_image(src);
    ps_jgfx   *g;
    int        ni;

    if(!s)
        return 0;                      /* not ours; ps_jre.c blits it */

    g = gfx_of(vm, nargs >= 1 ? args[0].o : NULL);
    if(!g) {
        *handled = 1;
        return 0;
    }

    ni = int_params(d);

    /* this, the image, and at least x and y. Anything shorter is a descriptor
     * this runtime has not seen and must not index past. */
    if(nargs < 4 || (ni == 4 && nargs < 6)) {
        *handled = 1;
        return 0;
    }

    /* The background overload, drawImage(img, x, y, bg, obs), paints bg
     * wherever the image is transparent. Ours never is, so filling the
     * destination first and blitting over it gives the same picture and needs
     * no second code path in the rasteriser. */
    if(strstr(d, "Ljava/awt/Color;") && nargs > (ni == 4 ? 6 : 4)) {
        ps_jobj *c = args[ni == 4 ? 6 : 4].o;

        if(c) {
            uint32_t save = g->color;

            g->color = (uint32_t)c->len;
            if(ni == 4)
                ps_jgfx_fill_rect(g, args[2].i, args[3].i, args[4].i,
                                  args[5].i);
            else
                ps_jgfx_fill_rect(g, args[2].i, args[3].i, s->s.w, s->s.h);
            g->color = save;
        }
    }

    if(ni == 4)
        ps_jgfx_draw_image_scaled(g, s->s.px, s->s.w, s->s.h, s->s.stride,
                                  args[2].i, args[3].i, args[4].i, args[5].i);
    else
        ps_jgfx_draw_image(g, s->s.px, s->s.w, s->s.h, s->s.stride,
                           args[2].i, args[3].i);

    /* drawImage returns whether the image is complete. An offscreen one always
     * is, and an applet that draws only when told true would otherwise never
     * draw at all. */
    ret->i = 1;
    *handled = 1;
    return 0;
}

/* --- java.awt.MediaTracker ----------------------------------------------- *
 *
 * The tracker exists so an applet can block until its artwork has arrived,
 * because AWT's image loading is asynchronous and drawImage on a half-loaded
 * image draws half a picture.
 *
 * Here the waits return immediately, and that is the correct answer rather
 * than a shortcut: ps_applet.c has already fetched and decoded every image the
 * applet asked for before the interpreter runs, so by the time addImage is
 * called the loading the tracker is there to wait for has finished. Blocking
 * would be waiting for an event that has been and gone - and there is no
 * second thread here to deliver it, so it would wait forever.
 *
 * Nothing is recorded, because nothing asked can have a different answer:
 * every image is complete and none of them errored.
 */
static int tracker(const char *n, const char *d, ps_jslot *ret, int *handled)
{
    *handled = 1;

    if(is(n, d, "<init>", NULL) ||
       is(n, d, "addImage", NULL) || is(n, d, "removeImage", NULL))
        return 0;

    if(is(n, d, "waitForID", NULL) || is(n, d, "waitForAll", NULL))
        return 0;

    if(is(n, d, "checkID", NULL) || is(n, d, "checkAll", NULL)) {
        ret->i = 1;                    /* everything is loaded */
        return 0;
    }
    if(is(n, d, "isErrorAny", NULL) || is(n, d, "isErrorID", NULL)) {
        ret->i = 0;
        return 0;
    }
    if(is(n, d, "getErrorsAny", NULL) || is(n, d, "getErrorsID", NULL)) {
        ret->o = NULL;
        return 0;
    }
    if(is(n, d, "statusID", NULL) || is(n, d, "statusAll", NULL)) {
        ret->i = 8;                    /* COMPLETE */
        return 0;
    }

    /* The status bits, read through getstatic. */
    if(!strcmp(n, "LOADING"))  { ret->i = 1; return 0; }
    if(!strcmp(n, "ABORTED"))  { ret->i = 2; return 0; }
    if(!strcmp(n, "ERRORED"))  { ret->i = 4; return 0; }
    if(!strcmp(n, "COMPLETE")) { ret->i = 8; return 0; }

    *handled = 0;
    return 0;
}

/* --- dispatch ------------------------------------------------------------ */

int ps_joff_call(ps_jvm *vm, const char *cls, const char *name,
                 const char *desc, ps_jslot *args, int nargs, ps_jslot *ret,
                 int *handled)
{
    *handled = 0;

    if(!strcmp(cls, "java/awt/MediaTracker"))
        return tracker(name, desc, ret, handled);

    if(!strcmp(cls, "java/awt/Image"))
        return image(vm, name, desc, args, nargs, ret, handled);

    if(!strcmp(cls, "java/awt/Graphics")) {
        if(is(name, desc, "drawImage", NULL))
            return draw_image(vm, desc, args, nargs, ret, handled);
        return 0;
    }

    /* createImage and getGraphics are Component's, and an applet reaches them
     * through whichever of its ancestors the compiler named. */
    if(!strcmp(cls, "java/awt/Component") || !strcmp(cls, "java/applet/Applet") ||
       !strcmp(cls, "java/awt/Panel")     || !strcmp(cls, "java/awt/Container")) {

        if(is(name, desc, "createImage", "(II)Ljava/awt/Image;")) {
            joff_slot *s = slot_take(vm, nargs >= 3 ? args[1].i : 0,
                                         nargs >= 3 ? args[2].i : 0);
            ps_jobj   *img;

            /* Null when there is no room, which is a value the real API also
             * returns - a component with no peer yet answers null - so applets
             * of the period are written to survive it. Better than a surface
             * that is not the size it was asked for. */
            if(!s) {
                ret->o = NULL;
                *handled = 1;
                return 0;
            }

            img = ps_jvm_new(vm, ps_jvm_class(vm, "java/awt/Image"));
            if(!img) {
                slot_free(s);
                ret->o = NULL;
                *handled = 1;
                return 0;
            }

            /* Borrowed, not owned: the surface belongs to the table above and
             * the sweep must not free it, because a Graphics handed out by
             * getGraphics() points into the same pixels. */
            img->native      = s;
            img->owns_native = 0;
            img->len         = 0;      /* not a network image handle */

            ret->o = img;
            *handled = 1;
            return 0;
        }

        if(is(name, desc, "getGraphics", "()Ljava/awt/Graphics;")) {
            /* The component's own context, copied so the applet can translate
             * and clip it without disturbing the one the browser is painting
             * with. Copied rather than rebuilt because it may be in vector
             * mode, where there is no surface to rebuild it from. */
            if(!vm->gfx) {
                ret->o = NULL;
                *handled = 1;
                return 0;
            }
            ret->o = wrap_gfx(vm, vm->gfx, NULL);
            *handled = 1;
            return 0;
        }
    }

    return 0;
}
