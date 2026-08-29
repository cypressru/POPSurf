/* AWT widgets and layout managers, as shells.
 *
 * READ THIS BEFORE TRUSTING ANYTHING BELOW. Nothing in this file draws a
 * widget, lays anything out, or delivers an event to one. A Button here is an
 * object that remembers its label; a BorderLayout is an object that remembers
 * two gap values and forgets everything else. There is no toolkit under this.
 *
 * That is the whole design, and it comes from measurement rather than
 * laziness. Sixty-three applets from five authors were run against this
 * runtime and sixty-two failed; the second largest cause, thirteen applets
 * across four of the authors, was `java.awt.Button` and friends not existing
 * as *classes*. The applets die in init() on "class not found" while building
 * a control panel they were only ever going to put above the drawing - and the
 * drawing is the part worth seeing on a Dreamcast. Making `new Button("Go")`
 * and `setLayout(new BorderLayout())` return without blowing up lets paint()
 * run. Making the Button actually paint would be a project, and would unblock
 * nobody who is not already unblocked.
 *
 * Two rules keep the shells honest, and they are the difference between a
 * shell and a lie:
 *
 *  - Anything an applet can set and read back round-trips exactly. setText
 *    then getText gives the same string, because an applet parses that string
 *    and draws from the result. Where the real AWT has a non-obvious default
 *    or a clamp - Scrollbar clamping to maximum minus visibleAmount, Choice
 *    selecting index 0 on the first addItem - it was checked against a real
 *    JDK on the host and is reproduced here.
 *  - Anything that cannot be answered truthfully is not answered at all. It
 *    falls through as unimplemented and the interpreter names the method. A
 *    missing method costs one line in the log; a plausible wrong value costs
 *    somebody a day on hardware. getComponent(int) is still left out for this
 *    reason - see the note where it would have gone.
 *
 * What changed since that was written: a shell can now hold a reference to
 * another object. It could not before, because the collector cannot see a
 * ps_jobj* kept in C and would free it out from under the widget, and that
 * single limitation is what left setLayout swallowing its argument,
 * getLayout() missing entirely - a blocker for thirteen measured applets -
 * GridBagLayout.setConstraints a no-op
 * and CheckboxGroup a constructor with nothing behind it. See the note above
 * ref_table for where a reference now goes and why the mark phase finds it.
 */
#include "ps_jawt.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ps_applet.h"
#include "ps_jgeom.h"

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

/* Matches on name and, when it matters, descriptor - the same helper ps_jre.c
 * uses, and for the same reason: passing NULL for the descriptor accepts any
 * overload, which is right wherever the shell ignores the difference. */
static int is(const char *name, const char *desc, const char *n,
              const char *d)
{
    if(strcmp(name, n))
        return 0;
    return d ? !strcmp(desc, d) : 1;
}

/* A java.awt.Color carries its value in the object's length field; ps_jre.c
 * says the same thing and this file has to agree with it. */
#define COLOR_OF(o) ((uint32_t)(o)->len)

/* --- what this file owns -------------------------------------------------
 *
 * Dispatch is on the class name the constant pool gave, which javac fills in
 * with the *static* type of the receiver rather than the class that declares
 * the method: `textField.getText()` is a reference to java/awt/TextField.
 * (Checked with javap; it is not obvious, and getting it backwards would put
 * every accessor under java/awt/TextComponent, a class no applet mentions.)
 *
 * Calls on `this` inside an applet are the exception. javac writes them
 * against the applet's own class, and the interpreter, finding no such method
 * anywhere in its chain, retries against the nearest native ancestor - so
 * `add(p)` in an Applet subclass arrives here as java/applet/Applet.add. That
 * is why K_COMPONENT covers Applet, Panel and Container as well as Component.
 */
enum {
    K_NONE = 0,
    K_COMPONENT,        /* Component, Container, Panel, Applet */
    K_CANVAS,
    K_BUTTON, K_LABEL, K_TEXTFIELD, K_TEXTAREA,
    K_CHECKBOX, K_CBGROUP, K_CHOICE, K_LIST, K_SCROLLBAR,
    K_BORDER, K_FLOW, K_GRID, K_GRIDBAG, K_CARD,
    K_WINDOW, K_FRAME, K_DIALOG,
    K_TOOLKIT, K_CURSOR,
    K_SHELL             /* Insets and GridBagConstraints; see below */
};

static const struct { const char *name; uint8_t kind; } g_kinds[] = {
    { "java/awt/Component",          K_COMPONENT },
    { "java/awt/Container",          K_COMPONENT },
    { "java/awt/Panel",              K_COMPONENT },
    { "java/applet/Applet",          K_COMPONENT },
    { "java/awt/Canvas",             K_CANVAS    },
    { "java/awt/Button",             K_BUTTON    },
    { "java/awt/Label",              K_LABEL     },
    { "java/awt/TextField",          K_TEXTFIELD },
    { "java/awt/TextArea",           K_TEXTAREA  },
    { "java/awt/TextComponent",      K_TEXTFIELD },
    { "java/awt/Checkbox",           K_CHECKBOX  },
    { "java/awt/CheckboxGroup",      K_CBGROUP   },
    { "java/awt/Choice",             K_CHOICE    },
    { "java/awt/List",               K_LIST      },
    { "java/awt/Scrollbar",          K_SCROLLBAR },
    { "java/awt/BorderLayout",       K_BORDER    },
    { "java/awt/FlowLayout",         K_FLOW      },
    { "java/awt/GridLayout",         K_GRID      },
    { "java/awt/GridBagLayout",      K_GRIDBAG   },
    { "java/awt/CardLayout",         K_CARD      },
    { "java/awt/Window",             K_WINDOW    },
    { "java/awt/Frame",              K_FRAME     },
    { "java/awt/Dialog",             K_DIALOG    },
    { "java/awt/Toolkit",            K_TOOLKIT   },
    { "java/awt/Cursor",             K_CURSOR    },
    { "ps/awt/shell",                K_SHELL     },
    { NULL, 0 }
};

static int kind_of(const char *cls)
{
    int i;

    for(i = 0; g_kinds[i].name; i++) {
        if(!strcmp(g_kinds[i].name, cls))
            return g_kinds[i].kind;
    }
    return K_NONE;
}

/* Is this kind something a Component method may be called on? Layout managers
 * and Toolkit are not Components and must not answer setBackground. */
static int kind_is_component(int kind)
{
    switch(kind) {
    case K_COMPONENT: case K_CANVAS: case K_BUTTON: case K_LABEL:
    case K_TEXTFIELD: case K_TEXTAREA: case K_CHECKBOX: case K_CHOICE:
    case K_LIST: case K_SCROLLBAR: case K_WINDOW: case K_FRAME:
    case K_DIALOG:
        return 1;
    default:
        return 0;
    }
}

/* A Component that is not the surface the page gave the applet. Every kind but
 * K_COMPONENT, which is the one a plain Applet, Panel or Container arrives as.
 * The difference decides who answers size() and getFont() - see ps_jawt.h. */
static int kind_is_widget(int kind)
{
    return kind != K_COMPONENT && kind_is_component(kind);
}

int ps_jawt_is_component(const char *cls)
{
    /* The same two early outs ps_jawt_call takes, and for the same reason:
     * this is now on the front of every native call ps_jgeom.c sees, and
     * Graphics and Color are most of them. */
    if(strncmp(cls, "java/a", 6) && strncmp(cls, "ps/awt/", 7))
        return 0;
    if(!strcmp(cls, "java/awt/Graphics") || !strcmp(cls, "java/awt/Color"))
        return 0;
    return kind_is_component(kind_of(cls));
}

int ps_jawt_is_widget(const ps_jobj *o)
{
    const ps_jclass *c;

    /* Up the chain, because what an applet actually makes is a subclass:
     * `class Sheet extends Canvas` gives an object whose own class name is the
     * applet's and whose super is one of ours. Stopping at the object's own
     * name would call every such child a container. */
    for(c = o ? o->cls : NULL; c; c = c->super) {
        if(kind_is_widget(kind_of(c->name)))
            return 1;
    }
    return 0;
}

/* --- registering the classes ---------------------------------------------
 *
 * ps_jvm.c has a fixed list of the classes the runtime implements in C, and
 * this file deliberately does not appear in it: several agents edit that file
 * at once and a new name there is a merge conflict for all of them. So the
 * shells register themselves, building the same placeholder ps_jclass that
 * ps_jvm.c's make_native does.
 *
 * The hook is java/applet/Applet.<init>, which is the one call every applet
 * makes before it can reach a `new Button` - javac emits the super() call as
 * the first instruction of the applet's own constructor, and the constructor
 * runs before init(). Registration is idempotent by looking for a class it
 * would have created.
 *
 * The cost is real and worth stating: this claims about two dozen of
 * PS_JVM_MAX_CLASSES's sixty-four slots up front, whether or not the applet
 * wants a widget. An applet with more than about thirty-five classes of its
 * own will now run out where before it would not. Nothing in the test corpus
 * is near that, and the failure is the existing loud one, but if it ever bites
 * the fix is to raise the limit rather than to make this cleverer.
 */

/* The shells with no fields, which are ordinary native placeholders. */
static const char *g_native_shells[] = {
    /* Ordered by how many applets in the compatibility corpus each unblocks,
     * if the class table fills up the tail is what gets dropped. */
    "java/awt/BorderLayout",
    "java/awt/Canvas",
    "java/awt/Button",
    "java/awt/TextField",
    "java/awt/Label",
    "java/awt/Checkbox",
    "java/awt/Choice",
    "java/awt/FlowLayout",
    "java/awt/GridLayout",
    "java/awt/GridBagLayout",
    "java/awt/CardLayout",
    "java/awt/TextArea",
    "java/awt/List",
    "java/awt/Scrollbar",
    "java/awt/CheckboxGroup",
    "java/awt/Toolkit",
    "java/awt/Cursor",
    "java/awt/Frame",
    "java/awt/Window",
    "java/awt/Dialog",

    /* TextField and TextArea's superclass. It was in the kind table from the
     * start and not in this one, which is not an inconsistency anybody would
     * spot by reading: the kind table decides where a *call* goes and this one
     * decides whether the *class* exists. javac writes the declaring type into
     * the constant pool, so a field declared TextComponent and assigned a
     * TextField calls java/awt/TextComponent.getText - fifteen groups across
     * two authors do exactly that - and the class not existing stopped them
     * before the call was ever looked up. */
    "java/awt/TextComponent",

    /* Interfaces, here only so that a name resolves. Choice and Checkbox
     * implement ItemSelectable and an applet's `implements ItemListener` names
     * the other; neither is ever dispatched to, because an interface has no
     * implementation. Same reasoning as ps_jvm.c's list of listener
     * interfaces, and the same zero cost. */
    "java/awt/ItemSelectable",
    "java/awt/event/ItemListener",
    "java/awt/event/AdjustmentListener",
    "java/awt/event/TextListener",

    /* The event objects those three would carry. Nothing here ever constructs
     * one - no widget receives input - but an applet that names the class in a
     * cast or reads a constant off it should get ps_jre.c's event handling
     * rather than "class not found". */
    "java/awt/event/ItemEvent",
    "java/awt/event/AdjustmentEvent",
    "java/awt/event/TextEvent",
    NULL
};

/* The two shells that have to carry public fields.
 *
 * `c.gridx = 0` is a putfield, and the interpreter's putfield has no path
 * through the native-call machinery at all - it looks the name up in the
 * class's field table and fails if it is not there. So these two are *not*
 * native classes: they are ordinary classes with a real field table and no
 * methods, whose superclass is the one native shell below. A call on them
 * finds no method in their own chain, and the interpreter's fallback for an
 * inherited method landing on a native ancestor brings it here with the
 * receiver still in hand.
 *
 * Field order is slot order. weightx and weighty are doubles and take two
 * slots each, which the interpreter works out from the descriptor.
 */
typedef struct { const char *name; const char *desc; } shell_field;
typedef struct { const char *name; int32_t v; } shell_static;

static const shell_field g_insets_fields[] = {
    { "top", "I" }, { "left", "I" }, { "bottom", "I" }, { "right", "I" },
    { NULL, NULL }
};

static const shell_field g_gbc_fields[] = {
    { "gridx", "I" }, { "gridy", "I" },
    { "gridwidth", "I" }, { "gridheight", "I" },
    { "weightx", "D" }, { "weighty", "D" },
    { "anchor", "I" }, { "fill", "I" },
    { "insets", "Ljava/awt/Insets;" },
    { "ipadx", "I" }, { "ipady", "I" },
    { NULL, NULL }
};

/* GridBagConstraints' constants, which an applet reads through getstatic. As
 * a non-native class it gets them from real static storage rather than from a
 * call, so they are filled in at registration. Values checked against a real
 * JDK; anchor is a numbering nobody would guess. */
static const shell_static g_gbc_statics[] = {
    { "RELATIVE",   -1 }, { "REMAINDER",   0 },
    { "NONE",        0 }, { "BOTH",        1 },
    { "HORIZONTAL",  2 }, { "VERTICAL",    3 },
    { "CENTER",     10 }, { "NORTH",      11 }, { "NORTHEAST", 12 },
    { "EAST",       13 }, { "SOUTHEAST",  14 }, { "SOUTH",     15 },
    { "SOUTHWEST",  16 }, { "WEST",       17 }, { "NORTHWEST", 18 },
    { NULL, 0 }
};

/* Builds the placeholder ps_jvm.c would have built. The name is copied because
 * there is no class file image for it to point into. */
static ps_jclass *make_shell(ps_jvm *vm, const char *name, int native)
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
    c->name   = (const char *)c->raw;
    c->native = native;

    vm->classes[vm->nclasses++] = c;
    return c;
}

static ps_jclass *find_class(ps_jvm *vm, const char *name)
{
    int i;

    for(i = 0; i < vm->nclasses; i++) {
        if(!strcmp(vm->classes[i]->name, name))
            return vm->classes[i];
    }
    return NULL;
}

/* One of the two field-bearing shells. base is already registered. */
static void make_field_shell(ps_jvm *vm, const char *name, ps_jclass *base,
                             const shell_field *inst, const shell_static *st)
{
    ps_jclass *c;
    uint16_t   n = 0, ns = 0, i, slot = 0;

    if(find_class(vm, name))
        return;

    while(inst[n].name)
        n++;
    while(st && st[ns].name)
        ns++;

    c = make_shell(vm, name, 0);
    if(!c)
        return;

    c->super  = base;
    c->fields = (ps_jfield *)calloc((size_t)n + ns, sizeof(ps_jfield));
    if(!c->fields)
        return;                     /* leaves a class with no fields; the
                                       putfield below it will say so */

    for(i = 0; i < n; i++) {
        /* The name and descriptor are string literals rather than slices of a
         * class file image, which is the one way these differ from a parsed
         * class. ps_jclass_free never touches them. */
        c->fields[i].name   = inst[i].name;
        c->fields[i].desc   = inst[i].desc;
        c->fields[i].access = 0;
        c->fields[i].kind   = ps_jdesc_kind(inst[i].desc);
        c->fields[i].slot   = slot;
        slot = (uint16_t)(slot + (c->fields[i].kind == PS_T_DOUBLE ||
                                  c->fields[i].kind == PS_T_LONG ? 2 : 1));
    }
    c->inst_slots  = slot;
    c->field_count = n;

    if(!ns)
        return;

    c->statics = calloc(ns, sizeof(ps_jslot));
    if(!c->statics)
        return;                     /* the constants stay unanswered, loudly */

    for(i = 0; i < ns; i++) {
        c->fields[n + i].name   = st[i].name;
        c->fields[n + i].desc   = "I";
        c->fields[n + i].access = PS_ACC_STATIC;
        c->fields[n + i].kind   = PS_T_INT;
        c->fields[n + i].slot   = i;
        ((ps_jslot *)c->statics)[i].i = st[i].v;
    }
    c->field_count  = (uint16_t)(n + ns);
    c->static_slots = ns;
}

/* --- references the collector has to be able to see ----------------------
 *
 * setLayout hands a container an object and getLayout has to hand back the
 * same one, so this file now has to keep a ps_jobj* alive across the applet's
 * own instructions. The note above add() explains why that used to be refused:
 * a reference held in C is invisible to the mark phase, the object is freed at
 * the next collection, and the container is left pointing into the free list.
 * That objection was never to keeping a reference - it was to keeping one the
 * collector cannot see.
 *
 * There is a place it can see. ps_jvm.c's mark_roots walks every loaded
 * class's static fields and follows the reference ones, and mark() follows
 * every element of a reference array. So a private class with a single static
 * field of array type is a root that is traced for free, and ps_jvm.c - which
 * this file is not allowed to touch, and should not have to - learns nothing.
 *
 * The table is triples of (owner, key, value) because two questions share it:
 *
 *   setLayout       owner = the container, key = NULL,      value = the layout
 *   setConstraints  owner = the GridBagLayout, key = the component,
 *                                                          value = a copy
 *
 * Linear scan. A Fendt panel puts about twenty entries in it and an applet
 * asks a few dozen times in the whole of init(), which is not worth a hash.
 *
 * Entries are never removed. A container the applet drops therefore stays
 * reachable until the applet ends - a leak, bounded by REF_MAX triples, and
 * the alternative is a weak reference the collector would have to be taught
 * about. Say what it is rather than pretend it is not there.
 *
 * Triple zero is not an entry. It is a parking space for one object across one
 * allocation: everything else in this runtime allocates a single object at a
 * time precisely because a second allocation may collect the first while it is
 * held only in a C local, and copying a GridBagConstraints needs two.
 */
#define REF_STRIDE  3
#define REF_FIRST   REF_STRIDE          /* triple zero is the parking space */
#define REF_INITIAL (16 * REF_STRIDE)
#define REF_MAX     (256 * REF_STRIDE)

/* The class whose one static field holds the table. Native, field-bearing and
 * never instantiated: it exists to be walked by mark_roots. */
static void make_ref_shell(ps_jvm *vm)
{
    ps_jclass *c;

    if(find_class(vm, "ps/awt/refs"))
        return;

    c = make_shell(vm, "ps/awt/refs", 1);
    if(!c)
        return;

    c->fields = (ps_jfield *)calloc(1, sizeof(ps_jfield));
    if(!c->fields)
        return;
    c->fields[0].name   = "t";
    c->fields[0].desc   = "[Ljava/lang/Object;";
    c->fields[0].access = PS_ACC_STATIC;
    c->fields[0].kind   = PS_T_REF;
    c->fields[0].slot   = 0;
    c->field_count      = 1;

    c->statics = calloc(1, sizeof(ps_jslot));
    if(!c->statics)
        return;                 /* mark_roots skips a class with no statics,
                                   and ref_table below refuses to use one */
    c->static_slots = 1;
}

/* The table, growing it to at least `need` elements. NULL if it cannot be had,
 * which every caller treats as "no entry" rather than as an error - a getLayout
 * that answers null is a shape applets are written for. */
static ps_jobj *ref_table(ps_jvm *vm, int32_t need)
{
    ps_jclass *c = find_class(vm, "ps/awt/refs");
    ps_jslot  *st;
    ps_jobj   *old, *nw;
    int32_t    cap;

    if(!c || !c->statics || !c->static_slots)
        return NULL;

    st  = (ps_jslot *)c->statics;
    old = st[0].o;
    cap = old ? old->len : 0;

    /* A read against an applet that never set a layout must not be what
     * allocates the table. */
    if(!old && need <= 0)
        return NULL;
    if(old && cap >= need)
        return old;
    if(need > REF_MAX)
        return NULL;

    cap = cap ? cap * 2 : REF_INITIAL;
    while(cap < need)
        cap *= 2;
    if(cap > REF_MAX)
        cap = REF_MAX;

    /* The old array is still in the static field while this allocates, so a
     * collection triggered here cannot take it. */
    nw = ps_jvm_new_array(vm, PS_T_REF, cap);
    if(!nw || !nw->data)
        return old;

    if(old && old->data)
        memcpy(nw->data, old->data, (size_t)old->len * sizeof(ps_jobj *));

    /* The parking space points at the table itself: something non-NULL so the
     * scan below never mistakes triple zero for a free entry, and something
     * already reachable so it costs nothing to retain. */
    ((ps_jobj **)nw->data)[0] = nw;
    st[0].o = nw;
    return nw;
}

/* Where the value for (owner, key) lives, or NULL.
 *
 * With make set this appends an entry when there is not one already. Entries
 * are never removed, so the used ones are a prefix and the first NULL owner is
 * the end - which is what lets one pass do both the lookup and the append. */
static ps_jobj **ref_slot(ps_jvm *vm, ps_jobj *owner, ps_jobj *key, int make)
{
    ps_jobj  *t;
    ps_jobj **e;
    int32_t   i;

    /* A NULL owner is how a free entry is spelled, so it cannot also be a
     * thing to look up. Nothing calls with one - every caller has a receiver -
     * and if something ever does it gets "no entry" rather than the first
     * empty slot in the table. */
    if(!owner)
        return NULL;

    t = ref_table(vm, make ? REF_FIRST + REF_STRIDE : 0);

    while(t && t->data) {
        e = (ps_jobj **)t->data;

        for(i = REF_FIRST; i + REF_STRIDE <= t->len; i += REF_STRIDE) {
            if(e[i] == owner && e[i + 1] == key)
                return &e[i + 2];
            if(!e[i]) {
                if(!make)
                    return NULL;
                e[i]     = owner;
                e[i + 1] = key;
                return &e[i + 2];
            }
        }

        if(!make)
            return NULL;

        /* Full. One grow and round again; if the cap has been reached the
         * caller gets NULL, which reads back as "never set". */
        {
            ps_jobj *g = ref_table(vm, t->len * 2);

            if(!g || g == t)
                return NULL;
            t = g;
        }
    }
    return NULL;
}

/* Parks `o` where the collector will find it, across exactly one allocation.
 * Passing NULL lets it go again. */
static void ref_hold(ps_jvm *vm, ps_jobj *o)
{
    ps_jobj *t = ref_table(vm, REF_FIRST);

    if(t && t->data)
        ((ps_jobj **)t->data)[2] = o;
}

static void register_shells(ps_jvm *vm)
{
    ps_jclass *base;
    int        i;

    /* Idempotent, and cheap enough at applet-construction time: this runs once
     * per applet, not once per call. */
    if(find_class(vm, "ps/awt/shell"))
        return;

    base = make_shell(vm, "ps/awt/shell", 1);
    if(!base)
        return;

    for(i = 0; g_native_shells[i]; i++) {
        if(!find_class(vm, g_native_shells[i]))
            make_shell(vm, g_native_shells[i], 1);
    }

    make_field_shell(vm, "java/awt/Insets", base, g_insets_fields, NULL);
    make_field_shell(vm, "java/awt/GridBagConstraints", base, g_gbc_fields,
                     g_gbc_statics);
    make_ref_shell(vm);
}

/* --- per-object state ----------------------------------------------------
 *
 * Everything a shell remembers lives in one allocation hung off the object's
 * native pointer, because the heap sweep frees that pointer with a single
 * free() - a struct with its own mallocs inside it would leak every one of
 * them. Strings therefore cannot be kept as String objects either: the
 * collector has no way to see a reference held in C and would collect it out
 * from under the widget. They are copied into the tail of this block instead.
 *
 * o->len carries the block's size less one, and vm->bytes is moved by the same
 * amount, because the sweep charges `len + 1` back to the heap when it frees a
 * native block. Getting that wrong drifts vm->bytes negative and the collector
 * silently stops running.
 */
#define AWT_MAGIC 0x57415350u   /* 'PSAW' */

typedef struct {
    uint32_t magic;

    uint32_t bg, fg;            /* packed ARGB; 0 means the applet never set
                                   one, which is not the same as transparent */
    int32_t  cursor;

    int32_t  hgap, vgap;        /* every layout manager except GridBag */
    int32_t  rows, cols;        /* GridLayout, TextField, TextArea */
    int32_t  align;             /* FlowLayout, Label */

    /* The box, and zero means the applet never set one - which is also what a
     * real AWT component that has not been laid out reports, checked on the
     * host. Nothing here lays anything out, so this is only ever what
     * setBounds/setSize/setLocation were handed. */
    int32_t  bx, by, bw, bh;

    /* The font size setFont was given, in pixels, the way java.awt.Font
     * carries its own. Zero means unset. */
    int32_t  font_px;

    int32_t  value, vmin, vmax, vext, orient, unit, block;   /* Scrollbar */

    int32_t  sel;               /* Choice/List selection, -1 for none */
    int32_t  nitems;
    int32_t  children;          /* see the note on add() */

    uint8_t  checked, editable, enabled, visible;

    /* The text, NUL terminated, followed by nitems item strings each NUL
     * terminated. blob_len counts every byte in use including the
     * terminators; blob_cap is what is actually allocated, and the tail only
     * ever grows - shrinking it would mean copying the item list out of
     * memory realloc had already handed back. */
    int32_t  blob_len;
    int32_t  blob_cap;
    char     blob[1];
} ps_awt;

#define AWT_BYTES(cap) (sizeof(ps_awt) - 1 + (size_t)(cap))

/* The block, if this object has one and it is ours. */
static ps_awt *peek(ps_jobj *o)
{
    ps_awt *s = o ? (ps_awt *)o->native : NULL;

    return (s && s->magic == AWT_MAGIC) ? s : NULL;
}

static ps_awt *state(ps_jvm *vm, ps_jobj *o)
{
    ps_awt *s;

    if(!o)
        return NULL;

    /* Already attached, or attached by somebody else - a String's bytes, a
     * Thread's Runnable. Refusing the latter is the only safe answer; the
     * caller reports the method as unimplemented rather than scribbling on
     * whatever it was. */
    if(o->native)
        return peek(o);

    s = (ps_awt *)calloc(1, AWT_BYTES(1));
    if(!s)
        return NULL;

    s->magic    = AWT_MAGIC;
    s->sel      = -1;
    s->editable = 1;
    s->enabled  = 1;
    s->visible  = 1;
    s->blob_len = 1;
    s->blob_cap = 1;

    o->native      = s;
    o->owns_native = 1;
    o->len         = (int32_t)AWT_BYTES(1) - 1;
    vm->bytes     += (long)AWT_BYTES(1);
    return s;
}

/* Makes room for `want` bytes of blob. Returns NULL only if the allocation
 * failed, in which case the old block is still attached and intact. */
static ps_awt *state_grow(ps_jvm *vm, ps_jobj *o, int32_t want)
{
    ps_awt *s = peek(o), *g;

    if(!s)
        return NULL;
    if(want <= s->blob_cap)
        return s;

    g = (ps_awt *)realloc(s, AWT_BYTES(want));
    if(!g)
        return NULL;

    vm->bytes += (long)AWT_BYTES(want) - (long)AWT_BYTES(g->blob_cap);
    g->blob_cap = want;
    o->native   = g;
    o->len      = (int32_t)AWT_BYTES(want) - 1;
    return g;
}

static const char *awt_text(const ps_awt *s)
{
    return s ? s->blob : NULL;
}

/* Replaces the text, keeping the item list that follows it. */
static int awt_set_text(ps_jvm *vm, ps_jobj *o, const char *t, size_t n)
{
    ps_awt *s = state(vm, o);
    int32_t old, items;

    if(!s)
        return -1;

    old   = (int32_t)strlen(s->blob) + 1;
    items = s->blob_len - old;

    s = state_grow(vm, o, (int32_t)n + 1 + items);
    if(!s)
        return -1;

    /* One allocation holds both regions, so this overlaps whenever the text
     * changed length; memmove either way. */
    memmove(s->blob + n + 1, s->blob + old, (size_t)items);
    if(t && n)
        memcpy(s->blob, t, n);
    s->blob[n]  = '\0';
    s->blob_len = (int32_t)n + 1 + items;
    return 0;
}

static int awt_add_item(ps_jvm *vm, ps_jobj *o, const char *t, size_t n)
{
    ps_awt *s = state(vm, o);
    int32_t at;

    if(!s)
        return -1;

    at = s->blob_len;
    s  = state_grow(vm, o, at + (int32_t)n + 1);
    if(!s)
        return -1;

    if(t && n)
        memcpy(s->blob + at, t, n);
    s->blob[at + (int32_t)n] = '\0';
    s->blob_len = at + (int32_t)n + 1;
    s->nitems++;
    return 0;
}

static const char *awt_item(const ps_awt *s, int32_t idx)
{
    const char *p;

    if(!s || idx < 0 || idx >= s->nitems)
        return NULL;

    p = s->blob + strlen(s->blob) + 1;
    while(idx--)
        p += strlen(p) + 1;
    return p;
}

/* --- small object factories ---------------------------------------------- */

static ps_jobj *new_color(ps_jvm *vm, uint32_t argb)
{
    ps_jobj *c = ps_jvm_new(vm, ps_jvm_class(vm, "java/awt/Color"));

    if(c)
        c->len = (int32_t)argb;
    return c;
}

static ps_jobj *new_insets(ps_jvm *vm, int32_t t, int32_t l, int32_t b,
                           int32_t r)
{
    ps_jclass *c = ps_jvm_class(vm, "java/awt/Insets");
    ps_jobj   *o = c ? ps_jvm_new(vm, c) : NULL;

    if(o && o->fields && c->inst_slots >= 4) {
        o->fields[0].i = t;
        o->fields[1].i = l;
        o->fields[2].i = b;
        o->fields[3].i = r;
    }
    return o;
}

static ps_jobj *new_string(ps_jvm *vm, const char *s)
{
    return ps_jvm_new_string(vm, s ? s : "", s ? strlen(s) : 0);
}

/* The idx'th argument type in a descriptor, counting from zero and not
 * counting the receiver. NULL if the descriptor has no such argument. */
static const char *desc_arg(const char *d, int idx)
{
    const char *p = d + 1;
    int         i;

    for(i = 0; *p && *p != ')'; i++) {
        const char *start = p;

        while(*p == '[')
            p++;
        if(*p == 'L') {
            while(*p && *p != ';')
                p++;
            if(*p)
                p++;
        }
        else if(*p) {
            p++;
        }
        if(i == idx)
            return start;
    }
    return NULL;
}

/* An argument as UTF-8, but only where the descriptor really declares a
 * String there.
 *
 * The check is not pedantry. A slot holds an int or a reference and nothing
 * records which, so `new TextField(10)` would have its 10 read as an object
 * and dereferenced as the address 10; and a widget's own state block sits on
 * ps_jobj::native where a String's bytes sit, so handing a Button to
 * ps_jvm_string_utf8 returns that block as if it were text. The descriptor is
 * the only thing that knows.
 *
 * `at` is a slot index - args[0] is the receiver - which is the same as the
 * argument number only while nothing wider than a slot comes first. Nothing
 * here takes a long or a double before a String. */
static const char *str_arg(const char *d, ps_jslot *args, int nargs, int at,
                           size_t *len)
{
    const char *ty = desc_arg(d, at - 1);

    if(!ty || strncmp(ty, "Ljava/lang/String;", 18))
        return NULL;
    if(at >= nargs || !args[at].o)
        return NULL;
    return ps_jvm_string_utf8(args[at].o, len);
}

/* --- java.awt.CheckboxGroup ----------------------------------------------
 *
 * This was left out, and the note where it would have gone said why: a group
 * has to hold a Checkbox, a Checkbox has to hold its group, and a reference
 * held in C is one the collector cannot see. The table above is one it can, so
 * the objection is gone and the two questions go in it:
 *
 *   (group,    NULL)     the selected Checkbox
 *   (checkbox, checkbox) the group it is in
 *
 * A group is never a container and a Checkbox is never one either, so those
 * keys cannot collide with setLayout's.
 *
 * Every rule below was read off a real JDK, and two of them are not guessable.
 * setState(false) on the box a group has selected does nothing at all - a
 * radio button cannot switch itself off - and setSelectedCheckbox(null)
 * switches off the one that was on. An applet reads getState() back either
 * way, so getting them wrong is a wrong picture rather than a missing method.
 */
static ps_jobj *cb_group(ps_jvm *vm, ps_jobj *cb)
{
    ps_jobj **e = ref_slot(vm, cb, cb, 0);

    return e ? *e : NULL;
}

static ps_jobj *grp_current(ps_jvm *vm, ps_jobj *g)
{
    ps_jobj **e = ref_slot(vm, g, NULL, 0);

    return e ? *e : NULL;
}

/* Selects cb in g, which is also how a group is emptied: cb NULL clears both
 * the selection and the state of whatever was selected. */
static void grp_select(ps_jvm *vm, ps_jobj *g, ps_jobj *cb)
{
    ps_jobj **cur = ref_slot(vm, g, NULL, 1);
    ps_awt   *s;

    if(!cur)
        return;

    if(*cur && *cur != cb) {
        s = peek(*cur);
        if(s)
            s->checked = 0;
    }
    *cur = cb;

    if(cb) {
        s = state(vm, cb);
        if(s)
            s->checked = 1;
    }
}

/* The layout a container that never called setLayout would already have.
 *
 * Checked on the host: a Panel and an Applet start with a FlowLayout, a Window
 * with a BorderLayout, and a bare java.awt.Container with nothing at all. The
 * receiver's own chain decides, because the class name a call arrives under is
 * java/awt/Container for all three. NULL means null, which is a real answer
 * here rather than a missing one. */
static const char *default_layout(const ps_jobj *o)
{
    const ps_jclass *c;

    for(c = o ? o->cls : NULL; c; c = c->super) {
        if(!strcmp(c->name, "java/awt/Panel") ||
           !strcmp(c->name, "java/applet/Applet"))
            return "java/awt/FlowLayout";
        if(!strcmp(c->name, "java/awt/Window") ||
           !strcmp(c->name, "java/awt/Frame") ||
           !strcmp(c->name, "java/awt/Dialog"))
            return "java/awt/BorderLayout";
    }
    return NULL;
}

/* --- java.awt.GridBagConstraints, copied ---------------------------------
 *
 * setConstraints takes a copy and getConstraints hands back another, both
 * observed on a real JDK and both load-bearing: the shape every one of Fendt's
 * twenty takes is one GridBagConstraints filled in, handed over, and filled in
 * again for the next component. Storing the reference would leave every
 * component sharing the last set of numbers.
 *
 * The insets are copied too, for the same reason and by the same observation -
 * mutating the source's insets after setConstraints does not move the stored
 * ones.
 */
enum { GBC_SLOTS = 13, GBC_INSETS = 10 };

static void gbc_defaults(ps_jobj *o)
{
    o->fields[0].i  = -1;       /* gridx      = RELATIVE */
    o->fields[1].i  = -1;       /* gridy      = RELATIVE */
    o->fields[2].i  = 1;        /* gridwidth  */
    o->fields[3].i  = 1;        /* gridheight */
    o->fields[4].d  = 0.0;      /* weightx    */
    o->fields[6].d  = 0.0;      /* weighty    */
    o->fields[8].i  = 10;       /* anchor     = CENTER */
    o->fields[9].i  = 0;        /* fill       = NONE */
    o->fields[11].i = 0;        /* ipadx      */
    o->fields[12].i = 0;        /* ipady      */
}

/* NULL src gives a default-constructed one, which is what a real
 * getConstraints answers for a component it has never been told about. */
static ps_jobj *gbc_copy(ps_jvm *vm, const ps_jobj *src)
{
    ps_jclass *c = ps_jvm_class(vm, "java/awt/GridBagConstraints");
    ps_jobj   *o;
    int32_t    t = 0, l = 0, b = 0, r = 0;
    uint16_t   i;

    if(!c || c->inst_slots < GBC_SLOTS)
        return NULL;

    if(src && src->fields && src->cls &&
       src->cls->inst_slots >= GBC_SLOTS && src->fields[GBC_INSETS].o) {
        const ps_jobj *in = src->fields[GBC_INSETS].o;

        if(in->fields && in->cls && in->cls->inst_slots >= 4) {
            t = in->fields[0].i;
            l = in->fields[1].i;
            b = in->fields[2].i;
            r = in->fields[3].i;
        }
    }

    o = ps_jvm_new(vm, c);
    if(!o || !o->fields)
        return NULL;

    if(src && src->fields && src->cls && src->cls->inst_slots >= GBC_SLOTS) {
        for(i = 0; i < GBC_SLOTS; i++)
            o->fields[i] = src->fields[i];
    }
    else {
        gbc_defaults(o);
    }

    /* Two allocations, and the first is held nowhere the collector looks until
     * it is stored. See ref_hold. */
    ref_hold(vm, o);
    o->fields[GBC_INSETS].o = new_insets(vm, t, l, b, r);
    ref_hold(vm, NULL);
    return o;
}

/* --- the component methods everything inherits ---------------------------
 *
 * Shared by every kind that is a Component, which is why they are matched on
 * name alone here rather than per class.
 */
static int component(ps_jvm *vm, int kind, const char *n, const char *d,
                     ps_jslot *args, int nargs, ps_jslot *ret, int *handled)
{
    ps_jobj *self = nargs > 0 ? args[0].o : NULL;
    ps_awt  *s;

    *handled = 1;

    /* Colours round-trip. The default is what the component would be once the
     * browser had realised it: white behind the applet and its containers,
     * because that is what ps_jre.c's clearRect already paints, and AWT's
     * control grey behind a widget. Neither is guesswork about a value an
     * applet set - if it set one, it gets that one back. */
    if(is(n, d, "setBackground", NULL) || is(n, d, "setForeground", NULL)) {
        s = state(vm, self);
        if(!s)
            return 0;
        if(n[3] == 'B')
            s->bg = nargs > 1 && args[1].o ? COLOR_OF(args[1].o) : 0;
        else
            s->fg = nargs > 1 && args[1].o ? COLOR_OF(args[1].o) : 0;
        return 0;
    }
    if(is(n, d, "getBackground", NULL)) {
        uint32_t def = (kind == K_COMPONENT || kind == K_CANVAS ||
                        kind == K_WINDOW || kind == K_FRAME || kind == K_DIALOG)
                     ? 0xffffffffu : 0xffc0c0c0u;

        s = peek(self);
        ret->o = new_color(vm, s && s->bg ? s->bg : def);
        return ret->o ? 0 : fail(vm, "out of memory in getBackground");
    }
    if(is(n, d, "getForeground", NULL)) {
        s = peek(self);
        ret->o = new_color(vm, s && s->fg ? s->fg : 0xff000000u);
        return ret->o ? 0 : fail(vm, "out of memory in getForeground");
    }

    /* enable()/disable() are the 1.0 spellings and enable(boolean) is a third;
     * the descriptor is what separates the one that carries the answer from
     * the two that are the answer. */
    if(is(n, d, "setEnabled", NULL) || is(n, d, "enable", NULL) ||
       is(n, d, "disable", NULL)) {
        s = state(vm, self);
        if(s)
            s->enabled = (uint8_t)(!strcmp(d, "(Z)V")
                                   ? (nargs > 1 && args[1].i) : n[0] != 'd');
        return 0;
    }
    if(is(n, d, "isEnabled", NULL)) {
        s = peek(self);
        ret->i = s ? s->enabled : 1;
        return 0;
    }
    if(is(n, d, "setVisible", NULL) || is(n, d, "show", NULL) ||
       is(n, d, "hide", NULL)) {
        s = state(vm, self);
        if(s)
            s->visible = (uint8_t)(!strcmp(d, "(Z)V")
                                   ? (nargs > 1 && args[1].i) : n[0] != 'h');
        return 0;
    }
    if(is(n, d, "isVisible", NULL)) {
        s = peek(self);
        ret->i = s ? s->visible : 1;
        return 0;
    }
    /* isShowing() is what an animation loop tests to decide whether to keep
     * going. The browser only ever runs an applet it is displaying, so
     * "showing" collapses to "visible" here - and answering false would stop
     * an applet that is on screen. */
    if(is(n, d, "isShowing", NULL) || is(n, d, "isValid", NULL) ||
       is(n, d, "isDisplayable", NULL)) {
        s = peek(self);
        ret->i = (n[2] == 'S' && s) ? s->visible : 1;
        return 0;
    }

    /* --- the box ---
     *
     * The browser decides how big an *applet* is from the <applet> tag and
     * nothing an applet sets can change that, so for the applet these are
     * still accepted and dropped and ps_jgeom.c answers the reads. A widget is
     * different: it is never laid out, so the only box it can ever have is one
     * the applet handed it, and recording that is the file's round-trip rule
     * rather than an opinion about layout. Nineteen compatibility groups call
     * setBounds on something.
     *
     * The Dimension and Rectangle forms go through ps_jgeom.c rather than
     * reading slot zero here, because which slot is width is that file's to
     * say and two files answering it differently is the bug this project keeps
     * having. */
    if(is(n, d, "setSize", NULL) || is(n, d, "resize", NULL) ||
       is(n, d, "setBounds", NULL) || is(n, d, "reshape", NULL) ||
       is(n, d, "setLocation", NULL) || is(n, d, "move", NULL)) {
        int32_t x = 0, y = 0, w = 0, h = 0;

        s = state(vm, self);
        if(!s)
            return 0;

        if(!strcmp(d, "(IIII)V") && nargs > 4) {
            s->bx = args[1].i; s->by = args[2].i;
            s->bw = args[3].i; s->bh = args[4].i;
        }
        else if(!strcmp(d, "(II)V") && nargs > 2) {
            if(n[3] == 'L' || n[0] == 'm') {        /* setLocation, move */
                s->bx = args[1].i;
                s->by = args[2].i;
            }
            else {                                  /* setSize, resize */
                s->bw = args[1].i;
                s->bh = args[2].i;
            }
        }
        else if(nargs > 1 && ps_jgeom_rect_of(args[1].o, &x, &y, &w, &h)) {
            s->bx = x; s->by = y; s->bw = w; s->bh = h;
        }
        else if(nargs > 1 && ps_jgeom_dim_of(args[1].o, &w, &h)) {
            s->bw = w; s->bh = h;
        }
        return 0;
    }

    /* Read back, for a widget only. ps_jgeom.c declines these when the
     * receiver is one - see ps_jawt_is_widget - and answers them from the
     * applet's box when it is not. Zero by zero for a widget nobody sized is
     * what a real AWT component that has not been laid out reports; it was
     * checked on the host rather than assumed, because "the size it would have
     * had" is exactly the plausible wrong number this file refuses to invent. */
    if(kind_is_widget(kind) || ps_jawt_is_widget(self)) {
        s = peek(self);

        if(is(n, d, "size", "()Ljava/awt/Dimension;") ||
           is(n, d, "getSize", "()Ljava/awt/Dimension;") ||
           is(n, d, "preferredSize", "()Ljava/awt/Dimension;") ||
           is(n, d, "getPreferredSize", "()Ljava/awt/Dimension;") ||
           is(n, d, "minimumSize", "()Ljava/awt/Dimension;") ||
           is(n, d, "getMinimumSize", "()Ljava/awt/Dimension;")) {
            ret->o = ps_jgeom_dimension(vm, s ? s->bw : 0, s ? s->bh : 0);
            return ret->o ? 0 : fail(vm, "out of memory in %s", n);
        }
        if(is(n, d, "bounds", "()Ljava/awt/Rectangle;") ||
           is(n, d, "getBounds", "()Ljava/awt/Rectangle;")) {
            ret->o = ps_jgeom_rect(vm, s ? s->bx : 0, s ? s->by : 0,
                                   s ? s->bw : 0, s ? s->bh : 0);
            return ret->o ? 0 : fail(vm, "out of memory in %s", n);
        }
        if(is(n, d, "location", "()Ljava/awt/Point;") ||
           is(n, d, "getLocation", "()Ljava/awt/Point;")) {
            ret->o = ps_jgeom_point(vm, s ? s->bx : 0, s ? s->by : 0);
            return ret->o ? 0 : fail(vm, "out of memory in %s", n);
        }

        /* setFont on a widget is the widget's own font and must not touch the
         * one the applet paints with. Twenty-two groups set a font on
         * something through java/awt/Component.setFont, and three of them are
         * stopped by it today.
         *
         * getFont with none set answers the applet's, which is what a real
         * component reports once it has a parent - a component with no parent
         * answers null, checked on the host, and every widget here is one the
         * applet made and added. Handing back that null would put an NPE in
         * g.setFont(tf.getFont()) for no gain. */
        if(is(n, d, "setFont", "(Ljava/awt/Font;)V")) {
            s = state(vm, self);
            if(s)
                s->font_px = nargs > 1 && args[1].o ? args[1].o->len : 0;
            return 0;
        }
        if(is(n, d, "getFont", "()Ljava/awt/Font;")) {
            ps_jobj *f = ps_jvm_new(vm, ps_jvm_class(vm, "java/awt/Font"));

            if(!f)
                return fail(vm, "out of memory in getFont");
            f->len = (s && s->font_px > 0) ? s->font_px
                   : (vm->gfx ? vm->gfx->font_px : 12);
            ret->o = f;
            return 0;
        }
    }

    /* Accepted and dropped: none of these has anything to move. */
    if(is(n, d, "requestFocus", NULL) ||
       is(n, d, "transferFocus", NULL) || is(n, d, "addNotify", NULL) ||
       is(n, d, "removeNotify", NULL) || is(n, d, "invalidate", NULL) ||
       is(n, d, "validate", NULL) || is(n, d, "doLayout", NULL) ||
       is(n, d, "layout", "()V") ||
       is(n, d, "setCursor", NULL) || is(n, d, "pack", NULL) ||
       is(n, d, "toFront", NULL) || is(n, d, "toBack", NULL) ||
       is(n, d, "dispose", NULL) || is(n, d, "setResizable", NULL) ||
       is(n, d, "setModal", NULL) || is(n, d, "setMenuBar", NULL)) {
        /* setCursor is the one with state worth keeping, since getCursor can
         * be asked for it back. */
        if(!strcmp(n, "setCursor")) {
            s = state(vm, self);
            if(s)
                s->cursor = nargs > 1 && args[1].o ? args[1].o->len : 0;
        }
        return 0;
    }

    if(is(n, d, "getCursor", NULL)) {
        ps_jobj *c = ps_jvm_new(vm, ps_jvm_class(vm, "java/awt/Cursor"));

        s = peek(self);
        if(c)
            c->len = s ? s->cursor : 0;
        ret->o = c;
        return c ? 0 : fail(vm, "out of memory in getCursor");
    }

    /* repaint() on anything but the applet still means "the browser should
     * paint again", because the applet's paint() is the only one that ever
     * runs. ps_jre.c answers this for Applet and Component; a Canvas subclass
     * calling it arrives here instead. */
    if(is(n, d, "repaint", NULL)) {
        vm->repaint = 1;
        return 0;
    }

    /* Nothing is ever really added to anything - see add() - so every
     * component here is genuinely parentless and null is the truth rather than
     * a placeholder. Applets walk getParent() up to find a Frame to hang a
     * dialog off; stopping immediately is the right outcome, since there is no
     * Frame either. */
    if(is(n, d, "getParent", NULL)) {
        ret->o = NULL;
        return 0;
    }
    if(is(n, d, "getToolkit", NULL)) {
        ret->o = ps_jvm_new(vm, ps_jvm_class(vm, "java/awt/Toolkit"));
        return ret->o ? 0 : fail(vm, "out of memory in getToolkit");
    }
    /* An applet's own insets really are zero - the browser gives it the box
     * from the <applet> tag and no border - and that is what an applet asks
     * for, to subtract them from its drawing area. A real Frame's would not be
     * zero, but a Frame here has no title bar to be inset by. */
    if(is(n, d, "getInsets", NULL) || is(n, d, "insets", NULL)) {
        ret->o = new_insets(vm, 0, 0, 0, 0);
        return ret->o ? 0 : fail(vm, "out of memory in getInsets");
    }

    /* --- listener registration ---
     *
     * The 1.1 event model bought nothing at all in the last measurement and
     * could not, because the machinery had no way in: it is answered on
     * Component in ps_jre.c and a Button never reaches there. Seventeen groups
     * call Button.addActionListener and twelve call TextField's, and between
     * them they are the first thing Fendt's twenty hit once the layout calls
     * work.
     *
     * The table itself is ps_jvm.c's and this only calls it - the same call
     * ps_jre.c makes for the applet. Two things about that are worth stating
     * here rather than discovering on hardware:
     *
     *  - The table has four kinds and Item, Adjustment and Text are not among
     *    them. Those are accepted and dropped. Nothing in this runtime can
     *    generate the events they carry, because no widget receives input, so
     *    the alternative is refusing a call whose whole effect would have been
     *    to register for something that never happens.
     *  - new_events_only is one latch for the whole VM, so registering an
     *    ActionListener on a Button also retires the *applet's* 1.0 mouseDown.
     *    A real AWT latches it per component. That is ps_jvm.c's to fix and
     *    this file cannot; what it costs today is an applet that used to die
     *    outright now running without its 1.0 clicks, which is the better of
     *    the two. Nothing in the corpus is in that position.
     *
     * This sits below the accept-and-drop block so that removeNotify is not
     * read as removing a "Notify" listener, and above the containment guard so
     * that a widget gets here at all. */
    {
        static const struct { const char *suffix; int kind; } kinds[] = {
            { "MouseListener",       PS_LSN_MOUSE  },
            { "MouseMotionListener", PS_LSN_MOTION },
            { "KeyListener",         PS_LSN_KEY    },
            { "ActionListener",      PS_LSN_ACTION },
            /* Kept nowhere, and named so that the call is answered rather than
             * reported missing. */
            { "ItemListener",        -1 },
            { "AdjustmentListener",  -1 },
            { "TextListener",        -1 },
            { "FocusListener",       -1 },
            { "ComponentListener",   -1 },
            { "ContainerListener",   -1 },
            { "WindowListener",      -1 },
            { NULL, 0 }
        };
        int adding = !strncmp(n, "add", 3);
        int i;

        if(adding || !strncmp(n, "remove", 6)) {
            const char *tail = n + (adding ? 3 : 6);

            for(i = 0; kinds[i].suffix; i++) {
                if(strcmp(tail, kinds[i].suffix))
                    continue;
                if(kinds[i].kind < 0)
                    return 0;
                if(adding)
                    ps_jvm_add_listener(vm, self,
                                        nargs > 1 ? args[1].o : NULL,
                                        kinds[i].kind);
                else
                    ps_jvm_remove_listener(vm, self,
                                           nargs > 1 ? args[1].o : NULL,
                                           kinds[i].kind);
                return 0;
            }
        }
    }

    /* --- containment ---
     *
     * Containers only. A Choice also has add() and remove(), and they mean
     * something entirely different - counting an item as a child and reporting
     * the wrong getItemCount afterwards is exactly the silent wrong answer
     * this file is not allowed to give. Choice and List handle their own in
     * widget(), which runs first.
     *
     * add() counts and returns its argument, and that is all it does. It could
     * now keep the child - the table above is a place the collector looks -
     * and it deliberately does not. Every entry in that table is permanent by
     * construction, a control panel adds twenty or thirty components, and
     * nothing in the sixty-nine-applet compatibility corpus calls
     * getComponent or getComponents at all. Paying a permanent entry per added
     * widget to answer a question nobody asks is the wrong trade; the count is
     * real - an applet that adds three things and asks gets three - and
     * getComponent(int) stays absent and loud.
     *
     * The day a child component's paint() is actually called, this is where
     * the tree goes, and the storage is now the easy half. The hard half is in
     * ps_jvm.c: ps_jvm_run_applet fails an applet that has no paint(Graphics)
     * of its own, and ps_jvm_paint only ever calls that one method. */
    if(kind != K_COMPONENT && kind != K_WINDOW && kind != K_FRAME &&
       kind != K_DIALOG) {
        *handled = 0;
        return 0;
    }

    /* --- the layout manager ---
     *
     * setLayout used to be accepted and dropped, and getLayout was in neither
     * this file nor ps_jgeom.c: thirteen applets across two authors stop on
     * it, the largest single blocker measured. It is not a layout feature.
     * Fendt's whole corpus is written as
     *
     *     ((GridBagLayout)getLayout()).setConstraints(c, gbc);
     *
     * so what it wants is the object back, not a laid-out panel.
     *
     * Holding it is the part that needed solving rather than writing - see the
     * note above ref_table. The collector can see this one.
     *
     * With none set the answer is the default the class would have had, made
     * once and kept so that getLayout() == getLayout(): a Panel and an Applet
     * are FlowLayout(5,5,CENTER), a Window is a BorderLayout and a bare
     * Container is genuinely null. All four checked against a real JDK. */
    if(is(n, d, "setLayout", NULL)) {
        ps_jobj **slot = ref_slot(vm, self, NULL, 1);

        if(slot)
            *slot = nargs > 1 ? args[1].o : NULL;
        return 0;
    }
    if(is(n, d, "getLayout", NULL)) {
        ps_jobj   **slot = ref_slot(vm, self, NULL, 0);
        const char *def;

        if(slot && *slot) {
            ret->o = *slot;
            return 0;
        }

        def = default_layout(self);
        if(!def) {
            ret->o = NULL;      /* a bare Container, which really has none */
            return 0;
        }

        /* Reserve the entry first: the allocation below may collect, and an
         * entry with a NULL value is exactly the state this branch handles. */
        slot = ref_slot(vm, self, NULL, 1);
        ret->o = ps_jvm_new(vm, ps_jvm_class(vm, def));
        if(!ret->o)
            return fail(vm, "out of memory in getLayout");
        if(slot)
            *slot = ret->o;

        /* The gaps a real one would have been constructed with. layout()'s
         * <init> is not run for an object this file made, so they go in here. */
        s = state(vm, ret->o);
        if(s && !strcmp(def, "java/awt/FlowLayout")) {
            s->align = 1;
            s->hgap  = 5;
            s->vgap  = 5;
        }
        return 0;
    }

    if(is(n, d, "add", NULL)) {
        s = state(vm, self);
        if(s)
            s->children++;

        /* add(Component) and add(String, Component) return the Component;
         * add(Component, Object) returns void. The descriptor is the only way
         * to tell which argument is which. */
        if(!strncmp(d, "(Ljava/lang/String;", 19))
            ret->o = nargs > 2 ? args[2].o : NULL;
        else
            ret->o = nargs > 1 ? args[1].o : NULL;
        return 0;
    }
    if(is(n, d, "remove", NULL)) {
        s = state(vm, self);
        if(s && s->children > 0)
            s->children--;
        return 0;
    }
    if(is(n, d, "removeAll", NULL)) {
        s = state(vm, self);
        if(s)
            s->children = 0;
        return 0;
    }
    if(is(n, d, "getComponentCount", NULL) || is(n, d, "countComponents", NULL)) {
        s = peek(self);
        ret->i = s ? s->children : 0;
        return 0;
    }

    *handled = 0;
    return 0;
}

/* --- the widgets --------------------------------------------------------- */

static int widget(ps_jvm *vm, int kind, const char *n, const char *d,
                  ps_jslot *args, int nargs, ps_jslot *ret, int *handled)
{
    ps_jobj    *self = nargs > 0 ? args[0].o : NULL;
    ps_awt     *s;
    size_t      len  = 0;
    const char *t;

    *handled = 1;

    if(is(n, d, "<init>", NULL)) {
        s = state(vm, self);
        if(!s)
            return 0;

        switch(kind) {
        case K_BUTTON:
        case K_LABEL:
        case K_CHECKBOX:
            /* All three take the label first when they take anything. */
            t = str_arg(d, args, nargs, 1, &len);
            if(t && awt_set_text(vm, self, t, len) != 0)
                return fail(vm, "out of memory constructing a widget");
            s = (ps_awt *)self->native;

            /* Label(String, int) is the only alignment-carrying form; a
             * Checkbox's second int argument is its state, and its group can
             * be in either of two positions depending on the overload. */
            if(kind == K_LABEL && !strcmp(d, "(Ljava/lang/String;I)V"))
                s->align = nargs > 2 ? args[2].i : 0;
            if(kind == K_CHECKBOX) {
                ps_jobj *grp = NULL;

                if(!strcmp(d, "(Ljava/lang/String;Z)V"))
                    s->checked = (uint8_t)(nargs > 2 && args[2].i);
                else if(!strcmp(d, "(Ljava/lang/String;ZLjava/awt/CheckboxGroup;)V")) {
                    s->checked = (uint8_t)(nargs > 2 && args[2].i);
                    grp = nargs > 3 ? args[3].o : NULL;
                }
                else if(!strcmp(d, "(Ljava/lang/String;Ljava/awt/CheckboxGroup;Z)V")) {
                    s->checked = (uint8_t)(nargs > 3 && args[3].i);
                    grp = nargs > 2 ? args[2].o : NULL;
                }

                /* Joining a group checked selects it and switches off whoever
                 * was selected before - so two members both constructed true
                 * leaves only the later one on, which is what a real one does
                 * and is visible to any applet that reads getState back. */
                if(grp) {
                    ps_jobj **e = ref_slot(vm, self, self, 1);
                    uint8_t   on;

                    if(e)
                        *e = grp;
                    s  = (ps_awt *)self->native;
                    on = s->checked;
                    if(on)
                        grp_select(vm, grp, self);
                }
            }
            return 0;

        case K_TEXTFIELD:
        case K_TEXTAREA:
            /* The text is the first argument when there is one, and the row
             * and column counts follow it. TextField(String) reports as many
             * columns as the string is long, which is the real constructor's
             * doing and the kind of thing an applet reads back. */
            t = str_arg(d, args, nargs, 1, &len);
            if(t && awt_set_text(vm, self, t, len) != 0)
                return fail(vm, "out of memory constructing a text widget");
            s = (ps_awt *)self->native;

            if(!strcmp(d, "(I)V"))
                s->cols = nargs > 1 ? args[1].i : 0;
            else if(!strcmp(d, "(II)V")) {
                s->rows = nargs > 1 ? args[1].i : 0;
                s->cols = nargs > 2 ? args[2].i : 0;
            }
            else if(!strcmp(d, "(Ljava/lang/String;I)V"))
                s->cols = nargs > 2 ? args[2].i : 0;
            else if(!strncmp(d, "(Ljava/lang/String;II", 21)) {
                s->rows = nargs > 2 ? args[2].i : 0;
                s->cols = nargs > 3 ? args[3].i : 0;
            }
            else if(t)
                s->cols = (int32_t)len;
            return 0;

        case K_SCROLLBAR:
            /* new Scrollbar() is a vertical bar over 0..100 showing 10 of it,
             * which is not a default anybody would guess and is exactly what
             * an applet's first getMaximum() reads. */
            s->orient = nargs > 1 && strcmp(d, "()V") ? args[1].i : 1;
            s->vmin   = 0;
            s->vmax   = 100;
            s->vext   = 10;
            s->unit   = 1;
            s->block  = 10;
            if(!strcmp(d, "(IIIII)V") && nargs > 5) {
                s->vext = args[3].i;
                s->vmin = args[4].i;
                s->vmax = args[5].i;
                s->value = args[2].i;
            }
            return 0;

        case K_FRAME:
        case K_DIALOG:
            /* The title, which getTitle reads back. It is the first argument
             * of Frame(String) and the second of Dialog(Frame, String,
             * boolean); the other overloads of both have none. */
            t = str_arg(d, args, nargs, kind == K_DIALOG ? 2 : 1, &len);
            if(t && awt_set_text(vm, self, t, len) != 0)
                return fail(vm, "out of memory constructing a window");
            return 0;

        default:
            return 0;           /* Canvas, Window, Choice, List,
                                   CheckboxGroup: no constructor arguments
                                   worth keeping */
        }
    }

    s = state(vm, self);
    if(!s) {
        *handled = 0;
        return 0;
    }

    /* --- text, under all four of its names ---
     *
     * Button and Checkbox spell it label, Label and the text widgets spell it
     * text, and Frame spells it title. One string underneath. */
    if(is(n, d, "getText", NULL) || is(n, d, "getLabel", NULL) ||
       is(n, d, "getTitle", NULL)) {
        ret->o = new_string(vm, awt_text(s));
        return ret->o ? 0 : fail(vm, "out of memory in %s", n);
    }
    if(is(n, d, "setText", NULL) || is(n, d, "setLabel", NULL) ||
       is(n, d, "setTitle", NULL)) {
        t = str_arg(d, args, nargs, 1, &len);
        if(awt_set_text(vm, self, t, t ? len : 0) != 0)
            return fail(vm, "out of memory in %s", n);
        return 0;
    }
    if((kind == K_TEXTAREA) &&
       (is(n, d, "append", NULL) || is(n, d, "appendText", NULL))) {
        char  *joined;
        size_t have = strlen(s->blob);

        t = str_arg(d, args, nargs, 1, &len);
        if(!t)
            return 0;

        joined = (char *)malloc(have + len + 1);
        if(!joined)
            return fail(vm, "out of memory in append");
        memcpy(joined, s->blob, have);
        memcpy(joined + have, t, len);
        joined[have + len] = '\0';
        if(awt_set_text(vm, self, joined, have + len) != 0) {
            free(joined);
            return fail(vm, "out of memory in append");
        }
        free(joined);
        return 0;
    }

    if(is(n, d, "setEditable", NULL)) {
        s->editable = (uint8_t)(nargs > 1 && args[1].i);
        return 0;
    }
    if(is(n, d, "isEditable", NULL)) {
        ret->i = s->editable;
        return 0;
    }
    if(is(n, d, "getRows", NULL))    { ret->i = s->rows;  return 0; }
    if(is(n, d, "getColumns", NULL)) { ret->i = s->cols;  return 0; }
    if(is(n, d, "setRows", NULL))    { s->rows = nargs > 1 ? args[1].i : 0; return 0; }
    if(is(n, d, "setColumns", NULL)) { s->cols = nargs > 1 ? args[1].i : 0; return 0; }

    /* select/selectAll/setEchoChar move a caret and a highlight that do not
     * exist, and nothing can read either back. On a Choice or a List, select
     * means something else entirely - see below. */
    if(is(n, d, "selectAll", NULL) || is(n, d, "setEchoChar", NULL) ||
       is(n, d, "setEchoCharacter", NULL) ||
       (kind != K_CHOICE && kind != K_LIST && is(n, d, "select", NULL)))
        return 0;

    if(kind == K_CHECKBOX) {
        if(is(n, d, "getState", NULL)) { ret->i = s->checked; return 0; }
        if(is(n, d, "setState", NULL)) {
            ps_jobj *grp = cb_group(vm, self);
            int      on  = nargs > 1 && args[1].i;

            if(!grp) {
                s->checked = (uint8_t)on;
                return 0;
            }
            if(on) {
                grp_select(vm, grp, self);
                return 0;
            }
            /* Switching off the one that is on does nothing: a radio button
             * cannot deselect itself, and an applet that reads getState back
             * still sees it set. Observed on a real JDK. */
            if(grp_current(vm, grp) != self)
                s->checked = 0;
            return 0;
        }
        if(is(n, d, "getCheckboxGroup", NULL)) {
            ret->o = cb_group(vm, self);
            return 0;
        }
        if(is(n, d, "setCheckboxGroup", NULL)) {
            ps_jobj  *was = cb_group(vm, self);
            ps_jobj  *grp = nargs > 1 ? args[1].o : NULL;
            ps_jobj **e;

            /* Leaving a group it was the selection of empties that group and
             * leaves this box set; joining one that already has a selection
             * switches this box off; joining an empty one while set selects
             * it. All three checked on the host - none of them follow from the
             * other two. */
            if(was && was != grp) {
                e = ref_slot(vm, was, NULL, 0);
                if(e && *e == self)
                    *e = NULL;      /* emptied, but the box stays set */
            }

            e = ref_slot(vm, self, self, 1);
            if(e)
                *e = grp;

            if(grp && s->checked) {
                if(grp_current(vm, grp))
                    s->checked = 0;
                else
                    grp_select(vm, grp, self);
            }
            return 0;
        }
    }

    if(kind == K_CBGROUP) {
        if(is(n, d, "getSelectedCheckbox", NULL) ||
           is(n, d, "getCurrent", NULL)) {
            ret->o = grp_current(vm, self);
            return 0;
        }
        if(is(n, d, "setSelectedCheckbox", NULL) ||
           is(n, d, "setCurrent", NULL)) {
            grp_select(vm, self, nargs > 1 ? args[1].o : NULL);
            return 0;
        }
    }

    if(kind == K_LABEL) {
        if(is(n, d, "getAlignment", NULL)) { ret->i = s->align; return 0; }
        if(is(n, d, "setAlignment", NULL)) {
            s->align = nargs > 1 ? args[1].i : 0;
            return 0;
        }
    }

    /* --- Choice and List ---
     *
     * The same item list with two different selection rules, both checked
     * against a real JDK: a Choice selects its first item as it is added and
     * can never have nothing selected, a List starts with nothing selected and
     * stays that way until the applet selects something. Since no event ever
     * reaches a widget here, the selection only ever changes because the
     * applet changed it. */
    if(kind == K_CHOICE || kind == K_LIST) {
        if(is(n, d, "addItem", NULL) || is(n, d, "add", NULL)) {
            t = str_arg(d, args, nargs, 1, &len);
            if(awt_add_item(vm, self, t, t ? len : 0) != 0)
                return fail(vm, "out of memory adding an item");
            s = (ps_awt *)self->native;
            if(kind == K_CHOICE && s->sel < 0)
                s->sel = 0;
            return 0;
        }
        if(is(n, d, "getItemCount", NULL) || is(n, d, "countItems", NULL)) {
            ret->i = s->nitems;
            return 0;
        }
        if(is(n, d, "getItem", NULL)) {
            const char *it = awt_item(s, nargs > 1 ? args[1].i : -1);

            /* An index off the end is a throw in the real API and has to stay
             * one: an applet that walks past the end and gets "" back would
             * loop forever rather than stop. */
            if(!it)
                return ps_jvm_throw(vm,
                                    "java/lang/ArrayIndexOutOfBoundsException",
                                    "getItem");
            ret->o = new_string(vm, it);
            return ret->o ? 0 : fail(vm, "out of memory in getItem");
        }
        if(is(n, d, "getSelectedIndex", NULL)) {
            ret->i = s->sel;
            return 0;
        }
        if(is(n, d, "getSelectedItem", NULL)) {
            const char *it = awt_item(s, s->sel);

            /* null, not "", when nothing is selected - an applet branches on
             * it and an empty string sends it the wrong way. */
            ret->o = it ? new_string(vm, it) : NULL;
            return (it && !ret->o) ? fail(vm, "out of memory") : 0;
        }
        if(is(n, d, "select", "(I)V")) {
            int32_t i = nargs > 1 ? args[1].i : -1;

            if(i >= 0 && i < s->nitems)
                s->sel = i;
            return 0;
        }
        if(is(n, d, "select", "(Ljava/lang/String;)V")) {
            int32_t i;

            t = str_arg(d, args, nargs, 1, &len);
            for(i = 0; t && i < s->nitems; i++) {
                const char *it = awt_item(s, i);

                if(it && !strcmp(it, t)) {
                    s->sel = i;
                    break;
                }
            }
            return 0;
        }
        /* Dropping every item is truncating the blob back to the text; the
         * allocation stays, which costs nothing worth reclaiming.
         *
         * Dropping *one* is not here. remove(int), remove(String) and delItem
         * would have to splice the blob and then decide what happens to the
         * selection - a Choice re-selects, a List does not - and getting that
         * subtly wrong would show up as the applet drawing the wrong item's
         * name. Absent and loud beats present and nearly right. */
        if(is(n, d, "removeAll", NULL) || is(n, d, "clear", NULL)) {
            s->blob_len = (int32_t)strlen(s->blob) + 1;
            s->nitems   = 0;
            s->sel      = -1;
            return 0;
        }
    }

    if(kind == K_SCROLLBAR) {
        if(is(n, d, "getValue", NULL))         { ret->i = s->value;  return 0; }
        if(is(n, d, "getMinimum", NULL))       { ret->i = s->vmin;   return 0; }
        if(is(n, d, "getMaximum", NULL))       { ret->i = s->vmax;   return 0; }
        if(is(n, d, "getVisibleAmount", NULL) ||
           is(n, d, "getVisible", NULL))       { ret->i = s->vext;   return 0; }
        if(is(n, d, "getOrientation", NULL))   { ret->i = s->orient; return 0; }
        if(is(n, d, "getUnitIncrement", NULL) ||
           is(n, d, "getLineIncrement", NULL)) { ret->i = s->unit;   return 0; }
        if(is(n, d, "getBlockIncrement", NULL) ||
           is(n, d, "getPageIncrement", NULL)) { ret->i = s->block;  return 0; }

        if(is(n, d, "setValue", NULL)) {
            /* Clamped to minimum..maximum-visibleAmount, which is what a real
             * Scrollbar does and is visible to any applet that sets a value
             * near the end and reads it back. */
            int32_t v  = nargs > 1 ? args[1].i : 0;
            int32_t hi = s->vmax - s->vext;

            if(v > hi)      v = hi;
            if(v < s->vmin) v = s->vmin;
            s->value = v;
            return 0;
        }
        if(is(n, d, "setMinimum", NULL)) { s->vmin = nargs > 1 ? args[1].i : 0; return 0; }
        if(is(n, d, "setMaximum", NULL)) { s->vmax = nargs > 1 ? args[1].i : 0; return 0; }
        if(is(n, d, "setVisibleAmount", NULL)) { s->vext = nargs > 1 ? args[1].i : 0; return 0; }
        if(is(n, d, "setOrientation", NULL)) { s->orient = nargs > 1 ? args[1].i : 0; return 0; }
        if(is(n, d, "setUnitIncrement", NULL) ||
           is(n, d, "setLineIncrement", NULL)) { s->unit = nargs > 1 ? args[1].i : 0; return 0; }
        if(is(n, d, "setBlockIncrement", NULL) ||
           is(n, d, "setPageIncrement", NULL)) { s->block = nargs > 1 ? args[1].i : 0; return 0; }
        if(is(n, d, "setValues", NULL) && nargs > 4) {
            s->value = args[1].i;
            s->vext  = args[2].i;
            s->vmin  = args[3].i;
            s->vmax  = args[4].i;
            return 0;
        }
    }

    *handled = 0;
    return 0;
}

/* --- the layout managers -------------------------------------------------
 *
 * A layout manager here is a bag of the numbers its constructor was given. It
 * is never asked to lay anything out, because nothing it could lay out is ever
 * drawn; addLayoutComponent and layoutContainer are accepted and dropped so
 * that an applet with a hand-written LayoutManager still gets through init().
 */
static int layout(ps_jvm *vm, int kind, const char *n, const char *d,
                  ps_jslot *args, int nargs, ps_jslot *ret, int *handled)
{
    ps_jobj *self = nargs > 0 ? args[0].o : NULL;
    ps_awt  *s;

    *handled = 1;

    if(is(n, d, "<init>", NULL)) {
        s = state(vm, self);
        if(!s)
            return 0;

        switch(kind) {
        case K_FLOW:
            /* FlowLayout(), FlowLayout(align), FlowLayout(align, h, v). The
             * no-argument form is centred with five-pixel gaps. */
            s->align = nargs > 1 ? args[1].i : 1;
            s->hgap  = 5;
            s->vgap  = 5;
            if(nargs > 3) {
                s->hgap = args[2].i;
                s->vgap = args[3].i;
            }
            break;
        case K_GRID:
            /* GridLayout() is one row and as many columns as it needs. */
            s->rows = nargs > 1 ? args[1].i : 1;
            s->cols = nargs > 2 ? args[2].i : 0;
            if(nargs > 4) {
                s->hgap = args[3].i;
                s->vgap = args[4].i;
            }
            break;
        case K_BORDER:
        case K_CARD:
            if(nargs > 2) {
                s->hgap = args[1].i;
                s->vgap = args[2].i;
            }
            break;
        default:
            break;
        }
        return 0;
    }

    s = state(vm, self);
    if(!s) {
        *handled = 0;
        return 0;
    }

    if(is(n, d, "getHgap", NULL)) { ret->i = s->hgap;  return 0; }
    if(is(n, d, "getVgap", NULL)) { ret->i = s->vgap;  return 0; }
    if(is(n, d, "setHgap", NULL)) { s->hgap = nargs > 1 ? args[1].i : 0; return 0; }
    if(is(n, d, "setVgap", NULL)) { s->vgap = nargs > 1 ? args[1].i : 0; return 0; }

    if(kind == K_FLOW) {
        if(is(n, d, "getAlignment", NULL)) { ret->i = s->align; return 0; }
        if(is(n, d, "setAlignment", NULL)) {
            s->align = nargs > 1 ? args[1].i : 0;
            return 0;
        }
    }
    if(kind == K_GRID) {
        if(is(n, d, "getRows", NULL))    { ret->i = s->rows; return 0; }
        if(is(n, d, "getColumns", NULL)) { ret->i = s->cols; return 0; }
        if(is(n, d, "setRows", NULL))    { s->rows = nargs > 1 ? args[1].i : 0; return 0; }
        if(is(n, d, "setColumns", NULL)) { s->cols = nargs > 1 ? args[1].i : 0; return 0; }
    }

    /* GridBagLayout's constraints, which is the one piece of a layout
     * manager's bookkeeping that has to be real: twenty-four groups across two
     * authors call setConstraints, and it is the next thing Fendt's twenty hit
     * once getLayout answers. It never lays anything out here - what it does
     * is remember, and hand back what it was told, which is the rule the whole
     * of this file is written to.
     *
     * Both directions copy. See gbc_copy. */
    if(kind == K_GRIDBAG && is(n, d, "setConstraints", NULL)) {
        ps_jobj **slot = ref_slot(vm, self, nargs > 1 ? args[1].o : NULL, 1);
        ps_jobj  *cp;

        if(!slot)
            return 0;           /* table at its cap; getConstraints then
                                   answers the defaults, which is what a
                                   component nobody constrained gets anyway */
        cp = gbc_copy(vm, nargs > 2 ? args[2].o : NULL);
        if(!cp)
            return fail(vm, "out of memory in setConstraints");
        *slot = cp;
        return 0;
    }
    if(kind == K_GRIDBAG && is(n, d, "getConstraints", NULL)) {
        ps_jobj **slot = ref_slot(vm, self, nargs > 1 ? args[1].o : NULL, 0);

        ret->o = gbc_copy(vm, slot ? *slot : NULL);
        return ret->o ? 0 : fail(vm, "out of memory in getConstraints");
    }

    /* The LayoutManager interface itself, plus CardLayout's navigation. All of
     * it moves components this runtime does not draw. */
    if(is(n, d, "addLayoutComponent", NULL) ||
       is(n, d, "removeLayoutComponent", NULL) ||
       is(n, d, "layoutContainer", NULL) ||
       is(n, d, "invalidateLayout", NULL) ||
       is(n, d, "setConstraints", NULL) ||
       is(n, d, "show", NULL) || is(n, d, "next", NULL) ||
       is(n, d, "previous", NULL) || is(n, d, "first", NULL) ||
       is(n, d, "last", NULL))
        return 0;

    *handled = 0;
    return 0;
}

/* --- the static fields ---------------------------------------------------
 *
 * Reached through getstatic, which hands this file a field name and descriptor
 * and no receiver at all. GridBagConstraints is missing here on purpose: it is
 * not a native class, so its constants live in real static storage filled in
 * at registration.
 */
static int statics(ps_jvm *vm, int kind, const char *n, ps_jslot *ret,
                   int *handled)
{
    static const struct { const char *name; const char *value; } border[] = {
        { "NORTH", "North" }, { "SOUTH", "South" }, { "EAST", "East" },
        { "WEST",  "West"  }, { "CENTER", "Center" },
        /* The 1.4 spellings, because a modern javac resolves them and they
         * are the same five strings underneath. */
        { "PAGE_START", "First" }, { "PAGE_END", "Last" },
        { "LINE_START", "Before" }, { "LINE_END", "After" },
        { "BEFORE_FIRST_LINE", "First" }, { "AFTER_LAST_LINE", "Last" },
        { "BEFORE_LINE_BEGINS", "Before" }, { "AFTER_LINE_ENDS", "After" },
        { NULL, NULL }
    };
    static const struct { const char *name; int32_t v; } ints[] = {
        /* FlowLayout and Label share LEFT/CENTER/RIGHT at 0/1/2, and
         * Scrollbar's HORIZONTAL/VERTICAL are 0/1. */
        { "LEFT", 0 }, { "CENTER", 1 }, { "RIGHT", 2 },
        { "LEADING", 3 }, { "TRAILING", 4 },
        { "HORIZONTAL", 0 }, { "VERTICAL", 1 },
        { NULL, 0 }
    };
    static const struct { const char *name; int32_t v; } cursors[] = {
        { "DEFAULT_CURSOR",    0 }, { "CROSSHAIR_CURSOR",  1 },
        { "TEXT_CURSOR",       2 }, { "WAIT_CURSOR",       3 },
        { "SW_RESIZE_CURSOR",  4 }, { "SE_RESIZE_CURSOR",  5 },
        { "NW_RESIZE_CURSOR",  6 }, { "NE_RESIZE_CURSOR",  7 },
        { "N_RESIZE_CURSOR",   8 }, { "S_RESIZE_CURSOR",   9 },
        { "W_RESIZE_CURSOR",  10 }, { "E_RESIZE_CURSOR",  11 },
        { "HAND_CURSOR",      12 }, { "MOVE_CURSOR",      13 },
        { NULL, 0 }
    };
    static const struct { const char *name; int32_t v; } scrollbars[] = {
        { "SCROLLBARS_BOTH",            0 },
        { "SCROLLBARS_VERTICAL_ONLY",   1 },
        { "SCROLLBARS_HORIZONTAL_ONLY", 2 },
        { "SCROLLBARS_NONE",            3 },
        { NULL, 0 }
    };
    int i;

    *handled = 1;

    if(kind == K_BORDER) {
        for(i = 0; border[i].name; i++) {
            if(!strcmp(border[i].name, n)) {
                ret->o = new_string(vm, border[i].value);
                return ret->o ? 0 : fail(vm, "out of memory");
            }
        }
    }
    if(kind == K_CURSOR) {
        for(i = 0; cursors[i].name; i++) {
            if(!strcmp(cursors[i].name, n)) {
                ret->i = cursors[i].v;
                return 0;
            }
        }
    }
    if(kind == K_TEXTAREA) {
        for(i = 0; scrollbars[i].name; i++) {
            if(!strcmp(scrollbars[i].name, n)) {
                ret->i = scrollbars[i].v;
                return 0;
            }
        }
    }
    if(kind == K_FLOW || kind == K_LABEL || kind == K_SCROLLBAR) {
        for(i = 0; ints[i].name; i++) {
            if(!strcmp(ints[i].name, n)) {
                ret->i = ints[i].v;
                return 0;
            }
        }
    }

    *handled = 0;
    return 0;
}

/* --- Insets and GridBagConstraints ---------------------------------------
 *
 * These arrive as ps/awt/shell because they are the two classes here that are
 * not native - see make_field_shell. The receiver's own class says which is
 * which. All they need is a constructor that fills the field table in; every
 * read and write after that is the interpreter's ordinary getfield and
 * putfield against real storage, so they round-trip without this file being
 * involved at all.
 */
static int field_shell(ps_jvm *vm, const char *n, const char *d,
                       ps_jslot *args, int nargs, int *handled)
{
    ps_jobj *self = nargs > 0 ? args[0].o : NULL;

    *handled = 0;

    if(!self || !self->cls || !self->fields || !is(n, d, "<init>", NULL))
        return 0;

    if(!strcmp(self->cls->name, "java/awt/Insets")) {
        if(self->cls->inst_slots < 4)
            return 0;               /* registration ran out of memory */
        if(nargs > 4) {
            self->fields[0].i = args[1].i;
            self->fields[1].i = args[2].i;
            self->fields[2].i = args[3].i;
            self->fields[3].i = args[4].i;
        }
        *handled = 1;
        return 0;
    }

    if(!strcmp(self->cls->name, "java/awt/GridBagConstraints")) {
        if(self->cls->inst_slots < GBC_SLOTS)
            return 0;               /* registration ran out of memory */

        /* Zero is wrong for five of these. A real GridBagConstraints starts at
         * gridx/gridy RELATIVE, one cell wide and tall, anchored CENTER - and
         * an applet that sets only the fields it cares about and hands the
         * rest to the layout would read the others back as zero, which means
         * something different. */
        gbc_defaults(self);
        self->fields[GBC_INSETS].o = new_insets(vm, 0, 0, 0, 0);
        *handled = 1;
        return 0;
    }
    return 0;
}

/* --- java.awt.Toolkit and java.awt.Cursor -------------------------------- */

static int toolkit(ps_jvm *vm, const char *n, const char *d, ps_jslot *args,
                   int nargs, ps_jslot *ret, int *handled)
{
    *handled = 1;

    if(is(n, d, "<init>", NULL) || is(n, d, "sync", NULL) ||
       is(n, d, "beep", NULL))
        return 0;

    if(is(n, d, "getDefaultToolkit", NULL)) {
        ret->o = ps_jvm_new(vm, ps_jvm_class(vm, "java/awt/Toolkit"));
        return ret->o ? 0 : fail(vm, "out of memory in getDefaultToolkit");
    }

    /* getImage(URL) and getImage(String), resolved against the code base the
     * same way ps_jre.c resolves Applet.getImage - a URL in this runtime is a
     * String, so both forms are one name to join. str_arg is no use here for
     * that same reason: the descriptor says java/net/URL and means String. */
    if(is(n, d, "getImage", NULL)) {
        char        url[512];
        const char *a = nargs > 1 && args[1].o
                      ? ps_jvm_string_utf8(args[1].o, NULL) : NULL;
        ps_jobj    *img;

        ps_applet_url_join(url, sizeof url, NULL, a);

        img = ps_jvm_new(vm, ps_jvm_class(vm, "java/awt/Image"));
        if(!img)
            return fail(vm, "out of memory in getImage");
        img->len = ps_applet_want_image(url);

        ret->o = img;
        return 0;
    }

    /* getScreenSize is deliberately absent: java.awt.Dimension is another
     * piece of work and inventing a second representation of it here is how
     * two code paths end up disagreeing. */
    *handled = 0;
    return 0;
}

static int cursor(ps_jvm *vm, const char *n, const char *d, ps_jslot *args,
                  int nargs, ps_jslot *ret, int *handled)
{
    *handled = 1;

    /* The type is the whole of a Cursor's state, so it rides in the object's
     * length field the way a Color's value does. */
    if(is(n, d, "<init>", NULL)) {
        if(nargs > 1 && args[0].o)
            args[0].o->len = args[1].i;
        return 0;
    }
    if(is(n, d, "getType", NULL)) {
        ret->i = nargs > 0 && args[0].o ? args[0].o->len : 0;
        return 0;
    }
    if(is(n, d, "getPredefinedCursor", NULL) ||
       is(n, d, "getDefaultCursor", NULL)) {
        ps_jobj *c = ps_jvm_new(vm, ps_jvm_class(vm, "java/awt/Cursor"));

        if(!c)
            return fail(vm, "out of memory in getPredefinedCursor");
        c->len = nargs > 0 && n[3] == 'P' ? args[0].i : 0;
        ret->o = c;
        return 0;
    }

    *handled = 0;
    return 0;
}

/* --- dispatch ------------------------------------------------------------ */

int ps_jawt_call(ps_jvm *vm, const char *cls, const char *name,
                 const char *desc, ps_jslot *args, int nargs, ps_jslot *ret,
                 int *handled)
{
    int kind, rc;

    *handled = 0;

    /* Registration rides on the applet's own construction, which is the last
     * moment before init() can ask for a Button. The call itself belongs to
     * ps_jre.c, so this leaves it unhandled. */
    if(name[0] == '<' && !strcmp(cls, "java/applet/Applet")) {
        register_shells(vm);
        return 0;
    }

    /* Everything below is java/awt, java/applet or the private shell base.
     * Two comparisons get Graphics and Color - between them most of the native
     * calls a frame makes - back out before the table walk, because this file
     * sits in front of ps_jre.c and must not slow the paint loop down. */
    if(strncmp(cls, "java/a", 6) && strncmp(cls, "ps/awt/", 7))
        return 0;
    if(!strcmp(cls, "java/awt/Graphics") || !strcmp(cls, "java/awt/Color"))
        return 0;

    kind = kind_of(cls);
    if(kind == K_NONE)
        return 0;

    if(kind == K_SHELL)
        return field_shell(vm, name, desc, args, nargs, handled);

    /* getstatic arrives with a field descriptor rather than a method one and
     * no receiver; that leading parenthesis is the only thing separating the
     * two cases. */
    if(desc[0] != '(')
        return statics(vm, kind, name, ret, handled);

    if(kind == K_TOOLKIT)
        return toolkit(vm, name, desc, args, nargs, ret, handled);
    if(kind == K_CURSOR)
        return cursor(vm, name, desc, args, nargs, ret, handled);

    if(kind >= K_BORDER && kind <= K_CARD)
        return layout(vm, kind, name, desc, args, nargs, ret, handled);

    /* A widget answers its own methods first and inherits the rest. The order
     * matters for exactly one name: select() means "highlight some text" on a
     * TextField and "choose an item" on a Choice.
     *
     * K_COMPONENT skips the widget half entirely. It is the kind a plain
     * Applet, Panel or Container arrives as, it has no label or item list, and
     * going straight to the shared methods keeps repaint() - which every
     * animated applet calls on every frame - off the widget path. */
    if(kind != K_COMPONENT) {
        rc = widget(vm, kind, name, desc, args, nargs, ret, handled);
        if(rc != 0 || *handled)
            return rc;
    }

    if(kind_is_component(kind))
        return component(vm, kind, name, desc, args, nargs, ret, handled);

    return 0;
}
