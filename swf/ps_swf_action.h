/* ActionScript 1.0: the SWF 3 and 4 action set, as a stack machine.
 *
 * SWF 4's ActionScript is not the class-based language it became. It is a
 * stack of values with about fifty opcodes and no objects, no functions and no
 * scope chain, which is what makes it something that can be finished rather
 * than approximated. Everything here is that set plus the two SWF 5 pieces the
 * decoder cannot skip past safely - ConstantPool and the Push type tags that
 * index it. Nothing else from SWF 5 is here, and the reason is in the note on
 * Add at the bottom of this comment.
 *
 * --- the value model, which is the part that is easy to get wrong ----------
 *
 * A SWF 4 value is a number or a string and converts freely between the two,
 * but - and this is the trap - the *operators* are not overloaded. Each one
 * states which conversion it wants:
 *
 *   Add (0x0A) is arithmetic. "3" + "4" is 7, not "34".
 *   StringAdd (0x21) is concatenation. 3 & 4 is "34".
 *   Equals (0x0E) and Less (0x0F) compare as numbers, so "10" < "9" is false.
 *   StringEquals (0x13) and StringLess (0x29) compare as bytes, so "10" < "9"
 *   is true.
 *
 * The overloaded add - concatenate if either side is a string, otherwise sum -
 * is Add2 (0x47), and it is SWF 5. Implementing 0x0A that way is the single
 * most damaging thing that can be done to a Flash 4 file, because it does not
 * fail: every numeric expression whose operands came from a variable, and in
 * SWF 4 that is most of them, silently becomes string concatenation and the
 * movie plays with plausible nonsense in it. So 0x0A is arithmetic here, and
 * Add2 is deliberately absent rather than half-present.
 *
 * The duality lives in the conversions instead, and those are Flash 4's, not
 * ECMAScript's. String-to-number takes the longest numeric prefix and yields 0
 * if there is none, so "12abc" is 12 and "abc" is 0 - never NaN. That much is
 * unchanged and is what the tests pin.
 *
 * What is *not* true, and was asserted here until Ruffle's avm1/divide_swf4
 * said otherwise, is that SWF 4 has no NaN and no Infinity. The format has no
 * way to *write* one - there is no Infinity literal, and division by zero
 * yields the string "#ERROR#" rather than a value - but a value can still
 * arrive at one, and Flash then keeps it. That file is version 4, is not on
 * Ruffle's known-failure list, and its trace of what a real player produced
 * reads Infinity, -Infinity and NaN in eleven places where clamping to DBL_MAX
 * gave 5.99231044954105e+307 and 0. Two routes reach a non-finite in a file
 * this player will actually load: Push's FLOAT tag, which is SWF 4 and can
 * carry an IEEE infinity directly, and overflow out of a multiply.
 *
 * So the clamp is gone and arithmetic here is IEEE end to end. The cost is
 * bounded by construction rather than by hope: every double-to-integer
 * conversion in the machine already went through clamp_long or to_int32, both
 * of which are total over NaN and both infinities, and the frame-number path
 * tests its range with a negated comparison that NaN fails. The only new
 * externally visible behaviour is the text a non-finite prints as, which is
 * "NaN", "Infinity" and "-Infinity" - Flash's spellings and not "%g"'s.
 *
 * Division by zero is still "#ERROR#", including 0/0. Division *by* a
 * non-finite is not, because it is not division by zero: 3/Infinity is 0.
 *
 * --- the host boundary -----------------------------------------------------
 *
 * The VM holds no variables, no display list and no timeline. Every one of
 * those is a callback in ps_swf_host, for two reasons. The real player wires
 * them to the display list and to begin_load; a test harness wires them to
 * assertions, and can then assert on what a script *asked for* rather than on
 * what some renderer did afterwards. Every callback is optional - a null
 * pointer means the capability is absent, and the action still consumes its
 * operands so the rest of the block stays aligned.
 *
 * Every `const char *` handed to a callback is borrowed for the duration of
 * that call only. It may point into the action block, into the VM's string
 * arena, or into a scratch buffer that the next action reuses. A host that
 * wants to keep one copies it.
 *
 * Strings cross the boundary as C strings rather than as ps_swf_value, which
 * costs a conversion and buys the whole interface being callable from code
 * that knows nothing about the VM. It is also faithful: a SWF 4 variable
 * genuinely is a string as far as anything outside the expression evaluator is
 * concerned.
 *
 * --- safety ----------------------------------------------------------------
 *
 * This runs bytecode that arrived over HTTP. The target has no preemption, so
 * a script that does not terminate does not make a bad page, it makes a dead
 * browser: nothing else in the system gets to run again. So the operand stack,
 * the arena, the Call depth and the number of actions executed are all bounded,
 * every jump target is checked against the block length, every read is bounds
 * checked by ps_bits, and the VM allocates nothing whatsoever - ps_swf_vm is
 * about seven kilobytes and the caller decides where it lives.
 */
#ifndef PS_SWF_ACTION_H
#define PS_SWF_ACTION_H

#include <stddef.h>
#include <stdint.h>

/* --- limits --------------------------------------------------------------
 *
 * Typed and scoped rather than #defined, because two of them are load-bearing
 * enough that static_assert checks them against each other in the .c file and
 * a macro cannot carry a type into that.
 */

/* Depth of the operand stack. Flash 4 expressions are two or three deep and
 * the deepest action in the set pops seven (StartDrag with constraints), so
 * this is not a budget a real script approaches - it is the ceiling that stops
 * `Push Push Push ...` from being an attack. */
constexpr int PS_SWF_VM_STACK = 64;

/* Bytes available to strings the script computes rather than quotes. Quoted
 * strings cost nothing: they are pointed at in place inside the action block,
 * which is why this can be small. See the note on arena_reset in the .c file
 * for why a loop that concatenates does not simply run it out. */
constexpr size_t PS_SWF_VM_ARENA = 4096;

/* Buffer size for text crossing the host boundary in either direction: a
 * variable read back from the host, a number formatted for one. A host value
 * longer than this is truncated rather than refused. */
constexpr size_t PS_SWF_VM_TEXT = 256;

/* Actions executed before the script is killed, counted across nested Calls
 * and not reset by them.
 *
 * Derived from the frame budget, because that is the only thing that makes it
 * a number rather than a guess. At a rough hundred SH-4 cycles for an average
 * action - dispatch, two stack touches, and occasionally a string - fifty
 * thousand actions is five million cycles, about 25ms at 200MHz. That is most
 * of one frame at 30fps, so a script may spend up to about a frame before it
 * is stopped, and a runaway costs a visible hitch instead of the machine. */
constexpr uint32_t PS_SWF_VM_STEPS = 50000;

/* Nested ActionCall depth. Call runs another frame's actions on the same
 * operand stack; each level costs a C stack frame, and the target has 32KB
 * thread stacks. Flash 4 content nests one deep if it nests at all. */
constexpr int PS_SWF_VM_DEPTH = 8;

/* Constant pool entries retained. The pool is SWF 5 and a SWF 4 file has none
 * at all, so this exists to keep such a file's Push tags decoding rather than
 * to serve it well. Entries past the limit are still parsed - that is what
 * keeps the stream aligned - but index to the empty string. */
constexpr int PS_SWF_VM_POOL = 128;

/* --- values --------------------------------------------------------------- */

typedef enum : uint8_t {
    /* SWF 4 has only the last two. The first three exist because Push carries
     * type tags for them and a file may use them whatever its version byte
     * says; all three read as 0 and as the empty string, which is what a SWF 4
     * expression would have done with an unset variable anyway. */
    PS_SWF_V_UNDEF = 0,
    PS_SWF_V_NULL,
    PS_SWF_V_BOOL,
    PS_SWF_V_NUM,
    PS_SWF_V_STR
} ps_swf_vkind;

/* `str` is borrowed, never owned: it points into the action block for a quoted
 * string or a constant, and into the VM's arena for a computed one. Both are
 * valid for as long as the block passed to ps_swf_action_run is - which is why
 * a caller inspecting the stack afterwards must not have freed it yet. */
typedef struct {
    ps_swf_vkind kind;
    double       num;
    const char  *str;
} ps_swf_value;

/* --- host ---------------------------------------------------------------- */

/* GetURL2's method byte, spelled out because the player and the test harness
 * both have to agree on it and the spec's own naming is not obvious. Bits 2-5
 * are reserved and are passed through untouched. */
#define PS_SWF_URL_METHOD(f)   ((f) & 3u)     /* 0 none, 1 GET, 2 POST */
#define PS_SWF_URL_LOADTARGET  0x40u          /* load into the named target */
#define PS_SWF_URL_LOADVARS    0x80u          /* response is a variable list */

/* Every member may be null. The VM never calls through a null pointer; it
 * performs the action's stack effect and drops the effect on the world, which
 * is what lets a harness wire up three callbacks and ignore the rest. */
typedef struct {
    void *user;

    /* Variables. get_var returns true and fills `out` when the variable
     * exists; false leaves the script with undefined, which is SWF 4's answer
     * for an unset name. `name` may be a slash path - resolving those is the
     * host's job because only it knows the display list. */
    bool (*get_var)(void *user, const char *name, char *out, size_t outlen);
    void (*set_var)(void *user, const char *name, const char *value);

    /* Properties. `prop` is the numeric index off the stack, 0.._x through
     * 21.._ymouse; ps_swf_property_name turns it into a name. Passed on
     * unvalidated because which indices exist is a display-list question. */
    bool (*get_prop)(void *user, const char *target, int prop,
                     char *out, size_t outlen);
    void (*set_prop)(void *user, const char *target, int prop,
                     const char *value);

    /* Timeline. `frame` is always zero-based here, which is worth stating
     * because the file is not: GotoFrame's operand is zero-based and
     * GotoFrame2's, Call's and WaitForFrame2's are one-based, and the VM
     * normalises all of them before they arrive. */
    void (*goto_frame)(void *user, int frame);
    void (*step_frame)(void *user, int delta);      /* Next/PreviousFrame */
    void (*set_play)(void *user, bool playing);
    int  (*find_label)(void *user, const char *label);   /* frame, or -1 */
    bool (*frame_ready)(void *user, int frame);          /* WaitForFrame */

    /* ActionCall: hand back the frame's action block, or return false. The
     * block is borrowed for the duration of the call and is executed on the
     * same operand stack, which is what the spec asks for. */
    bool (*frame_code)(void *user, int frame,
                       const uint8_t **code, size_t *len);

    void (*set_target)(void *user, const char *target);  /* "" = main timeline */
    void (*get_url)(void *user, const char *url, const char *target,
                    unsigned flags);
    void (*trace)(void *user, const char *text);
    void (*stop_sounds)(void *user);
    void (*toggle_quality)(void *user);

    /* Impure, and so callbacks rather than library calls: a test that cannot
     * pin these down cannot assert on a script that uses them. Absent, both
     * yield 0, which is deterministic and harmless. */
    int    (*rand_below)(void *user, int max);
    double (*time_ms)(void *user);
} ps_swf_host;

/* --- result --------------------------------------------------------------- */

typedef enum : uint8_t {
    PS_SWF_ACT_OK = 0,
    PS_SWF_ACT_TRUNCATED,       /* an action ran off the end of the block */
    PS_SWF_ACT_BAD_JUMP,        /* If or Jump aimed outside the block */
    PS_SWF_ACT_STACK_OVERFLOW,
    PS_SWF_ACT_STEP_LIMIT,      /* did not terminate; see PS_SWF_VM_STEPS */
    PS_SWF_ACT_DEPTH_LIMIT,     /* Call nested past PS_SWF_VM_DEPTH */
    PS_SWF_ACT_STRING_LIMIT,    /* computed strings exhausted the arena */
    PS_SWF_ACT_UNSUPPORTED      /* an action whose length does not cover its
                                 * own body, so the next one cannot be found */
} ps_swf_act_status;

typedef struct {
    ps_swf_act_status status;
    size_t   pc;            /* offset of the action that ended the run */
    uint32_t steps;         /* actions executed, including nested Calls */
    uint32_t skipped;       /* actions ignored because the opcode is unknown */
    uint32_t underflows;    /* pops from an empty stack; see the .c file */
    uint8_t  first_skipped; /* the first unknown opcode, for a diagnostic */
} ps_swf_action_result;

/* --- vm ------------------------------------------------------------------- */

/* Opaque only by convention - it is here so a caller can place it, since there
 * is no allocator involved and on the target it will most likely be a single
 * static instance owned by the player. About 7KB. */
typedef struct {
    const ps_swf_host *host;

    ps_swf_value stack[PS_SWF_VM_STACK];
    int          sp;

    char   arena[PS_SWF_VM_ARENA];
    size_t alen;

    const char    *pool[PS_SWF_VM_POOL];
    int            npool;
    const uint8_t *pool_owner;   /* block whose ConstantPool filled it */

    /* Two, because a binary operator needs both operands as text at once and
     * a number has to be formatted somewhere. Used by explicit index so an
     * aliasing mistake is visible at the call site rather than at run time. */
    char tmp[2][PS_SWF_VM_TEXT];
    char io[PS_SWF_VM_TEXT];     /* what the host writes into */

    uint32_t steps, max_steps;
    int      depth, max_depth;
    uint32_t skipped, underflows;
    uint8_t  first_skipped;
    bool     retargeted;         /* SetTarget moved us off the main timeline */

    ps_swf_act_status fault;     /* sticky, like ps_bits' overrun flag */
    size_t            fault_pc;
} ps_swf_vm;

/* Clears the machine and binds a host. max_steps and max_depth are set to the
 * defaults above and may be lowered afterwards; the fixed arrays cannot be
 * raised, which is the point of them being fixed. */
void ps_swf_action_init(ps_swf_vm *vm, const ps_swf_host *host);

/* Executes one action block - the body of a DoAction or DoInitAction tag, or
 * anything else holding the same encoding. The operand stack and the arena are
 * cleared first, so a block that faulted cannot leave debris for the next one.
 *
 * Returns true when the block ran to its end, false on any of the statuses
 * above; `res` may be null if only that matters. A false return is a script
 * failure and never a host failure - the host is intact either way, which is
 * the whole reason the limits exist. */
bool ps_swf_action_run(ps_swf_vm *vm, const uint8_t *code, size_t len,
                       ps_swf_action_result *res);

const char *ps_swf_action_status_name(ps_swf_act_status s);

/* "_x", "_alpha" and so on, or null outside 0..21. */
const char *ps_swf_property_name(int prop);

/* --- inspection, for tests and for a debugger ----------------------------- */

/* Values left on the stack when the block ended. Depth is normally zero - a
 * well-formed block balances - so anything else is either a test that meant to
 * leave something there or a script that is not what it claims to be. */
int                 ps_swf_action_depth(const ps_swf_vm *vm);
const ps_swf_value *ps_swf_action_peek(const ps_swf_vm *vm, int from_top);

/* The two conversions, exposed because a test asserting on SWF 4 duality has
 * to be able to ask for both readings of the same value. ps_swf_value_text
 * returns either the value's own pointer or `buf`, so the result is valid for
 * as long as the shorter-lived of the two. */
double      ps_swf_value_number(const ps_swf_value *v);
const char *ps_swf_value_text(const ps_swf_value *v, char *buf, size_t buflen);

#endif /* PS_SWF_ACTION_H */
