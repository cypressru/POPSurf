/* java.awt.Polygon, the Graphics calls that take one, and four singles.
 *
 * The compatibility corpus ranks java.awt.Polygon as the largest missing class
 * in a corpus of sixty-nine applets from five authors, and the only entry that
 * three independent authors hit *first*. Physics applets draw their arrows out
 * of it and Sun's GraphicsTest draws its shapes out of it. It needs no layout,
 * no events and no image pipeline, which is why it is here rather than in the
 * project it sits next to.
 *
 * Why Polygon is not in ps_jvm.c's native list, which is where most JDK
 * classes in this runtime live:
 *
 * A native class answers a field read through ps_jre_call and the interpreter
 * makes that call with no receiver, because a native class is not expected to
 * have per-instance state a getfield can see. Polygon cannot live with that -
 * `p.npoints` and `p.xpoints[i]` are how applets of the period use it, and
 * `p.xpoints[i] = x` is how they animate one. putfield is worse: it goes
 * straight to the slot path and fails outright on a class with no fields. So
 * Polygon is registered as an ordinary field-carrying class that happens to
 * have no bytecode, exactly as ps_jgeom.c registers Dimension, and getfield
 * and putfield take the interpreter's normal slot path knowing nothing about
 * any of this. Its methods, having no code, fall through to the first native
 * ancestor - java/lang/Object - and arrive here named java/lang/Object with
 * the real receiver in argument zero. That indirection is confined to the
 * router at the bottom.
 *
 * Semantics are AWT's and were captured by running a real JDK on the host, not
 * by reading one. The ones that are not guessable:
 *
 *  - The constructor *copies* its arrays, and copies exactly npoints of them:
 *    the array you handed in and the one on the polygon are different objects
 *    and usually different lengths.
 *  - addPoint grows the arrays to the next power of two strictly greater than
 *    npoints, with a floor of four. `new Polygon().xpoints.length` is 4.
 *  - getBounds caches, and the cache is *observably stale* after an applet
 *    writes into xpoints directly. invalidate() is the documented way out and
 *    the reason it exists. contains() shares the cache, so it goes stale too.
 *  - contains() is even-odd, and a point exactly on the left or top edge of a
 *    box is inside while one on the right or bottom edge is outside. The rule
 *    below was checked against a real Polygon over 676,000 point/polygon pairs
 *    of small random integer polygons - the shapes where vertices and edges
 *    land on the tested points constantly - with no disagreement.
 *  - draw3DRect and fill3DRect draw the exact spans mapped out in the comment
 *    above them, including what happens at zero and one pixel, and their
 *    highlight colours are Color.brighter()/darker() to the digit.
 */
#include "ps_jpoly.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ps_jgeom.h"
#include "ps_jlib.h"

/* Matches on name and, when it matters, descriptor. Passing NULL for the
 * descriptor accepts any overload. Same idiom as ps_jre.c, ps_jgeom.c and
 * ps_jlib.c - these files are dispatch tables and share the shape rather than
 * a symbol. */
static int is(const char *name, const char *desc, const char *n,
              const char *d)
{
    if(strcmp(name, n))
        return 0;
    return d ? !strcmp(desc, d) : 1;
}

/* Non-zero when the object is an instance of that class or of a subclass of
 * it.
 *
 * The chain rather than an exact name match, because `class Arrow extends
 * Polygon` is a reasonable thing for an applet to write and its inherited
 * fields land in the same slots - a subclass's own fields are allocated above
 * whatever its superclass declared. An Arrow reaches this file the same way a
 * Polygon does: no bytecode is found for addPoint anywhere in its chain, so
 * the interpreter walks up to the first native ancestor, which is
 * java/lang/Object, and calls in with the Arrow as the receiver. */
static int instance_of(const ps_jobj *o, const char *name)
{
    const ps_jclass *c;

    for(c = o ? o->cls : NULL; c; c = c->super) {
        if(!strcmp(c->name, name))
            return 1;
    }
    return 0;
}

static ps_jclass *find_loaded(ps_jvm *vm, const char *name)
{
    int i;

    for(i = 0; i < vm->nclasses; i++) {
        if(!strcmp(vm->classes[i]->name, name))
            return vm->classes[i];
    }
    return NULL;
}

/* --- the classes ---------------------------------------------------------
 *
 * Slot order is field order, and slot zero is the first field Polygon
 * declares: it derives from java/lang/Object, which contributes none.
 *
 * The three public fields are the ones an applet can name. The five after
 * them are the bounds cache, spelled with a leading '$' so that no name javac
 * writes for java.awt.Polygon can collide with them - the real class keeps the
 * same state in a `protected Rectangle bounds`, which no applet in the corpus
 * touches and which would cost an object per polygon here.
 */
enum { PF_N, PF_X, PF_Y, PF_BOK, PF_BX, PF_BY, PF_BW, PF_BH, PF_SLOTS };

typedef struct {
    const char *name;
    const char *desc;
    uint8_t     kind;
} poly_field;

/* xpoints and ypoints are PS_T_REF and that is load-bearing: the collector
 * traces an instance of a bytecode-shaped class by walking its class's field
 * table and following exactly the reference-kinded slots. Anything else here
 * and the arrays would be freed out from under a live polygon. */
static const poly_field g_poly_fields[] = {
    { "npoints", "I",  PS_T_INT },
    { "xpoints", "[I", PS_T_REF },
    { "ypoints", "[I", PS_T_REF },
    { "$bok",    "I",  PS_T_INT },
    { "$bx",     "I",  PS_T_INT },
    { "$by",     "I",  PS_T_INT },
    { "$bw",     "I",  PS_T_INT },
    { "$bh",     "I",  PS_T_INT }
};

/* Builds a class with no bytecode and hangs it on the VM.
 *
 * Field names live in a single blob held as the class's `raw` image, which is
 * what ps_jclass_free expects to own - the same arrangement ps_jvm.c uses for
 * the classes it synthesises and ps_jgeom.c for its five, so nothing has to
 * know these were built here. Descriptors are string literals and are not
 * copied; nothing frees them.
 *
 * `super` may be a class with instance slots of its own, in which case this
 * one's fields start above them. That is what java/util/Stack needs: it is a
 * Vector with five extra methods and no extra state.
 */
static ps_jclass *define_class(ps_jvm *vm, const char *name, ps_jclass *super,
                               const poly_field *f, int nf, int native)
{
    ps_jclass *c;
    char      *blob;
    size_t     n, at;
    uint16_t   base;
    int        i;

    if(vm->nclasses >= PS_JVM_MAX_CLASSES)
        return NULL;

    n = strlen(name) + 1;
    for(i = 0; i < nf; i++)
        n += strlen(f[i].name) + 1;

    c    = (ps_jclass *)calloc(1, sizeof *c);
    blob = (char *)malloc(n);
    if(!c || !blob) {
        free(c);
        free(blob);
        return NULL;
    }

    memcpy(blob, name, strlen(name) + 1);
    at = strlen(name) + 1;

    c->raw     = (uint8_t *)blob;
    c->raw_len = n;
    c->name    = blob;
    c->super   = super;
    c->native  = native;

    base = super ? super->inst_slots : 0;

    if(nf) {
        c->fields = (ps_jfield *)calloc((size_t)nf, sizeof(ps_jfield));
        if(!c->fields) {
            free(blob);
            free(c);
            return NULL;
        }
        for(i = 0; i < nf; i++) {
            memcpy(blob + at, f[i].name, strlen(f[i].name) + 1);
            c->fields[i].name = blob + at;
            c->fields[i].desc = f[i].desc;
            c->fields[i].slot = (uint16_t)(base + i);
            c->fields[i].kind = f[i].kind;
            at += strlen(f[i].name) + 1;
        }
        c->field_count = (uint16_t)nf;
    }
    c->inst_slots = (uint16_t)(base + nf);

    vm->classes[vm->nclasses++] = c;
    return c;
}

/* Registers the three, once per VM.
 *
 * It has to have happened before the applet's own code runs, because `new
 * java/awt/Polygon` resolves the class before any call reaches this file. The
 * trigger is any constructor: an applet's <init> opens with an invokespecial
 * on java/applet/Applet, which is a native class, which is a call through
 * ps_jre_call - so this fires exactly once, before init() is entered.
 *
 * java/util/Stack is registered last and its presence is the "already done"
 * flag, because it is the one that can fail to register: it needs
 * java/util/Vector, which ps_jlib.c puts in the table on the same trigger.
 * ps_jlib_call is offered the call before this file is, so Vector is always
 * there by now - but if it ever is not, this simply tries again on the next
 * constructor rather than defining a Stack with no superclass.
 */
static void poly_register(ps_jvm *vm)
{
    ps_jclass *vec;

    if(find_loaded(vm, "java/util/Stack"))
        return;

    if(!find_loaded(vm, "java/awt/Polygon")) {
        define_class(vm, "java/awt/Polygon",
                     ps_jvm_class(vm, "java/lang/Object"),
                     g_poly_fields, PF_SLOTS, 0);
    }

    /* The throwables this file raises that the interpreter does not already
     * carry. Each has to be in the class table for ps_jvm_throw to build one
     * and for an applet's catch clause to accept it - a throw whose class does
     * not resolve unwinds with no name on it and no handler will take it. The
     * interpreter's own table already knows what each of these extends.
     * Registered native and stateless, which is how ps_jre.c's throwable
     * branch expects to find them. */
    if(!find_loaded(vm, "java/util/EmptyStackException"))
        define_class(vm, "java/util/EmptyStackException", NULL, NULL, 0, 1);
    if(!find_loaded(vm, "java/lang/NegativeArraySizeException"))
        define_class(vm, "java/lang/NegativeArraySizeException", NULL, NULL,
                     0, 1);
    if(!find_loaded(vm, "java/lang/OutOfMemoryError"))
        define_class(vm, "java/lang/OutOfMemoryError", NULL, NULL, 0, 1);

    /* Stack *is* a Vector - not a copy of one. Being a non-native subclass of
     * a native class is what routes every inherited method to ps_jlib.c's own
     * Vector: the interpreter finds no bytecode on Stack, walks up to the
     * first native ancestor, and calls in named java/util/Vector with the
     * Stack as the receiver. Only the five methods Stack adds arrive here, and
     * they are implemented by calling back into that same Vector rather than
     * by touching its storage. */
    vec = find_loaded(vm, "java/util/Vector");
    if(vec)
        define_class(vm, "java/util/Stack", vec, NULL, 0, 0);
}

/* --- field access --------------------------------------------------------
 *
 * Bounds-checked against the instance rather than the table, because a
 * bytecode subclass of Polygon is a legal thing to write and its slot zero is
 * still Polygon's npoints.
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

static ps_jobj *gr(const ps_jobj *o, int slot)
{
    if(!o || !o->fields || !o->cls || slot >= o->cls->inst_slots)
        return NULL;
    return o->fields[slot].o;
}

static void sr(ps_jobj *o, int slot, ps_jobj *v)
{
    if(!o || !o->fields || !o->cls || slot >= o->cls->inst_slots)
        return;
    o->fields[slot].o = v;
}

/* An int array, and nothing else.
 *
 * xpoints and ypoints are public and their descriptor is not verified, so an
 * applet can putfield a byte[] into one - and reading four bytes per element
 * out of a one-byte-per-element buffer is a heap overflow on a machine with no
 * memory protection. Everything below reaches the coordinates through here,
 * and anything that is not an int array reads as empty. */
static int32_t arr_len(const ps_jobj *a)
{
    return (a && a->kind == PS_OBJ_ARRAY && a->elem == PS_T_INT && a->data)
         ? a->len : 0;
}

static int32_t arr_at(const ps_jobj *a, int32_t i)
{
    if(i < 0 || i >= arr_len(a))
        return 0;
    return ((int32_t *)a->data)[i];
}

static void arr_put(ps_jobj *a, int32_t i, int32_t v)
{
    if(i < 0 || i >= arr_len(a))
        return;
    ((int32_t *)a->data)[i] = v;
}

/* How many points there really are.
 *
 * npoints is public and an applet can set it to anything, including more than
 * the arrays hold. A real JDK answers that with an
 * ArrayIndexOutOfBoundsException from whichever method looked; this clamps
 * instead, which is the one place below that knowingly differs. Reading past
 * the array is not an option on a machine with no memory protection, and an
 * applet that has broken its own invariant is already drawing the wrong
 * picture either way. */
static int32_t poly_count(const ps_jobj *p)
{
    int32_t n  = gf(p, PF_N);
    int32_t xl = arr_len(gr(p, PF_X));
    int32_t yl = arr_len(gr(p, PF_Y));

    if(n < 0)  n = 0;
    if(n > xl) n = xl;
    if(n > yl) n = yl;
    return n;
}

/* --- the bounds cache ----------------------------------------------------
 *
 * Reproduced rather than simplified. AWT computes the bounding box on the
 * first getBounds() and keeps it until something it can see changes the shape;
 * an applet that writes xpoints[0] itself and then asks for the bounds gets
 * the *old* box back, which is exactly why java.awt.Polygon has a public
 * invalidate(). Computing fresh every time would be a better class and a
 * different one, and an applet written against the real rule would then draw
 * differently here than it does on a desktop.
 */
static void poly_bounds(ps_jobj *p, int32_t *bx, int32_t *by, int32_t *bw,
                        int32_t *bh)
{
    /* An empty polygon answers with an empty box and *does not* fill the
     * cache. That is not tidiness: a reset() polygon whose bounds were asked
     * for and then had one point added must report a box at that point, and it
     * only does if the empty answer left nothing behind for addPoint to grow.
     * Observed on a real JDK, which is the only way anyone would find it. */
    if(poly_count(p) == 0) {
        *bx = *by = *bw = *bh = 0;
        return;
    }

    if(!gf(p, PF_BOK)) {
        const ps_jobj *xs = gr(p, PF_X), *ys = gr(p, PF_Y);
        int32_t        n  = poly_count(p), i;
        int32_t        x0, y0, x1, y1;

        x0 = x1 = arr_at(xs, 0);
        y0 = y1 = arr_at(ys, 0);
        for(i = 1; i < n; i++) {
            int32_t x = arr_at(xs, i), y = arr_at(ys, i);

            if(x < x0) x0 = x;
            if(x > x1) x1 = x;
            if(y < y0) y0 = y;
            if(y > y1) y1 = y;
        }
        sf(p, PF_BX, x0);
        sf(p, PF_BY, y0);
        sf(p, PF_BW, x1 - x0);
        sf(p, PF_BH, y1 - y0);
        sf(p, PF_BOK, 1);
    }

    *bx = gf(p, PF_BX);
    *by = gf(p, PF_BY);
    *bw = gf(p, PF_BW);
    *bh = gf(p, PF_BH);
}

/* addPoint's half of the cache. The box grows so the new point lands on its
 * far edge, which for a box that is exactly max-minus-min is the same answer a
 * recomputation would give - but it is applied to whatever the cache currently
 * holds, stale or not, which is what AWT does. */
static void poly_bounds_add(ps_jobj *p, int32_t x, int32_t y)
{
    if(!gf(p, PF_BOK))
        return;

    if(x < gf(p, PF_BX)) {
        sf(p, PF_BW, gf(p, PF_BW) + (gf(p, PF_BX) - x));
        sf(p, PF_BX, x);
    }
    else if(x - gf(p, PF_BX) > gf(p, PF_BW)) {
        sf(p, PF_BW, x - gf(p, PF_BX));
    }

    if(y < gf(p, PF_BY)) {
        sf(p, PF_BH, gf(p, PF_BH) + (gf(p, PF_BY) - y));
        sf(p, PF_BY, y);
    }
    else if(y - gf(p, PF_BY) > gf(p, PF_BH)) {
        sf(p, PF_BH, y - gf(p, PF_BY));
    }
}

/* --- contains ------------------------------------------------------------
 *
 * Even-odd, which is what java.awt.Polygon specifies and what ps_jgfx.c's
 * fill_polygon already rasterises with - so a self-crossing star is hollow in
 * the middle to both of them and an applet's hit test agrees with its own
 * picture.
 *
 * The bounding box is consulted first because AWT consults it first, and that
 * is not an optimisation here: it is what makes a stale cache observable, and
 * a point outside a stale box is reported outside even when the crossing count
 * would say otherwise.
 *
 * Doubles for the edge intersection. Applet coordinates are screen
 * coordinates, so nothing here comes near the precision the comparison
 * needs, and a real Polygon does the same arithmetic in double.
 */
static int poly_contains(ps_jobj *p, int32_t px, int32_t py)
{
    const ps_jobj *xs = gr(p, PF_X), *ys = gr(p, PF_Y);
    int32_t        n  = poly_count(p);
    int32_t        bx, by, bw, bh, i, j;
    int            in = 0;

    if(gf(p, PF_N) <= 2)
        return 0;

    poly_bounds(p, &bx, &by, &bw, &bh);
    if(px < bx || py < by || px >= bx + bw || py >= by + bh)
        return 0;

    for(i = 0, j = n - 1; i < n; j = i++) {
        int32_t yi = arr_at(ys, i), yj = arr_at(ys, j);

        /* Half-open in y: an edge counts when it spans the ray, and a vertex
         * exactly on the ray belongs to one of its two edges and not both.
         * That is the whole of why a point on a vertex is not counted twice
         * and left as "outside". */
        if((yi > py) != (yj > py)) {
            double xint = (double)(arr_at(xs, j) - arr_at(xs, i))
                        * (double)(py - yi) / (double)(yj - yi)
                        + (double)arr_at(xs, i);

            if((double)px < xint)
                in = !in;
        }
    }
    return in;
}

/* --- java.awt.Polygon ---------------------------------------------------- */

/* Both arrays at once, so a half-built polygon never exists.
 *
 * The order matters for the collector: each array is stored on the polygon
 * before the next allocation happens, and the polygon itself is an argument
 * sitting on the caller's operand stack, which the collector scans. Allocating
 * both and then storing them would leave the first one reachable from a C
 * local only - which is precisely the shape of a dangling array. */
static int poly_alloc(ps_jvm *vm, ps_jobj *p, int32_t cap)
{
    ps_jobj *a;

    a = ps_jvm_new_array(vm, PS_T_INT, cap);
    if(!a)
        return -1;
    sr(p, PF_X, a);

    a = ps_jvm_new_array(vm, PS_T_INT, cap);
    if(!a)
        return -1;
    sr(p, PF_Y, a);
    return 0;
}

/* AWT's growth rule, read off a real JDK: the next power of two strictly
 * greater than the current point count, floored at four. Measured at every
 * count from one to nine, and it is the reason `new Polygon().xpoints.length`
 * is 4 rather than 0. */
static int32_t poly_grown(int32_t n)
{
    int32_t cap = 4;

    while(cap <= n && cap < (1 << 20))
        cap *= 2;
    return cap;
}

static int poly_grow(ps_jvm *vm, ps_jobj *p, int32_t n)
{
    int32_t  cap = poly_grown(n), i;
    ps_jobj *old, *a;

    old = gr(p, PF_X);
    a   = ps_jvm_new_array(vm, PS_T_INT, cap);
    if(!a)
        return -1;
    for(i = 0; i < n && i < arr_len(old); i++)
        arr_put(a, i, arr_at(old, i));
    sr(p, PF_X, a);

    old = gr(p, PF_Y);
    a   = ps_jvm_new_array(vm, PS_T_INT, cap);
    if(!a)
        return -1;
    for(i = 0; i < n && i < arr_len(old); i++)
        arr_put(a, i, arr_at(old, i));
    sr(p, PF_Y, a);
    return 0;
}

static int polygon(ps_jvm *vm, const char *n, const char *d, ps_jslot *args,
                   int nargs, ps_jslot *ret, int *handled)
{
    ps_jobj *self = args[0].o;

    *handled = 1;

    if(is(n, d, "<init>", "()V")) {
        return poly_alloc(vm, self, 4) != 0
             ? ps_jvm_throw(vm, "java/lang/OutOfMemoryError", "Polygon") : 0;
    }
    if(is(n, d, "<init>", "([I[II)V")) {
        ps_jobj *xs = nargs >= 2 ? args[1].o : NULL;
        ps_jobj *ys = nargs >= 3 ? args[2].o : NULL;
        int32_t  cnt = nargs >= 4 ? args[3].i : 0, i;

        /* The three failures a real Polygon reports, in the order it reports
         * them. An applet that passes a short array is told so rather than
         * being handed a polygon whose points are zero. */
        if(!xs || !ys)
            return ps_jvm_throw(vm, "java/lang/NullPointerException",
                                "Polygon");
        if(cnt < 0)
            return ps_jvm_throw(vm, "java/lang/NegativeArraySizeException",
                                "npoints");
        if(cnt > arr_len(xs) || cnt > arr_len(ys))
            return ps_jvm_throw(vm, "java/lang/IndexOutOfBoundsException",
                                "npoints > length of coordinate arrays");

        /* Copied, not aliased, and exactly npoints long - both observable:
         * writing through the caller's array afterwards must not move the
         * polygon, and xpoints.length is npoints and not the source's. */
        if(poly_alloc(vm, self, cnt) != 0)
            return ps_jvm_throw(vm, "java/lang/OutOfMemoryError", "Polygon");
        for(i = 0; i < cnt; i++) {
            arr_put(gr(self, PF_X), i, arr_at(xs, i));
            arr_put(gr(self, PF_Y), i, arr_at(ys, i));
        }
        sf(self, PF_N, cnt);
        return 0;
    }

    if(is(n, d, "addPoint", "(II)V")) {
        int32_t cnt = gf(self, PF_N);
        int32_t cap = arr_len(gr(self, PF_X));
        int32_t cy  = arr_len(gr(self, PF_Y));

        if(nargs < 3)
            return 0;
        if(cy < cap)
            cap = cy;
        if(cnt < 0)
            cnt = 0;
        if(cnt >= cap && poly_grow(vm, self, cnt) != 0)
            return ps_jvm_throw(vm, "java/lang/OutOfMemoryError", "addPoint");

        arr_put(gr(self, PF_X), cnt, args[1].i);
        arr_put(gr(self, PF_Y), cnt, args[2].i);
        sf(self, PF_N, cnt + 1);
        poly_bounds_add(self, args[1].i, args[2].i);
        return 0;
    }

    /* getBoundingBox is the 1.0 spelling of getBounds. Both hand back a fresh
     * Rectangle: an applet that mutates what it was given must not be
     * reshaping the polygon by accident, and a real one does not let it. */
    if(is(n, d, "getBounds", NULL) || is(n, d, "getBoundingBox", NULL)) {
        int32_t bx, by, bw, bh;

        poly_bounds(self, &bx, &by, &bw, &bh);
        ret->o = ps_jgeom_rect(vm, bx, by, bw, bh);
        return 0;
    }

    /* inside is the 1.0 spelling of contains(int, int). */
    if(is(n, d, "contains", "(II)Z") || is(n, d, "inside", "(II)Z")) {
        ret->i = nargs >= 3 ? poly_contains(self, args[1].i, args[2].i) : 0;
        return 0;
    }
    if(is(n, d, "contains", "(Ljava/awt/Point;)Z")) {
        ps_jobj *pt = nargs >= 2 ? args[1].o : NULL;

        /* Point's layout is ps_jgeom.c's: x in slot zero, y in slot one. */
        ret->i = pt ? poly_contains(self, gf(pt, 0), gf(pt, 1)) : 0;
        return 0;
    }

    if(is(n, d, "translate", "(II)V")) {
        ps_jobj *xs = gr(self, PF_X), *ys = gr(self, PF_Y);
        int32_t  cnt = poly_count(self), i;

        if(nargs < 3)
            return 0;
        for(i = 0; i < cnt; i++) {
            arr_put(xs, i, arr_at(xs, i) + args[1].i);
            arr_put(ys, i, arr_at(ys, i) + args[2].i);
        }
        /* The cache moves with the shape rather than being thrown away, which
         * keeps a stale box stale - again, AWT's behaviour. */
        if(gf(self, PF_BOK)) {
            sf(self, PF_BX, gf(self, PF_BX) + args[1].i);
            sf(self, PF_BY, gf(self, PF_BY) + args[2].i);
        }
        return 0;
    }

    if(is(n, d, "reset", "()V")) {
        /* The arrays are kept, which is what makes reset() the cheap way to
         * reuse a polygon every frame - and it is what a real one does. */
        sf(self, PF_N, 0);
        sf(self, PF_BOK, 0);
        return 0;
    }
    if(is(n, d, "invalidate", "()V")) {
        sf(self, PF_BOK, 0);
        return 0;
    }

    *handled = 0;
    return 0;
}

/* --- java.awt.Graphics --------------------------------------------------- */

/* The receiver's own context when it has one - an applet that took a create()
 * copy and translated it must draw through that. Same rule as ps_jre.c's
 * gfx_of, which this deliberately mirrors. */
static ps_jgfx *gfx_of(ps_jvm *vm, ps_jobj *o)
{
    if(o && o->native)
        return (ps_jgfx *)o->native;
    return vm->gfx;
}

/* Unpacks a Polygon into two int buffers for ps_jgfx. One allocation for both,
 * freed by the caller. Returns the point count, or zero having allocated
 * nothing. */
static int poly_points(ps_jobj *p, int **xo, int **yo)
{
    int32_t n = poly_count(p), i;
    int    *b;

    *xo = *yo = NULL;
    if(n <= 0)
        return 0;

    b = (int *)malloc((size_t)n * sizeof(int) * 2);
    if(!b)
        return 0;

    for(i = 0; i < n; i++) {
        b[i]     = (int)arr_at(gr(p, PF_X), i);
        b[n + i] = (int)arr_at(gr(p, PF_Y), i);
    }
    *xo = b;
    *yo = b + n;
    return (int)n;
}

/* java.awt.Color.brighter() and darker(), to the digit.
 *
 * Only 3DRect needs them, and it needs them exactly: an applet's raised button
 * is three colours and getting the shades wrong is visible on a television.
 * The rules were read off a real JDK across eight colours including the two
 * that are not a plain scaling - black, whose brighter() is (3,3,3) rather
 * than black, and a component of 1 or 2, which is lifted to 3 before the
 * division so that it can actually get lighter. */
#define AWT_DIM 0.7

static uint32_t brighter(uint32_t argb)
{
    int32_t r = (int32_t)((argb >> 16) & 0xff);
    int32_t g = (int32_t)((argb >> 8) & 0xff);
    int32_t b = (int32_t)(argb & 0xff);
    int32_t i = (int32_t)(1.0 / (1.0 - AWT_DIM));   /* 3 */

    if(!r && !g && !b)
        return (argb & 0xff000000u) | ((uint32_t)i << 16) |
               ((uint32_t)i << 8) | (uint32_t)i;

    if(r > 0 && r < i) r = i;
    if(g > 0 && g < i) g = i;
    if(b > 0 && b < i) b = i;

    r = (int32_t)((double)r / AWT_DIM);
    g = (int32_t)((double)g / AWT_DIM);
    b = (int32_t)((double)b / AWT_DIM);
    if(r > 255) r = 255;
    if(g > 255) g = 255;
    if(b > 255) b = 255;

    return (argb & 0xff000000u) | ((uint32_t)r << 16) | ((uint32_t)g << 8) |
           (uint32_t)b;
}

static uint32_t darker(uint32_t argb)
{
    int32_t r = (int32_t)((double)((argb >> 16) & 0xff) * AWT_DIM);
    int32_t g = (int32_t)((double)((argb >> 8) & 0xff) * AWT_DIM);
    int32_t b = (int32_t)((double)(argb & 0xff) * AWT_DIM);

    return (argb & 0xff000000u) | ((uint32_t)r << 16) | ((uint32_t)g << 8) |
           (uint32_t)b;
}

/* One-pixel runs. A run of no pixels is not drawn, which is what separates
 * this from a pair of drawLine calls: AWT's 3D rectangles put nothing at all
 * on the screen for the edges a zero-width box does not have, and a drawLine
 * with its ends the wrong way round would still paint. */
static void span_h(ps_jgfx *g, int x, int y, int w)
{
    if(w > 0)
        ps_jgfx_fill_rect(g, x, y, w, 1);
}

static void span_v(ps_jgfx *g, int x, int y, int h)
{
    if(h > 0)
        ps_jgfx_fill_rect(g, x, y, 1, h);
}

/* draw3DRect(x, y, w, h, raised).
 *
 * Four runs, and the spans are not symmetrical - the highlight owns the whole
 * left column and all but the first pixel of the top row, the shadow owns the
 * bottom row from x+1 and the right column up to y+h-1. Both figures were read
 * off a real JDK's pixels rather than reasoned about, and the result was
 * checked against it for every width and height from zero to six, in raised
 * and lowered, over three colours: 588 renderings, no disagreement.
 *
 * The outline spans x..x+w and y..y+h inclusive, like drawRect. */
static void draw_3d(ps_jgfx *g, int x, int y, int w, int h, int raised)
{
    uint32_t c  = g->color;
    uint32_t hi = raised ? brighter(c) : darker(c);
    uint32_t lo = raised ? darker(c) : brighter(c);

    ps_jgfx_set_color(g, hi);
    span_v(g, x, y, h + 1);
    span_h(g, x + 1, y, w - 1);

    ps_jgfx_set_color(g, lo);
    span_h(g, x + 1, y + h, w);
    span_v(g, x + w, y, h);

    ps_jgfx_set_color(g, c);
}

/* fill3DRect(x, y, w, h, raised). The filled box is w by h exactly, like
 * fillRect, with the border drawn inside it - so the interior is two pixels
 * smaller in each direction. A lowered one fills with the darker colour, which
 * is why a pressed button looks pressed and not merely outlined. */
static void fill_3d(ps_jgfx *g, int x, int y, int w, int h, int raised)
{
    uint32_t c  = g->color;
    uint32_t hi = raised ? brighter(c) : darker(c);
    uint32_t lo = raised ? darker(c) : brighter(c);

    ps_jgfx_set_color(g, raised ? c : darker(c));
    if(w > 2 && h > 2)
        ps_jgfx_fill_rect(g, x + 1, y + 1, w - 2, h - 2);

    ps_jgfx_set_color(g, hi);
    span_v(g, x, y, h);
    span_h(g, x + 1, y, w - 2);

    ps_jgfx_set_color(g, lo);
    span_h(g, x + 1, y + h - 1, w - 1);
    span_v(g, x + w - 1, y, h - 1);

    ps_jgfx_set_color(g, c);
}

static int graphics(ps_jvm *vm, const char *n, const char *d, ps_jslot *args,
                    int nargs, ps_jslot *ret, int *handled)
{
    ps_jgfx *g = gfx_of(vm, nargs > 0 ? args[0].o : NULL);

    (void)ret;

    if(!g)
        return 0;

    /* The single-argument polygon calls. ps_jre.c's graphics() already has the
     * three-argument array forms and this file may not edit it, so these are
     * claimed here - before ps_jre.c is offered the call at all - and the
     * descriptors are spelled out so that the array forms still reach it
     * untouched. */
    if(is(n, d, "drawPolygon", "(Ljava/awt/Polygon;)V") ||
       is(n, d, "fillPolygon", "(Ljava/awt/Polygon;)V")) {
        ps_jobj *p = nargs >= 2 ? args[1].o : NULL;
        int     *xs, *ys, cnt;

        *handled = 1;
        if(!p)
            return ps_jvm_throw(vm, "java/lang/NullPointerException",
                                n[0] == 'f' ? "fillPolygon" : "drawPolygon");

        cnt = poly_points(p, &xs, &ys);
        if(!cnt)
            return 0;

        if(n[0] == 'f')
            ps_jgfx_fill_polygon(g, xs, ys, cnt);
        else
            ps_jgfx_draw_polygon(g, xs, ys, cnt);
        free(xs);
        return 0;
    }

    /* drawPolyline: the open form, which is not a polygon with the closing
     * edge left off by accident - applets draw graphs and traces with it. */
    if(is(n, d, "drawPolyline", "([I[II)V")) {
        ps_jobj *xs = nargs >= 2 ? args[1].o : NULL;
        ps_jobj *ys = nargs >= 3 ? args[2].o : NULL;
        int      cnt = nargs >= 4 ? args[3].i : 0, i;
        int     *xi, *yi;

        *handled = 1;
        if(cnt <= 0 || cnt > arr_len(xs) || cnt > arr_len(ys))
            return 0;

        xi = (int *)malloc((size_t)cnt * sizeof(int) * 2);
        if(!xi)
            return 0;
        yi = xi + cnt;

        for(i = 0; i < cnt; i++) {
            xi[i] = (int)arr_at(xs, i);
            yi[i] = (int)arr_at(ys, i);
        }
        ps_jgfx_draw_polyline(g, xi, yi, cnt);
        free(xi);
        return 0;
    }

    if(is(n, d, "draw3DRect", "(IIIIZ)V") ||
       is(n, d, "fill3DRect", "(IIIIZ)V")) {
        *handled = 1;
        if(nargs < 6)
            return 0;
        if(n[0] == 'f')
            fill_3d(g, args[1].i, args[2].i, args[3].i, args[4].i, args[5].i);
        else
            draw_3d(g, args[1].i, args[2].i, args[3].i, args[4].i, args[5].i);
        return 0;
    }

    /* drawChars(char[], off, len, x, y) and drawBytes(byte[], off, len, x, y).
     *
     * Both narrow to one byte per character, which is what the rest of this
     * runtime does with text: ps_jvm_new_string keeps UTF-8 bytes and
     * ps_jgeom.c's charsWidth measures the same narrowing, so a label drawn
     * this way and a label measured for centring agree. Correct for ASCII,
     * which is what an applet's own char arrays hold. */
    if(is(n, d, "drawChars", "([CIIII)V") ||
       is(n, d, "drawBytes", "([BIIII)V")) {
        ps_jobj *a   = nargs >= 2 ? args[1].o : NULL;
        int32_t  off = nargs >= 3 ? args[2].i : 0;
        int32_t  cnt = nargs >= 4 ? args[3].i : 0;
        char    *buf;
        int32_t  i;

        *handled = 1;
        if(nargs < 6 || !a || !a->data || off < 0 || cnt <= 0 ||
           off + cnt > a->len)
            return 0;

        buf = (char *)malloc((size_t)cnt);
        if(!buf)
            return 0;
        for(i = 0; i < cnt; i++)
            buf[i] = (char)(a->elem == PS_T_CHAR
                            ? ((uint16_t *)a->data)[off + i]
                            : ((unsigned char *)a->data)[off + i]);

        ps_jgfx_draw_string(g, buf, (size_t)cnt, args[4].i, args[5].i);
        free(buf);
        return 0;
    }

    return 0;
}

/* --- java.util.Stack -----------------------------------------------------
 *
 * Five methods on top of ps_jlib.c's Vector, and not one line of storage.
 * Everything below goes back through ps_jlib_call with the class name the
 * interpreter would have used, so the elements live in the same Object[] the
 * collector already traces and a Stack used as a Vector - which applets do,
 * `s.size()` and `s.elementAt(i)` - behaves identically because it *is* one.
 *
 * The call arrives named java/util/Vector, because that is the first native
 * ancestor of the Stack class registered above, and it is dispatched here only
 * when the receiver is genuinely a Stack.
 */
static int lib_vector(ps_jvm *vm, const char *name, const char *desc,
                      ps_jslot *args, int nargs, ps_jslot *ret)
{
    int handled = 0;

    memset(ret, 0, sizeof *ret);
    if(ps_jlib_call(vm, "java/util/Vector", name, desc, args, nargs, ret,
                    &handled))
        return handled ? 0 : -1;
    return -1;
}

static int stack_size(ps_jvm *vm, ps_jobj *self, int32_t *out)
{
    ps_jslot a[1], r;

    a[0].o = self;
    if(lib_vector(vm, "size", "()I", a, 1, &r) != 0)
        return -1;
    *out = r.i;
    return 0;
}

static int jstack(ps_jvm *vm, const char *n, const char *d, ps_jslot *args,
                  int nargs, ps_jslot *ret, int *handled)
{
    ps_jobj *self = args[0].o;
    ps_jslot a[2], r;
    int32_t  cnt;

    *handled = 1;

    if(is(n, d, "push", NULL)) {
        if(nargs < 2)
            return 0;
        a[0].o = self;
        a[1].o = args[1].o;
        if(lib_vector(vm, "addElement", "(Ljava/lang/Object;)V", a, 2, &r) != 0)
            return -1;
        ret->o = args[1].o;             /* push returns what it was given */
        return 0;
    }

    if(is(n, d, "peek", NULL) || is(n, d, "pop", NULL)) {
        if(stack_size(vm, self, &cnt) != 0)
            return -1;
        if(cnt <= 0)
            return ps_jvm_throw(vm, "java/util/EmptyStackException", NULL);

        a[0].o = self;
        a[1].i = cnt - 1;
        if(lib_vector(vm, "elementAt", "(I)Ljava/lang/Object;", a, 2, &r) != 0)
            return -1;
        ret->o = r.o;

        if(n[1] == 'o') {               /* pop, not peek */
            a[0].o = self;
            a[1].i = cnt - 1;
            if(lib_vector(vm, "removeElementAt", "(I)V", a, 2, &r) != 0)
                return -1;
        }
        return 0;
    }

    if(is(n, d, "empty", NULL)) {
        if(stack_size(vm, self, &cnt) != 0)
            return -1;
        ret->i = cnt == 0;
        return 0;
    }

    if(is(n, d, "search", NULL)) {
        /* One-based, counted down from the top, and -1 when it is not there.
         * The topmost element is 1, which is the part nobody guesses right. */
        if(nargs < 2)
            return 0;
        if(stack_size(vm, self, &cnt) != 0)
            return -1;

        a[0].o = self;
        a[1].o = args[1].o;
        if(lib_vector(vm, "lastIndexOf", "(Ljava/lang/Object;)I", a, 2, &r) != 0)
            return -1;

        ret->i = r.i < 0 ? -1 : cnt - r.i;
        return 0;
    }

    *handled = 0;
    return 0;
}

/* --- the router ---------------------------------------------------------- */

int ps_jpoly_call(ps_jvm *vm, const char *cls, const char *name,
                  const char *desc, ps_jslot *args, int nargs, ps_jslot *ret,
                  int *handled)
{
    *handled = 0;

    /* Registration hangs off any constructor rather than off a flag, for the
     * reason spelled out over poly_register: it has to have happened before
     * bytecode can say `new java/awt/Polygon`, and the applet's own <init>
     * calls java/applet/Applet.<init> before it can do anything at all. */
    if(name[0] == '<')
        poly_register(vm);

    if(!strcmp(cls, "java/awt/Graphics"))
        return graphics(vm, name, desc, args, nargs, ret, handled);

    /* Math.log used to be claimed here, because ps_jre.c's Math block did not
     * have it and eighteen references in one author's block wanted it. It now
     * lives with the rest of Math in ps_jre.c: two homes for one class is how
     * a function ends up implemented twice and differently. */

    /* A Stack's inherited methods are ps_jlib.c's and were handled long before
     * this; only the five Stack adds get here, and only for a Stack. */
    if(!strcmp(cls, "java/util/Vector")) {
        ps_jobj *self = (nargs >= 1 && args) ? args[0].o : NULL;

        if(instance_of(self, "java/util/Stack"))
            return jstack(vm, name, desc, args, nargs, ret, handled);
        return 0;
    }

    /* An instance call on a Polygon arrives named java/lang/Object, because
     * that is the first ancestor with an implementation. Argument zero is
     * always the receiver here - java.lang.Object has no static methods for it
     * to be anything else - so it is safe to read as one. */
    if(!strcmp(cls, "java/lang/Object")) {
        ps_jobj *self = (nargs >= 1 && args) ? args[0].o : NULL;

        if(instance_of(self, "java/awt/Polygon"))
            return polygon(vm, name, desc, args, nargs, ret, handled);
    }

    return 0;
}
