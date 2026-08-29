#include "ps_jvm.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* --- failure ------------------------------------------------------------- */

/* Every abort routes through here. An applet that fails leaves the browser
 * standing: the box is blank, the message says why, and the page around it is
 * untouched. Nothing in this file calls abort() or exit(). */
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

/* --- heap ----------------------------------------------------------------
 *
 * Every object is on one list, which is what a collector would walk. There is
 * no collector yet: an applet's paint() allocates a bounded number of Colors
 * and is done, and ps_jvm_free reclaims the lot. That is honest for a single
 * paint and wrong for an animation loop, which is the next thing to fix here.
 */

static int elem_size(uint8_t k)
{
    switch(k) {
    case PS_T_BOOL: case PS_T_BYTE:            return 1;
    case PS_T_CHAR: case PS_T_SHORT:           return 2;
    case PS_T_LONG: case PS_T_DOUBLE:          return 8;
    case PS_T_REF:                             return (int)sizeof(void *);
    default:                                   return 4;
    }
}

static ps_jobj *obj_alloc(ps_jvm *vm)
{
    ps_jobj *o;

    /* Only between instructions, which is where every allocation happens -
     * the interpreter holds no object in a C local across an allocation, so
     * there is nothing a collection here could pull out from under it. */
    if(vm->gc_at && vm->bytes > vm->gc_at)
        ps_jvm_gc(vm);

    o = (ps_jobj *)calloc(1, sizeof *o);

    if(!o)
        return NULL;

    o->gc_next = vm->gc_head;
    vm->gc_head = o;
    vm->objects++;

    /* The header counts. A java.awt.Color carries its value in the object's
     * own length field and allocates nothing else, so accounting only for
     * fields and array data made a million Colors look like zero bytes - and
     * the collector, thresholded on bytes, never ran. An applet's per-frame
     * garbage is overwhelmingly objects like that. */
    vm->bytes += (long)sizeof(ps_jobj);
    return o;
}

ps_jobj *ps_jvm_new(ps_jvm *vm, ps_jclass *c)
{
    ps_jobj *o;

    if(!c)
        return NULL;

    o = obj_alloc(vm);
    if(!o)
        return NULL;

    o->cls  = c;
    o->kind = PS_OBJ_INSTANCE;

    if(c->inst_slots) {
        o->fields = (ps_jslot *)calloc(c->inst_slots, sizeof(ps_jslot));
        if(!o->fields)
            return NULL;
        vm->bytes += (long)c->inst_slots * (long)sizeof(ps_jslot);
    }
    return o;
}

ps_jobj *ps_jvm_new_array(ps_jvm *vm, uint8_t elem, int32_t len)
{
    ps_jobj *o;
    int      es = elem_size(elem);

    if(len < 0)
        return NULL;

    /* A length an applet can name but the machine cannot hold. Refusing is
     * the whole point: on a console with no memory protection, letting a
     * hostile new int[0x40000000] through takes the browser with it. */
    if((long)len * es > 4L * 1024 * 1024)
        return NULL;

    o = obj_alloc(vm);
    if(!o)
        return NULL;

    o->kind = PS_OBJ_ARRAY;
    o->elem = elem;
    o->len  = len;

    if(len) {
        o->data = calloc((size_t)len, (size_t)es);
        if(!o->data)
            return NULL;
        vm->bytes += (long)len * es;
    }
    return o;
}

ps_jobj *ps_jvm_new_string(ps_jvm *vm, const char *utf8, size_t len)
{
    ps_jobj *o = obj_alloc(vm);

    if(!o)
        return NULL;

    o->kind   = PS_OBJ_INSTANCE;
    o->elem   = PS_T_CHAR;          /* marks it as the runtime's String */
    o->len    = (int32_t)len;
    o->native = malloc(len + 1);
    if(!o->native)
        return NULL;
    o->owns_native = 1;
    memcpy(o->native, utf8, len);
    ((char *)o->native)[len] = '\0';
    vm->bytes += (long)len + 1;
    return o;
}

const char *ps_jvm_string_utf8(const ps_jobj *o, size_t *len)
{
    if(!o || !o->native)
        return NULL;
    if(len)
        *len = (size_t)o->len;
    return (const char *)o->native;
}

/* --- collection ----------------------------------------------------------
 *
 * Mark and sweep, stop the world. The world here is one applet between two of
 * its own instructions, so "stop the world" costs nothing anybody can name.
 *
 * It is needed because of animation and nothing else. A single paint()
 * allocates a bounded handful of Colors and could be reclaimed wholesale at
 * the end; an applet looping at 25Hz allocates that handful sixty times a
 * second forever, and on a machine with twelve megabytes that is a page that
 * dies after a few minutes.
 *
 * The root set is exactly: every live frame's locals and operand stack, every
 * loaded class's statics, the applet instance, and the Runnable its thread is
 * holding. There is nowhere else a reference can be - the interpreter keeps no
 * temporaries outside a frame, which is a property worth preserving.
 */

static void mark(ps_jobj *o);

/* Membership set for conservative marking.
 *
 * A frame slot's type varies with the pc and the interpreter does not track
 * it, so scanning a frame means treating every slot as a possible reference -
 * and an int that happens to look like an address must not be dereferenced.
 * Following one is not a leak, it is a segfault, which is how the first
 * version of this collector announced itself.
 *
 * So every allocation is entered into a set at the start of a collection, and
 * a slot is only followed if it is in it. Open addressing over a power-of-two
 * table, built in one pass, thrown away at the end. */
static ps_jobj **g_live;
static size_t    g_live_cap;

static size_t live_hash(const ps_jobj *o)
{
    uintptr_t v = (uintptr_t)o;

    /* The low bits are always zero on a malloc'd pointer, so they are shifted
     * out before the mix - otherwise every entry lands in the same bucket. */
    v >>= 4;
    v ^= v >> 15;
    v *= 0x2545F491u;
    v ^= v >> 13;
    return (size_t)v;
}

static int live_contains(const ps_jobj *o)
{
    size_t i;

    if(!g_live || !o)
        return 0;

    i = live_hash(o) & (g_live_cap - 1);
    for(;;) {
        if(!g_live[i])
            return 0;
        if(g_live[i] == o)
            return 1;
        i = (i + 1) & (g_live_cap - 1);
    }
}

static int live_build(ps_jvm *vm)
{
    ps_jobj *o;
    size_t   cap = 16;

    while(cap < (size_t)vm->objects * 2 + 8)
        cap <<= 1;

    g_live = (ps_jobj **)calloc(cap, sizeof(ps_jobj *));
    if(!g_live)
        return -1;
    g_live_cap = cap;

    for(o = vm->gc_head; o; o = o->gc_next) {
        size_t i = live_hash(o) & (cap - 1);

        while(g_live[i])
            i = (i + 1) & (cap - 1);
        g_live[i] = o;
    }
    return 0;
}

static void live_drop(void)
{
    free(g_live);
    g_live = NULL;
    g_live_cap = 0;
}

/* Follows a slot that may or may not hold a reference. */
static void mark_maybe(ps_jobj *o)
{
    if(live_contains(o))
        mark(o);
}

static void mark(ps_jobj *o)
{
    if(!o || o->marked)
        return;

    o->marked = 1;

    /* Reference arrays hold objects; primitive arrays cannot. */
    if(o->kind == PS_OBJ_ARRAY) {
        if(o->elem == PS_T_REF && o->data) {
            int32_t i;

            for(i = 0; i < o->len; i++)
                mark(((ps_jobj **)o->data)[i]);
        }
        return;
    }

    if(o->fields && o->cls && o->cls->native) {
        uint16_t i;

        /* A runtime class has no field table to say which slot holds what, so
         * an event object's slots are offered the same way a frame's are:
         * mark_maybe follows only what is genuinely one of ours. The mixture
         * here is deliberate - PS_EVF_SOURCE is a reference and everything
         * around it is an int. */
        for(i = 0; i < o->cls->inst_slots; i++)
            mark_maybe(o->fields[i].o);
        return;
    }

    if(o->fields && o->cls) {
        uint16_t i;

        /* Field slots are typed by the class, so only the reference ones are
         * followed - an int that happens to look like a pointer must not
         * keep an object alive. */
        ps_jclass *c;

        for(c = o->cls; c; c = c->super) {
            for(i = 0; i < c->field_count; i++) {
                ps_jfield *f = &c->fields[i];

                if((f->access & PS_ACC_STATIC) || f->kind != PS_T_REF)
                    continue;
                if(f->slot < o->cls->inst_slots)
                    mark(o->fields[f->slot].o);
            }
        }
    }
}

static void mark_roots(ps_jvm *vm)
{
    int i, k;

    for(i = 0; i < vm->depth; i++) {
        ps_jframe *fr = &vm->frames[i];

        /* Every slot is offered, not only the ones currently holding a
         * reference: a slot's type varies with the pc and the interpreter does
         * not track it. mark_maybe checks the value is genuinely one of our
         * objects before following it. Conservative in the true sense - it can
         * retain an int that happens to match a live address, which wastes a
         * little memory, and it can never free something still in use. */
        for(k = 0; k < fr->m->max_locals; k++)
            mark_maybe(fr->locals[k].o);

        /* The whole operand stack, not the part below sp.
         *
         * The interpreter keeps the running frame's sp in a register and only
         * writes it back on a call or a suspension, so for the topmost frame
         * fr->sp is behind by however many instructions have run since. Half a
         * dozen opcodes in between allocate - `new`, a String constant, a
         * native call, a thrown exception - and a collection during one of
         * those used to scan to the stale sp and free whatever the frame had
         * pushed since. Not a leak: the object comes back out of malloc as
         * something else and the next instruction writes through it.
         *
         * Scanning to max_stack costs the collector a few slots per frame and
         * the interpreter nothing at all, which is the right way round. It can
         * retain a dead object that a slot above sp still points at, and that
         * is the same trade the locals above already make - they are scanned
         * to max_locals whether or not the pc has reached them. */
        for(k = 0; k < fr->m->max_stack; k++)
            mark_maybe(fr->stack[k].o);
    }

    for(i = 0; i < vm->nclasses; i++) {
        ps_jclass *c = vm->classes[i];
        uint16_t   f;

        if(!c->statics)
            continue;
        for(f = 0; f < c->field_count; f++) {
            ps_jfield *fd = &c->fields[f];

            if(!(fd->access & PS_ACC_STATIC) || fd->kind != PS_T_REF)
                continue;
            if(fd->slot < c->static_slots)
                mark(((ps_jslot *)c->statics)[fd->slot].o);
        }
    }

    /* A registered listener is reachable from the browser rather than from the
     * applet: `addMouseListener(new MouseAdapter(){...})` keeps no reference to
     * the adapter anywhere the applet can see, so without this the first
     * collection would free the object the next click is going to call. */
    for(i = 0; i < vm->nlisteners; i++) {
        mark(vm->listeners[i].target);
        mark(vm->listeners[i].obj);
    }

    mark(vm->applet);
    mark(vm->thread_run);
    mark(vm->thrown);
    mark(vm->ret.o);
}

long ps_jvm_gc(ps_jvm *vm)
{
    ps_jobj  *o, **link;
    long      freed = 0, before = vm->bytes;

    for(o = vm->gc_head; o; o = o->gc_next)
        o->marked = 0;

    if(live_build(vm) != 0)
        return 0;          /* no set, no safe scan: skip this collection */

    mark_roots(vm);
    live_drop();

    link = &vm->gc_head;
    o    = vm->gc_head;

    while(o) {
        ps_jobj *next = o->gc_next;

        if(o->marked) {
            link = &o->gc_next;
            o    = next;
            continue;
        }

        *link = next;

        vm->bytes -= (long)sizeof(ps_jobj);
        if(o->fields && o->cls)
            vm->bytes -= (long)o->cls->inst_slots * (long)sizeof(ps_jslot);
        if(o->data)
            vm->bytes -= (long)o->len * elem_size(o->elem);
        if(o->owns_native)
            vm->bytes -= (long)o->len + 1;

        free(o->fields);
        free(o->data);
        if(o->owns_native)
            free(o->native);
        free(o);

        vm->objects--;
        o = next;
    }

    freed = before - vm->bytes;
    vm->gc_runs++;

    /* Next collection when the heap has doubled over what survived, with a
     * floor so a tiny applet is not collected on every allocation. */
    vm->gc_at = vm->bytes * 2;
    if(vm->gc_at < 64 * 1024)
        vm->gc_at = 64 * 1024;

    return freed;
}

/* --- classes ------------------------------------------------------------- */

/* The JDK classes an applet actually touches, implemented in C.
 *
 * Each is a ps_jclass with no bytecode: the interpreter recognises them by
 * name at the invoke and calls native_call instead of pushing a frame. They
 * carry no fields, because the state they need lives in ps_jobj::native. */
static const char *g_native_classes[] = {
    "java/lang/Object",
    "java/lang/String",
    "java/lang/Math",
    "java/lang/StringBuilder",
    "java/lang/StringBuffer",
    "java/lang/System",
    "java/io/PrintStream",
    "java/applet/Applet",
    "java/applet/AudioClip",
    "java/awt/Component",
    "java/awt/Panel",
    "java/awt/Container",
    "java/awt/Graphics",
    "java/awt/Color",
    "java/awt/Font",
    "java/awt/Image",
    "java/awt/Event",
    "java/awt/Point",
    "java/awt/MediaTracker",
    "java/lang/Thread",
    "java/lang/Runnable",

    /* The 1.1 event model. The listener interfaces are here so that a class
     * naming one in its `implements` clause resolves; nothing is ever
     * dispatched *to* them, because an interface has no implementation and
     * invokeinterface resolves against the object in hand instead.
     *
     * The adapters are here because applets subclass them constantly, almost
     * always as an anonymous inner class, and a subclass needs its superclass
     * to exist before it can be linked. Their methods are empty, which is the
     * entire point of an adapter. */
    "java/awt/event/MouseListener",
    "java/awt/event/MouseMotionListener",
    "java/awt/event/KeyListener",
    "java/awt/event/ActionListener",
    "java/awt/event/WindowListener",
    "java/awt/event/MouseAdapter",
    "java/awt/event/MouseMotionAdapter",
    "java/awt/event/KeyAdapter",
    "java/awt/event/WindowAdapter",
    "java/awt/event/AWTEvent",
    "java/awt/event/InputEvent",
    "java/awt/event/ComponentEvent",
    "java/awt/event/MouseEvent",
    "java/awt/event/KeyEvent",
    "java/awt/event/ActionEvent",
    "java/awt/event/WindowEvent",
    "java/util/EventObject",

    /* The throwable hierarchy. Applets catch by these names and almost never
     * inspect what they caught, so what matters is that the names resolve and
     * that a handler for a supertype accepts a subtype. */
    "java/lang/Throwable",
    "java/lang/Exception",
    "java/lang/RuntimeException",
    "java/lang/Error",
    "java/lang/InterruptedException",
    "java/lang/NullPointerException",
    "java/lang/ArithmeticException",
    "java/lang/ArrayIndexOutOfBoundsException",
    "java/lang/IndexOutOfBoundsException",
    "java/lang/ClassCastException",
    "java/lang/NumberFormatException",
    "java/lang/IllegalArgumentException",
    NULL
};

static int is_native_class(const char *name)
{
    int i;

    for(i = 0; g_native_classes[i]; i++) {
        if(!strcmp(g_native_classes[i], name))
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

/* Instance slots a runtime class needs.
 *
 * Almost none do: a Graphics or a String keeps its state behind ps_jobj's
 * native pointer, and a Color packs into the length field. The event objects
 * are the exception, because an applet reads several independent ints off one
 * of them and there is no class file to give them a field table. They get
 * ordinary slots with the layout PS_EVF_* names, and the collector treats
 * those slots conservatively - see mark(). */
static uint16_t native_slots(const char *name)
{
    static const char *const with_fields[] = {
        "java/awt/event/MouseEvent",
        "java/awt/event/KeyEvent",
        "java/awt/event/ActionEvent",
        "java/awt/event/WindowEvent",
        "java/awt/Event",
        "java/awt/Point",
        NULL
    };
    int i;

    for(i = 0; with_fields[i]; i++) {
        if(!strcmp(with_fields[i], name))
            return PS_EVF_SLOTS;
    }
    return 0;
}

/* Builds the placeholder for a class the runtime implements in C. */
static ps_jclass *make_native(ps_jvm *vm, const char *name)
{
    ps_jclass *c = (ps_jclass *)calloc(1, sizeof *c);

    if(!c || vm->nclasses >= PS_JVM_MAX_CLASSES) {
        /* Silence here cost an afternoon: the table filled, every applet
         * stopped loading, and the status line the browser printed was empty
         * because nothing had said why. A limit that can be reached has to
         * name itself when it is. */
        fail(vm, "class table full at %d, adding %s", vm->nclasses, name);
        free(c);
        return NULL;
    }

    /* The name is copied because, unlike a parsed class, there is no file
     * image for it to point into. */
    c->raw = (uint8_t *)malloc(strlen(name) + 1);
    if(!c->raw) {
        free(c);
        return NULL;
    }
    strcpy((char *)c->raw, name);
    c->name       = (const char *)c->raw;
    c->native     = 1;
    c->inst_slots = native_slots(name);

    vm->classes[vm->nclasses++] = c;
    return c;
}

/* Assigns field slots and resolves the superclass. Instance slots continue
 * the parent's numbering so an inherited field keeps its index in a subclass,
 * which is what lets getfield resolve against the declaring class and still
 * index a subclass instance correctly. */
static int link_class(ps_jvm *vm, ps_jclass *c)
{
    uint16_t i, inst, stat;

    if(c->super_name && !c->super) {
        c->super = ps_jvm_class(vm, c->super_name);
        if(!c->super)
            return fail(vm, "cannot resolve superclass %s of %s",
                        c->super_name, c->name);
    }

    inst = c->super ? c->super->inst_slots : 0;
    stat = 0;

    for(i = 0; i < c->field_count; i++) {
        ps_jfield *f = &c->fields[i];
        int        n = (f->kind == PS_T_LONG || f->kind == PS_T_DOUBLE) ? 2 : 1;

        if(f->access & PS_ACC_STATIC) {
            f->slot = stat;
            stat = (uint16_t)(stat + n);
        }
        else {
            f->slot = inst;
            inst = (uint16_t)(inst + n);
        }
    }

    c->inst_slots   = inst;
    c->static_slots = stat;

    if(stat) {
        c->statics = calloc(stat, sizeof(ps_jslot));
        if(!c->statics)
            return fail(vm, "out of memory linking %s", c->name);
    }
    return 0;
}

ps_jclass *ps_jvm_define(ps_jvm *vm, uint8_t *raw, size_t len)
{
    const char *err = NULL;
    ps_jclass  *c;

    if(vm->nclasses >= PS_JVM_MAX_CLASSES) {
        fail(vm, "too many classes");
        return NULL;
    }

    c = ps_jclass_parse(raw, len, &err);
    if(!c) {
        fail(vm, "%s", err ? err : "bad class file");
        return NULL;
    }

    vm->classes[vm->nclasses++] = c;

    if(link_class(vm, c) != 0)
        return NULL;

    return c;
}

ps_jclass *ps_jvm_class(ps_jvm *vm, const char *name)
{
    ps_jclass *c = find_loaded(vm, name);
    uint8_t   *img;
    size_t     n = 0;

    if(c)
        return c;

    if(is_native_class(name))
        return make_native(vm, name);

    if(!vm->load)
        return NULL;

    img = vm->load(vm->load_user, name, &n);
    if(!img)
        return NULL;

    return ps_jvm_define(vm, img, n);
}

void ps_jvm_init(ps_jvm *vm, ps_jgfx *gfx)
{
    memset(vm, 0, sizeof *vm);
    vm->gfx    = gfx;
    vm->budget = 20L * 1000 * 1000;
    vm->gc_at  = 64 * 1024;
}

void ps_jvm_free(ps_jvm *vm)
{
    ps_jobj *o = vm->gc_head;
    int      i;

    while(o) {
        ps_jobj *next = o->gc_next;

        free(o->fields);
        free(o->data);
        if(o->owns_native)
            free(o->native);
        free(o);
        o = next;
    }

    for(i = 0; i < vm->nclasses; i++)
        ps_jclass_free(vm->classes[i]);

    memset(vm, 0, sizeof *vm);
}

void ps_jvm_set_loader(ps_jvm *vm, ps_jload_fn fn, void *user)
{
    vm->load = fn;
    vm->load_user = user;
}

/* --- array access -------------------------------------------------------- */

/* The JVM specifies these as throws, not failures, and applets rely on it -
 * a catch around an index is how half the parsing code of the era terminated
 * its loops. */
static int arr_load(ps_jvm *vm, ps_jobj *a, int32_t idx, ps_jslot *out)
{
    char m[64];

    if(!a)
        return ps_jvm_throw(vm, "java/lang/NullPointerException", "array");
    if(a->kind != PS_OBJ_ARRAY)
        return fail(vm, "not an array");
    if(idx < 0 || idx >= a->len) {
        snprintf(m, sizeof m, "index %d, length %d", (int)idx, (int)a->len);
        return ps_jvm_throw(vm,
                            "java/lang/ArrayIndexOutOfBoundsException", m);
    }

    memset(out, 0, sizeof *out);

    switch(a->elem) {
    case PS_T_BOOL: case PS_T_BYTE:
        out->i = ((int8_t *)a->data)[idx];
        break;
    case PS_T_CHAR:
        out->i = ((uint16_t *)a->data)[idx];
        break;
    case PS_T_SHORT:
        out->i = ((int16_t *)a->data)[idx];
        break;
    case PS_T_LONG:
        out->j = ((int64_t *)a->data)[idx];
        break;
    case PS_T_DOUBLE:
        out->d = ((double *)a->data)[idx];
        break;
    case PS_T_FLOAT:
        out->f = ((float *)a->data)[idx];
        break;
    case PS_T_REF:
        out->o = ((ps_jobj **)a->data)[idx];
        break;
    default:
        out->i = ((int32_t *)a->data)[idx];
        break;
    }
    return 0;
}

static int arr_store(ps_jvm *vm, ps_jobj *a, int32_t idx, ps_jslot v)
{
    char m[64];

    if(!a)
        return ps_jvm_throw(vm, "java/lang/NullPointerException", "array");
    if(a->kind != PS_OBJ_ARRAY)
        return fail(vm, "not an array");
    if(idx < 0 || idx >= a->len) {
        snprintf(m, sizeof m, "index %d, length %d", (int)idx, (int)a->len);
        return ps_jvm_throw(vm,
                            "java/lang/ArrayIndexOutOfBoundsException", m);
    }

    switch(a->elem) {
    case PS_T_BOOL: case PS_T_BYTE:
        ((int8_t *)a->data)[idx] = (int8_t)v.i;
        break;
    case PS_T_CHAR:
        ((uint16_t *)a->data)[idx] = (uint16_t)v.i;
        break;
    case PS_T_SHORT:
        ((int16_t *)a->data)[idx] = (int16_t)v.i;
        break;
    case PS_T_LONG:
        ((int64_t *)a->data)[idx] = v.j;
        break;
    case PS_T_DOUBLE:
        ((double *)a->data)[idx] = v.d;
        break;
    case PS_T_FLOAT:
        ((float *)a->data)[idx] = v.f;
        break;
    case PS_T_REF:
        ((ps_jobj **)a->data)[idx] = v.o;
        break;
    default:
        ((int32_t *)a->data)[idx] = v.i;
        break;
    }
    return 0;
}

/* --- native dispatch -----------------------------------------------------
 *
 * Declared here, implemented in ps_jre.c, which is where every piece of
 * knowledge about what java/awt/Graphics means lives. Keeping it out of this
 * file is what stops the interpreter growing an opinion about AWT.
 */
int ps_jre_call(ps_jvm *vm, const char *cls, const char *name,
                const char *desc, ps_jslot *args, int nargs, ps_jslot *ret,
                int *handled);

/* --- interpreter --------------------------------------------------------- */

#define OP(x) case x:

/* Reads a big-endian operand out of the instruction stream. */
#define U1() (code[pc + 1])
#define S1() ((int8_t)code[pc + 1])
#define U2() ((uint16_t)((code[pc + 1] << 8) | code[pc + 2]))
#define S2() ((int16_t)((code[pc + 1] << 8) | code[pc + 2]))

/* goto rather than return: the frame's locals and stack are heap allocated
 * and the only path that frees them runs through `out`. */
#define PUSH(v) do {                                                        \
        if(sp >= stack_max + 2) {                                           \
            rc = fail(vm, "operand stack overflow in %s.%s",                \
                      cls->name, m->name);                                  \
            goto err;                                                       \
        }                                                                   \
        stack[sp++] = (v);                                                  \
    } while(0)

#define POP()  (sp > 0 ? stack[--sp] : (fail(vm, "operand stack underflow in "\
                                             "%s.%s", cls->name, m->name),   \
                                        zero))

#define LOCAL(n) (*( (n) < nlocals ? &locals[(n)]                            \
                   : (fail(vm, "local %d out of range in %s.%s", (int)(n),   \
                           cls->name, m->name), &zero) ))

/* --- frames --------------------------------------------------------------
 *
 * Pushing a call is allocating its locals and operand stack and recording
 * where it is; the interpreter loop below always works on whatever is on top.
 * Nothing recurses, so a call is a data structure rather than a C stack frame
 * and can therefore be put down and picked up again - which is the whole
 * mechanism behind Thread.sleep.
 */

/* The element kind a descriptor holds one level down. Anything that is not a
 * primitive letter is a reference, which covers both '[' and 'L'. */
static uint8_t elem_kind(const char *d)
{
    switch(*d) {
    case 'I': return PS_T_INT;   case 'J': return PS_T_LONG;
    case 'F': return PS_T_FLOAT; case 'D': return PS_T_DOUBLE;
    case 'B': return PS_T_BYTE;  case 'C': return PS_T_CHAR;
    case 'S': return PS_T_SHORT; case 'Z': return PS_T_BOOL;
    default:  return PS_T_REF;
    }
}

/* One level of a multianewarray, recursing for the lengths below it.
 *
 * `d` is the descriptor of the array being built and so begins with '['.
 * Levels past the supplied lengths are left null, which is what the JVM does
 * and what `new int[n][]` means. */
static ps_jobj *build_multi(ps_jvm *vm, const char *d, const int32_t *counts,
                            int n)
{
    ps_jobj *a;
    int32_t  i;

    if(n <= 0 || !d || *d != '[')
        return NULL;

    a = ps_jvm_new_array(vm, elem_kind(d + 1), counts[0]);
    if(!a || n == 1)
        return a;

    for(i = 0; i < counts[0]; i++) {
        ps_jobj *row = build_multi(vm, d + 1, counts + 1, n - 1);

        if(!row)
            return NULL;
        ((ps_jobj **)a->data)[i] = row;
    }
    return a;
}

static int push_frame(ps_jvm *vm, ps_jclass *cls, ps_jmethod *m,
                      const ps_jslot *args, int nargs)
{
    ps_jframe *fr;
    int        i;

    if(vm->depth >= PS_JVM_MAX_DEPTH)
        return fail(vm, "call stack too deep (recursion in %s.%s?)",
                    cls->name, m->name);
    if(!m->code)
        return fail(vm, "%s.%s has no code", cls->name, m->name);
    if(m->max_stack > PS_JVM_MAX_STACK || m->max_locals > PS_JVM_MAX_STACK)
        return fail(vm, "%s.%s wants an implausible frame", cls->name,
                    m->name);

    fr = &vm->frames[vm->depth];
    memset(fr, 0, sizeof *fr);

    fr->locals = (ps_jslot *)calloc((size_t)m->max_locals + 1,
                                    sizeof(ps_jslot));
    fr->stack  = (ps_jslot *)calloc((size_t)m->max_stack + 2,
                                    sizeof(ps_jslot));
    if(!fr->locals || !fr->stack) {
        free(fr->locals);
        free(fr->stack);
        return fail(vm, "out of memory entering %s.%s", cls->name, m->name);
    }

    for(i = 0; i < nargs && i < m->max_locals; i++)
        fr->locals[i] = args[i];

    fr->cls      = cls;
    fr->m        = m;
    fr->pc       = 0;
    fr->sp       = 0;
    fr->ret_kind = m->ret_kind;

    vm->depth++;
    return 0;
}

static void pop_frame(ps_jvm *vm)
{
    ps_jframe *fr;

    if(vm->depth <= 0)
        return;

    fr = &vm->frames[--vm->depth];
    free(fr->locals);
    free(fr->stack);
    fr->locals = NULL;
    fr->stack  = NULL;
}

static void unwind_to(ps_jvm *vm, int floor)
{
    while(vm->depth > floor)
        pop_frame(vm);
}

/* Which of the four invoke opcodes is being executed. They differ in exactly
 * one thing that matters here - what the method name is resolved against - and
 * conflating them is how a super call turns into a call to itself. */
enum { INV_VIRTUAL = 0, INV_SPECIAL, INV_STATIC, INV_INTERFACE };

/* ps_jclass_find_method's answer, plus the class that declared it.
 *
 * A frame has to run with its *declaring* class, not the receiver's: fr->cls
 * is the constant pool every instruction in the method indexes into, and a
 * method inherited from a superclass indexes into that superclass's pool.
 * Handing it the subclass resolves the callee's own references against the
 * wrong table, which fails in whichever way the two pools happen to differ. */
static ps_jmethod *find_method_in(ps_jclass *c, const char *name,
                                  const char *desc, ps_jclass **decl)
{
    uint16_t i;

    *decl = NULL;

    for(; c; c = c->super) {
        for(i = 0; i < c->method_count; i++) {
            if(strcmp(c->methods[i].name, name))
                continue;
            if(desc && strcmp(c->methods[i].desc, desc))
                continue;
            *decl = c;
            return &c->methods[i];
        }
    }
    return NULL;
}

/* Resolves a Methodref and either pushes a frame for it or, for the classes
 * the runtime implements in C, runs it here and now.
 *
 * Returns 1 when a frame was pushed, in which case the caller must go back
 * round the outer loop rather than continue in the current method. */
static int do_invoke(ps_jvm *vm, ps_jclass *cls, uint16_t idx, int kind,
                     ps_jslot *stack, int *spp, int *pushed)
{
    const char *cn = NULL, *mn = NULL, *md = NULL;
    ps_jclass  *tc, *dc = NULL;
    ps_jmethod *tm;
    int         slots, handled = 0;
    int         is_static = (kind == INV_STATIC);
    ps_jslot    r;
    uint8_t     rk;

    *pushed = 0;

    if(ps_jcp_ref(cls, idx, &cn, &mn, &md) != 0)
        return fail(vm, "bad method reference");

    slots = ps_jdesc_arg_slots(md, &rk);
    if(slots < 0)
        return fail(vm, "bad descriptor %s", md);
    if(!is_static)
        slots++;

    if(*spp < slots)
        return fail(vm, "stack underflow calling %s.%s", cn, mn);

    *spp -= slots;
    memset(&r, 0, sizeof r);

    /* Virtual and interface calls resolve against the object in hand, ahead of
     * anything the constant pool says. For an interface that is the only thing
     * that can work: java/awt/event/MouseListener has no implementation at all,
     * so the name means nothing until there is a receiver to look it up on.
     * For a virtual call it is what lets an applet override a method of a class
     * the runtime implements in C - a subclass of MouseAdapter, or of Thread -
     * instead of the empty C version answering on its behalf.
     *
     * invokespecial is deliberately not in this set. It names the exact method
     * to run, and dispatching it on the receiver would make super.<init>() a
     * call to the constructor already running. */
    if((kind == INV_VIRTUAL || kind == INV_INTERFACE) &&
       stack[*spp].o && stack[*spp].o->cls) {
        tm = find_method_in(stack[*spp].o->cls, mn, md, &dc);
        if(tm && tm->code) {
            if(push_frame(vm, dc, tm, &stack[*spp], slots) != 0)
                return -1;
            *pushed = 1;
            return 0;
        }
    }

    tc = ps_jvm_class(vm, cn);

    if(tc && tc->native) {
        if(ps_jre_call(vm, cn, mn, md, &stack[*spp], slots, &r, &handled) != 0)
            return -1;
        if(!handled)
            return fail(vm, "unimplemented: %s.%s%s", cn, mn, md);
    }
    else {
        if(!tc)
            return fail(vm, "class not found: %s", cn);

        /* Whatever the receiver was going to contribute has already been tried
         * above, so what is left resolves against the class the pool names:
         * the exact method for invokespecial, and for the others an inherited
         * one the receiver did not override. */
        tm = find_method_in(tc, mn, md, &dc);

        if(!tm) {
            /* An inherited method landing on a native ancestor - most often
             * Applet.<init> reached from the applet's own constructor. */
            ps_jclass *a;

            for(a = tc; a; a = a->super) {
                if(a->native) {
                    if(ps_jre_call(vm, a->name, mn, md, &stack[*spp], slots,
                                   &r, &handled) != 0)
                        return -1;
                    break;
                }
            }
            if(!handled)
                return fail(vm, "method not found: %s.%s%s", cn, mn, md);
        }
        else if(tm->code) {
            if(push_frame(vm, dc, tm, &stack[*spp], slots) != 0)
                return -1;
            *pushed = 1;
            return 0;
        }
        else {
            return fail(vm, "abstract or native method %s.%s", cn, mn);
        }
    }

    if(rk != PS_T_VOID) {
        stack[(*spp)++] = r;
        if(rk == PS_T_LONG || rk == PS_T_DOUBLE)
            stack[(*spp)++] = r;
    }
    return 0;
}

/* Hands a returning frame's value to the one underneath it. */
/* The floor matters here.
 *
 * paint() runs while the animation thread is suspended partway down its own
 * call chain, so "the bottom of the stack" is not zero - it is wherever the
 * thread left off. A frame returning to the floor is returning to the browser,
 * and its value goes to vm->ret rather than being pushed onto a frame that
 * belongs to somebody else's call. */
static void return_value(ps_jvm *vm, uint8_t kind, ps_jslot v)
{
    ps_jframe *caller;

    if(vm->depth <= vm->floor) {
        vm->ret = v;
        return;
    }

    caller = &vm->frames[vm->depth - 1];

    if(kind == PS_T_VOID)
        return;

    caller->stack[caller->sp++] = v;
    if(kind == PS_T_LONG || kind == PS_T_DOUBLE)
        caller->stack[caller->sp++] = v;
}

/* --- exceptions ----------------------------------------------------------
 *
 * Applets throw far less than they catch. The common shapes are a try/catch
 * around Thread.sleep (which is declared to throw InterruptedException and in
 * practice never does) and a catch-all around parsing a parameter. What has to
 * work is the unwinding: a throw walks out through frames until a handler
 * claims it, and the frames it passes are discarded.
 */

/* Whether a handler for `want` accepts a `have`.
 *
 * A full answer needs the class hierarchy, which for the runtime's own
 * throwables is not in any class file - they are native placeholders with no
 * superclass recorded. So the relation is spelled out: the four names that sit
 * above everything an applet can catch accept anything, and otherwise the
 * names must match. That is more permissive than a real JVM in the case where
 * an applet catches one specific exception and expects a different one to pass
 * through - rare, and the failure is a caught exception rather than a crash. */
/* What each standard throwable extends.
 *
 * The runtime's exception classes are C placeholders with no super link, so
 * there is no chain to walk and this table is the chain. Only the ones an
 * applet can actually produce or catch are here; anything absent falls back to
 * the four universal supertypes below, which is what the whole rule used to
 * be.
 *
 * The case this exists for: String.substring throws
 * StringIndexOutOfBoundsException, and an applet that validates an index
 * writes catch(IndexOutOfBoundsException). Matching on the exact name missed
 * it, so a guard the author wrote correctly did not fire and the applet died
 * on an error it had handled. */
static const struct { const char *cls, *super; } g_throwable_super[] = {
    { "java/lang/StringIndexOutOfBoundsException", "java/lang/IndexOutOfBoundsException" },
    { "java/lang/ArrayIndexOutOfBoundsException",  "java/lang/IndexOutOfBoundsException" },
    { "java/lang/IndexOutOfBoundsException",       "java/lang/RuntimeException" },
    { "java/lang/NumberFormatException",           "java/lang/IllegalArgumentException" },
    { "java/lang/IllegalArgumentException",        "java/lang/RuntimeException" },
    { "java/lang/NullPointerException",            "java/lang/RuntimeException" },
    { "java/lang/ArithmeticException",             "java/lang/RuntimeException" },
    { "java/lang/ClassCastException",              "java/lang/RuntimeException" },
    { "java/lang/ArrayStoreException",             "java/lang/RuntimeException" },
    { "java/lang/NegativeArraySizeException",      "java/lang/RuntimeException" },
    { "java/lang/IllegalStateException",           "java/lang/RuntimeException" },
    { "java/lang/UnsupportedOperationException",   "java/lang/RuntimeException" },
    { "java/util/NoSuchElementException",          "java/lang/RuntimeException" },
    { "java/util/EmptyStackException",             "java/lang/RuntimeException" },
    { "java/lang/RuntimeException",                "java/lang/Exception" },
    { "java/io/FileNotFoundException",             "java/io/IOException" },
    { "java/net/MalformedURLException",            "java/io/IOException" },
    { "java/io/IOException",                       "java/lang/Exception" },
    { "java/lang/InterruptedException",            "java/lang/Exception" },
    { "java/lang/ClassNotFoundException",          "java/lang/Exception" },
    { "java/lang/CloneNotSupportedException",      "java/lang/Exception" },
    { "java/lang/InstantiationException",          "java/lang/Exception" },
    { "java/lang/IllegalAccessException",          "java/lang/Exception" },
    { "java/lang/Exception",                       "java/lang/Throwable" },
    { "java/lang/OutOfMemoryError",                "java/lang/Error" },
    { "java/lang/StackOverflowError",              "java/lang/Error" },
    { "java/lang/NoClassDefFoundError",            "java/lang/Error" },
    { "java/lang/Error",                           "java/lang/Throwable" },
    { NULL, NULL }
};

static const char *throwable_super(const char *cls)
{
    int i;

    for(i = 0; g_throwable_super[i].cls; i++) {
        if(!strcmp(cls, g_throwable_super[i].cls))
            return g_throwable_super[i].super;
    }
    return NULL;
}

static int handler_accepts(const char *want, const char *have)
{
    int hops;

    if(!want || !*want)
        return 1;                       /* catch-all, or finally */
    if(!have)
        return 0;

    /* Up the chain from what was thrown. Bounded rather than while(1): the
     * table is hand-written and a typo that made a cycle would hang the
     * browser instead of failing a test. */
    for(hops = 0; have && hops < 8; hops++) {
        if(!strcmp(want, have))
            return 1;
        have = throwable_super(have);
    }

    /* Anything the table does not name - an applet's own exception class, or
     * one of ours not listed - still lands in the four universal catches. */
    return !strcmp(want, "java/lang/Throwable") ||
           !strcmp(want, "java/lang/Exception") ||
           !strcmp(want, "java/lang/RuntimeException") ||
           !strcmp(want, "java/lang/Error");
}

/* Finds a handler in one frame. Returns the handler pc, or -1. */
static int32_t find_handler(ps_jframe *fr, uint32_t pc, const char *thrown)
{
    const uint8_t *e = fr->m->etab;
    uint16_t       i;

    if(!e)
        return -1;

    for(i = 0; i < fr->m->etab_len; i++, e += 8) {
        uint16_t start = (uint16_t)((e[0] << 8) | e[1]);
        uint16_t end   = (uint16_t)((e[2] << 8) | e[3]);
        uint16_t h     = (uint16_t)((e[4] << 8) | e[5]);
        uint16_t type  = (uint16_t)((e[6] << 8) | e[7]);
        const char *want = type ? ps_jcp_class_name(fr->cls, type) : NULL;

        /* The range is [start, end) and pc is the instruction that threw. */
        if(pc < start || pc >= end)
            continue;
        if(!handler_accepts(want, thrown))
            continue;
        return (int32_t)h;
    }
    return -1;
}

int ps_jvm_throw(ps_jvm *vm, const char *cls_name, const char *msg)
{
    ps_jclass *c = ps_jvm_class(vm, cls_name);
    ps_jobj   *o;

    o = c ? ps_jvm_new(vm, c) : NULL;
    if(o && msg)
        o->native = NULL;              /* the message is not retained yet */

    vm->thrown   = o;
    vm->throwing = 1;

    /* Kept for the case where nothing catches it: an uncaught exception has to
     * name itself, or the applet just stops with no explanation. */
    snprintf(vm->err, sizeof vm->err, "%s%s%s", cls_name,
             msg ? ": " : "", msg ? msg : "");
    return -1;
}

/* --- the loop ------------------------------------------------------------
 *
 * Runs whatever is on the frame stack until it empties, the budget runs out,
 * or the applet sleeps. The inner loop caches the top frame's fields and only
 * writes them back on a call, a return or a suspension, so the common case
 * costs no more than the recursive version did.
 */
static ps_jrun run(ps_jvm *vm, int floor)
{
    ps_jslot zero;

    memset(&zero, 0, sizeof zero);

    vm->floor = floor;

    while(vm->depth > floor) {
        ps_jframe     *fr      = &vm->frames[vm->depth - 1];
        ps_jclass     *cls     = fr->cls;
        ps_jmethod    *m       = fr->m;
        const uint8_t *code    = m->code;
        uint32_t       pc      = fr->pc;
        ps_jslot      *locals  = fr->locals;
        ps_jslot      *stack   = fr->stack;
        int            sp      = fr->sp;
        int            nlocals = m->max_locals;
        int            stack_max = m->max_stack;
        int            rc = 0;

        while(pc < m->code_len) {
            uint8_t op = code[pc];

            if(vm->budget && --vm->budget <= 0) {
                /* Out of time, not out of road. The frame is saved exactly as
                 * it stands and the caller decides whether to give it more. */
                fr->pc = pc;
                fr->sp = sp;
                return PS_RUN_YIELD;
            }
            if(vm->failed)
                goto err;

            switch(op) {

        /* --- constants --- */
        OP(0x00) pc += 1; break;                                  /* nop */
        OP(0x01) { ps_jslot v; v.o = NULL; PUSH(v); } pc += 1; break; /* aconst_null */
        OP(0x02) case 0x03: case 0x04: case 0x05:
        case 0x06: case 0x07: case 0x08: {                        /* iconst_m1..5 */
            ps_jslot v; v.i = (int32_t)op - 0x03; PUSH(v); pc += 1; break;
        }
        OP(0x09) case 0x0a: {                                     /* lconst_0/1 */
            ps_jslot v; v.j = op - 0x09; PUSH(v); PUSH(v); pc += 1; break;
        }
        OP(0x0b) case 0x0c: case 0x0d: {                          /* fconst_0/1/2 */
            ps_jslot v; v.f = (float)(op - 0x0b); PUSH(v); pc += 1; break;
        }
        OP(0x0e) case 0x0f: {                                     /* dconst_0/1 */
            ps_jslot v; v.d = (double)(op - 0x0e); PUSH(v); PUSH(v); pc += 1; break;
        }
        OP(0x10) { ps_jslot v; v.i = S1(); PUSH(v); pc += 2; break; }  /* bipush */
        OP(0x11) { ps_jslot v; v.i = S2(); PUSH(v); pc += 3; break; }  /* sipush */

        OP(0x12) case 0x13: {                                     /* ldc, ldc_w */
            uint16_t ci = (op == 0x12) ? U1() : U2();
            ps_jslot v;

            memset(&v, 0, sizeof v);
            if(ci == 0 || ci >= cls->cp_count) {
                rc = fail(vm, "ldc index out of range");
                goto err;
            }
            switch(cls->cp[ci].tag) {
            case PS_CP_INTEGER: v.i = cls->cp[ci].u.i; break;
            case PS_CP_FLOAT:   v.f = cls->cp[ci].u.f; break;
            case PS_CP_STRING: {
                const char *s = ps_jcp_utf8(cls, cls->cp[ci].u.index);

                if(!s) {
                    rc = fail(vm, "bad string constant");
                    goto err;
                }
                v.o = ps_jvm_new_string(vm, s, strlen(s));
                if(!v.o) {
                    rc = fail(vm, "out of memory for string constant");
                    goto err;
                }
                break;
            }
            default:
                rc = fail(vm, "unsupported ldc constant (tag %d)",
                          cls->cp[ci].tag);
                goto err;
            }
            PUSH(v);
            pc += (op == 0x12) ? 2 : 3;
            break;
        }
        OP(0x14) {                                                /* ldc2_w */
            uint16_t ci = U2();
            ps_jslot v;

            memset(&v, 0, sizeof v);
            if(ci == 0 || ci >= cls->cp_count) {
                rc = fail(vm, "ldc2_w index out of range");
                goto err;
            }
            if(cls->cp[ci].tag == PS_CP_LONG)        v.j = cls->cp[ci].u.j;
            else if(cls->cp[ci].tag == PS_CP_DOUBLE) v.d = cls->cp[ci].u.d;
            else { rc = fail(vm, "bad ldc2_w constant"); goto err; }
            PUSH(v); PUSH(v);
            pc += 3;
            break;
        }

        /* --- loads --- */
        OP(0x15) case 0x17: {                                     /* iload, fload */
            PUSH(LOCAL(U1())); pc += 2; break;
        }
        OP(0x16) case 0x18: {                                     /* lload, dload */
            ps_jslot v = LOCAL(U1()); PUSH(v); PUSH(v); pc += 2; break;
        }
        OP(0x19) { PUSH(LOCAL(U1())); pc += 2; break; }           /* aload */
        OP(0x1a) case 0x1b: case 0x1c: case 0x1d:                 /* iload_0..3 */
            PUSH(LOCAL(op - 0x1a)); pc += 1; break;
        OP(0x1e) case 0x1f: case 0x20: case 0x21: {               /* lload_0..3 */
            ps_jslot v = LOCAL(op - 0x1e); PUSH(v); PUSH(v); pc += 1; break;
        }
        OP(0x22) case 0x23: case 0x24: case 0x25:                 /* fload_0..3 */
            PUSH(LOCAL(op - 0x22)); pc += 1; break;
        OP(0x26) case 0x27: case 0x28: case 0x29: {               /* dload_0..3 */
            ps_jslot v = LOCAL(op - 0x26); PUSH(v); PUSH(v); pc += 1; break;
        }
        OP(0x2a) case 0x2b: case 0x2c: case 0x2d:                 /* aload_0..3 */
            PUSH(LOCAL(op - 0x2a)); pc += 1; break;

        /* --- array loads --- */
        OP(0x2e) case 0x2f: case 0x30: case 0x31:
        case 0x32: case 0x33: case 0x34: case 0x35: {             /* *aload */
            ps_jslot idx = POP(), aref = POP(), v;

            if(arr_load(vm, aref.o, idx.i, &v) != 0) { rc = -1; goto err; }
            PUSH(v);
            if(op == 0x2f || op == 0x31)   /* laload, daload */
                PUSH(v);
            pc += 1;
            break;
        }

        /* --- stores --- */
        OP(0x36) case 0x38: case 0x3a: {                          /* istore/fstore/astore */
            ps_jslot v = POP(); LOCAL(U1()) = v; pc += 2; break;
        }
        OP(0x37) case 0x39: {                                     /* lstore, dstore */
            ps_jslot v = POP(); POP(); LOCAL(U1()) = v; pc += 2; break;
        }
        OP(0x3b) case 0x3c: case 0x3d: case 0x3e: {               /* istore_0..3 */
            ps_jslot v = POP(); LOCAL(op - 0x3b) = v; pc += 1; break;
        }
        OP(0x3f) case 0x40: case 0x41: case 0x42: {               /* lstore_0..3 */
            ps_jslot v = POP(); POP(); LOCAL(op - 0x3f) = v; pc += 1; break;
        }
        OP(0x43) case 0x44: case 0x45: case 0x46: {               /* fstore_0..3 */
            ps_jslot v = POP(); LOCAL(op - 0x43) = v; pc += 1; break;
        }
        OP(0x47) case 0x48: case 0x49: case 0x4a: {               /* dstore_0..3 */
            ps_jslot v = POP(); POP(); LOCAL(op - 0x47) = v; pc += 1; break;
        }
        OP(0x4b) case 0x4c: case 0x4d: case 0x4e: {               /* astore_0..3 */
            ps_jslot v = POP(); LOCAL(op - 0x4b) = v; pc += 1; break;
        }

        /* --- array stores --- */
        OP(0x4f) case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x50: {             /* *astore */
            ps_jslot v, idx, aref;

            v = POP();
            if(op == 0x50 || op == 0x52)   /* lastore, dastore: cat 2 */
                POP();
            idx  = POP();
            aref = POP();
            if(arr_store(vm, aref.o, idx.i, v) != 0) { rc = -1; goto err; }
            pc += 1;
            break;
        }

        /* --- stack --- */
        OP(0x57) POP(); pc += 1; break;                            /* pop */
        OP(0x58) POP(); POP(); pc += 1; break;                     /* pop2 */
        OP(0x59) {                                                 /* dup */
            ps_jslot v = POP(); PUSH(v); PUSH(v); pc += 1; break;
        }
        OP(0x5a) {                                                 /* dup_x1 */
            ps_jslot a = POP(), b = POP();
            PUSH(a); PUSH(b); PUSH(a); pc += 1; break;
        }
        OP(0x5b) {                                                 /* dup_x2 */
            ps_jslot a = POP(), b = POP(), c = POP();
            PUSH(a); PUSH(c); PUSH(b); PUSH(a); pc += 1; break;
        }
        OP(0x5c) {                                                 /* dup2 */
            ps_jslot a = POP(), b = POP();
            PUSH(b); PUSH(a); PUSH(b); PUSH(a); pc += 1; break;
        }
        OP(0x5f) {                                                 /* swap */
            ps_jslot a = POP(), b = POP(); PUSH(a); PUSH(b); pc += 1; break;
        }

        /* --- int arithmetic --- */
        OP(0x60) { ps_jslot b = POP(), a = POP(), v; v.i = a.i + b.i; PUSH(v); pc += 1; break; }
        OP(0x64) { ps_jslot b = POP(), a = POP(), v; v.i = a.i - b.i; PUSH(v); pc += 1; break; }
        OP(0x68) { ps_jslot b = POP(), a = POP(), v; v.i = a.i * b.i; PUSH(v); pc += 1; break; }
        OP(0x6c) {                                                 /* idiv */
            ps_jslot b = POP(), a = POP(), v;

            if(b.i == 0) {
                rc = ps_jvm_throw(vm, "java/lang/ArithmeticException",
                                  "/ by zero");
                goto err;
            }
            /* INT_MIN / -1 overflows on the SH-4 as it does everywhere; the
             * JVM spec defines the result as INT_MIN. */
            v.i = (a.i == INT32_MIN && b.i == -1) ? INT32_MIN : a.i / b.i;
            PUSH(v); pc += 1; break;
        }
        OP(0x70) {                                                 /* irem */
            ps_jslot b = POP(), a = POP(), v;

            if(b.i == 0) {
                rc = ps_jvm_throw(vm, "java/lang/ArithmeticException",
                                  "/ by zero");
                goto err;
            }
            v.i = (a.i == INT32_MIN && b.i == -1) ? 0 : a.i % b.i;
            PUSH(v); pc += 1; break;
        }
        OP(0x74) { ps_jslot a = POP(), v; v.i = -a.i; PUSH(v); pc += 1; break; }
        OP(0x78) { ps_jslot b = POP(), a = POP(), v; v.i = a.i << (b.i & 31); PUSH(v); pc += 1; break; }
        OP(0x7a) { ps_jslot b = POP(), a = POP(), v; v.i = a.i >> (b.i & 31); PUSH(v); pc += 1; break; }
        OP(0x7c) { ps_jslot b = POP(), a = POP(), v; v.i = (int32_t)((uint32_t)a.i >> (b.i & 31)); PUSH(v); pc += 1; break; }
        OP(0x7e) { ps_jslot b = POP(), a = POP(), v; v.i = a.i & b.i; PUSH(v); pc += 1; break; }
        OP(0x80) { ps_jslot b = POP(), a = POP(), v; v.i = a.i | b.i; PUSH(v); pc += 1; break; }
        OP(0x82) { ps_jslot b = POP(), a = POP(), v; v.i = a.i ^ b.i; PUSH(v); pc += 1; break; }
        OP(0x84) {                                                 /* iinc */
            uint8_t li  = code[pc + 1];
            int8_t  inc = (int8_t)code[pc + 2];

            LOCAL(li).i += inc;
            pc += 3; break;
        }

        /* --- float arithmetic --- */
        OP(0x62) { ps_jslot b = POP(), a = POP(), v; v.f = a.f + b.f; PUSH(v); pc += 1; break; }
        OP(0x66) { ps_jslot b = POP(), a = POP(), v; v.f = a.f - b.f; PUSH(v); pc += 1; break; }
        OP(0x6a) { ps_jslot b = POP(), a = POP(), v; v.f = a.f * b.f; PUSH(v); pc += 1; break; }
        OP(0x6e) { ps_jslot b = POP(), a = POP(), v; v.f = a.f / b.f; PUSH(v); pc += 1; break; }
        OP(0x76) { ps_jslot a = POP(), v; v.f = -a.f; PUSH(v); pc += 1; break; }

        /* --- double arithmetic (two slots each) --- */
        OP(0x63) { ps_jslot b = POP(); POP(); { ps_jslot a = POP(); POP(); ps_jslot v; v.d = a.d + b.d; PUSH(v); PUSH(v); } pc += 1; break; }
        OP(0x67) { ps_jslot b = POP(); POP(); { ps_jslot a = POP(); POP(); ps_jslot v; v.d = a.d - b.d; PUSH(v); PUSH(v); } pc += 1; break; }
        OP(0x6b) { ps_jslot b = POP(); POP(); { ps_jslot a = POP(); POP(); ps_jslot v; v.d = a.d * b.d; PUSH(v); PUSH(v); } pc += 1; break; }
        OP(0x6f) { ps_jslot b = POP(); POP(); { ps_jslot a = POP(); POP(); ps_jslot v; v.d = a.d / b.d; PUSH(v); PUSH(v); } pc += 1; break; }

        /* --- long arithmetic --- */
        OP(0x61) { ps_jslot b = POP(); POP(); { ps_jslot a = POP(); POP(); ps_jslot v; v.j = a.j + b.j; PUSH(v); PUSH(v); } pc += 1; break; }
        OP(0x65) { ps_jslot b = POP(); POP(); { ps_jslot a = POP(); POP(); ps_jslot v; v.j = a.j - b.j; PUSH(v); PUSH(v); } pc += 1; break; }
        OP(0x69) { ps_jslot b = POP(); POP(); { ps_jslot a = POP(); POP(); ps_jslot v; v.j = a.j * b.j; PUSH(v); PUSH(v); } pc += 1; break; }

        /* --- conversions --- */
        OP(0x85) { ps_jslot a = POP(), v; v.j = a.i; PUSH(v); PUSH(v); pc += 1; break; }  /* i2l */
        OP(0x86) { ps_jslot a = POP(), v; v.f = (float)a.i; PUSH(v); pc += 1; break; }    /* i2f */
        OP(0x87) { ps_jslot a = POP(), v; v.d = (double)a.i; PUSH(v); PUSH(v); pc += 1; break; } /* i2d */
        OP(0x88) { ps_jslot a = POP(); POP(); ps_jslot v; v.i = (int32_t)a.j; PUSH(v); pc += 1; break; } /* l2i */
        OP(0x8b) { ps_jslot a = POP(), v; v.i = (int32_t)a.f; PUSH(v); pc += 1; break; }  /* f2i */
        OP(0x8d) { ps_jslot a = POP(), v; v.d = (double)a.f; PUSH(v); PUSH(v); pc += 1; break; } /* f2d */
        OP(0x8e) { ps_jslot a = POP(); POP(); ps_jslot v; v.i = (int32_t)a.d; PUSH(v); pc += 1; break; } /* d2i */
        OP(0x90) { ps_jslot a = POP(); POP(); ps_jslot v; v.f = (float)a.d; PUSH(v); pc += 1; break; }   /* d2f */
        OP(0x91) { ps_jslot a = POP(), v; v.i = (int8_t)a.i;  PUSH(v); pc += 1; break; }  /* i2b */
        OP(0x92) { ps_jslot a = POP(), v; v.i = (uint16_t)a.i; PUSH(v); pc += 1; break; } /* i2c */
        OP(0x93) { ps_jslot a = POP(), v; v.i = (int16_t)a.i; PUSH(v); pc += 1; break; }  /* i2s */

        /* --- comparisons --- */
        OP(0x94) {                                                 /* lcmp */
            ps_jslot b = POP(); POP();
            { ps_jslot a = POP(); POP(); ps_jslot v;
              v.i = a.j < b.j ? -1 : (a.j > b.j ? 1 : 0); PUSH(v); }
            pc += 1; break;
        }
        /* fcmpl/fcmpg, and dcmpl/dcmpg below.
         *
         * The pair differ only when one side is NaN, and that difference is
         * the whole reason there are two opcodes: fcmpl answers -1 and fcmpg
         * answers 1, so javac can pick the one that makes its branch fall the
         * right way. Answering 0 for unordered - as this did - makes NaN
         * compare equal to everything, so the idiom `if(x != x)` that tests
         * for NaN is false for every NaN there is. */
        OP(0x95) case 0x96: {                                      /* fcmpl/fcmpg */
            ps_jslot b = POP(), a = POP(), v;

            if(a.f != a.f || b.f != b.f)
                v.i = (op == 0x95) ? -1 : 1;
            else
                v.i = a.f < b.f ? -1 : (a.f > b.f ? 1 : 0);
            PUSH(v); pc += 1; break;
        }
        OP(0x97) case 0x98: {                                      /* dcmpl/dcmpg */
            ps_jslot b = POP(); POP();
            { ps_jslot a = POP(); POP(); ps_jslot v;

              if(a.d != a.d || b.d != b.d)
                  v.i = (op == 0x97) ? -1 : 1;
              else
                  v.i = a.d < b.d ? -1 : (a.d > b.d ? 1 : 0);
              PUSH(v); }
            pc += 1; break;
        }

        /* --- branches --- */
        OP(0x99) case 0x9a: case 0x9b: case 0x9c: case 0x9d: case 0x9e: {
            ps_jslot a = POP();                                    /* if<cond> */
            int t = 0;

            switch(op) {
            case 0x99: t = (a.i == 0); break;
            case 0x9a: t = (a.i != 0); break;
            case 0x9b: t = (a.i <  0); break;
            case 0x9c: t = (a.i >= 0); break;
            case 0x9d: t = (a.i >  0); break;
            case 0x9e: t = (a.i <= 0); break;
            }
            pc = t ? (uint32_t)((int32_t)pc + S2()) : pc + 3;
            break;
        }
        OP(0x9f) case 0xa0: case 0xa1: case 0xa2: case 0xa3: case 0xa4: {
            ps_jslot b = POP(), a = POP();                         /* if_icmp<cond> */
            int t = 0;

            switch(op) {
            case 0x9f: t = (a.i == b.i); break;
            case 0xa0: t = (a.i != b.i); break;
            case 0xa1: t = (a.i <  b.i); break;
            case 0xa2: t = (a.i >= b.i); break;
            case 0xa3: t = (a.i >  b.i); break;
            case 0xa4: t = (a.i <= b.i); break;
            }
            pc = t ? (uint32_t)((int32_t)pc + S2()) : pc + 3;
            break;
        }
        OP(0xa5) case 0xa6: {                                      /* if_acmp<cond> */
            ps_jslot b = POP(), a = POP();
            int t = (op == 0xa5) ? (a.o == b.o) : (a.o != b.o);

            pc = t ? (uint32_t)((int32_t)pc + S2()) : pc + 3;
            break;
        }
        OP(0xc6) case 0xc7: {                                      /* ifnull/ifnonnull */
            ps_jslot a = POP();
            int t = (op == 0xc6) ? (a.o == NULL) : (a.o != NULL);

            pc = t ? (uint32_t)((int32_t)pc + S2()) : pc + 3;
            break;
        }
        OP(0xa7) pc = (uint32_t)((int32_t)pc + S2()); break;        /* goto */
        OP(0xc8) {                                                  /* goto_w */
            int32_t off = (int32_t)((code[pc+1] << 24) | (code[pc+2] << 16) |
                                    (code[pc+3] << 8)  |  code[pc+4]);
            pc = (uint32_t)((int32_t)pc + off);
            break;
        }

        /* --- returns --- */
        OP(0xb1) {                                                  /* return */
            pop_frame(vm);
            return_value(vm, PS_T_VOID, zero);
            goto next_frame;
        }
        OP(0xac) case 0xae: case 0xb0: {                            /* ireturn/freturn/areturn */
            ps_jslot v = POP();
            uint8_t  k = fr->ret_kind;

            pop_frame(vm);
            return_value(vm, k, v);
            goto next_frame;
        }
        OP(0xad) case 0xaf: {                                       /* lreturn/dreturn */
            ps_jslot v = POP(); POP();
            uint8_t  k = fr->ret_kind;

            pop_frame(vm);
            return_value(vm, k, v);
            goto next_frame;
        }

        /* --- fields --- */
        OP(0xb2) case 0xb4: {                                       /* getstatic/getfield */
            const char *cn, *fn, *fd;
            ps_jclass  *fc;
            ps_jfield  *f;
            ps_jslot    v;

            if(ps_jcp_ref(cls, U2(), &cn, &fn, &fd) != 0) {
                rc = fail(vm, "bad field reference"); goto err;
            }
            fc = ps_jvm_class(vm, cn);
            if(!fc) { rc = fail(vm, "class not found: %s", cn); goto err; }

            /* A field of a class the runtime implements in C: System.out,
             * Color.red, and the public fields the event classes expose -
             * evt.x on a 1.0 Event, p.x on a Point.
             *
             * The receiver is popped first and passed along, because the
             * instance ones are meaningless without it. It reaches ps_jre_call
             * in argument zero, exactly where a method call would put it, so
             * one accessor there serves both e.getX() and e.x. */
            if(fc->native) {
                ps_jslot self;
                int      handled = 0;

                memset(&self, 0, sizeof self);
                if(op == 0xb4)
                    self = POP();

                memset(&v, 0, sizeof v);
                if(ps_jre_call(vm, cn, fn, fd, &self, op == 0xb4 ? 1 : 0, &v,
                               &handled) != 0) {
                    rc = -1; goto err;
                }
                if(!handled) {
                    rc = fail(vm, "unimplemented field %s.%s", cn, fn);
                    goto err;
                }
                PUSH(v);
                pc += 3;
                break;
            }

            f = ps_jclass_find_field(fc, fn);
            if(!f) { rc = fail(vm, "no field %s.%s", cn, fn); goto err; }

            if(op == 0xb2) {
                v = ((ps_jslot *)fc->statics)[f->slot];
            }
            else {
                ps_jslot self = POP();

                if(!self.o) {
                    rc = ps_jvm_throw(vm, "java/lang/NullPointerException", fn);
                    goto err;
                }
                if(f->slot >= self.o->cls->inst_slots) {
                    rc = fail(vm, "field slot out of range"); goto err;
                }
                v = self.o->fields[f->slot];
            }
            PUSH(v);
            if(f->kind == PS_T_LONG || f->kind == PS_T_DOUBLE) PUSH(v);
            pc += 3;
            break;
        }
        OP(0xb3) case 0xb5: {                                       /* putstatic/putfield */
            const char *cn, *fn, *fd;
            ps_jclass  *fc;
            ps_jfield  *f;
            ps_jslot    v;

            if(ps_jcp_ref(cls, U2(), &cn, &fn, &fd) != 0) {
                rc = fail(vm, "bad field reference"); goto err;
            }
            fc = ps_jvm_class(vm, cn);
            if(!fc) { rc = fail(vm, "class not found: %s", cn); goto err; }

            f = ps_jclass_find_field(fc, fn);
            if(!f) { rc = fail(vm, "no field %s.%s", cn, fn); goto err; }

            v = POP();
            if(f->kind == PS_T_LONG || f->kind == PS_T_DOUBLE) { v = POP(); }

            if(op == 0xb3) {
                ((ps_jslot *)fc->statics)[f->slot] = v;
            }
            else {
                ps_jslot self = POP();

                if(!self.o) {
                    rc = ps_jvm_throw(vm, "java/lang/NullPointerException", fn);
                    goto err;
                }
                if(f->slot >= self.o->cls->inst_slots) {
                    rc = fail(vm, "field slot out of range"); goto err;
                }
                self.o->fields[f->slot] = v;
            }
            pc += 3;
            break;
        }

        /* --- invocation --- */
        OP(0xb6) case 0xb7: case 0xb9: case 0xb8: {                 /* invoke* */
            int pushed = 0;
            int kind   = (op == 0xb8) ? INV_STATIC
                       : (op == 0xb7) ? INV_SPECIAL
                       : (op == 0xb9) ? INV_INTERFACE : INV_VIRTUAL;

            if(do_invoke(vm, cls, U2(), kind, stack, &sp, &pushed) != 0) {
                rc = -1; goto err;
            }

            /* Past the invoke before saving, so a frame resumed later carries
             * on after the call rather than making it again. */
            pc += (op == 0xb9) ? 5 : 3;

            if(pushed) {
                fr->pc = pc;
                fr->sp = sp;
                goto next_frame;
            }
            if(vm->sleeping) {
                fr->pc = pc;
                fr->sp = sp;
                return PS_RUN_SLEEP;
            }
            break;
        }

        /* --- allocation --- */
        OP(0xbb) {                                                  /* new */
            const char *cn = ps_jcp_class_name(cls, U2());
            ps_jclass  *nc;
            ps_jslot    v;

            if(!cn) { rc = fail(vm, "bad new"); goto err; }
            nc = ps_jvm_class(vm, cn);
            if(!nc) { rc = fail(vm, "class not found: %s", cn); goto err; }

            v.o = ps_jvm_new(vm, nc);
            if(!v.o) { rc = fail(vm, "out of memory allocating %s", cn); goto err; }
            PUSH(v);
            pc += 3;
            break;
        }
        OP(0xbc) {                                                  /* newarray */
            static const uint8_t map[] = {
                0,0,0,0, PS_T_BOOL, PS_T_CHAR, PS_T_FLOAT, PS_T_DOUBLE,
                PS_T_BYTE, PS_T_SHORT, PS_T_INT, PS_T_LONG
            };
            uint8_t  t = U1();
            ps_jslot n = POP(), v;

            if(t < 4 || t > 11) { rc = fail(vm, "bad newarray type"); goto err; }
            v.o = ps_jvm_new_array(vm, map[t], n.i);
            if(!v.o) { rc = fail(vm, "cannot allocate array of %d", n.i); goto err; }
            PUSH(v);
            pc += 2;
            break;
        }
        OP(0xbd) {                                                  /* anewarray */
            ps_jslot n = POP(), v;

            v.o = ps_jvm_new_array(vm, PS_T_REF, n.i);
            if(!v.o) { rc = fail(vm, "cannot allocate array of %d", n.i); goto err; }
            PUSH(v);
            pc += 3;
            break;
        }
        OP(0xbe) {                                                  /* arraylength */
            ps_jslot a = POP(), v;

            if(!a.o) {
                rc = ps_jvm_throw(vm, "java/lang/NullPointerException",
                                  "arraylength");
                goto err;
            }
            v.i = a.o->len;
            PUSH(v);
            pc += 1;
            break;
        }

        OP(0xbf) {                                                  /* athrow */
            ps_jslot e = POP();

            vm->thrown   = e.o;
            vm->throwing = 1;
            if(!vm->err[0])
                snprintf(vm->err, sizeof vm->err, "%s",
                         e.o && e.o->cls ? e.o->cls->name : "exception");
            fr->pc = pc;
            fr->sp = sp;
            goto unwind;
        }

        /* --- casts, which this VM takes on trust ---
         *
         * checkcast and instanceof need a class hierarchy walk that only
         * matters for code doing runtime type tests. Applets of this vintage
         * do not; when one does, it gets a permissive answer rather than a
         * wrong branch. */
        OP(0xc0) pc += 3; break;                                    /* checkcast */
        OP(0xc1) {                                                  /* instanceof */
            ps_jslot a = POP(), v;
            v.i = a.o ? 1 : 0;
            PUSH(v); pc += 3; break;
        }


        /* --- switch ---
         *
         * A switch statement is ordinary Java and both forms of it turn up in
         * anything with a state machine, which is most animated applets.
         * tableswitch is the dense form and indexes directly; lookupswitch is
         * the sparse form and carries sorted key/offset pairs.
         *
         * Both are padded to a four-byte boundary measured from the start of
         * the method, not from the opcode - which is the detail that makes
         * hand-decoding them wrong the first time. */
        OP(0xaa) {                                                  /* tableswitch */
            uint32_t base = (pc + 4) & ~3u;
            int32_t  def, lo, hi;
            ps_jslot k = POP();

            if(base + 12 > m->code_len) {
                rc = fail(vm, "truncated tableswitch"); goto err;
            }
            def = (int32_t)((code[base]   << 24) | (code[base+1] << 16) |
                            (code[base+2] << 8)  |  code[base+3]);
            lo  = (int32_t)((code[base+4] << 24) | (code[base+5] << 16) |
                            (code[base+6] << 8)  |  code[base+7]);
            hi  = (int32_t)((code[base+8] << 24) | (code[base+9] << 16) |
                            (code[base+10] << 8) |  code[base+11]);

            if(hi < lo || (uint32_t)(hi - lo) > m->code_len) {
                rc = fail(vm, "implausible tableswitch range"); goto err;
            }

            if(k.i < lo || k.i > hi) {
                pc = (uint32_t)((int32_t)pc + def);
            }
            else {
                uint32_t e = base + 12 + (uint32_t)(k.i - lo) * 4;

                if(e + 4 > m->code_len) {
                    rc = fail(vm, "tableswitch entry past end"); goto err;
                }
                pc = (uint32_t)((int32_t)pc +
                     (int32_t)((code[e] << 24) | (code[e+1] << 16) |
                               (code[e+2] << 8) | code[e+3]));
            }
            break;
        }
        OP(0xab) {                                                  /* lookupswitch */
            uint32_t base = (pc + 4) & ~3u;
            int32_t  def, npairs, i2;
            ps_jslot k = POP();
            int32_t  target;

            if(base + 8 > m->code_len) {
                rc = fail(vm, "truncated lookupswitch"); goto err;
            }
            def    = (int32_t)((code[base]   << 24) | (code[base+1] << 16) |
                               (code[base+2] << 8)  |  code[base+3]);
            npairs = (int32_t)((code[base+4] << 24) | (code[base+5] << 16) |
                               (code[base+6] << 8)  |  code[base+7]);

            if(npairs < 0 || base + 8 + (uint32_t)npairs * 8 > m->code_len) {
                rc = fail(vm, "implausible lookupswitch"); goto err;
            }

            target = def;
            /* Linear rather than binary: the pairs are sorted and a binary
             * search would be correct, but real switch tables are a handful
             * of cases and the compare is cheaper than the setup. */
            for(i2 = 0; i2 < npairs; i2++) {
                uint32_t e = base + 8 + (uint32_t)i2 * 8;
                int32_t  key = (int32_t)((code[e] << 24) | (code[e+1] << 16) |
                                         (code[e+2] << 8) | code[e+3]);

                if(key == k.i) {
                    target = (int32_t)((code[e+4] << 24) | (code[e+5] << 16) |
                                       (code[e+6] << 8)  |  code[e+7]);
                    break;
                }
            }
            pc = (uint32_t)((int32_t)pc + target);
            break;
        }

        /* --- long arithmetic, the rest of it --- */
        OP(0x6d) {                                                  /* ldiv */
            ps_jslot b = POP(); POP();
            { ps_jslot a = POP(); POP(); ps_jslot v;
              if(b.j == 0) { rc = fail(vm, "divide by zero"); goto err; }
              v.j = (a.j == INT64_MIN && b.j == -1) ? INT64_MIN : a.j / b.j;
              PUSH(v); PUSH(v); }
            pc += 1; break;
        }
        OP(0x71) {                                                  /* lrem */
            ps_jslot b = POP(); POP();
            { ps_jslot a = POP(); POP(); ps_jslot v;
              if(b.j == 0) { rc = fail(vm, "divide by zero"); goto err; }
              v.j = (a.j == INT64_MIN && b.j == -1) ? 0 : a.j % b.j;
              PUSH(v); PUSH(v); }
            pc += 1; break;
        }
        OP(0x75) {                                                  /* lneg */
            ps_jslot a = POP(); POP(); { ps_jslot v; v.j = -a.j; PUSH(v); PUSH(v); }
            pc += 1; break;
        }
        OP(0x77) {                                                  /* dneg */
            ps_jslot a = POP(); POP(); { ps_jslot v; v.d = -a.d; PUSH(v); PUSH(v); }
            pc += 1; break;
        }
        OP(0x79) case 0x7b: case 0x7d: {                            /* lshl/lshr/lushr */
            ps_jslot n2 = POP();
            { ps_jslot a = POP(); POP(); ps_jslot v;
              int sh = n2.i & 63;
              if(op == 0x79)      v.j = (int64_t)((uint64_t)a.j << sh);
              else if(op == 0x7d) v.j = (int64_t)((uint64_t)a.j >> sh);
              else                v.j = a.j >> sh;
              PUSH(v); PUSH(v); }
            pc += 1; break;
        }
        OP(0x7f) case 0x81: case 0x83: {                            /* land/lor/lxor */
            ps_jslot b = POP(); POP();
            { ps_jslot a = POP(); POP(); ps_jslot v;
              if(op == 0x7f)      v.j = a.j & b.j;
              else if(op == 0x81) v.j = a.j | b.j;
              else                v.j = a.j ^ b.j;
              PUSH(v); PUSH(v); }
            pc += 1; break;
        }
        OP(0x89) { ps_jslot a = POP(); POP(); { ps_jslot v; v.f = (float)a.j;  PUSH(v); } pc += 1; break; }  /* l2f */
        OP(0x8a) { ps_jslot a = POP(); POP(); { ps_jslot v; v.d = (double)a.j; PUSH(v); PUSH(v); } pc += 1; break; } /* l2d */
        OP(0x8c) { ps_jslot a = POP(); { ps_jslot v; v.j = (int64_t)a.f; PUSH(v); PUSH(v); } pc += 1; break; }       /* f2l */
        OP(0x8f) { ps_jslot a = POP(); POP(); { ps_jslot v; v.j = (int64_t)a.d; PUSH(v); PUSH(v); } pc += 1; break; }/* d2l */

        /* --- the remaining dup forms ---
         *
         * javac emits these for compound assignment into an array element or
         * a field of a category-2 type: `a[i] += 1` on a long[]. */
        OP(0x5d) {                                                  /* dup2_x1 */
            ps_jslot a = POP(), b = POP(), c = POP();
            PUSH(b); PUSH(a); PUSH(c); PUSH(b); PUSH(a); pc += 1; break;
        }
        OP(0x5e) {                                                  /* dup2_x2 */
            ps_jslot a = POP(), b = POP(), c = POP(), e = POP();
            PUSH(b); PUSH(a); PUSH(e); PUSH(c); PUSH(b); PUSH(a); pc += 1; break;
        }

        /* --- wide ---
         *
         * A method with more than 255 locals, or an iinc past a signed byte.
         * Rare in applets, and cheap enough to support that leaving it out
         * would be a strange place to stop. */
        OP(0xc4) {
            uint8_t  wop = code[pc + 1];
            uint16_t idx = (uint16_t)((code[pc + 2] << 8) | code[pc + 3]);

            switch(wop) {
            case 0x15: case 0x17: case 0x19:            /* iload/fload/aload */
                PUSH(LOCAL(idx)); pc += 4; break;
            case 0x16: case 0x18: {                     /* lload/dload */
                ps_jslot v = LOCAL(idx); PUSH(v); PUSH(v); pc += 4; break;
            }
            case 0x36: case 0x38: case 0x3a: {          /* istore/fstore/astore */
                ps_jslot v = POP(); LOCAL(idx) = v; pc += 4; break;
            }
            case 0x37: case 0x39: {                     /* lstore/dstore */
                ps_jslot v = POP(); POP(); LOCAL(idx) = v; pc += 4; break;
            }
            case 0x84:                                  /* iinc */
                LOCAL(idx).i += (int16_t)((code[pc + 4] << 8) | code[pc + 5]);
                pc += 6; break;
            default:
                rc = fail(vm, "unsupported wide opcode 0x%02x", wop);
                goto err;
            }
            break;
        }

        /* --- multianewarray ---
         *
         * The count byte is how many lengths were supplied, not how many
         * dimensions the type has: `new int[n][]` is a two-dimensional type
         * built from one length, and javac emits exactly that. Insisting on
         * two rejected it. Worse, when the count did match, the rows were made
         * PS_T_INT whatever the descriptor said, so `new String[a][b]` came
         * back as arrays of int. Reading the descriptor fixes both. */
        OP(0xc5) {
            const char *d = ps_jcp_class_name(cls,
                                (uint16_t)((code[pc + 1] << 8) | code[pc + 2]));
            uint8_t     dims = code[pc + 3];
            int32_t     counts[PS_JVM_MAX_DIMS];
            ps_jslot    v;
            long        saved;
            int         i3;

            if(!d) { rc = fail(vm, "bad array type"); goto err; }
            if(dims < 1 || dims > PS_JVM_MAX_DIMS) {
                rc = fail(vm, "multianewarray with %d lengths", dims);
                goto err;
            }
            /* Lengths were pushed outermost first, so they pop backwards. */
            for(i3 = dims - 1; i3 >= 0; i3--) {
                ps_jslot n = POP();

                if(n.i < 0) { rc = fail(vm, "negative array size"); goto err; }
                counts[i3] = n.i;
            }

            /* build_multi holds each level while it fills the one below, which
             * is the one thing obj_alloc's comment says the interpreter never
             * does. Rather than teach the collector to find a half-built tree,
             * hold it off for the duration: the growth is bounded by the array
             * being built, and that array is about to be reachable anyway. */
            saved     = vm->gc_at;
            vm->gc_at = 0;
            v.o       = build_multi(vm, d, counts, dims);
            vm->gc_at = saved;

            if(!v.o) { rc = fail(vm, "cannot allocate %s", d); goto err; }
            PUSH(v);
            pc += 4;
            break;
        }

        /* --- monitors ---
         *
         * There is one thread, so entering a monitor is entering a monitor
         * nobody else holds. Honouring the opcode as a no-op is correct here
         * in a way it would not be with a scheduler. */
        OP(0xc2) case 0xc3: POP(); pc += 1; break;

            default:
                rc = fail(vm, "unimplemented opcode 0x%02x at %u in %s.%s",
                          op, pc, cls->name, m->name);
                goto err;
            }

        }

        /* Ran off the end without a return, which javac does not emit. */
        rc = fail(vm, "%s.%s ran past the end of its code", cls->name,
                  m->name);
        goto err;

    unwind:
        /* Walk out until a frame claims it. Each frame that does not is
         * discarded, which is what makes the locals of an abandoned call go
         * away rather than linger. */
        while(vm->depth > floor) {
            ps_jframe  *top = &vm->frames[vm->depth - 1];
            const char *tn  = vm->thrown && vm->thrown->cls
                            ? vm->thrown->cls->name : NULL;
            int32_t     h;

            /* The pc recorded is the instruction after the one that threw for
             * an invoke, and the throwing instruction itself for athrow. The
             * exception table's range is half-open on the end, so stepping
             * back one byte keeps a throw on the last instruction of a try
             * block inside it. */
            h = find_handler(top, top->pc > 0 ? top->pc - 1 : 0, tn);

            if(h >= 0) {
                /* JVMS: the handler starts with an empty stack holding only
                 * the exception. */
                top->sp = 0;
                top->stack[top->sp++].o = vm->thrown;
                top->pc = (uint32_t)h;

                vm->throwing = 0;
                vm->thrown   = NULL;
                vm->err[0]   = '\0';
                vm->failed   = 0;
                goto next_frame;
            }

            pop_frame(vm);
        }

        /* Nobody wanted it. vm->err already names it. */
        vm->throwing = 0;
        vm->failed   = 1;
        return PS_RUN_ERROR;

    err:
        (void)rc;
        /* A failure the VM raised rather than the applet. If it was raised as
         * a throw, give the applet's own handlers a chance at it first - that
         * is the whole difference between "the applet caught a divide by zero"
         * and "the browser stopped the applet". */
        if(vm->throwing) {
            fr->pc = pc;
            fr->sp = sp;
            goto unwind;
        }
        /* Only this call's frames. Whatever was suspended underneath is not
         * ours to throw away - a paint that fails must not take the animation
         * thread's stack with it. */
        unwind_to(vm, floor);
        return PS_RUN_ERROR;

    next_frame:
        continue;
    }

    return PS_RUN_DONE;
}

int ps_jvm_call(ps_jvm *vm, ps_jclass *cls, ps_jmethod *m,
                const ps_jslot *args, int nargs, ps_jslot *ret)
{
    /* Stacks on top of whatever is already there. The animation thread is
     * normally suspended below this - paint() is called between two of its
     * instructions, which is precisely what a real browser does to a real
     * applet, only there it needs a lock to do it safely. */
    int     base = vm->depth;
    int     saved_floor = vm->floor;
    ps_jrun r;

    if(push_frame(vm, cls, m, args, nargs) != 0)
        return -1;

    r = run(vm, base);
    vm->floor = saved_floor;

    if(r == PS_RUN_ERROR)
        return -1;
    if(r != PS_RUN_DONE) {
        /* paint() must not sleep and must not run out of budget: the browser
         * has a frame to finish. Saying so beats a half-drawn applet. */
        unwind_to(vm, base);
        vm->sleeping = 0;
        return fail(vm, "%s.%s did not finish (%s)", cls->name, m->name,
                    r == PS_RUN_SLEEP ? "it slept" : "it ran too long");
    }

    if(ret)
        *ret = vm->ret;
    return 0;
}

/* --- the animation thread ------------------------------------------------
 *
 * An applet animates like this, near enough universally:
 *
 *     public void start() { t = new Thread(this); t.start(); }
 *     public void run()   { while (true) { step(); repaint(); sleep(50); } }
 *
 * There is one hardware thread and no scheduler, so run() is executed in
 * slices against the browser's frame clock: some instructions per frame, and
 * a sleep suspends the frame stack until the wall clock catches up. Nothing
 * about the applet can tell the difference, because none of them are relying
 * on genuine parallelism - they are relying on getting to run periodically,
 * which is what this provides.
 *
 * The frame stack lives on the VM, so a slice that stops halfway down a call
 * chain resumes exactly there.
 */

int ps_jvm_has_thread(const ps_jvm *vm)
{
    return vm->thread_run && !vm->thread_done;
}

ps_jrun ps_jvm_pump(ps_jvm *vm, int dt_ms, long budget)
{
    ps_jrun r;

    if(vm->failed || !ps_jvm_has_thread(vm))
        return PS_RUN_DONE;

    /* Sleeping is counted down here rather than against a clock the applet
     * can see, so a browser that drops frames slows the animation instead of
     * running a burst to catch up - which is what a page in the background
     * should do. */
    if(vm->wake_ms > 0) {
        vm->wake_ms -= dt_ms;
        if(vm->wake_ms > 0)
            return PS_RUN_SLEEP;
        vm->wake_ms = 0;
    }

    /* First pump: enter run() and leave it entered. Every pump after this
     * resumes whatever the frame stack is holding. */
    if(!vm->thread_started) {
        ps_jclass  *rc_cls = vm->thread_run->cls;
        ps_jmethod *rm     = ps_jclass_find_method(rc_cls, "run", "()V");
        ps_jslot    self;

        if(!rm || !rm->code) {
            vm->thread_done = 1;
            return PS_RUN_DONE;
        }

        memset(&self, 0, sizeof self);
        self.o = vm->thread_run;

        if(push_frame(vm, rc_cls, rm, &self, 1) != 0) {
            vm->thread_done = 1;
            return PS_RUN_ERROR;
        }
        vm->thread_started = 1;
    }

    vm->sleeping = 0;
    vm->budget   = budget;

    r = run(vm, 0);

    if(r == PS_RUN_DONE || r == PS_RUN_ERROR) {
        /* run() returning is an applet whose loop ended, which is legitimate
         * and final. */
        vm->thread_done = 1;
    }
    if(r == PS_RUN_SLEEP)
        vm->sleeping = 0;

    return r;
}

int ps_jvm_take_repaint(ps_jvm *vm)
{
    int r = vm->repaint;

    vm->repaint = 0;
    return r;
}

/* Runs one class's <clinit>, its superclass first.
 *
 * A failing static initialiser is reported and then stepped over rather than
 * stopping the applet: an applet whose lookup table did not build will draw
 * something wrong, which is bad, but an applet that refuses to start because
 * one of its twelve classes has an initialiser we cannot run yet draws
 * nothing at all, which is worse and much harder to diagnose from a console.
 */
static void clinit_one(ps_jvm *vm, ps_jclass *c)
{
    ps_jmethod *m;

    if(!c || c->inited || c->native)
        return;

    c->inited = 1;              /* before the call, so recursion terminates */

    if(c->super)
        clinit_one(vm, c->super);

    /* This class's own method table, not ps_jclass_find_method - that walks
     * the superclass chain, and the superclass has just been initialised, so
     * a class with no <clinit> of its own would inherit one and run it a
     * second time. <clinit> is the one method that must never be inherited. */
    m = NULL;
    {
        uint16_t i;

        for(i = 0; i < c->method_count; i++) {
            if(!strcmp(c->methods[i].name, "<clinit>") &&
               !strcmp(c->methods[i].desc, "()V")) {
                m = &c->methods[i];
                break;
            }
        }
    }
    if(!m || !m->code)
        return;

    vm->budget = 20L * 1000 * 1000;
    if(ps_jvm_call(vm, c, m, NULL, 0, NULL) != 0) {
        printf("popsurf: %s.<clinit> failed: %s\n", c->name, vm->err);
        vm->failed = 0;
    }
}

static void clinit_all(ps_jvm *vm)
{
    int i;

    for(i = 0; i < vm->nclasses; i++)
        clinit_one(vm, vm->classes[i]);
}

int ps_jvm_run_applet(ps_jvm *vm, ps_jclass *applet, ps_jgfx *g)
{
    ps_jmethod *init, *paint, *start;
    ps_jobj    *self;
    ps_jslot    argv[2];

    if(!applet)
        return fail(vm, "no applet class");

    vm->gfx = g;

    /* Static initialisers, before anything of the applet's runs.
     *
     * `static final int[] SHAPE = {...}` is not data in the class file - javac
     * compiles it into a <clinit> method that has to be executed, exactly as
     * `int[] x = {...}` becomes bytecode in <init>. Nothing here ever ran it,
     * so every static table in every applet was still null when paint() read
     * it, and period applets keep their lookup tables, sprite offsets and
     * colour ramps in precisely that shape.
     *
     * Run for every class now rather than lazily on first use. The JVM's rule
     * is first active use, and getting that exactly right means hooking
     * getstatic, putstatic, invokestatic and new - four opcodes that would
     * each have to re-enter the interpreter mid-instruction. The applet's
     * whole class graph is resolved up front here, so there is no moment
     * between load and use to be lazy about. */
    clinit_all(vm);
    if(vm->failed)
        return -1;

    self = ps_jvm_new(vm, applet);
    if(!self)
        return fail(vm, "cannot instantiate %s", applet->name);
    vm->applet = self;

    /* The constructor runs the field initialisers - `int[] data = {...}` is
     * bytecode in <init>, not data in the class file - so an applet whose
     * paint() reads a field would read zeroes without it. */
    init = ps_jclass_find_method(applet, "<init>", "()V");
    if(init && init->code) {
        memset(&argv[0], 0, sizeof argv[0]);
        argv[0].o = self;
        vm->budget = 20L * 1000 * 1000;
        if(ps_jvm_call(vm, applet, init, argv, 1, NULL) != 0)
            return -1;
    }

    /* init() then start(), in that order, which is the applet lifecycle a
     * browser is required to follow. start() is where a thread gets created,
     * so this is what arms the animation. */
    {
        ps_jmethod *am = ps_jclass_find_method(applet, "init", "()V");

        if(am && am->code) {
            memset(&argv[0], 0, sizeof argv[0]);
            argv[0].o = self;
            vm->budget = 20L * 1000 * 1000;
            if(ps_jvm_call(vm, applet, am, argv, 1, NULL) != 0)
                return -1;
        }
    }

    start = ps_jclass_find_method(applet, "start", "()V");
    if(start && start->code) {
        memset(&argv[0], 0, sizeof argv[0]);
        argv[0].o = self;
        vm->budget = 20L * 1000 * 1000;
        if(ps_jvm_call(vm, applet, start, argv, 1, NULL) != 0)
            return -1;
    }

    paint = ps_jclass_find_method(applet, "paint", "(Ljava/awt/Graphics;)V");
    if(!paint || !paint->code)
        return fail(vm, "%s has no paint(Graphics)", applet->name);

    vm->paint_method = paint;
    vm->paint_class  = applet;

    return ps_jvm_paint(vm, g);
}

/* Repaints. Separate from run_applet because after the first frame this is
 * the only part that runs again. */
int ps_jvm_paint(ps_jvm *vm, ps_jgfx *g)
{
    ps_jslot argv[2], r;

    if(!vm->paint_method || !vm->applet)
        return fail(vm, "nothing to paint");

    vm->gfx = g;

    memset(argv, 0, sizeof argv);
    argv[0].o = vm->applet;
    argv[1].o = ps_jvm_new(vm, ps_jvm_class(vm, "java/awt/Graphics"));
    if(!argv[1].o)
        return fail(vm, "cannot create Graphics");
    argv[1].o->native = g;

    memset(&r, 0, sizeof r);

    /* Whatever the caller set stands. A repaint runs on a much tighter budget
     * than the applet's first run, and overwriting it here would put every
     * repaint back on the twenty million that let one bad applet stall the
     * browser for seconds. */
    if(vm->budget <= 0)
        vm->budget = 20L * 1000 * 1000;

    return ps_jvm_call(vm, vm->paint_class, vm->paint_method, argv, 2, &r);
}

/* --- the 1.1 event model -------------------------------------------------
 *
 * Registration, and delivery of one event to everything registered for it.
 * The listener table is on the VM rather than on the Component because the
 * Component an applet registers against is the applet - there is one, and a
 * per-object list to hold it would be machinery in place of a comparison. The
 * target is recorded anyway, so a second component later is a change to this
 * file and nothing else.
 *
 * What is deliberately absent, rather than faked: mouseEntered and
 * mouseExited. The browser reports positions and buttons and has no notion of
 * the cursor crossing an applet's boundary, so synthesising a crossing from
 * the first move inside the box would fire it again on every scroll. An applet
 * that only lights up on hover stays dark, which is visibly incomplete rather
 * than quietly wrong.
 */

int ps_jvm_add_listener(ps_jvm *vm, ps_jobj *target, ps_jobj *l, int kind)
{
    if(!l || kind < 0 || kind >= PS_LSN_KINDS)
        return 0;                       /* addMouseListener(null) is a no-op */

    if(vm->nlisteners >= PS_JVM_MAX_LISTENERS)
        return fail(vm, "more than %d listeners registered",
                    PS_JVM_MAX_LISTENERS);

    vm->listeners[vm->nlisteners].target = target;
    vm->listeners[vm->nlisteners].obj    = l;
    vm->listeners[vm->nlisteners].kind   = (uint8_t)kind;
    vm->nlisteners++;

    /* The latch, and the whole reason the 1.0 path has to ask before it runs.
     * It does not come down again when the listener is removed - that is AWT's
     * behaviour, observed rather than assumed. */
    vm->new_events_only = 1;
    return 0;
}

int ps_jvm_remove_listener(ps_jvm *vm, ps_jobj *target, ps_jobj *l, int kind)
{
    int i;

    for(i = 0; i < vm->nlisteners; i++) {
        if(vm->listeners[i].obj != l || vm->listeners[i].kind != kind)
            continue;
        if(target && vm->listeners[i].target != target)
            continue;

        /* The first match only, and the order of the rest is preserved:
         * listeners fire in the order they were added, and an applet that
         * registers the same object twice is entitled to two calls. */
        memmove(&vm->listeners[i], &vm->listeners[i + 1],
                sizeof vm->listeners[0] * (size_t)(vm->nlisteners - i - 1));
        vm->nlisteners--;
        return 1;
    }
    return 0;
}

int ps_jvm_new_events_only(const ps_jvm *vm)
{
    return vm->new_events_only;
}

/* Allocates an event object and fills the header every one of them carries. */
static ps_jobj *make_event(ps_jvm *vm, const char *cls_name, int id, int mods)
{
    ps_jobj *e = ps_jvm_new(vm, ps_jvm_class(vm, cls_name));

    if(!e || !e->fields)
        return NULL;

    e->fields[PS_EVF_SOURCE].o = vm->applet;
    e->fields[PS_EVF_ID].i     = id;
    e->fields[PS_EVF_MODS].i   = mods;
    return e;
}

/* Calls `name` on every listener of `kind`, handing it `ev`.
 *
 * A listener that does not implement the method is skipped rather than
 * refused: that is exactly what subclassing MouseAdapter and overriding one of
 * its five methods means, and it is the commonest shape a real applet takes. */
static int deliver(ps_jvm *vm, int kind, const char *name, const char *desc,
                   ps_jobj *ev)
{
    ps_jobj *targets[PS_JVM_MAX_LISTENERS];
    int      n = 0, i, ran = 0;

    /* Snapshotted first because a listener may remove itself, or another one,
     * while it runs. AWT delivers to the set that was registered when the
     * event went out; walking the live table would skip whoever had shuffled
     * down into the slot just vacated. */
    for(i = 0; i < vm->nlisteners; i++) {
        if(vm->listeners[i].kind == kind)
            targets[n++] = vm->listeners[i].obj;
    }

    for(i = 0; i < n; i++) {
        ps_jobj    *l = targets[i];
        ps_jclass  *dc;
        ps_jmethod *m;
        ps_jslot    argv[2], r;

        if(!l || !l->cls)
            continue;

        m = find_method_in(l->cls, name, desc, &dc);
        if(!m || !m->code)
            continue;

        memset(argv, 0, sizeof argv);
        argv[0].o = l;
        argv[1].o = ev;
        memset(&r, 0, sizeof r);

        /* ev is safe across this even though nothing on the VM points at it:
         * it goes in as an argument, so it is in the callee frame's locals for
         * as long as the callee can allocate, and nothing between two of these
         * calls allocates at all. */
        if(ps_jvm_call(vm, dc, m, argv, 2, &r) != 0) {
            /* One bad listener must not stop the others and must not stop the
             * applet. The browser reports it and carries on, which is what it
             * already does for a paint() that fails. */
            printf("applet: %s.%s failed: %s\n", l->cls->name, name, vm->err);
            vm->failed = 0;
            vm->err[0] = '\0';
            continue;
        }
        ran++;
    }
    return ran;
}

int ps_jvm_post_mouse(ps_jvm *vm, int id, int x, int y, int clicks, int mods)
{
    static const char *const desc = "(Ljava/awt/event/MouseEvent;)V";
    const char *name = NULL;
    int         kind = PS_LSN_MOUSE;
    ps_jobj    *ev;

    switch(id) {
    case PS_EV_MOUSE_PRESSED:  name = "mousePressed";  break;
    case PS_EV_MOUSE_RELEASED: name = "mouseReleased"; break;
    case PS_EV_MOUSE_CLICKED:  name = "mouseClicked";  break;
    case PS_EV_MOUSE_ENTERED:  name = "mouseEntered";  break;
    case PS_EV_MOUSE_EXITED:   name = "mouseExited";   break;
    case PS_EV_MOUSE_MOVED:    name = "mouseMoved";   kind = PS_LSN_MOTION; break;
    case PS_EV_MOUSE_DRAGGED:  name = "mouseDragged"; kind = PS_LSN_MOTION; break;
    default:                   return 0;
    }

    /* Nothing registered means no object either. A page being scrolled over
     * delivers a move per frame, and an allocation per frame is what the
     * collector was written to stop. */
    if(!vm->nlisteners)
        return 0;

    ev = make_event(vm, "java/awt/event/MouseEvent", id, mods);
    if(!ev)
        return 0;

    ev->fields[PS_EVF_X].i      = x;
    ev->fields[PS_EVF_Y].i      = y;
    ev->fields[PS_EVF_CLICKS].i = clicks;

    return deliver(vm, kind, name, desc, ev);
}

int ps_jvm_post_key(ps_jvm *vm, int id, int code, int ch, int mods)
{
    static const char *const desc = "(Ljava/awt/event/KeyEvent;)V";
    const char *name = NULL;
    ps_jobj    *ev;

    switch(id) {
    case PS_EV_KEY_PRESSED:  name = "keyPressed";  break;
    case PS_EV_KEY_RELEASED: name = "keyReleased"; break;
    case PS_EV_KEY_TYPED:    name = "keyTyped";    break;
    default:                 return 0;
    }

    if(!vm->nlisteners)
        return 0;

    ev = make_event(vm, "java/awt/event/KeyEvent", id, mods);
    if(!ev)
        return 0;

    /* keyTyped is the one that carries a character and no key code, which is
     * how an applet tells "the A key went down" from "the letter a arrived".
     * Observed on a real JDK: keyPressed(VK_A) carries 'a' as well, and a key
     * with no character at all - an arrow - reports CHAR_UNDEFINED. */
    ev->fields[PS_EVF_KEY].i  = (id == PS_EV_KEY_TYPED) ? PS_VK_UNDEFINED
                                                        : code;
    ev->fields[PS_EVF_CHAR].i = ch;

    return deliver(vm, PS_LSN_KEY, name, desc, ev);
}
