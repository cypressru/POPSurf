/* java.awt.image. See ps_jimg.h for what an applet uses this for and why
 * production is deferred rather than done at createImage().
 *
 * --- where the state lives ------------------------------------------------
 *
 * Every object here is an instance of a runtime class with instance slots, and
 * everything it remembers is in those slots. That is not the obvious choice -
 * ps_jawt.c hangs a malloc'd block off ps_jobj::native and ps_joff.c owns a
 * table of surfaces - and it is the right one here for a reason neither of
 * those files has: a producer *points at other Java objects*. A
 * FilteredImageSource holds its upstream producer and its filter, and both must
 * stay alive exactly as long as it does.
 *
 * The collector cannot see a reference held in C. ps_jawt.c says so where it
 * copies a widget's label out of the String object rather than keeping the
 * object, and a producer graph is the same hazard with sharper teeth, because
 * the applet builds the graph in init() and does not touch it again until
 * paint() - several collections later. Instance slots are marked (ps_jvm.c's
 * mark() offers a runtime class's slots to mark_maybe, the same conservative
 * treatment the event objects get), so a graph built out of slots keeps itself
 * alive with no help from this file at all.
 *
 * The one exception is the pixel block of a materialised image, which holds no
 * references and is a plain owned native allocation. ps_joff.c cannot do that
 * with its surfaces because Image.getGraphics() hands out contexts pointing
 * into them; nothing here ever hands out a pointer into a produced image, so
 * the ordinary sweep is enough.
 */
#include "ps_jimg.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ps_applet.h"
#include "ps_joff.h"

/* Same shape as ps_jvm.c's, which is static there. */
static int fail(ps_jvm *vm, const char *fmt, ...)
{
    va_list ap;

    if(!vm->failed) {
        va_start(ap, fmt);
        vsnprintf(vm->err, sizeof vm->err, fmt, ap);
        va_end(ap);
        vm->failed = 1;
    }
    return -1;
}

/* Matches on name and, when it matters, descriptor - the helper ps_jre.c,
 * ps_joff.c and ps_jawt.c all use. NULL for the descriptor accepts any
 * overload. */
static int is(const char *name, const char *desc, const char *n,
              const char *d)
{
    if(strcmp(name, n))
        return 0;
    return d ? !strcmp(desc, d) : 1;
}

/* --- limits --------------------------------------------------------------
 *
 * A produced image is charged to the ordinary Java heap and reclaimed by the
 * ordinary sweep, so the collector bounds the total. What it cannot bound is a
 * single allocation named by a hostile page - createImage of a
 * MemoryImageSource claiming to be 40000 by 40000 - so each dimension is
 * bounded before they are multiplied. long is 32 bits on SH-4 and the product
 * would otherwise wrap into a size that passes the check. */
#define JIMG_MAX_DIM 4096
#define JIMG_MAX_PX  (512L * 1024L)      /* 2 MB of ARGB, as ps_joff.c */

/* How deep a producer graph may nest. Chaining two or three filters is
 * ordinary; sixteen is far past anything an applet does and stops a graph that
 * somehow refers to itself from recursing until the C stack is gone. */
#define JIMG_MAX_DEPTH 16

/* --- object layouts ------------------------------------------------------
 *
 * Slot numbers, not field tables: these are runtime classes with no class
 * file, exactly like the event objects in ps_jvm.h, and the indices are
 * spelled out because several functions here have to agree on them.
 *
 * java.awt.image.RGBImageFilter is the one exception and is a real class with
 * a real field table - see register_classes.
 */
#define SRC_IMAGE   0            /* ps/awt/imgsrc: the Image it reads */

#define PIMG_PROD   0            /* ps/awt/image: the ImageProducer */

#define FIS_SRC     0            /* FilteredImageSource */
#define FIS_FILTER  1

#define CROP_X      0            /* CropImageFilter */
#define CROP_Y      1
#define CROP_W      2
#define CROP_H      3

#define MIS_PIX     0            /* MemoryImageSource */
#define MIS_CM      1
#define MIS_W       2
#define MIS_H       3
#define MIS_OFF     4
#define MIS_SCAN    5

/* One layout serves ColorModel and both its subclasses, because the only
 * thing that reads them is the pixel loop below and it switches on the kind
 * anyway. A and B and C and D are masks for a DirectColorModel and palette
 * arrays for an IndexColorModel; the collector's conservative marking is what
 * makes one slot able to hold either. */
#define CM_KIND     0
#define CM_A        1            /* red mask   | reds   */
#define CM_B        2            /* green mask | greens */
#define CM_C        3            /* blue mask  | blues  */
#define CM_D        4            /* alpha mask | alphas */
#define CM_BITS     5
#define CM_SIZE     6            /* IndexColorModel map size */
#define CM_SLOTS    7

enum { CM_DEFAULT = 0, CM_DIRECT, CM_INDEX };

#define PG_SRC      0            /* PixelGrabber */
#define PG_PIX      1
#define PG_X        2
#define PG_Y        3
#define PG_W        4
#define PG_H        5
#define PG_OFF      6
#define PG_SCAN     7
#define PG_STAT     8
#define PG_SLOTS    9

/* java.awt.image.ImageObserver's status bits, as an applet's imageUpdate
 * compares against them. */
#define OBS_ALLBITS   32
#define OBS_FRAMEBITS 16

/* The pixels of a materialised image.
 *
 * Owned by the ps_jobj and freed by the sweep. The magic word is what makes it
 * safe to look inside a block reached through ps_jobj::native, the same guard
 * ps_joff.c's own_gfx uses and for the same reason: probing any owned native
 * block would read off the end of, say, a one-byte String. */
#define JIMG_MAGIC 0x6a494d47u           /* 'jIMG' */

typedef struct {
    uint32_t magic;
    int32_t  w, h;
    uint32_t px[1];
} jimg_pix;

#define JIMG_BYTES(n) (sizeof(jimg_pix) - sizeof(uint32_t) + \
                       (size_t)(n) * sizeof(uint32_t))

/* --- class registration --------------------------------------------------
 *
 * ps_jvm.c has a fixed list of the classes the runtime implements in C, and
 * this file deliberately does not appear in it: several agents edit that file
 * at once and a new name there is a merge conflict for all of them. ps_jawt.c
 * reached the same conclusion and this is its mechanism, hooked on the same
 * call - java/applet/Applet.<init>, which every applet makes from the first
 * instruction of its own constructor, before init() can reach a `new
 * CropImageFilter`.
 */
typedef struct { const char *name; uint16_t slots; } jimg_cls;

static const jimg_cls g_classes[] = {
    /* The interfaces exist so a class naming one in `implements`, or a
     * checkcast against one, resolves. Nothing is ever dispatched to them. */
    { "java/awt/image/ImageProducer",      0 },
    { "java/awt/image/ImageConsumer",      0 },
    { "java/awt/image/ImageObserver",      0 },

    { "java/awt/image/ImageFilter",        0 },
    { "java/awt/image/CropImageFilter",    4 },
    { "java/awt/image/FilteredImageSource", 2 },
    { "java/awt/image/MemoryImageSource",  6 },
    { "java/awt/image/ColorModel",         CM_SLOTS },
    { "java/awt/image/DirectColorModel",   CM_SLOTS },
    { "java/awt/image/IndexColorModel",    CM_SLOTS },
    { "java/awt/image/PixelGrabber",       PG_SLOTS },

    /* Two classes with no name in the real API.
     *
     * getSource() returns an ImageProducer the JDK keeps private, and an image
     * made from a producer is a private class there too. Giving them names in
     * a ps/ package rather than reusing java/awt/Image keeps them out of the
     * way of ps_joff.c and ps_jre.c, both of which recognise an Image by what
     * its native pointer and length field hold. */
    { "ps/awt/imgsrc",                     1 },
    { "ps/awt/image",                      1 },
    { NULL, 0 }
};

static ps_jclass *find_class(ps_jvm *vm, const char *name)
{
    int i;

    for(i = 0; i < vm->nclasses; i++) {
        if(!strcmp(vm->classes[i]->name, name))
            return vm->classes[i];
    }
    return NULL;
}

/* Builds the placeholder ps_jvm.c's make_native would have built. The name is
 * copied because, unlike a parsed class, there is no file image to point
 * into. */
static ps_jclass *make_shell(ps_jvm *vm, const char *name, uint16_t slots,
                             int native)
{
    ps_jclass *c;

    if(vm->nclasses >= PS_JVM_MAX_CLASSES)
        return NULL;

    c = (ps_jclass *)calloc(1, sizeof *c);
    if(!c)
        return NULL;

    c->raw = (uint8_t *)malloc(strlen(name) + 1);
    if(!c->raw) {
        free(c);
        return NULL;
    }
    strcpy((char *)c->raw, name);
    c->name       = (const char *)c->raw;
    c->native     = native;
    c->inst_slots = slots;

    vm->classes[vm->nclasses++] = c;
    return c;
}

/* java.awt.image.RGBImageFilter, which cannot be a native placeholder.
 *
 * The whole point of it is to be subclassed, and the standard subclass sets a
 * public field in its constructor:
 *
 *     class Grey extends RGBImageFilter {
 *         Grey() { canFilterIndexColorModel = true; }
 *         public int filterRGB(int x, int y, int rgb) { ... }
 *     }
 *
 * That is a putfield, and the interpreter's putfield has no path through the
 * native-call machinery at all - it looks the name up in the class's field
 * table and fails if it is not there. So this is an ordinary class with a real
 * field table and no methods, whose superclass is the native ImageFilter. A
 * call on it finds no method in its own chain and the interpreter's fallback
 * for an inherited method landing on a native ancestor brings it here.
 *
 * ps_jawt.c does the same for Insets and GridBagConstraints; the note there
 * explains the mechanism at length.
 */
static void make_rgb_filter(ps_jvm *vm, ps_jclass *base)
{
    ps_jclass *c;

    if(find_class(vm, "java/awt/image/RGBImageFilter"))
        return;

    c = make_shell(vm, "java/awt/image/RGBImageFilter", 1, 0);
    if(!c)
        return;

    c->super  = base;
    c->fields = (ps_jfield *)calloc(1, sizeof(ps_jfield));
    if(!c->fields) {
        c->inst_slots = 0;
        return;                    /* leaves a class with no fields; the
                                      putfield below it will say so */
    }

    /* The name and descriptor are string literals rather than slices of a
     * class file image, which is the one way this differs from a parsed class.
     * ps_jclass_free never touches them. */
    c->fields[0].name = "canFilterIndexColorModel";
    c->fields[0].desc = "Z";
    c->fields[0].kind = PS_T_BOOL;
    c->fields[0].slot = 0;
    c->field_count    = 1;
}

static void register_classes(ps_jvm *vm)
{
    ps_jclass *base;
    int        i;

    /* Idempotent, and cheap enough at applet-construction time: this runs once
     * per applet, not once per call. */
    if(find_class(vm, "ps/awt/image"))
        return;

    for(i = 0; g_classes[i].name; i++) {
        if(!find_class(vm, g_classes[i].name))
            make_shell(vm, g_classes[i].name, g_classes[i].slots, 1);
    }

    base = find_class(vm, "java/awt/image/ImageFilter");
    if(base)
        make_rgb_filter(vm, base);
}

/* --- recognising objects ------------------------------------------------- */

static int class_named(const ps_jobj *o, const char *name)
{
    return o && o->cls && o->cls->name && !strcmp(o->cls->name, name);
}

/* Whether name appears anywhere in o's superclass chain. An applet subclasses
 * MemoryImageSource and RGBImageFilter routinely, so identifying either by the
 * object's own class name alone would miss every real use. */
static int derives_from(const ps_jobj *o, const char *name)
{
    const ps_jclass *c;

    for(c = o ? o->cls : NULL; c; c = c->super) {
        if(c->name && !strcmp(c->name, name))
            return 1;
    }
    return 0;
}

/* The pixel block of an image this file produced, or NULL. */
static jimg_pix *pix_of(const ps_jobj *o)
{
    if(!o || o->kind != PS_OBJ_INSTANCE || !o->native || !o->owns_native)
        return NULL;
    if(!class_named(o, "ps/awt/image"))
        return NULL;
    if(((const jimg_pix *)o->native)->magic != JIMG_MAGIC)
        return NULL;
    return (jimg_pix *)o->native;
}

static int is_produced(const ps_jobj *o)
{
    return class_named(o, "ps/awt/image");
}

/* Whether o is any kind of java.awt.Image.
 *
 * Three kinds exist and they are told apart by what they carry rather than by
 * their class: an offscreen image points at one of ps_joff.c's surfaces, a
 * produced one points at a block of ours, and a network one carries a small
 * integer handle in ->len with a NULL native pointer. */
static int is_image(ps_jvm *vm, const ps_jobj *o)
{
    int w = 0, h = 0, stride = 0;

    (void)vm;
    if(!o || o->kind != PS_OBJ_INSTANCE)
        return 0;
    if(is_produced(o))
        return 1;
    if(ps_joff_image_px(o, &w, &h, &stride))
        return 1;
    return class_named(o, "java/awt/Image");
}

/* --- colour models ------------------------------------------------------- */

/* Scales an n-bit component up to eight, the way DirectColorModel does.
 *
 * Checked against a real JDK rather than assumed: a 5-bit 3 comes back as 25,
 * not the 24 that truncation gives, so the rounding is half-up and not a
 * shift. Sixty-four intermediate values of a 5-6-5 model were compared and all
 * of them match. The arithmetic is 64-bit because a 32-bit mask times 510
 * overflows. */
static uint32_t scale8(uint32_t v, uint32_t maxv)
{
    if(!maxv)
        return 0;
    if(maxv == 255)
        return v;
    return (uint32_t)(((int64_t)v * 510 + maxv) / (2 * (int64_t)maxv));
}

static int mask_shift(uint32_t m)
{
    int s = 0;

    if(!m)
        return 0;
    while(!(m & 1u)) {
        m >>= 1;
        s++;
    }
    return s;
}

static uint32_t direct_argb(const ps_jslot *f, uint32_t v)
{
    uint32_t rm = (uint32_t)f[CM_A].i, gm = (uint32_t)f[CM_B].i;
    uint32_t bm = (uint32_t)f[CM_C].i, am = (uint32_t)f[CM_D].i;
    uint32_t r, g, b, a;

    r = scale8((v & rm) >> mask_shift(rm), rm >> mask_shift(rm));
    g = scale8((v & gm) >> mask_shift(gm), gm >> mask_shift(gm));
    b = scale8((v & bm) >> mask_shift(bm), bm >> mask_shift(bm));
    a = am ? scale8((v & am) >> mask_shift(am), am >> mask_shift(am)) : 255u;

    return (a << 24) | (r << 16) | (g << 8) | b;
}

/* One entry of a palette array, or a default when the applet gave none. */
static uint32_t pal(const ps_jobj *arr, int32_t i, uint32_t dflt)
{
    if(!arr || arr->kind != PS_OBJ_ARRAY || !arr->data ||
       arr->elem != PS_T_BYTE || i < 0 || i >= arr->len)
        return dflt;
    return (uint32_t)((const uint8_t *)arr->data)[i];
}

static uint32_t index_argb(const ps_jslot *f, uint32_t v)
{
    int32_t  n = f[CM_SIZE].i;
    int32_t  i = (int32_t)(v & 0xffu);
    uint32_t r, g, b, a;

    if(n > 0 && i >= n)
        return 0;                      /* outside the map: nothing to draw */

    r = pal(f[CM_A].o, i, 0);
    g = pal(f[CM_B].o, i, 0);
    b = pal(f[CM_C].o, i, 0);
    a = f[CM_D].o ? pal(f[CM_D].o, i, 255u) : 255u;

    return (a << 24) | (r << 16) | (g << 8) | b;
}

/* A pixel value through a colour model, to the default RGB model this runtime
 * draws in. A null model is the default RGB one, which is what the
 * MemoryImageSource constructors without a ColorModel argument mean. */
static uint32_t cm_argb(const ps_jobj *cm, uint32_t v)
{
    const ps_jslot *f;

    if(!cm || !cm->fields || !cm->cls || cm->cls->inst_slots < CM_SLOTS)
        return v;

    f = cm->fields;
    switch(f[CM_KIND].i) {
    case CM_DIRECT: return direct_argb(f, v);
    case CM_INDEX:  return index_argb(f, v);
    default:        return v;
    }
}

/* --- production ---------------------------------------------------------- */

/* What a request for pixels came back with. A source that has not arrived is
 * not an error: ps_applet.c fetches an applet's artwork over the network after
 * init() has already built its tiles out of it, so "not yet" is the ordinary
 * answer for the first frame or two and the image is simply produced again
 * next time. */
enum { PX_OK = 0, PX_WAIT, PX_BAD };

static int producer_px(ps_jvm *vm, ps_jobj *p, uint32_t **out, int *w, int *h,
                       int depth);
static int image_px(ps_jvm *vm, ps_jobj *img, const uint32_t **px, int *w,
                    int *h, int *stride, int depth);

/* Zero is allowed through, and every allocation below asks for one pixel more
 * than it needs so that it stays a real allocation. An empty crop is a legal
 * thing for an applet to build - pointless, but legal, and the real API makes
 * an empty image out of it rather than complaining - so it must not be the
 * failure that stops the applet. Too big still is. */
static int size_ok(long w, long h)
{
    return w >= 0 && h >= 0 && w <= JIMG_MAX_DIM && h <= JIMG_MAX_DIM &&
           w * h <= JIMG_MAX_PX;
}

/* --- filters ------------------------------------------------------------- */

enum { FLT_BAD = 0, FLT_IDENTITY, FLT_CROP, FLT_RGB };

/* The consumer-protocol methods. An applet that overrides any of them is
 * writing against the asynchronous pipeline this file does not have: it
 * expects to be handed scanlines and to push them on to a consumer it was
 * given. Answering such a filter by ignoring the override would draw a
 * plausible wrong picture, so it is refused by name instead - which costs one
 * line in the log and names the class that has to be looked at. */
static const char *const g_consumer_methods[] = {
    "setDimensions", "setColorModel", "setHints", "setProperties",
    "setPixels", "imageComplete", "substituteColorModel",
    "resendTopDownLeftRight", "getFilterInstance", NULL
};

static int overrides_consumer(const ps_jclass *c, const char **which)
{
    uint16_t i;
    int      k;

    for(; c; c = c->super) {
        if(c->native)
            continue;              /* the runtime's own shells declare none */
        for(i = 0; i < c->method_count; i++) {
            if(!c->methods[i].code)
                continue;
            for(k = 0; g_consumer_methods[k]; k++) {
                if(!strcmp(c->methods[i].name, g_consumer_methods[k])) {
                    *which = g_consumer_methods[k];
                    return 1;
                }
            }
        }
    }
    return 0;
}

/* Which of the three filters this object is, and for an RGB one the method to
 * call back into. */
static int filter_kind(ps_jvm *vm, ps_jobj *f, ps_jmethod **frgb,
                       ps_jclass **fcls)
{
    const char *which = NULL;

    *frgb = NULL;
    *fcls = NULL;

    if(!f || !f->cls)
        return FLT_BAD;

    if(overrides_consumer(f->cls, &which)) {
        fail(vm, "unimplemented: %s.%s, an ImageFilter that produces its own "
                 "pixels", f->cls->name, which);
        return FLT_BAD;
    }

    if(derives_from(f, "java/awt/image/CropImageFilter"))
        return FLT_CROP;

    if(derives_from(f, "java/awt/image/RGBImageFilter")) {
        ps_jclass  *c;
        ps_jmethod *m = NULL;

        /* Only a method with bytecode. ps_jvm_call runs a nested method to
         * completion, which is how this file reaches back into the applet, and
         * ps_jlib.c's obj_string documents the limit: a native one would come
         * back through ps_jre_call with the same object and recurse. */
        for(c = f->cls; c; c = c->super) {
            uint16_t i;

            for(i = 0; i < c->method_count; i++) {
                if(!strcmp(c->methods[i].name, "filterRGB") &&
                   !strcmp(c->methods[i].desc, "(III)I")) {
                    m = &c->methods[i];
                    break;
                }
            }
            if(m) {
                *fcls = c;
                break;
            }
        }
        if(!m || !m->code) {
            fail(vm, "unimplemented: %s.filterRGB(III)I", f->cls->name);
            return FLT_BAD;
        }
        *frgb = m;
        return FLT_RGB;
    }

    if(derives_from(f, "java/awt/image/ImageFilter"))
        return FLT_IDENTITY;       /* the base class passes everything through */

    fail(vm, "unimplemented: %s as an ImageFilter",
         f->cls->name ? f->cls->name : "?");
    return FLT_BAD;
}

/* The size a filter turns sw by sh into, without producing anything. This is
 * what lets Image.getWidth() answer for a cropped tile whose sheet has not
 * arrived yet - the crop rectangle says the answer outright. */
static int filter_size(ps_jvm *vm, ps_jobj *f, int *w, int *h)
{
    ps_jmethod *m;
    ps_jclass  *c;
    int         kind = filter_kind(vm, f, &m, &c);

    if(kind == FLT_BAD)
        return PX_BAD;
    if(kind == FLT_CROP) {
        if(!f->fields)
            return PX_BAD;
        *w = f->fields[CROP_W].i;
        *h = f->fields[CROP_H].i;
    }
    return PX_OK;
}

/* What a crop leaves where it hangs off the edge of its source.
 *
 * A CropImageFilter delivers only the part of the rectangle that exists, so
 * the rest of the image is whatever the consumer's buffer started as - and
 * that is the source's colour model's zero pixel, not a colour anybody chose.
 * A model with an alpha channel makes it transparent; one without makes it
 * opaque black. Both were checked against a real JDK: the same crop off the
 * same sheet comes back transparent when the sheet was built through
 * ColorModel.getRGBdefault() and black when it was an ordinary opaque
 * offscreen image.
 *
 * There is no colour model here to ask, so the source pixels are asked
 * instead: a source with a transparent pixel in it had a model with alpha.
 * That is exact for a transparent GIF, which is the case sprite sheets are,
 * and it calls an opaque-but-alpha-capable source opaque - a difference that
 * can only show through a crop that has gone off the edge of its own sheet.
 *
 * Scanned only when the crop actually overhangs, which no working applet's
 * does, so the ordinary tile costs nothing for this.
 */
static uint32_t crop_ground(const uint32_t *src, int sw, int sh, int sstride)
{
    int x, y;

    for(y = 0; y < sh; y++) {
        for(x = 0; x < sw; x++) {
            if((src[(size_t)y * sstride + x] >> 24) != 0xffu)
                return 0u;
        }
    }
    return 0xff000000u;
}

/* Applies a filter to src, allocating the result. */
static int filter_px(ps_jvm *vm, ps_jobj *f, const uint32_t *src, int sw,
                     int sh, int sstride, uint32_t **out, int *ow, int *oh)
{
    ps_jmethod *frgb = NULL;
    ps_jclass  *fcls = NULL;
    int         kind = filter_kind(vm, f, &frgb, &fcls);
    uint32_t   *dst;
    int         x, y, w, h;

    if(kind == FLT_BAD)
        return PX_BAD;

    w = sw;
    h = sh;
    if(kind == FLT_CROP) {
        if(!f->fields)
            return PX_BAD;
        w = f->fields[CROP_W].i;
        h = f->fields[CROP_H].i;
    }

    if(!size_ok(w, h))
        return fail(vm, "filtered image is %dx%d", w, h), PX_BAD;

    /* calloc, because a crop that stays inside its sheet writes every pixel
     * and a crop that does not is corrected below. */
    dst = (uint32_t *)calloc((size_t)w * h + 1, sizeof *dst);
    if(!dst)
        return fail(vm, "out of memory filtering a %dx%d image", w, h), PX_BAD;

    if(kind == FLT_CROP) {
        /* 64-bit because the origin is whatever the applet named: a crop at
         * INT_MAX plus a width would wrap, and a wrapped index is one that
         * passes the range test below. */
        int64_t cx = f->fields[CROP_X].i, cy = f->fields[CROP_Y].i;

        if(cx < 0 || cy < 0 || cx + w > sw || cy + h > sh) {
            uint32_t ground = crop_ground(src, sw, sh, sstride);

            if(ground) {
                long i, n = (long)w * h;

                for(i = 0; i < n; i++)
                    dst[i] = ground;
            }
        }

        for(y = 0; y < h; y++) {
            int64_t sy = cy + y;

            if(sy < 0 || sy >= sh)
                continue;
            for(x = 0; x < w; x++) {
                int64_t sx = cx + x;

                if(sx < 0 || sx >= sw)
                    continue;
                dst[(size_t)y * w + x] = src[(size_t)sy * sstride + sx];
            }
        }
    }
    else {
        for(y = 0; y < h; y++) {
            for(x = 0; x < w; x++)
                dst[(size_t)y * w + x] = src[(size_t)y * sstride + x];
        }
    }

    /* The callback runs last, over a block this function owns and nothing else
     * can see. That ordering is the point: filterRGB is applet bytecode, it
     * may allocate, an allocation may collect, and a collection frees the
     * pixels of any produced image that has become unreachable - which the
     * source of this filter very well might be. Copying first means the loop
     * below reads only from memory the collector has no opinion about. */
    if(kind == FLT_RGB) {
        for(y = 0; y < h; y++) {
            for(x = 0; x < w; x++) {
                ps_jslot argv[4], r;

                memset(&r, 0, sizeof r);
                argv[0].o = f;
                argv[1].i = x;
                argv[2].i = y;
                argv[3].i = (int32_t)dst[(size_t)y * w + x];

                if(ps_jvm_call(vm, fcls, frgb, argv, 4, &r) != 0) {
                    free(dst);
                    return PX_BAD;
                }
                dst[(size_t)y * w + x] = (uint32_t)r.i;
            }
        }
    }

    *out = dst;
    *ow  = w;
    *oh  = h;
    return PX_OK;
}

/* --- MemoryImageSource --------------------------------------------------- */

static int memory_px(ps_jvm *vm, ps_jobj *p, uint32_t **out, int *ow, int *oh)
{
    const ps_jslot *f = p->fields;
    ps_jobj        *arr;
    uint32_t       *dst;
    int             w, h, off, scan, x, y;

    if(!f)
        return PX_BAD;

    w    = f[MIS_W].i;
    h    = f[MIS_H].i;
    off  = f[MIS_OFF].i;
    scan = f[MIS_SCAN].i;
    arr  = f[MIS_PIX].o;

    if(!size_ok(w, h))
        return fail(vm, "MemoryImageSource is %dx%d", w, h), PX_BAD;
    if(!arr || arr->kind != PS_OBJ_ARRAY || !arr->data ||
       (arr->elem != PS_T_INT && arr->elem != PS_T_BYTE))
        return fail(vm, "MemoryImageSource wants an int[] or a byte[]"),
               PX_BAD;

    dst = (uint32_t *)calloc((size_t)w * h + 1, sizeof *dst);
    if(!dst)
        return fail(vm, "out of memory for a %dx%d image", w, h), PX_BAD;

    for(y = 0; y < h; y++) {
        for(x = 0; x < w; x++) {
            /* 64-bit, because scan is whatever the applet said and long is
             * 32 bits on SH-4: a hostile scan of two billion would otherwise
             * wrap into an index that passes the bounds check below. */
            int64_t  i = (int64_t)off + (int64_t)y * scan + x;
            uint32_t v;

            /* An index off the end is what a real AWT throws on. Leaving the
             * pixel transparent instead keeps a badly built source from taking
             * the applet down for a picture nobody would have seen. */
            if(i < 0 || i >= arr->len)
                continue;

            v = arr->elem == PS_T_INT
                ? (uint32_t)((const int32_t *)arr->data)[i]
                : (uint32_t)((const uint8_t *)arr->data)[i];

            dst[(size_t)y * w + x] = cm_argb(f[MIS_CM].o, v);
        }
    }

    *out = dst;
    *ow  = w;
    *oh  = h;
    return PX_OK;
}

/* --- the graph ----------------------------------------------------------- */

/* Pixels for one producer, allocated. */
static int producer_px(ps_jvm *vm, ps_jobj *p, uint32_t **out, int *w, int *h,
                       int depth)
{
    if(depth > JIMG_MAX_DEPTH)
        return fail(vm, "ImageProducer nested more than %d deep",
                    JIMG_MAX_DEPTH), PX_BAD;
    if(!p || !p->cls)
        return PX_BAD;

    if(class_named(p, "ps/awt/imgsrc")) {
        const uint32_t *src = NULL;
        int             sw = 0, sh = 0, ss = 0, rc;
        uint32_t       *dst;
        int             y;

        rc = image_px(vm, p->fields ? p->fields[SRC_IMAGE].o : NULL, &src,
                      &sw, &sh, &ss, depth + 1);
        if(rc != PX_OK)
            return rc;
        if(!size_ok(sw, sh))
            return fail(vm, "source image is %dx%d", sw, sh), PX_BAD;

        dst = (uint32_t *)malloc(((size_t)sw * sh + 1) * sizeof *dst);
        if(!dst)
            return fail(vm, "out of memory for a %dx%d image", sw, sh), PX_BAD;
        for(y = 0; y < sh; y++)
            memcpy(dst + (size_t)y * sw, src + (size_t)y * ss,
                   (size_t)sw * sizeof *dst);

        *out = dst;
        *w   = sw;
        *h   = sh;
        return PX_OK;
    }

    if(class_named(p, "java/awt/image/FilteredImageSource")) {
        uint32_t *src = NULL;
        int       sw = 0, sh = 0, rc;

        if(!p->fields)
            return PX_BAD;

        rc = producer_px(vm, p->fields[FIS_SRC].o, &src, &sw, &sh, depth + 1);
        if(rc != PX_OK)
            return rc;

        rc = filter_px(vm, p->fields[FIS_FILTER].o, src, sw, sh, sw, out, w, h);
        free(src);
        return rc;
    }

    if(derives_from(p, "java/awt/image/MemoryImageSource"))
        return memory_px(vm, p, out, w, h);

    return fail(vm, "unimplemented: %s as an ImageProducer", p->cls->name),
           PX_BAD;
}

/* The size a producer will make, without making it. */
static int producer_size(ps_jvm *vm, ps_jobj *p, int *w, int *h, int depth)
{
    if(depth > JIMG_MAX_DEPTH || !p || !p->cls || !p->fields)
        return PX_BAD;

    if(class_named(p, "ps/awt/imgsrc")) {
        const uint32_t *px = NULL;
        int             ss = 0;

        return image_px(vm, p->fields[SRC_IMAGE].o, &px, w, h, &ss, depth + 1);
    }

    if(class_named(p, "java/awt/image/FilteredImageSource")) {
        int rc = producer_size(vm, p->fields[FIS_SRC].o, w, h, depth + 1);

        /* A crop knows its size whether or not the sheet has arrived, which is
         * the case worth having: an applet that asks a tile for its width in
         * init() gets the answer rather than -1. */
        if(rc != PX_OK && rc != PX_WAIT)
            return rc;
        if(filter_size(vm, p->fields[FIS_FILTER].o, w, h) != PX_OK)
            return PX_BAD;
        return derives_from(p->fields[FIS_FILTER].o,
                            "java/awt/image/CropImageFilter") ? PX_OK : rc;
    }

    if(derives_from(p, "java/awt/image/MemoryImageSource")) {
        *w = p->fields[MIS_W].i;
        *h = p->fields[MIS_H].i;
        return size_ok(*w, *h) ? PX_OK : PX_BAD;
    }

    return PX_BAD;
}

/* Gives a produced image its pixels, if it has not got them and can. */
static int materialise(ps_jvm *vm, ps_jobj *img, int depth)
{
    uint32_t *px = NULL;
    jimg_pix *blk;
    int       w = 0, h = 0, rc;
    size_t    bytes;

    if(pix_of(img))
        return PX_OK;
    if(!img->fields)
        return PX_BAD;

    rc = producer_px(vm, img->fields[PIMG_PROD].o, &px, &w, &h, depth + 1);
    if(rc != PX_OK)
        return rc;

    bytes = JIMG_BYTES((size_t)w * h);
    blk   = (jimg_pix *)malloc(bytes);
    if(!blk) {
        free(px);
        return fail(vm, "out of memory for a %dx%d image", w, h), PX_BAD;
    }
    blk->magic = JIMG_MAGIC;
    blk->w     = w;
    blk->h     = h;
    memcpy(blk->px, px, (size_t)w * h * sizeof *px);
    free(px);

    img->native      = blk;
    img->owns_native = 1;

    /* The sweep charges an owned native block as ->len + 1 bytes, because the
     * only one it was written for is a String's. Sizing ->len to match keeps
     * the collector's byte count honest - without it a sheet cut into forty
     * tiles is invisible to the threshold that decides when to collect.
     * Nothing reads ->len on an image of this kind; ps_jre.c reads it on a
     * network one, which this is not. */
    img->len = (int32_t)(bytes - 1);
    vm->bytes += (long)bytes;

    return PX_OK;
}

/* Borrowed pixels for any of the three kinds of Image. */
static int image_px(ps_jvm *vm, ps_jobj *img, const uint32_t **px, int *w,
                    int *h, int *stride, int depth)
{
    const uint32_t *p;

    if(!img)
        return PX_BAD;

    if(is_produced(img)) {
        jimg_pix *blk;
        int       rc = materialise(vm, img, depth);

        if(rc != PX_OK)
            return rc;
        blk     = (jimg_pix *)img->native;
        *px     = blk->px;
        *w      = blk->w;
        *h      = blk->h;
        *stride = blk->w;
        return PX_OK;
    }

    p = ps_joff_image_px(img, w, h, stride);
    if(p) {
        *px = p;
        return PX_OK;
    }

    /* A network image. Its handle is the object's length field, and a NULL
     * back means the fetch has not landed yet - not that anything is wrong. */
    p = ps_applet_image_px(img->len, w, h);
    if(!p)
        return PX_WAIT;

    *px     = p;
    *stride = *w;
    return PX_OK;
}

/* --- constructors -------------------------------------------------------- */

/* The parameter types of a descriptor, one character each: 'I' for an int, 'i'
 * for an int array, 'b' for a byte array, 'L' for anything else by reference.
 *
 * MemoryImageSource has six constructors that differ only in whether a
 * ColorModel is present and whether the pixels are int or byte, so matching
 * them by full descriptor string would be six literals that all have to stay
 * in step with each other. Reading the shape instead handles the two with a
 * trailing Hashtable of properties for free.
 *
 * Nothing here takes a long or a double, so a parameter's index is also its
 * argument slot - which is what lets the callers index args[n + 1] directly.
 */
static int params_of(const char *d, char *out, int max)
{
    int n = 0;

    if(!d || *d != '(')
        return 0;
    d++;

    while(*d && *d != ')' && n < max) {
        if(*d == '[') {
            char k = d[1] == 'I' ? 'i' : d[1] == 'B' ? 'b' : 'L';

            d++;
            while(*d == '[')
                d++;
            if(*d == 'L') {
                while(*d && *d != ';')
                    d++;
                if(*d)
                    d++;
            }
            else if(*d) {
                d++;
            }
            out[n++] = k;
            continue;
        }
        if(*d == 'L') {
            while(*d && *d != ';')
                d++;
            if(*d)
                d++;
            out[n++] = 'L';
            continue;
        }
        out[n++] = *d == 'I' ? 'I' : 'L';
        d++;
    }
    return n;
}

static void mis_init(ps_jvm *vm, const char *d, ps_jslot *args, int nargs)
{
    char     p[12];
    int      np = params_of(d, p, (int)sizeof p);
    ps_jobj *self = nargs >= 1 ? args[0].o : NULL;
    int      i, cm = -1, pix = -1;

    (void)vm;
    if(!self || !self->fields || !self->cls ||
       self->cls->inst_slots < 6 || np < 5)
        return;

    /* (w, h [, cm], pix, off, scan [, props]) in every overload. */
    for(i = 2; i < np; i++) {
        if(p[i] == 'i' || p[i] == 'b') {
            pix = i;
            break;
        }
        if(p[i] == 'L' && cm < 0)
            cm = i;
    }
    /* off and scan follow the array, and args[k + 1] is parameter k because
     * none of these parameters is a long or a double. */
    if(pix < 0 || pix + 2 >= np || nargs < pix + 4)
        return;

    self->fields[MIS_W].i    = args[1].i;
    self->fields[MIS_H].i    = args[2].i;
    self->fields[MIS_CM].o   = cm >= 0 ? args[cm + 1].o : NULL;
    self->fields[MIS_PIX].o  = args[pix + 1].o;
    self->fields[MIS_OFF].i  = args[pix + 2].i;
    self->fields[MIS_SCAN].i = args[pix + 3].i;
}

/* --- java.awt.image dispatch --------------------------------------------- */

static ps_jobj *new_of(ps_jvm *vm, const char *cls)
{
    return ps_jvm_new(vm, ps_jvm_class(vm, cls));
}

static int colour_model(ps_jvm *vm, const char *cls, const char *n,
                        const char *d, ps_jslot *args, int nargs,
                        ps_jslot *ret, int *handled)
{
    ps_jobj *self = nargs >= 1 ? args[0].o : NULL;
    int      direct = !strcmp(cls, "DirectColorModel");
    int      index  = !strcmp(cls, "IndexColorModel");

    if(is(n, d, "getRGBdefault", NULL)) {
        ps_jobj *o = new_of(vm, "java/awt/image/ColorModel");

        if(o && o->fields) {
            o->fields[CM_KIND].i = CM_DEFAULT;
            o->fields[CM_BITS].i = 32;
        }
        ret->o = o;
        *handled = 1;
        return 0;
    }

    if(is(n, d, "<init>", NULL)) {
        char p[12];
        int  np = params_of(d, p, (int)sizeof p);

        *handled = 1;
        if(!self || !self->fields || !self->cls ||
           self->cls->inst_slots < CM_SLOTS)
            return 0;

        if(direct && np >= 4 && nargs >= 5) {
            /* DirectColorModel(bits, rmask, gmask, bmask[, amask]) */
            self->fields[CM_KIND].i = CM_DIRECT;
            self->fields[CM_BITS].i = args[1].i;
            self->fields[CM_A].i    = args[2].i;
            self->fields[CM_B].i    = args[3].i;
            self->fields[CM_C].i    = args[4].i;
            self->fields[CM_D].i    = (np >= 5 && nargs >= 6) ? args[5].i : 0;
            return 0;
        }
        if(index && np >= 5 && nargs >= 6) {
            /* IndexColorModel(bits, size, reds, greens, blues[, alphas]) and
             * the transparent-index form, whose sixth argument is an int. The
             * two are told apart by the shape rather than by the descriptor:
             * only the alpha form passes an array there. */
            self->fields[CM_KIND].i = CM_INDEX;
            self->fields[CM_BITS].i = args[1].i;
            self->fields[CM_SIZE].i = args[2].i;
            self->fields[CM_A].o    = args[3].o;
            self->fields[CM_B].o    = args[4].o;
            self->fields[CM_C].o    = args[5].o;
            self->fields[CM_D].o    = (np >= 6 && nargs >= 7 && p[5] == 'b')
                                      ? args[6].o : NULL;
            return 0;
        }

        /* Anything else - an IndexColorModel over a packed byte[] of triples,
         * a ColorModel constructed directly - is left as the default RGB
         * model rather than guessed at. */
        self->fields[CM_KIND].i = CM_DEFAULT;
        self->fields[CM_BITS].i = 32;
        return 0;
    }

    if(!self || !self->fields || !self->cls ||
       self->cls->inst_slots < CM_SLOTS)
        return 0;

    if(is(n, d, "getPixelSize", NULL)) {
        ret->i = self->fields[CM_BITS].i;
        *handled = 1;
        return 0;
    }
    if(is(n, d, "getMapSize", NULL)) {
        ret->i = self->fields[CM_SIZE].i;
        *handled = 1;
        return 0;
    }
    if(is(n, d, "getRedMask", NULL) || is(n, d, "getGreenMask", NULL) ||
       is(n, d, "getBlueMask", NULL) || is(n, d, "getAlphaMask", NULL)) {
        ret->i = self->fields[n[3] == 'R' ? CM_A : n[3] == 'G' ? CM_B
                            : n[3] == 'B' ? CM_C : CM_D].i;
        *handled = 1;
        return 0;
    }
    if(is(n, d, "getRGB", "(I)I")) {
        ret->i = (int32_t)cm_argb(self, (uint32_t)(nargs >= 2 ? args[1].i : 0));
        *handled = 1;
        return 0;
    }

    return 0;
}

/* java.awt.image.PixelGrabber.
 *
 * Synchronous, which is what the class is for: an applet calls grabPixels()
 * and blocks until the pixels are in its array. Here they either are or the
 * source has not arrived, and false is the documented answer for a grab that
 * did not complete. */
static int grabber(ps_jvm *vm, const char *n, const char *d, ps_jslot *args,
                   int nargs, ps_jslot *ret, int *handled)
{
    ps_jobj *self = nargs >= 1 ? args[0].o : NULL;

    if(!self || !self->fields || !self->cls ||
       self->cls->inst_slots < PG_SLOTS)
        return 0;

    if(is(n, d, "<init>", NULL)) {
        char p[12];
        int  np = params_of(d, p, (int)sizeof p);

        *handled = 1;

        /* (src, x, y, w, h, pix, off, scan). The two-argument form
         * (src, forceRGB) grabs the whole image and is left out: it needs the
         * array to be allocated here and handed back through getPixels(),
         * which no applet in the corpus asks for. */
        if(np < 8 || nargs < 9)
            return fail(vm, "unimplemented: PixelGrabber.<init>%s", d);

        self->fields[PG_SRC].o  = args[1].o;
        self->fields[PG_X].i    = args[2].i;
        self->fields[PG_Y].i    = args[3].i;
        self->fields[PG_W].i    = args[4].i;
        self->fields[PG_H].i    = args[5].i;
        self->fields[PG_PIX].o  = args[6].o;
        self->fields[PG_OFF].i  = args[7].i;
        self->fields[PG_SCAN].i = args[8].i;
        return 0;
    }

    if(is(n, d, "grabPixels", NULL) || is(n, d, "startGrabbing", NULL)) {
        ps_jobj        *src = self->fields[PG_SRC].o;
        ps_jobj        *arr = self->fields[PG_PIX].o;
        const uint32_t *px  = NULL;
        uint32_t       *own = NULL;
        int             gw  = self->fields[PG_W].i;
        int             gh  = self->fields[PG_H].i;
        int             sw = 0, sh = 0, ss = 0, rc, x, y;

        *handled = 1;
        ret->i = 0;

        if(!arr || arr->kind != PS_OBJ_ARRAY || arr->elem != PS_T_INT ||
           !arr->data)
            return 0;

        /* The rectangle is whatever the applet named, and the write below is
         * bounds-checked - but the loop itself is not, so a grab claiming to
         * be two billion pixels wide would spin rather than write. */
        if(gw < 0 || gh < 0 || gw > JIMG_MAX_DIM || gh > JIMG_MAX_DIM)
            return 0;

        if(is_image(vm, src)) {
            rc = image_px(vm, src, &px, &sw, &sh, &ss, 0);
        }
        else {
            rc = producer_px(vm, src, &own, &sw, &sh, 0);
            px = own;
            ss = sw;
        }
        if(rc != PX_OK) {
            free(own);
            return rc == PX_BAD
                   ? fail(vm, "grabPixels: the image could not be produced")
                   : 0;
        }

        for(y = 0; y < gh; y++) {
            int sy = self->fields[PG_Y].i + y;

            for(x = 0; x < gw; x++) {
                int     sx = self->fields[PG_X].i + x;
                int64_t i  = (int64_t)self->fields[PG_OFF].i +
                             (int64_t)y * self->fields[PG_SCAN].i + x;

                if(i < 0 || i >= arr->len)
                    continue;
                ((int32_t *)arr->data)[i] =
                    (sx >= 0 && sx < sw && sy >= 0 && sy < sh)
                    ? (int32_t)px[(size_t)sy * ss + sx] : 0;
            }
        }
        free(own);

        self->fields[PG_STAT].i = OBS_ALLBITS | OBS_FRAMEBITS;
        ret->i = 1;
        return 0;
    }

    if(is(n, d, "getStatus", NULL) || is(n, d, "status", NULL)) {
        ret->i = self->fields[PG_STAT].i;
        *handled = 1;
        return 0;
    }
    if(is(n, d, "getWidth", NULL)) {
        ret->i = self->fields[PG_W].i;
        *handled = 1;
        return 0;
    }
    if(is(n, d, "getHeight", NULL)) {
        ret->i = self->fields[PG_H].i;
        *handled = 1;
        return 0;
    }
    if(is(n, d, "getColorModel", NULL)) {
        ps_jobj *o = new_of(vm, "java/awt/image/ColorModel");

        if(o && o->fields) {
            o->fields[CM_KIND].i = CM_DEFAULT;
            o->fields[CM_BITS].i = 32;
        }
        ret->o = o;
        *handled = 1;
        return 0;
    }

    return 0;
}

/* The status bits an applet's imageUpdate compares against, read through
 * getstatic on the interface. */
static int observer_const(const char *n, ps_jslot *ret, int *handled)
{
    static const struct { const char *name; int32_t v; } b[] = {
        { "WIDTH", 1 }, { "HEIGHT", 2 }, { "PROPERTIES", 4 },
        { "SOMEBITS", 8 }, { "FRAMEBITS", 16 }, { "ALLBITS", 32 },
        { "ERROR", 64 }, { "ABORT", 128 }, { NULL, 0 }
    };
    int i;

    for(i = 0; b[i].name; i++) {
        if(!strcmp(n, b[i].name)) {
            ret->i = b[i].v;
            *handled = 1;
            return 1;
        }
    }
    return 0;
}

static int image_pkg(ps_jvm *vm, const char *cls, const char *n,
                     const char *d, ps_jslot *args, int nargs, ps_jslot *ret,
                     int *handled)
{
    ps_jobj *self = nargs >= 1 ? args[0].o : NULL;

    /* getstatic arrives with a field descriptor rather than a method one and
     * no receiver; that leading parenthesis is the only thing separating the
     * two cases. */
    if(d && d[0] != '(') {
        if(!strcmp(cls, "ImageObserver"))
            observer_const(n, ret, handled);
        return 0;
    }

    if(!strcmp(cls, "CropImageFilter")) {
        if(is(n, d, "<init>", "(IIII)V")) {
            *handled = 1;
            if(self && self->fields && self->cls &&
               self->cls->inst_slots >= 4 && nargs >= 5) {
                self->fields[CROP_X].i = args[1].i;
                self->fields[CROP_Y].i = args[2].i;
                self->fields[CROP_W].i = args[3].i;
                self->fields[CROP_H].i = args[4].i;
            }
            return 0;
        }
        return 0;
    }

    if(!strcmp(cls, "FilteredImageSource")) {
        if(is(n, d, "<init>", NULL)) {
            *handled = 1;
            if(self && self->fields && self->cls &&
               self->cls->inst_slots >= 2 && nargs >= 3) {
                self->fields[FIS_SRC].o    = args[1].o;
                self->fields[FIS_FILTER].o = args[2].o;
            }
            return 0;
        }
        return 0;
    }

    if(!strcmp(cls, "MemoryImageSource")) {
        if(is(n, d, "<init>", NULL)) {
            mis_init(vm, d, args, nargs);
            *handled = 1;
            return 0;
        }
        /* setAnimated, newPixels, addConsumer and the rest are the live half
         * of this class and this file has no consumer to deliver to. They are
         * left unimplemented so an applet that animates a MemoryImageSource
         * says which call it wanted rather than showing a frozen frame. */
        return 0;
    }

    if(!strcmp(cls, "ImageFilter") || !strcmp(cls, "RGBImageFilter")) {
        /* The constructors, which is all a subclass needs from the base: an
         * ImageFilter has nothing to initialise here, and RGBImageFilter's own
         * field defaults to false the way calloc left it. */
        if(is(n, d, "<init>", NULL)) {
            *handled = 1;
            return 0;
        }
        return 0;
    }

    if(!strncmp(cls, "PixelGrabber", 12))
        return grabber(vm, n, d, args, nargs, ret, handled);

    if(!strcmp(cls, "ColorModel") || !strcmp(cls, "DirectColorModel") ||
       !strcmp(cls, "IndexColorModel"))
        return colour_model(vm, cls, n, d, args, nargs, ret, handled);

    return 0;
}

/* --- java.awt.Image ------------------------------------------------------ */

static int image_class(ps_jvm *vm, const char *n, const char *d,
                       ps_jslot *args, int nargs, ps_jslot *ret, int *handled)
{
    ps_jobj *self = nargs >= 1 ? args[0].o : NULL;

    /* getSource() answers for all three kinds of image, because all three are
     * legal sources for a filter: an applet crops tiles out of a sheet it
     * fetched, and it also crops them out of one it drew for itself. The
     * producer holds the Image rather than its pixels, so a sheet that has not
     * arrived yet is not a problem here. */
    if(is(n, d, "getSource", NULL)) {
        ps_jobj *p;

        if(!is_image(vm, self)) {
            ret->o = NULL;
            *handled = 1;
            return 0;
        }
        p = new_of(vm, "ps/awt/imgsrc");
        if(p && p->fields)
            p->fields[SRC_IMAGE].o = self;
        ret->o = p;
        *handled = 1;
        return 0;
    }

    if(!is_produced(self))
        return 0;                  /* ps_joff.c or ps_jre.c has it */

    if(is(n, d, "getWidth", NULL) || is(n, d, "getHeight", NULL)) {
        int w = -1, h = -1;

        /* -1 is what the real API answers for a dimension it does not know
         * yet, and an applet that asks before its artwork has landed is
         * written to expect it. A crop knows its answer regardless. */
        if(self->fields &&
           producer_size(vm, self->fields[PIMG_PROD].o, &w, &h, 0) != PX_OK) {
            w = -1;
            h = -1;
        }
        ret->i = n[3] == 'W' ? w : h;
        *handled = 1;
        return 0;
    }

    if(is(n, d, "flush", NULL)) {
        /* Discards the pixels so they are produced again. Unlike an offscreen
         * image, this one can be: the recipe is still in the object. */
        if(self->owns_native && self->native) {
            vm->bytes -= (long)self->len + 1;
            if(vm->bytes < 0)
                vm->bytes = 0;
            free(self->native);
            self->native      = NULL;
            self->owns_native = 0;
            self->len         = 0;
        }
        *handled = 1;
        return 0;
    }

    if(is(n, d, "getGraphics", NULL)) {
        /* The real API refuses this on an image that is not a back buffer, and
         * ps_jre.c already answers null for a network one. Drawing into a
         * produced image would also have nowhere to put the result: the pixels
         * are a cache of the producer and flush() throws them away. */
        ret->o = NULL;
        *handled = 1;
        return 0;
    }

    return 0;
}

/* --- Graphics.drawImage, for a produced source --------------------------- */

/* Counts the plain-int parameters of a descriptor, which is what tells a blit
 * from a scaled blit. ps_joff.c has the same helper and the same note: this is
 * not a scan for 'I', because "Ljava/awt/Image;" has one in it. */
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

static int draw_image(ps_jvm *vm, const char *d, ps_jslot *args, int nargs,
                      ps_jslot *ret, int *handled)
{
    ps_jobj        *src = nargs >= 2 ? args[1].o : NULL;
    const uint32_t *px  = NULL;
    ps_jgfx        *g;
    int             w = 0, h = 0, stride = 0, ni, rc;

    if(!is_produced(src))
        return 0;                  /* not ours; ps_joff.c or ps_jre.c blits it */

    *handled = 1;

    /* The same rule ps_jre.c and ps_joff.c use, and it has to stay the same
     * rule: a Graphics with a context of its own uses it, and the bare one the
     * browser handed paint() falls back to the VM's. */
    g = (nargs >= 1 && args[0].o && args[0].o->native)
        ? (ps_jgfx *)args[0].o->native : vm->gfx;

    ni = int_params(d);

    /* this, the image, and at least x and y. Anything shorter is a descriptor
     * this runtime has not seen and must not index past. */
    if(!g || nargs < 4 || (ni == 4 && nargs < 6))
        return 0;

    rc = image_px(vm, src, &px, &w, &h, &stride, 0);
    if(rc == PX_BAD)
        return fail(vm, "drawImage: the image could not be produced");
    if(rc != PX_OK) {
        /* Still producing. drawImage returns false, which is exactly what it
         * means - and an applet that branches on it will try again. */
        ret->i = 0;
        return 0;
    }

    /* The background overload, drawImage(img, x, y, bg, obs), paints bg
     * wherever the image is transparent. A cropped tile very often is - that
     * is the whole point of a transparent GIF - so this one fills first and
     * blits over it, which gives the same picture with no second path in the
     * rasteriser. */
    if(strstr(d, "Ljava/awt/Color;") && nargs > (ni == 4 ? 6 : 4)) {
        ps_jobj *c = args[ni == 4 ? 6 : 4].o;

        if(c) {
            uint32_t save = g->color;

            g->color = (uint32_t)c->len;
            if(ni == 4)
                ps_jgfx_fill_rect(g, args[2].i, args[3].i, args[4].i,
                                  args[5].i);
            else
                ps_jgfx_fill_rect(g, args[2].i, args[3].i, w, h);
            g->color = save;
        }
    }

    if(ni == 4)
        ps_jgfx_draw_image_scaled(g, px, w, h, stride, args[2].i, args[3].i,
                                  args[4].i, args[5].i);
    else
        ps_jgfx_draw_image(g, px, w, h, stride, args[2].i, args[3].i);

    ret->i = 1;
    return 0;
}

/* --- dispatch ------------------------------------------------------------ */

int ps_jimg_call(ps_jvm *vm, const char *cls, const char *name,
                 const char *desc, ps_jslot *args, int nargs, ps_jslot *ret,
                 int *handled)
{
    *handled = 0;

    /* Registration rides on the applet's own construction, which is the last
     * moment before init() can ask for a CropImageFilter. The call itself
     * belongs to ps_jre.c, so this leaves it unhandled - ps_jawt.c hooks the
     * same call the same way. */
    if(name[0] == '<' && !strcmp(cls, "java/applet/Applet")) {
        register_classes(vm);
        return 0;
    }

    /* Two comparisons get Graphics and Color - between them most of the native
     * calls a frame makes - back out before anything else, because this file
     * sits in front of ps_jre.c's own tables and must not slow the paint loop
     * down. Graphics is caught below, but only for drawImage. */
    if(!strcmp(cls, "java/awt/Graphics")) {
        if(is(name, desc, "drawImage", NULL))
            return draw_image(vm, desc, args, nargs, ret, handled);
        return 0;
    }
    if(strncmp(cls, "java/a", 6))
        return 0;
    if(!strcmp(cls, "java/awt/Color"))
        return 0;

    if(!strncmp(cls, "java/awt/image/", 15))
        return image_pkg(vm, cls + 15, name, desc, args, nargs, ret, handled);

    if(!strcmp(cls, "java/awt/Image"))
        return image_class(vm, name, desc, args, nargs, ret, handled);

    /* createImage is Component's, and an applet reaches it through whichever
     * of its ancestors the compiler named. Toolkit has one of its own with the
     * same signature and the same meaning. */
    if(!strcmp(cls, "java/awt/Component") || !strcmp(cls, "java/applet/Applet") ||
       !strcmp(cls, "java/awt/Panel")     || !strcmp(cls, "java/awt/Container") ||
       !strcmp(cls, "java/awt/Toolkit")) {

        if(is(name, desc, "createImage",
              "(Ljava/awt/image/ImageProducer;)Ljava/awt/Image;")) {
            ps_jobj *p = nargs >= 2 ? args[1].o : NULL;
            ps_jobj *img;

            *handled = 1;

            if(!p) {
                ret->o = NULL;
                return 0;
            }

            img = new_of(vm, "ps/awt/image");
            if(img && img->fields)
                img->fields[PIMG_PROD].o = p;
            ret->o = img;
            return 0;
        }
    }

    return 0;
}
