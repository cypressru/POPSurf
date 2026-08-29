/* Bytecode interpreter.
 *
 * Scoped to what a Java 1.1 applet is: one thread of straight-line drawing
 * code over ints, floats and arrays, calling into a handful of AWT classes.
 * That is a far smaller machine than a general JVM and the difference is the
 * whole reason this is worth writing rather than porting - see the licence
 * survey, where every existing implementation is either GPL, or needs a JIT
 * backend that does not exist for SH-4, or ships a class library larger than
 * our entire memory budget.
 *
 * Design decisions that follow from the target:
 *
 *  - No verifier. Bytecode from the open web is hostile whether or not it
 *    passes a verifier, so every stack push, local index, array index and
 *    constant pool reference is checked as it executes. That is slower per
 *    instruction and much smaller than a verifier, and the safety property is
 *    identical.
 *  - The JDK classes are C, not bytecode. java/awt/Graphics forwards to
 *    ps_jgfx; java/lang/Math forwards to libm. Loading a real class library
 *    to get drawLine would be most of a megabyte for a function call.
 *  - One thread. Applets that animate do it from a Thread calling repaint();
 *    that becomes the shell calling paint() on a timer, which is the same
 *    picture without a scheduler.
 *
 * Everything here is allocation-bounded and reports failure rather than
 * aborting: a browser cannot let one bad applet take down the page it is on.
 */
#ifndef PS_JVM_H
#define PS_JVM_H

#include "ps_jclass.h"
#include "ps_jgfx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One operand-stack or local-variable slot.
 *
 * Eight bytes, with long and double stored whole in the first of the two slots
 * the JVM spec gives them. The spec's two-slot rule still has to be honoured
 * because javac allocates local indices assuming it - a long in local 1 puts
 * the next variable in local 3 - but nothing requires the halves to be stored
 * separately, and splitting them would cost a shift on every access. */
typedef union {
    int32_t         i;
    int64_t         j;
    float           f;
    double          d;
    struct ps_jobj *o;
} ps_jslot;

enum { PS_OBJ_INSTANCE = 0, PS_OBJ_ARRAY };

typedef struct ps_jobj {
    ps_jclass      *cls;
    struct ps_jobj *gc_next;

    uint8_t kind;
    uint8_t elem;       /* PS_T_* for arrays */
    int32_t len;        /* elements, arrays only */

    /* Set for the instances the runtime backs with C state rather than
     * fields - a Graphics wrapping a ps_jgfx, a String wrapping bytes.
     *
     * owns_native says whether the heap sweep should free it. A String's
     * bytes are the VM's; a Graphics points at a context the caller owns and
     * very often does not even have on the heap. Freeing that is how the
     * first end-to-end run of this aborted. */
    void   *native;
    uint8_t owns_native;

    /* Mark bit, live only during a collection. */
    uint8_t marked;

    /* Arrays store elements packed by width here; instances store one slot
     * per field. Never both. */
    void     *data;
    ps_jslot *fields;
} ps_jobj;

/* Loaded classes, the runtime's own included.
 *
 * This was 64 when the runtime implemented a dozen classes and an applet
 * brought three. It now registers the java.lang and java.util containers, the
 * AWT geometry objects and twenty-odd widget shells before the applet's own
 * constructor has run - about fifty - and 64 left too few for the applet
 * itself, which showed up as every applet in the tree failing to load at once.
 *
 * It is an array of pointers, so the headroom costs four bytes an entry and
 * nothing else. Being generous here is much cheaper than being wrong. */
#define PS_JVM_MAX_CLASSES 256
#define PS_JVM_MAX_DEPTH   64
#define PS_JVM_MAX_STACK   4096

/* Lengths a single multianewarray may supply. The format allows 255 and no
 * applet has ever wanted four; the bound is here so a byte out of the code
 * stream cannot size a stack array. */
#define PS_JVM_MAX_DIMS    8

/* --- the 1.1 event model -------------------------------------------------
 *
 * Java 1.1 replaced the overrides with listeners, and the authoring tools of
 * the late 90s emitted the new shape, so a component that receives no input at
 * all is very often one that registered a MouseListener and got nothing back.
 *
 * The two models are not additive. Observed against a real JDK: adding any
 * listener to a component stops the 1.0 methods being called on it entirely -
 * not only for the category the listener covers, but for every category, and
 * it stays that way after the listener is removed again. So an applet that
 * adds a MouseListener and also overrides keyDown gets neither keyDown nor a
 * mouseMove. That looks like a bug in AWT and is not one worth diverging from:
 * an applet delivered a click twice is as broken as one delivered none, and
 * matching the real rule is the only way to be sure which we are doing.
 */
enum {
    PS_LSN_MOUSE = 0,   /* java/awt/event/MouseListener */
    PS_LSN_MOTION,      /* java/awt/event/MouseMotionListener */
    PS_LSN_KEY,         /* java/awt/event/KeyListener */
    PS_LSN_ACTION,      /* java/awt/event/ActionListener */
    PS_LSN_KINDS
};

/* Event ids, the values java.awt.event uses. Read off a real JDK rather than
 * assumed, because applets do compare getID() against them. */
enum {
    PS_EV_KEY_TYPED     = 400,
    PS_EV_KEY_PRESSED   = 401,
    PS_EV_KEY_RELEASED  = 402,
    PS_EV_MOUSE_CLICKED = 500,
    PS_EV_MOUSE_PRESSED = 501,
    PS_EV_MOUSE_RELEASED= 502,
    PS_EV_MOUSE_MOVED   = 503,
    PS_EV_MOUSE_ENTERED = 504,
    PS_EV_MOUSE_EXITED  = 505,
    PS_EV_MOUSE_DRAGGED = 506,
    PS_EV_ACTION        = 1001
};

/* Where an event object keeps its state.
 *
 * MouseEvent, KeyEvent, ActionEvent, the 1.0 Event and Point are runtime
 * classes with no class file, so they have no field table to resolve names
 * against. They are given plain instance slots instead, and the indices are
 * spelled out here because ps_jvm.c fills them in and ps_jre.c reads them back
 * and the two have to agree. One layout serves all of them: the union of what
 * they carry is nine slots, which is cheaper than five layouts and the
 * bookkeeping to tell them apart. */
enum {
    PS_EVF_SOURCE = 0,  /* the Component it happened on; Event.target */
    PS_EVF_ID,
    PS_EVF_X,           /* mouse x, or Point.x */
    PS_EVF_Y,           /* mouse y, or Point.y */
    PS_EVF_CLICKS,
    PS_EVF_MODS,
    PS_EVF_KEY,         /* KeyEvent.getKeyCode, Event.key */
    PS_EVF_CHAR,        /* KeyEvent.getKeyChar */
    PS_EVF_CMD,         /* ActionEvent's command String; Event.arg */
    PS_EVF_SLOTS
};

/* What a key with no character reports, and what an untyped key code is. Both
 * are values applets compare against. */
#define PS_CHAR_UNDEFINED 0xFFFF
#define PS_VK_UNDEFINED   0

/* java.awt.event.InputEvent's modifier bits, as a real JDK reports them.
 * BUTTON2 and ALT are the same bit, and so are BUTTON3 and META - that is not
 * a transcription error, it is how AWT encoded a two-button mouse on a machine
 * with a one-button one. */
#define PS_MOD_SHIFT   1
#define PS_MOD_CTRL    2
#define PS_MOD_META    4
#define PS_MOD_ALT     8
#define PS_MOD_BUTTON1 16
#define PS_MOD_BUTTON2 8
#define PS_MOD_BUTTON3 4

/* Registered listeners, flat rather than per-component.
 *
 * An applet is one Component in this browser - there is no layout and nothing
 * to put a Button in - so a list keyed by target is the whole of what a
 * per-component list would buy, at a fraction of the machinery. The target is
 * kept anyway so the day a second component exists this does not have to be
 * unpicked. */
#define PS_JVM_MAX_LISTENERS 16

typedef struct {
    ps_jobj *target;
    ps_jobj *obj;
    uint8_t  kind;      /* PS_LSN_* */
} ps_jlistener;

/* One call in progress.
 *
 * Frames live in an array on the VM rather than on the C stack, and that is
 * not a style choice - it is what makes execution suspendable. An applet
 * animates by starting a Thread whose run() loops forever around
 * Thread.sleep(50), and the only way to honour that on one hardware thread is
 * to stop mid-method, hand the frame back to the browser, and resume where we
 * left off next tick. A recursive interpreter cannot do that: its state is
 * spread through C stack frames it has no way to save.
 *
 * It also gives exceptions somewhere to unwind through, and removes the C
 * stack depth as a thing hostile bytecode can attack. */
typedef struct {
    ps_jclass  *cls;
    ps_jmethod *m;
    uint32_t    pc;
    int         sp;
    ps_jslot   *locals;
    ps_jslot   *stack;

    /* Return kind the caller is expecting, PS_T_VOID for none. Kept per frame
     * because the callee is the one that knows when it is finished. */
    uint8_t ret_kind;
} ps_jframe;

/* What a call returned to the browser, as opposed to what it returned to its
 * caller. */
typedef enum {
    PS_RUN_DONE = 0,   /* the frame stack emptied */
    PS_RUN_YIELD,      /* out of budget this tick; resume as-is */
    PS_RUN_SLEEP,      /* Thread.sleep; resume when wake_ms passes */
    PS_RUN_ERROR
} ps_jrun;

/* Answers a request for a class the applet referenced but did not ship, by
 * name in internal form ("Chart", "java/lang/Object"). Returns a malloc'd
 * image the VM takes over, or NULL. */
typedef uint8_t *(*ps_jload_fn)(void *user, const char *name, size_t *out_len);

typedef struct ps_jvm {
    ps_jclass  *classes[PS_JVM_MAX_CLASSES];
    int         nclasses;

    ps_jload_fn load;
    void       *load_user;

    ps_jobj *gc_head;
    long     objects;
    long     bytes;

    /* Collection threshold, in live bytes. Raised after each sweep to a
     * multiple of what survived, so a big applet does not collect constantly
     * and a small one does not sit on a huge heap. */
    long gc_at;
    long gc_runs;

    /* Where java/awt/Graphics calls land. */
    ps_jgfx *gfx;

    /* Set when execution stopped early. A browser shows a broken applet as a
     * blank box and carries on with the page, so this is a report, not a
     * crash. */
    char err[192];
    int  failed;

    /* Instruction budget for one call, so a runaway loop in an applet cannot
     * hang the browser. Decremented as it runs; zero means unlimited. */
    long budget;

    /* The suspendable frame stack. */
    ps_jframe frames[PS_JVM_MAX_DEPTH];
    int       depth;

    /* Bottom of the call currently running. Non-zero while paint() runs on
     * top of a suspended animation thread. */
    int       floor;

    /* Where a suspended call left its result, and when it may resume. */
    ps_jslot ret;
    long     wake_ms;      /* remaining sleep, counted down by the caller */

    /* The applet's animation thread, if it started one.
     *
     * A Thread here is a second frame stack, not a second processor: the
     * browser runs a slice of it per frame and paint() runs on the same one.
     * That is enough for every applet that animates, because they all do the
     * same thing - loop, mutate a field, call repaint(), sleep - and none of
     * them are relying on genuine parallelism. */
    ps_jobj *thread_run;       /* the Runnable, NULL when none */
    int      thread_started;
    int      thread_done;

    /* The applet instance and its paint method, kept so repaints after the
     * first do not have to find them again. */
    ps_jobj    *applet;
    ps_jclass  *paint_class;
    ps_jmethod *paint_method;

    /* Set by repaint(). The browser reads and clears it to decide whether the
     * surface needs uploading again - an applet that has not asked to be
     * repainted must not cost a texture upload. */
    int repaint;

    /* The 1.1 listeners, and the latch that retires the 1.0 path.
     *
     * new_events_only goes up on the first registration and never comes down,
     * which is AWT's own behaviour and is checked in the host tests. */
    ps_jlistener listeners[PS_JVM_MAX_LISTENERS];
    int          nlisteners;
    int          new_events_only;

    /* Raised by Thread.sleep, which cannot block: it records how long and lets
     * the interpreter hand the frame stack back intact. */
    int sleeping;

    /* The exception in flight, and the flag that says so.
     *
     * Held on the VM rather than the stack because unwinding walks frames
     * looking for a handler and the object has to outlive every frame it
     * passes through. */
    ps_jobj *thrown;
    int      throwing;
} ps_jvm;

void ps_jvm_init(ps_jvm *vm, ps_jgfx *gfx);
void ps_jvm_free(ps_jvm *vm);

void ps_jvm_set_loader(ps_jvm *vm, ps_jload_fn fn, void *user);

/* Loads and links a class image. The buffer is taken over on success. */
ps_jclass *ps_jvm_define(ps_jvm *vm, uint8_t *raw, size_t len);

/* By name, from the cache or via the loader. NULL if it cannot be had. */
ps_jclass *ps_jvm_class(ps_jvm *vm, const char *name);

ps_jobj *ps_jvm_new(ps_jvm *vm, ps_jclass *c);
ps_jobj *ps_jvm_new_array(ps_jvm *vm, uint8_t elem, int32_t len);
ps_jobj *ps_jvm_new_string(ps_jvm *vm, const char *utf8, size_t len);

/* Raises an exception of the named class with a message.
 *
 * The runtime uses this for the conditions the JVM specifies as throws rather
 * than failures - a null dereference, a divide by zero, an index off the end
 * of an array. Before this they stopped the applet dead; now an applet that
 * catches them behaves as it was written to. */
int ps_jvm_throw(ps_jvm *vm, const char *cls_name, const char *msg);
const char *ps_jvm_string_utf8(const ps_jobj *o, size_t *len);

/* Calls a method to completion. args holds arg_slots entries, `this` first for
 * instance methods. Returns 0 on success; on failure vm->err says why.
 *
 * A call that suspends is an error here: paint() must not sleep, and an applet
 * that tries has to be told rather than silently half-painted. */
int ps_jvm_call(ps_jvm *vm, ps_jclass *cls, ps_jmethod *m,
                const ps_jslot *args, int nargs, ps_jslot *ret);

/* --- the animation thread ------------------------------------------------
 *
 * ps_jvm_start_thread arms whatever the applet passed to Thread.start(), and
 * ps_jvm_pump runs a slice of it. Together they are the whole of threading:
 * one Runnable, cooperatively scheduled against the browser's frame clock.
 */

/* Non-zero if the applet has a thread that has not finished. */
int ps_jvm_has_thread(const ps_jvm *vm);

/* Runs the applet's thread for up to `budget` instructions or until it sleeps.
 * dt_ms is the wall clock since the last pump, which is what Thread.sleep is
 * measured against. Returns PS_RUN_* ; anything but PS_RUN_ERROR is normal. */
ps_jrun ps_jvm_pump(ps_jvm *vm, int dt_ms, long budget);

/* Non-zero if the applet called repaint() since this was last asked, and
 * clears the flag. */
int ps_jvm_take_repaint(ps_jvm *vm);

/* Reclaims unreachable objects. Called automatically when the heap grows past
 * its threshold; exposed so tests can force one. Returns bytes freed. */
long ps_jvm_gc(ps_jvm *vm);

/* Constructs an applet and runs its paint(Graphics). The two-step that a real
 * browser does - instantiate, then paint on every damage - collapsed into the
 * one call the tests and the first shell integration need. */
int ps_jvm_run_applet(ps_jvm *vm, ps_jclass *applet, ps_jgfx *g);

/* Runs paint(Graphics) again on an applet already constructed. This is what a
 * repaint costs after the first frame. */
int ps_jvm_paint(ps_jvm *vm, ps_jgfx *g);

/* --- listeners -----------------------------------------------------------
 *
 * addMouseListener and friends land in ps_jre.c and end up here. Registering
 * the same object twice is legal in AWT and delivers to it twice, so it is
 * legal here; removing takes the first match off, which is the same rule.
 */

/* Returns 0 on success, -1 if the table is full. */
int ps_jvm_add_listener(ps_jvm *vm, ps_jobj *target, ps_jobj *l, int kind);
int ps_jvm_remove_listener(ps_jvm *vm, ps_jobj *target, ps_jobj *l, int kind);

/* Non-zero once anything has been registered, which is the browser's signal to
 * stop calling the 1.0 methods. See the note above PS_LSN_MOUSE. */
int ps_jvm_new_events_only(const ps_jvm *vm);

/* Delivers one event to every listener that covers it, in registration order.
 * Returns how many listener methods actually ran, which is what tells the
 * browser whether the applet took the input or the page should have it.
 *
 * Coordinates are component space - the applet's own box, origin at its top
 * left - which is what getX() is defined to report and what the browser
 * already subtracts before it calls in. */
int ps_jvm_post_mouse(ps_jvm *vm, int id, int x, int y, int clicks, int mods);
int ps_jvm_post_key(ps_jvm *vm, int id, int code, int ch, int mods);

#ifdef __cplusplus
}
#endif

#endif /* PS_JVM_H */
