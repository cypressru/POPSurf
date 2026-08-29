/* java.awt.Dimension, Point, Rectangle, Insets and FontMetrics.
 *
 * These are the classes an applet meets before it draws anything. It asks the
 * browser how big it is, reads width and height off what comes back, and lays
 * everything else out relative to that - so until this file existed, most of
 * an applet's paint() was unreachable no matter how complete the drawing side
 * was.
 *
 * Why they are not in ps_jvm.c's native list, which is where every other JDK
 * class in this runtime lives:
 *
 * A native class answers a field read through ps_jre_call, and the interpreter
 * makes that call with no arguments - there is no receiver to look at, because
 * a native class is not expected to have per-instance state that getfield can
 * see. Colour gets away with it by never being read as a field. Dimension
 * cannot: `d.width` is how applets of the period use it, far more often than
 * getSize(), and a getfield that cannot see which Dimension it was asked about
 * has nothing to answer with. putfield is worse - it goes straight to the
 * slot path and fails outright on a class with no fields.
 *
 * So these are registered as ordinary field-carrying classes that happen to
 * have no bytecode. getfield and putfield then take the interpreter's normal
 * slot path and need to know nothing about any of this, which is the whole
 * point. Their methods, having no code, fall through ps_jclass_find_method to
 * the first native ancestor - java/lang/Object - and arrive here named
 * java/lang/Object with the real receiver in argument zero. That is the one
 * piece of indirection the arrangement costs, and it is confined to the router
 * at the bottom of this file.
 *
 * Semantics are AWT's and were captured by running the real JDK, not by
 * reading it. The ones that are not guessable:
 *
 *  - intersection() of two disjoint rectangles returns negative extents rather
 *    than an empty box at the origin.
 *  - union() ignores a rectangle whose width or height is *negative*, and
 *    includes one whose extent is merely zero. Applets union a zero-sized
 *    accumulator into a running bounds and depend on the second half.
 *  - add(x, y) grows the box so the point lands on its far edge, which means
 *    contains() is then false for the point that was just added.
 *  - intersects() and contains(Rectangle) are false whenever either box is
 *    empty, and touching edges do not intersect.
 */
#include "ps_jgeom.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Only for the two predicates at the bottom of this file. Nothing here calls
 * into the widget shells; it asks them what they are. */
#include "ps_jawt.h"

/* --- the classes ---------------------------------------------------------
 *
 * Field order here is the slot order, and slot zero is the first field: these
 * classes derive from java/lang/Object, which contributes none.
 */
enum { D_W, D_H };
enum { P_X, P_Y };
enum { R_X, R_Y, R_W, R_H };
enum { I_TOP, I_LEFT, I_BOTTOM, I_RIGHT };

#define GEOM_MAX_FIELDS 4

static const struct {
    const char *name;
    const char *fields[GEOM_MAX_FIELDS + 1];
} g_geom[] = {
    { "java/awt/Dimension",   { "width", "height", NULL } },
    { "java/awt/Point",       { "x", "y", NULL } },
    { "java/awt/Rectangle",   { "x", "y", "width", "height", NULL } },
    { "java/awt/Insets",      { "top", "left", "bottom", "right", NULL } },
    /* FontMetrics is abstract in the real API and an applet never constructs
     * one, so it carries no fields - its size rides in the object's length
     * word, the way java.awt.Font's does. */
    { "java/awt/FontMetrics", { NULL } },
    { NULL, { NULL } }
};

static ps_jclass *find_loaded(ps_jvm *vm, const char *name)
{
    int i;

    for(i = 0; i < vm->nclasses; i++) {
        if(!strcmp(vm->classes[i]->name, name))
            return vm->classes[i];
    }
    return NULL;
}

/* Builds one of the five and hangs it on the VM.
 *
 * Names point into a single blob held as the class's `raw` image, which is
 * what ps_jclass_free expects to own - the same arrangement ps_jvm.c uses for
 * the classes it synthesises, so nothing has to know these were built here. */
static void define_geom(ps_jvm *vm, int which)
{
    const char *name = g_geom[which].name;
    ps_jclass  *c;
    char       *blob;
    size_t      n, at;
    int         i, nf;

    if(vm->nclasses >= PS_JVM_MAX_CLASSES)
        return;

    n = strlen(name) + 1;
    for(nf = 0; g_geom[which].fields[nf]; nf++)
        n += strlen(g_geom[which].fields[nf]) + 1;

    c    = (ps_jclass *)calloc(1, sizeof *c);
    blob = (char *)malloc(n);
    if(!c || !blob) {
        free(c);
        free(blob);
        return;
    }

    memcpy(blob, name, strlen(name) + 1);
    at = strlen(name) + 1;

    c->raw     = (uint8_t *)blob;
    c->raw_len = n;
    c->name    = blob;

    /* Object rather than nothing: it is what these extend, and it is also
     * what makes an unrecognised method arrive here instead of failing as
     * "method not found". */
    c->super = ps_jvm_class(vm, "java/lang/Object");

    if(nf) {
        c->fields = (ps_jfield *)calloc((size_t)nf, sizeof(ps_jfield));
        if(!c->fields) {
            free(blob);
            free(c);
            return;
        }
        for(i = 0; i < nf; i++) {
            const char *fn = g_geom[which].fields[i];

            memcpy(blob + at, fn, strlen(fn) + 1);
            c->fields[i].name = blob + at;
            c->fields[i].desc = "I";
            c->fields[i].slot = (uint16_t)i;
            c->fields[i].kind = PS_T_INT;
            at += strlen(fn) + 1;
        }
        c->field_count = (uint16_t)nf;
    }
    c->inst_slots = (uint16_t)nf;

    vm->classes[vm->nclasses++] = c;
}

/* Registers all five, once per VM.
 *
 * It has to have happened before the applet's own code runs, because `new
 * java/awt/Dimension` resolves the class before any call reaches this file.
 * The trigger is any constructor: an applet's <init> opens with an
 * invokespecial on its superclass, which is a native class, which is a call
 * through here - so this fires exactly once, before init() is entered.
 *
 * Cheap to ask twice; the first name found means the set is already in. */
static void geom_register(ps_jvm *vm)
{
    int i;

    if(find_loaded(vm, g_geom[0].name))
        return;

    for(i = 0; g_geom[i].name; i++)
        define_geom(vm, i);
}

/* --- field access --------------------------------------------------------
 *
 * Bounds-checked against the instance rather than the table, because a
 * bytecode subclass of Dimension is a legal thing to write and its slot zero
 * is still Dimension's width.
 */
static int32_t gf(const ps_jobj *o, int slot)
{
    if(!o || !o->fields || !o->cls || slot >= o->cls->inst_slots)
        return 0;
    return o->fields[slot].i;
}

static void sf(ps_jobj *o, int slot, int32_t v)
{
    if(!o || !o->fields || !o->cls || slot >= o->cls->inst_slots)
        return;
    o->fields[slot].i = v;
}

/* Non-zero when the object is an instance of exactly that class.
 *
 * Exactly, not "or a subclass of": the VM takes casts on trust and has no
 * hierarchy walk to offer. equals() is the only caller and the distinction
 * only shows up for a bytecode subclass of Dimension, which is not a thing
 * anyone writes. What it does have to get right is that a Dimension is never
 * equal to a Point however equal the two numbers are, and it does. */
static int is_a(const ps_jobj *o, const char *name)
{
    return o && o->cls && !strcmp(o->cls->name, name);
}

static ps_jobj *geom_new(ps_jvm *vm, const char *name)
{
    geom_register(vm);
    return ps_jvm_new(vm, ps_jvm_class(vm, name));
}

ps_jobj *ps_jgeom_dimension(ps_jvm *vm, int32_t w, int32_t h)
{
    ps_jobj *o = geom_new(vm, "java/awt/Dimension");

    sf(o, D_W, w);
    sf(o, D_H, h);
    return o;
}

ps_jobj *ps_jgeom_point(ps_jvm *vm, int32_t x, int32_t y)
{
    ps_jobj *o = geom_new(vm, "java/awt/Point");

    sf(o, P_X, x);
    sf(o, P_Y, y);
    return o;
}

ps_jobj *ps_jgeom_rect(ps_jvm *vm, int32_t x, int32_t y, int32_t w, int32_t h)
{
    ps_jobj *o = geom_new(vm, "java/awt/Rectangle");

    sf(o, R_X, x);
    sf(o, R_Y, y);
    sf(o, R_W, w);
    sf(o, R_H, h);
    return o;
}

int ps_jgeom_dim_of(const ps_jobj *o, int32_t *w, int32_t *h)
{
    /* Exactly the class, not a subclass: a bytecode class that happens to have
     * two int fields is not a Dimension and reading its slots as one would be
     * a wrong answer rather than a refused one. */
    if(!is_a(o, "java/awt/Dimension"))
        return 0;
    *w = gf(o, D_W);
    *h = gf(o, D_H);
    return 1;
}

int ps_jgeom_rect_of(const ps_jobj *o, int32_t *x, int32_t *y,
                     int32_t *w, int32_t *h)
{
    if(!is_a(o, "java/awt/Rectangle"))
        return 0;
    *x = gf(o, R_X);
    *y = gf(o, R_Y);
    *w = gf(o, R_W);
    *h = gf(o, R_H);
    return 1;
}

/* --- shared helpers ------------------------------------------------------ */

/* Matches on name and, when it matters, descriptor - the same helper ps_jre.c
 * uses, and needed far more here because these classes are nearly all
 * overloads. */
static int is(const char *name, const char *desc, const char *n,
              const char *d)
{
    if(strcmp(name, n))
        return 0;
    return d ? !strcmp(desc, d) : 1;
}

static ps_jobj *str_of(ps_jvm *vm, const char *buf)
{
    return ps_jvm_new_string(vm, buf, strlen(buf));
}

/* The applet's box.
 *
 * vm->gfx is the applet's own context, not a Graphics an applet may have
 * translated or clipped, so this is the component's size and not the current
 * drawing region. In vector mode there is no surface to ask - the clip is
 * initialised to the box and is the only record of it. */
static void applet_box(const ps_jvm *vm, int *w, int *h)
{
    const ps_jgfx *g = vm->gfx;

    *w = 0;
    *h = 0;
    if(!g)
        return;

    if(g->s) {
        *w = g->s->w;
        *h = g->s->h;
    }
    else {
        *w = g->cx1 - g->cx0;
        *h = g->cy1 - g->cy0;
    }
}

/* --- java.awt.Dimension -------------------------------------------------- */

static int dimension(ps_jvm *vm, const char *n, const char *d, ps_jslot *args,
                     int nargs, ps_jslot *ret, int *handled)
{
    ps_jobj *self = args[0].o;

    *handled = 1;

    if(is(n, d, "<init>", "(II)V")) {
        if(nargs >= 3) {
            sf(self, D_W, args[1].i);
            sf(self, D_H, args[2].i);
        }
        return 0;
    }
    if(is(n, d, "<init>", "(Ljava/awt/Dimension;)V")) {
        if(nargs >= 2 && args[1].o) {
            sf(self, D_W, gf(args[1].o, D_W));
            sf(self, D_H, gf(args[1].o, D_H));
        }
        return 0;
    }
    if(is(n, d, "<init>", "()V"))
        return 0;                              /* zeroed by the allocator */

    if(is(n, d, "setSize", "(II)V")) {
        if(nargs >= 3) {
            sf(self, D_W, args[1].i);
            sf(self, D_H, args[2].i);
        }
        return 0;
    }
    if(is(n, d, "setSize", "(Ljava/awt/Dimension;)V")) {
        if(nargs >= 2 && args[1].o) {
            sf(self, D_W, gf(args[1].o, D_W));
            sf(self, D_H, gf(args[1].o, D_H));
        }
        return 0;
    }
    if(is(n, d, "getSize", NULL)) {
        /* A copy, which is what the real one hands back - an applet that
         * mutates it must not be resizing itself by accident. */
        ret->o = ps_jgeom_dimension(vm, gf(self, D_W), gf(self, D_H));
        return 0;
    }
    if(is(n, d, "getWidth", NULL) || is(n, d, "getHeight", NULL)) {
        /* Doubles in the real API, since Dimension2D is the declared type. */
        ret->d = (double)gf(self, n[3] == 'W' ? D_W : D_H);
        return 0;
    }
    if(is(n, d, "equals", NULL)) {
        ps_jobj *o = nargs >= 2 ? args[1].o : NULL;

        ret->i = (is_a(o, "java/awt/Dimension") &&
                  gf(o, D_W) == gf(self, D_W) &&
                  gf(o, D_H) == gf(self, D_H)) ? 1 : 0;
        return 0;
    }
    if(is(n, d, "toString", NULL)) {
        char buf[96];

        snprintf(buf, sizeof buf, "java.awt.Dimension[width=%d,height=%d]",
                 (int)gf(self, D_W), (int)gf(self, D_H));
        ret->o = str_of(vm, buf);
        return 0;
    }

    *handled = 0;
    return 0;
}

/* --- java.awt.Point ------------------------------------------------------ */

static int point(ps_jvm *vm, const char *n, const char *d, ps_jslot *args,
                 int nargs, ps_jslot *ret, int *handled)
{
    ps_jobj *self = args[0].o;

    *handled = 1;

    if(is(n, d, "<init>", "(II)V")) {
        if(nargs >= 3) {
            sf(self, P_X, args[1].i);
            sf(self, P_Y, args[2].i);
        }
        return 0;
    }
    if(is(n, d, "<init>", "(Ljava/awt/Point;)V")) {
        if(nargs >= 2 && args[1].o) {
            sf(self, P_X, gf(args[1].o, P_X));
            sf(self, P_Y, gf(args[1].o, P_Y));
        }
        return 0;
    }
    if(is(n, d, "<init>", "()V"))
        return 0;

    /* move and setLocation are the 1.0 and 1.1 spellings of the same thing. */
    if(is(n, d, "move", "(II)V") || is(n, d, "setLocation", "(II)V")) {
        if(nargs >= 3) {
            sf(self, P_X, args[1].i);
            sf(self, P_Y, args[2].i);
        }
        return 0;
    }
    if(is(n, d, "setLocation", "(Ljava/awt/Point;)V")) {
        if(nargs >= 2 && args[1].o) {
            sf(self, P_X, gf(args[1].o, P_X));
            sf(self, P_Y, gf(args[1].o, P_Y));
        }
        return 0;
    }
    if(is(n, d, "translate", "(II)V")) {
        if(nargs >= 3) {
            sf(self, P_X, gf(self, P_X) + args[1].i);
            sf(self, P_Y, gf(self, P_Y) + args[2].i);
        }
        return 0;
    }
    if(is(n, d, "getLocation", NULL)) {
        ret->o = ps_jgeom_point(vm, gf(self, P_X), gf(self, P_Y));
        return 0;
    }
    if(is(n, d, "getX", NULL) || is(n, d, "getY", NULL)) {
        ret->d = (double)gf(self, n[3] == 'X' ? P_X : P_Y);
        return 0;
    }
    if(is(n, d, "equals", NULL)) {
        ps_jobj *o = nargs >= 2 ? args[1].o : NULL;

        ret->i = (is_a(o, "java/awt/Point") &&
                  gf(o, P_X) == gf(self, P_X) &&
                  gf(o, P_Y) == gf(self, P_Y)) ? 1 : 0;
        return 0;
    }
    if(is(n, d, "toString", NULL)) {
        char buf[96];

        snprintf(buf, sizeof buf, "java.awt.Point[x=%d,y=%d]",
                 (int)gf(self, P_X), (int)gf(self, P_Y));
        ret->o = str_of(vm, buf);
        return 0;
    }

    *handled = 0;
    return 0;
}

/* --- java.awt.Rectangle --------------------------------------------------
 *
 * Everything below works in int and cannot overflow the way the real one
 * guards against, because an applet's coordinates are screen coordinates: the
 * distance between the two extremes of a 640x480 box does not come near the
 * top of the range. Where the real API's overflow handling is *observable* -
 * a negative extent surviving intersection() - it is reproduced.
 */
static int rect_empty(const ps_jobj *r)
{
    return gf(r, R_W) <= 0 || gf(r, R_H) <= 0;
}

static void rect_set(ps_jobj *r, int32_t x, int32_t y, int32_t w, int32_t h)
{
    sf(r, R_X, x);
    sf(r, R_Y, y);
    sf(r, R_W, w);
    sf(r, R_H, h);
}

/* union, in place. The negative-extent rule is the surprising half: a box with
 * a negative width is ignored entirely, while one that is merely zero-sized
 * takes part and can move the result's origin. */
static void rect_union(ps_jobj *a, const ps_jobj *b)
{
    int32_t ax1, ay1, ax2, ay2, bx1, by1, bx2, by2;

    if(gf(a, R_W) < 0 || gf(a, R_H) < 0) {
        rect_set(a, gf(b, R_X), gf(b, R_Y), gf(b, R_W), gf(b, R_H));
        return;
    }
    if(gf(b, R_W) < 0 || gf(b, R_H) < 0)
        return;

    ax1 = gf(a, R_X); ay1 = gf(a, R_Y);
    ax2 = ax1 + gf(a, R_W); ay2 = ay1 + gf(a, R_H);
    bx1 = gf(b, R_X); by1 = gf(b, R_Y);
    bx2 = bx1 + gf(b, R_W); by2 = by1 + gf(b, R_H);

    if(bx1 < ax1) ax1 = bx1;
    if(by1 < ay1) ay1 = by1;
    if(bx2 > ax2) ax2 = bx2;
    if(by2 > ay2) ay2 = by2;

    rect_set(a, ax1, ay1, ax2 - ax1, ay2 - ay1);
}

/* add(x, y): the box grows to reach the point, which leaves the point on the
 * far edge rather than inside. A negative-extent box is replaced by the point
 * instead of being grown from it. */
static void rect_add_point(ps_jobj *a, int32_t px, int32_t py)
{
    int32_t x1, y1, x2, y2;

    if(gf(a, R_W) < 0 || gf(a, R_H) < 0) {
        rect_set(a, px, py, 0, 0);
        return;
    }

    x1 = gf(a, R_X); y1 = gf(a, R_Y);
    x2 = x1 + gf(a, R_W); y2 = y1 + gf(a, R_H);

    if(px < x1) x1 = px;
    if(px > x2) x2 = px;
    if(py < y1) y1 = py;
    if(py > y2) y2 = py;

    rect_set(a, x1, y1, x2 - x1, y2 - y1);
}

static int rect_contains_xy(const ps_jobj *r, int32_t x, int32_t y)
{
    if(rect_empty(r))
        return 0;
    return x >= gf(r, R_X) && y >= gf(r, R_Y) &&
           x < gf(r, R_X) + gf(r, R_W) && y < gf(r, R_Y) + gf(r, R_H);
}

static int rectangle(ps_jvm *vm, const char *n, const char *d, ps_jslot *args,
                     int nargs, ps_jslot *ret, int *handled)
{
    ps_jobj *self = args[0].o;
    ps_jobj *b    = nargs >= 2 ? args[1].o : NULL;

    *handled = 1;

    if(is(n, d, "<init>", NULL)) {
        /* Six constructors, told apart by descriptor alone. (II)V is a size,
         * not a position - the one place Rectangle disagrees with Point about
         * what a pair of ints means. */
        if(!strcmp(d, "(IIII)V") && nargs >= 5)
            rect_set(self, args[1].i, args[2].i, args[3].i, args[4].i);
        else if(!strcmp(d, "(II)V") && nargs >= 3)
            rect_set(self, 0, 0, args[1].i, args[2].i);
        else if(!strcmp(d, "(Ljava/awt/Rectangle;)V") && b)
            rect_set(self, gf(b, R_X), gf(b, R_Y), gf(b, R_W), gf(b, R_H));
        else if(!strcmp(d, "(Ljava/awt/Dimension;)V") && b)
            rect_set(self, 0, 0, gf(b, D_W), gf(b, D_H));
        else if(!strcmp(d, "(Ljava/awt/Point;)V") && b)
            rect_set(self, gf(b, P_X), gf(b, P_Y), 0, 0);
        else if(!strcmp(d, "(Ljava/awt/Point;Ljava/awt/Dimension;)V") &&
                nargs >= 3)
            rect_set(self, gf(args[1].o, P_X), gf(args[1].o, P_Y),
                     gf(args[2].o, D_W), gf(args[2].o, D_H));
        return 0;
    }

    /* reshape and setBounds are the 1.0 and 1.1 spellings. */
    if(is(n, d, "setBounds", "(IIII)V") || is(n, d, "reshape", "(IIII)V")) {
        if(nargs >= 5)
            rect_set(self, args[1].i, args[2].i, args[3].i, args[4].i);
        return 0;
    }
    if(is(n, d, "setBounds", "(Ljava/awt/Rectangle;)V")) {
        if(b)
            rect_set(self, gf(b, R_X), gf(b, R_Y), gf(b, R_W), gf(b, R_H));
        return 0;
    }
    if(is(n, d, "getBounds", NULL)) {
        ret->o = ps_jgeom_rect(vm, gf(self, R_X), gf(self, R_Y),
                               gf(self, R_W), gf(self, R_H));
        return 0;
    }
    if(is(n, d, "setSize", "(II)V") || is(n, d, "resize", "(II)V")) {
        if(nargs >= 3) {
            sf(self, R_W, args[1].i);
            sf(self, R_H, args[2].i);
        }
        return 0;
    }
    if(is(n, d, "setSize", "(Ljava/awt/Dimension;)V")) {
        if(b) {
            sf(self, R_W, gf(b, D_W));
            sf(self, R_H, gf(b, D_H));
        }
        return 0;
    }
    if(is(n, d, "getSize", NULL)) {
        ret->o = ps_jgeom_dimension(vm, gf(self, R_W), gf(self, R_H));
        return 0;
    }
    if(is(n, d, "setLocation", "(II)V") || is(n, d, "move", "(II)V")) {
        if(nargs >= 3) {
            sf(self, R_X, args[1].i);
            sf(self, R_Y, args[2].i);
        }
        return 0;
    }
    if(is(n, d, "setLocation", "(Ljava/awt/Point;)V")) {
        if(b) {
            sf(self, R_X, gf(b, P_X));
            sf(self, R_Y, gf(b, P_Y));
        }
        return 0;
    }
    if(is(n, d, "getLocation", NULL)) {
        ret->o = ps_jgeom_point(vm, gf(self, R_X), gf(self, R_Y));
        return 0;
    }
    if(is(n, d, "translate", "(II)V")) {
        if(nargs >= 3) {
            sf(self, R_X, gf(self, R_X) + args[1].i);
            sf(self, R_Y, gf(self, R_Y) + args[2].i);
        }
        return 0;
    }
    if(is(n, d, "grow", "(II)V")) {
        if(nargs >= 3) {
            sf(self, R_X, gf(self, R_X) - args[1].i);
            sf(self, R_Y, gf(self, R_Y) - args[2].i);
            sf(self, R_W, gf(self, R_W) + 2 * args[1].i);
            sf(self, R_H, gf(self, R_H) + 2 * args[2].i);
        }
        return 0;
    }
    if(is(n, d, "isEmpty", NULL)) {
        ret->i = rect_empty(self);
        return 0;
    }

    /* inside is the 1.0 spelling of contains(int, int). */
    if(is(n, d, "contains", "(II)Z") || is(n, d, "inside", "(II)Z")) {
        ret->i = nargs >= 3 ? rect_contains_xy(self, args[1].i, args[2].i) : 0;
        return 0;
    }
    if(is(n, d, "contains", "(Ljava/awt/Point;)Z")) {
        ret->i = b ? rect_contains_xy(self, gf(b, P_X), gf(b, P_Y)) : 0;
        return 0;
    }
    if(is(n, d, "contains", "(Ljava/awt/Rectangle;)Z")) {
        ret->i = 0;
        if(b && !rect_empty(self) && !rect_empty(b))
            ret->i = gf(b, R_X) >= gf(self, R_X) &&
                     gf(b, R_Y) >= gf(self, R_Y) &&
                     gf(b, R_X) + gf(b, R_W) <= gf(self, R_X) + gf(self, R_W) &&
                     gf(b, R_Y) + gf(b, R_H) <= gf(self, R_Y) + gf(self, R_H);
        return 0;
    }
    if(is(n, d, "intersects", NULL)) {
        ret->i = 0;
        if(b && !rect_empty(self) && !rect_empty(b))
            ret->i = gf(b, R_X) + gf(b, R_W) > gf(self, R_X) &&
                     gf(b, R_Y) + gf(b, R_H) > gf(self, R_Y) &&
                     gf(b, R_X) < gf(self, R_X) + gf(self, R_W) &&
                     gf(b, R_Y) < gf(self, R_Y) + gf(self, R_H);
        return 0;
    }
    if(is(n, d, "intersection", NULL)) {
        int32_t x1 = gf(self, R_X), y1 = gf(self, R_Y);
        int32_t x2 = x1 + gf(self, R_W), y2 = y1 + gf(self, R_H);

        if(b) {
            if(gf(b, R_X) > x1) x1 = gf(b, R_X);
            if(gf(b, R_Y) > y1) y1 = gf(b, R_Y);
            if(gf(b, R_X) + gf(b, R_W) < x2) x2 = gf(b, R_X) + gf(b, R_W);
            if(gf(b, R_Y) + gf(b, R_H) < y2) y2 = gf(b, R_Y) + gf(b, R_H);
        }
        /* Negative extents on purpose: that is what the real one returns for
         * boxes that do not meet, and isEmpty() reads them as empty. */
        ret->o = ps_jgeom_rect(vm, x1, y1, x2 - x1, y2 - y1);
        return 0;
    }
    if(is(n, d, "union", NULL)) {
        ps_jobj *u = ps_jgeom_rect(vm, gf(self, R_X), gf(self, R_Y),
                                   gf(self, R_W), gf(self, R_H));

        if(u && b)
            rect_union(u, b);
        ret->o = u;
        return 0;
    }
    if(is(n, d, "add", "(II)V")) {
        if(nargs >= 3)
            rect_add_point(self, args[1].i, args[2].i);
        return 0;
    }
    if(is(n, d, "add", "(Ljava/awt/Point;)V")) {
        if(b)
            rect_add_point(self, gf(b, P_X), gf(b, P_Y));
        return 0;
    }
    if(is(n, d, "add", "(Ljava/awt/Rectangle;)V")) {
        if(b)
            rect_union(self, b);
        return 0;
    }
    if(is(n, d, "getX", NULL) || is(n, d, "getY", NULL) ||
       is(n, d, "getWidth", NULL) || is(n, d, "getHeight", NULL)) {
        ret->d = (double)gf(self, !strcmp(n, "getX")     ? R_X :
                                 !strcmp(n, "getY")     ? R_Y :
                                 !strcmp(n, "getWidth") ? R_W : R_H);
        return 0;
    }
    if(is(n, d, "equals", NULL)) {
        ret->i = (is_a(b, "java/awt/Rectangle") &&
                  gf(b, R_X) == gf(self, R_X) &&
                  gf(b, R_Y) == gf(self, R_Y) &&
                  gf(b, R_W) == gf(self, R_W) &&
                  gf(b, R_H) == gf(self, R_H)) ? 1 : 0;
        return 0;
    }
    if(is(n, d, "toString", NULL)) {
        char buf[128];

        snprintf(buf, sizeof buf,
                 "java.awt.Rectangle[x=%d,y=%d,width=%d,height=%d]",
                 (int)gf(self, R_X), (int)gf(self, R_Y),
                 (int)gf(self, R_W), (int)gf(self, R_H));
        ret->o = str_of(vm, buf);
        return 0;
    }

    *handled = 0;
    return 0;
}

/* --- java.awt.Insets ----------------------------------------------------- */

static int insets(ps_jvm *vm, const char *n, const char *d, ps_jslot *args,
                  int nargs, ps_jslot *ret, int *handled)
{
    ps_jobj *self = args[0].o;

    *handled = 1;

    if(is(n, d, "<init>", "(IIII)V")) {
        if(nargs >= 5) {
            sf(self, I_TOP,    args[1].i);
            sf(self, I_LEFT,   args[2].i);
            sf(self, I_BOTTOM, args[3].i);
            sf(self, I_RIGHT,  args[4].i);
        }
        return 0;
    }
    if(is(n, d, "<init>", NULL))
        return 0;
    if(is(n, d, "equals", NULL)) {
        ps_jobj *o = nargs >= 2 ? args[1].o : NULL;

        ret->i = (is_a(o, "java/awt/Insets") &&
                  gf(o, I_TOP)    == gf(self, I_TOP) &&
                  gf(o, I_LEFT)   == gf(self, I_LEFT) &&
                  gf(o, I_BOTTOM) == gf(self, I_BOTTOM) &&
                  gf(o, I_RIGHT)  == gf(self, I_RIGHT)) ? 1 : 0;
        return 0;
    }
    if(is(n, d, "toString", NULL)) {
        char buf[128];

        snprintf(buf, sizeof buf,
                 "java.awt.Insets[top=%d,left=%d,bottom=%d,right=%d]",
                 (int)gf(self, I_TOP), (int)gf(self, I_LEFT),
                 (int)gf(self, I_BOTTOM), (int)gf(self, I_RIGHT));
        ret->o = str_of(vm, buf);
        return 0;
    }

    *handled = 0;
    return 0;
}

/* --- java.awt.FontMetrics ------------------------------------------------
 *
 * The size is the whole of the state, and it rides in the object's length word
 * exactly as java.awt.Font's does - there is one face in this browser, so a
 * metric is a function of the size and nothing else.
 *
 * Every measurement below comes from the same text backend drawString uses.
 * Nothing here is a constant fitted to a font: a stringWidth that disagrees
 * with what drawString lays down mis-centres every label an applet draws, and
 * would look like a layout bug rather than a metrics one.
 */
static int fm_px(const ps_jvm *vm, const ps_jobj *o)
{
    if(o && o->len > 0)
        return o->len;
    return vm->gfx ? vm->gfx->font_px : 12;
}

static int fm_measure(const ps_jvm *vm, const char *s, size_t len, int px)
{
    ps_jgfx tmp;

    if(!vm->gfx || !s || !len)
        return 0;

    /* A copy of the applet's own context with only the size changed, so this
     * goes down whichever text path is live - the glyph cache in vector mode,
     * stb_truetype on the host - without knowing which. */
    ps_jgfx_copy(&tmp, vm->gfx);
    ps_jgfx_set_font_size(&tmp, px);
    return ps_jgfx_string_width(&tmp, s, len);
}

/* Vector mode's ops table carries no vertical metrics - it hands text to the
 * page's glyph cache, which does its own layout. These proportions are the
 * fallback for that path only; every other path asks the font. */
static int fm_ascent(const ps_jvm *vm, int px)
{
    if(vm->gfx && vm->gfx->text && vm->gfx->text->ascent)
        return vm->gfx->text->ascent(vm->gfx->text->user, px);
    return (px * 4) / 5;
}

static int fm_descent(const ps_jvm *vm, int px)
{
    if(vm->gfx && vm->gfx->text && vm->gfx->text->descent)
        return vm->gfx->text->descent(vm->gfx->text->user, px);
    return px / 5;
}

static int fontmetrics(ps_jvm *vm, const char *n, const char *d,
                       ps_jslot *args, int nargs, ps_jslot *ret, int *handled)
{
    int px = fm_px(vm, args[0].o);

    *handled = 1;

    if(is(n, d, "stringWidth", NULL)) {
        size_t      len = 0;
        const char *s   = nargs >= 2 ? ps_jvm_string_utf8(args[1].o, &len)
                                     : NULL;

        ret->i = fm_measure(vm, s, len, px);
        return 0;
    }
    if(is(n, d, "charWidth", NULL)) {
        char c = nargs >= 2 ? (char)args[1].i : ' ';

        /* charWidth(char) and charWidth(int) are the same measurement; the
         * runtime sees one int either way. */
        ret->i = fm_measure(vm, &c, 1, px);
        return 0;
    }
    if(is(n, d, "charsWidth", NULL) || is(n, d, "bytesWidth", NULL)) {
        ps_jobj *a   = nargs >= 2 ? args[1].o : NULL;
        int32_t  off = nargs >= 3 ? args[2].i : 0;
        int32_t  cnt = nargs >= 4 ? args[3].i : 0;
        char    *buf;
        int32_t  i;

        ret->i = 0;
        if(!a || !a->data || off < 0 || cnt <= 0 || off + cnt > a->len)
            return 0;

        buf = (char *)malloc((size_t)cnt);
        if(!buf)
            return 0;
        for(i = 0; i < cnt; i++)
            buf[i] = (char)(a->elem == PS_T_CHAR
                            ? ((uint16_t *)a->data)[off + i]
                            : ((unsigned char *)a->data)[off + i]);
        ret->i = fm_measure(vm, buf, (size_t)cnt, px);
        free(buf);
        return 0;
    }
    if(is(n, d, "getAscent", NULL) || is(n, d, "getMaxAscent", NULL)) {
        ret->i = fm_ascent(vm, px);
        return 0;
    }
    if(is(n, d, "getDescent", NULL) || is(n, d, "getMaxDescent", NULL)) {
        ret->i = fm_descent(vm, px);
        return 0;
    }
    if(is(n, d, "getLeading", NULL)) {
        /* Zero, and honestly so: the text backends this runtime has report an
         * ascent and a descent and nothing about line gap. Inventing one would
         * space an applet's lines to a number no font agreed to. */
        ret->i = 0;
        return 0;
    }
    if(is(n, d, "getHeight", NULL)) {
        ret->i = fm_ascent(vm, px) + fm_descent(vm, px);
        return 0;
    }
    if(is(n, d, "getMaxAdvance", NULL)) {
        /* The widest glyph, which nothing here can enumerate. 'W' is the
         * widest in every Latin face and is what this is used to reserve room
         * for. */
        ret->i = fm_measure(vm, "W", 1, px);
        return 0;
    }
    if(is(n, d, "getFont", NULL)) {
        ps_jobj *f = ps_jvm_new(vm, ps_jvm_class(vm, "java/awt/Font"));

        if(f)
            f->len = px;
        ret->o = f;
        return 0;
    }
    if(is(n, d, "toString", NULL)) {
        char buf[96];

        snprintf(buf, sizeof buf,
                 "java.awt.FontMetrics[ascent=%d,descent=%d,height=%d]",
                 fm_ascent(vm, px), fm_descent(vm, px),
                 fm_ascent(vm, px) + fm_descent(vm, px));
        ret->o = str_of(vm, buf);
        return 0;
    }

    *handled = 0;
    return 0;
}

/* --- the accessors on Component -----------------------------------------
 *
 * An applet reaches these through its own class name - javac writes the
 * qualifying type into the constant pool - and the interpreter walks up to the
 * first native ancestor, which is java/applet/Applet. So this is spelled for
 * whichever of the AWT container classes the call resolved through.
 *
 * The box is the one the browser gave the applet. There is no layout here and
 * there is not going to be: the size is decided by the page.
 */
static ps_jobj *new_metrics(ps_jvm *vm, int px)
{
    ps_jobj *fm = geom_new(vm, "java/awt/FontMetrics");

    if(fm)
        fm->len = px > 0 ? px : 12;
    return fm;
}

static int component(ps_jvm *vm, const char *n, const char *d, ps_jslot *args,
                     int nargs, ps_jslot *ret, int *handled)
{
    ps_jobj *self = nargs >= 1 ? args[0].o : NULL;
    int      w, h;

    *handled = 1;

    /* getFontMetrics(Font) is a function of its argument and nothing else, so
     * it is the same answer whoever asks - which is why it comes before the
     * receiver is looked at. It is also the one of these the widget shells
     * needed: three compatibility applets measure a Font through a
     * TextField. */
    if(is(n, d, "getFontMetrics", "(Ljava/awt/Font;)Ljava/awt/FontMetrics;")) {
        ps_jobj *f = nargs >= 2 ? args[1].o : NULL;

        ret->o = new_metrics(vm, f && f->len > 0 ? f->len
                                 : (vm->gfx ? vm->gfx->font_px : 12));
        return 0;
    }

    /* Everything below answers from the applet's own box and the applet's own
     * font, and that is only the truth when the applet is what is being asked.
     * A widget has neither until it is given one, and where it keeps what it
     * was given is ps_jawt.c - so it gets the call instead. Declining is the
     * whole mechanism: ps_jre_call offers this file first and ps_jawt.c next.
     *
     * The test is on the receiver rather than the class name because the name
     * is the static type javac wrote, and a Fendt panel sets a Button's font
     * through java/awt/Component.setFont. See ps_jawt.h. */
    if(ps_jawt_is_widget(self)) {
        *handled = 0;
        return 0;
    }

    applet_box(vm, &w, &h);

    /* size() is 1.0 and getSize() is 1.1; preferred and minimum are the same
     * box, because a browser has already decided how big the applet is and
     * nothing an applet returns here can change it. */
    if(is(n, d, "size", "()Ljava/awt/Dimension;") ||
       is(n, d, "getSize", "()Ljava/awt/Dimension;") ||
       is(n, d, "preferredSize", "()Ljava/awt/Dimension;") ||
       is(n, d, "getPreferredSize", "()Ljava/awt/Dimension;") ||
       is(n, d, "minimumSize", "()Ljava/awt/Dimension;") ||
       is(n, d, "getMinimumSize", "()Ljava/awt/Dimension;")) {
        ret->o = ps_jgeom_dimension(vm, w, h);
        return 0;
    }
    if(is(n, d, "bounds", "()Ljava/awt/Rectangle;") ||
       is(n, d, "getBounds", "()Ljava/awt/Rectangle;")) {
        /* At the origin: an applet's bounds are in its parent's coordinates,
         * and its parent is the page, which it cannot see. Every applet that
         * reads this wants the size. */
        ret->o = ps_jgeom_rect(vm, 0, 0, w, h);
        return 0;
    }
    if(is(n, d, "location", "()Ljava/awt/Point;") ||
       is(n, d, "getLocation", "()Ljava/awt/Point;")) {
        ret->o = ps_jgeom_point(vm, 0, 0);
        return 0;
    }
    if(is(n, d, "insets", "()Ljava/awt/Insets;") ||
       is(n, d, "getInsets", "()Ljava/awt/Insets;")) {
        ret->o = geom_new(vm, "java/awt/Insets");
        return 0;
    }

    /* Graphics.setFont already existed; this is the Component spelling, and it
     * is the one applets use when they set a font in init() and then never
     * touch it again. Both land on the same context. */
    if(is(n, d, "setFont", "(Ljava/awt/Font;)V")) {
        if(nargs >= 2 && args[1].o && vm->gfx)
            ps_jgfx_set_font_size(vm->gfx, args[1].o->len);
        return 0;
    }
    if(is(n, d, "getFont", "()Ljava/awt/Font;")) {
        ps_jobj *f = ps_jvm_new(vm, ps_jvm_class(vm, "java/awt/Font"));

        if(f)
            f->len = vm->gfx ? vm->gfx->font_px : 12;
        ret->o = f;
        return 0;
    }

    *handled = 0;
    return 0;
}

/* --- java.awt.Graphics --------------------------------------------------- */

static int graphics(ps_jvm *vm, const char *n, const char *d, ps_jslot *args,
                    int nargs, ps_jslot *ret, int *handled)
{
    /* The receiver's own context when it has one - an applet that took a
     * create() copy and set a font on it must measure in that font. */
    ps_jgfx *g = (nargs >= 1 && args[0].o && args[0].o->native)
               ? (ps_jgfx *)args[0].o->native : vm->gfx;

    if(is(n, d, "getFontMetrics", NULL)) {
        ps_jobj *f = (nargs >= 2) ? args[1].o : NULL;

        /* getFontMetrics() takes the context's font; getFontMetrics(Font)
         * takes the one it was handed. */
        ret->o = new_metrics(vm, f && f->len > 0 ? f->len
                                 : (g ? g->font_px : 12));
        *handled = 1;
        return 0;
    }
    if(is(n, d, "getFont", "()Ljava/awt/Font;")) {
        ps_jobj *f = ps_jvm_new(vm, ps_jvm_class(vm, "java/awt/Font"));

        if(f)
            f->len = g ? g->font_px : 12;
        ret->o = f;
        *handled = 1;
        return 0;
    }
    if(is(n, d, "getClipBounds", NULL) || is(n, d, "getClipRect", NULL)) {
        /* In the applet's own coordinates, so the translate comes back off. */
        ret->o = g ? ps_jgeom_rect(vm, g->cx0 - g->tx, g->cy0 - g->ty,
                                   g->cx1 - g->cx0, g->cy1 - g->cy0)
                   : NULL;
        *handled = 1;
        return 0;
    }

    *handled = 0;
    return 0;
}

/* --- the router ---------------------------------------------------------- */

/* This used to be four strcmps against Component, Container, Panel and Applet,
 * and the four were the whole of what this file thought a Component was. The
 * twenty-three widget shells ps_jawt.c registers were not among them, so a
 * Button never reached size(), a TextField never reached setFont() and a
 * Checkbox never reached getFontMetrics(), and it was two lists rather than
 * one bug. There is now
 * one predicate and it is ps_jawt.c's, because that is where the class table
 * that would go out of date lives. */
int ps_jgeom_call(ps_jvm *vm, const char *cls, const char *name,
                  const char *desc, ps_jslot *args, int nargs, ps_jslot *ret,
                  int *handled)
{
    *handled = 0;

    /* Graphics first, and by a wide margin the reason the order matters: it is
     * the hottest class in the runtime, this function is now on the front of
     * every native call, and nothing below can ever match it. */
    if(!strcmp(cls, "java/awt/Graphics"))
        return graphics(vm, name, desc, args, nargs, ret, handled);

    if(ps_jawt_is_component(cls)) {
        /* Registration hangs off this rather than off a flag, because an
         * applet's <init> opens with an invokespecial on its superclass and
         * that superclass is one of these. It therefore happens before init()
         * is entered and before any bytecode can name a geometry class. */
        geom_register(vm);
        return component(vm, name, desc, args, nargs, ret, handled);
    }

    /* An instance call on one of the five arrives named java/lang/Object,
     * because that is the first ancestor with an implementation. Argument
     * zero is always the receiver here - java.lang.Object has no static
     * methods for it to be anything else - so it is safe to read as one,
     * which it would not be for a class that has. */
    if(!strcmp(cls, "java/lang/Object")) {
        ps_jobj *self = (nargs >= 1 && args) ? args[0].o : NULL;

        if(name[0] == '<')
            geom_register(vm);

        if(self && self->cls) {
            const char *rc = self->cls->name;

            if(!strcmp(rc, "java/awt/Dimension"))
                return dimension(vm, name, desc, args, nargs, ret, handled);
            if(!strcmp(rc, "java/awt/Point"))
                return point(vm, name, desc, args, nargs, ret, handled);
            if(!strcmp(rc, "java/awt/Rectangle"))
                return rectangle(vm, name, desc, args, nargs, ret, handled);
            if(!strcmp(rc, "java/awt/Insets"))
                return insets(vm, name, desc, args, nargs, ret, handled);
            if(!strcmp(rc, "java/awt/FontMetrics"))
                return fontmetrics(vm, name, desc, args, nargs, ret, handled);
        }
    }

    return 0;
}
