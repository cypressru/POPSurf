/* The ActionScript 1.0 stack machine.
 *
 * ps_swf_action.h carries the argument for the value model and the host
 * boundary; this file is the machine itself and the notes here are about how
 * it stays alive on hostile input.
 *
 * Two things drive everything below. The first is that a jump offset is a
 * signed sixteen-bit field inside the file, so a block can branch backwards
 * and every loop in every SWF is built that way - which means "does this
 * terminate" is not answerable by inspection and has to be answered by a
 * budget. The second is that the target has no preemption: a script that spins
 * does not hang a tab, it hangs the machine, because nothing else will ever be
 * scheduled again. So the budget is not a nicety, it is the only thing between
 * a malformed page and a power cycle.
 *
 * The failure discipline is ps_bits': a fault is sticky and the loop head
 * tests it, rather than every one of sixty cases threading a status back. What
 * is threaded back is the one class ps_bits cannot express - a push that will
 * not fit - and that is why the push and arena helpers are [[nodiscard]].
 * Silently dropping a value on a full stack would leave the stack misaligned
 * against the code that produced it and turn an attack into wrong arithmetic
 * instead of a stopped script.
 */
#include "ps_swf_action.h"
#include "ps_swf_bits.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Invariants a stack machine depends on, checked where they can fail the build
 * rather than stated where they can rot. */
static_assert(sizeof(ps_swf_value) <= 32,
              "a value must stay small: the stack is sized in whole values");
static_assert(PS_SWF_VM_STACK * sizeof(ps_swf_value) <= 4096,
              "operand stack over 4KB: the target has 32KB thread stacks");
static_assert(PS_SWF_VM_STACK >= 8,
              "StartDrag pops seven operands; the stack cannot be shallower");
static_assert(PS_SWF_VM_TEXT >= 32,
              "a formatted double needs about 24 bytes plus its terminator");
static_assert(PS_SWF_VM_ARENA >= 2 * PS_SWF_VM_TEXT,
              "the arena must hold a concatenation of two host-sized strings");
static_assert(PS_SWF_VM_POOL <= 65535,
              "Push's constant16 index is a u16; a larger pool is unreachable");
/* The budget is a time budget - see PS_SWF_VM_STEPS - so it is bounded above
 * by what fits in a frame, not by what fits in the counter. */
static_assert(PS_SWF_VM_STEPS <= 100000,
              "over a frame's worth of SH-4 time at ~100 cycles per action");
/* Push's float and double tags are reassembled from little-endian integers and
 * then reinterpreted, which is only the same number if the machine lays its
 * floats out the way it lays out its integers. True on x86 and on the SH-4,
 * and now false at build time anywhere it is not.
 *
 * Asked of the compiler rather than of <stdbit.h>, which is C23 and which the
 * Dreamcast toolchain's libc does not ship even though its compiler is C23. */
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
              "Push's float tags assume a little-endian target");

/* Action codes. Named as an enum with a stated width so the dispatch switch is
 * over a type rather than over integers, and so a typo becomes a build error
 * rather than an opcode nobody implements. The three at the bottom are here
 * only to be refused: see the default case. */
typedef enum : uint8_t {
    A_END               = 0x00,
    A_NEXT_FRAME        = 0x04,
    A_PREV_FRAME        = 0x05,
    A_PLAY              = 0x06,
    A_STOP              = 0x07,
    A_TOGGLE_QUALITY    = 0x08,
    A_STOP_SOUNDS       = 0x09,
    A_ADD               = 0x0a,
    A_SUBTRACT          = 0x0b,
    A_MULTIPLY          = 0x0c,
    A_DIVIDE            = 0x0d,
    A_EQUALS            = 0x0e,
    A_LESS              = 0x0f,
    A_AND               = 0x10,
    A_OR                = 0x11,
    A_NOT               = 0x12,
    A_STRING_EQUALS     = 0x13,
    A_STRING_LENGTH     = 0x14,
    A_STRING_EXTRACT    = 0x15,
    A_POP               = 0x17,
    A_TO_INTEGER        = 0x18,
    A_GET_VARIABLE      = 0x1c,
    A_SET_VARIABLE      = 0x1d,
    A_SET_TARGET2       = 0x20,
    A_STRING_ADD        = 0x21,
    A_GET_PROPERTY      = 0x22,
    A_SET_PROPERTY      = 0x23,
    A_CLONE_SPRITE      = 0x24,
    A_REMOVE_SPRITE     = 0x25,
    A_TRACE             = 0x26,
    A_START_DRAG        = 0x27,
    A_END_DRAG          = 0x28,
    A_STRING_LESS       = 0x29,
    A_RANDOM_NUMBER     = 0x30,
    A_MB_STRING_LENGTH  = 0x31,
    A_CHAR_TO_ASCII     = 0x32,
    A_ASCII_TO_CHAR     = 0x33,
    A_GET_TIME          = 0x34,
    A_MB_STRING_EXTRACT = 0x35,
    A_MB_CHAR_TO_ASCII  = 0x36,
    A_MB_ASCII_TO_CHAR  = 0x37,
    A_GOTO_FRAME        = 0x81,
    A_GET_URL           = 0x83,
    A_CONSTANT_POOL     = 0x88,
    A_WAIT_FOR_FRAME    = 0x8a,
    A_SET_TARGET        = 0x8b,
    A_GOTO_LABEL        = 0x8c,
    A_WAIT_FOR_FRAME2   = 0x8d,
    A_DEFINE_FUNCTION2  = 0x8e,
    A_WITH              = 0x94,
    A_PUSH              = 0x96,
    A_JUMP              = 0x99,
    A_GET_URL2          = 0x9a,
    A_DEFINE_FUNCTION   = 0x9b,
    A_IF                = 0x9d,
    A_CALL              = 0x9e,
    A_GOTO_FRAME2       = 0x9f
} ps_action;

/* Used when the caller supplies no host, so the sixty call sites below test
 * one member for null instead of two. */
static const ps_swf_host ps_swf_host_none = { .user = nullptr };

/* --- numbers -------------------------------------------------------------- */

/* Double to int, total and without undefined behaviour. Converting an
 * out-of-range double with a cast is UB, and every integer this machine needs
 * - a frame, a property index, a substring offset - comes from a double that
 * came from a file. */
static long clamp_long(double d)
{
    if(d != d)
        return 0;
    if(d >= 2147483647.0)
        return 2147483647L;
    if(d <= -2147483648.0)
        return -2147483648L;
    return (long)d;
}

/* ECMA ToInt32, which is what ActionToInteger actually is: truncate towards
 * zero, then take it modulo 2^32 into the signed range. The wrap is the part
 * that is easy to leave out and impossible to notice, because it only shows on
 * values no sane script produces - but a hostile one produces them on purpose. */
static double to_int32(double d)
{
    double t;

    if(d != d || d == 0.0 || d > DBL_MAX || d < -DBL_MAX)
        return 0.0;
    t = (d < 0.0) ? -floor(-d) : floor(d);
    t = fmod(t, 4294967296.0);
    if(t < 0.0)
        t += 4294967296.0;
    if(t >= 2147483648.0)
        t -= 4294967296.0;
    return t;
}

/* Flash 4's string-to-number: the longest numeric prefix, or 0.
 *
 * Hand-written rather than strtod for three reasons, in order of how much they
 * would hurt. strtod honours the locale's decimal point, and a browser that
 * reads "1.5" as 1 in a French locale is a fault nobody would look for here.
 * strtod accepts "inf", "nan" and C99 hex floats, and Flash 4 accepts none of
 * those spellings: "Infinity" reads as 0 here because "I" is not a digit, and
 * that is the answer, not an approximation of one.
 * And ECMAScript's ToNumber - which is what SWF 5 switched to - rejects
 * trailing junk, where Flash 4 takes the prefix: "12abc" is 12 here and NaN
 * there, and scripts of this vintage rely on the prefix reading. */
static double scan_number(const char *s, const char **end)
{
    const char *p = s;
    double      m = 0.0;
    int         neg = 0, frac = 0, sig = 0, exp10 = 0;
    bool        any = false;

    if(end)
        *end = s;
    if(!s)
        return 0.0;

    while(*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f')
        p++;
    if(*p == '+' || *p == '-') {
        neg = (*p == '-');
        p++;
    }
    for(; ; p++) {
        if(*p >= '0' && *p <= '9') {
            any = true;
            /* Past eighteen significant digits a double cannot hold any more,
             * so further digits only move the decimal point. Accumulating them
             * anyway is how a long digit string reaches infinity. */
            if(sig < 18) {
                m = m * 10.0 + (double)(*p - '0');
                sig++;
                if(frac)
                    exp10--;
            } else if(!frac) {
                exp10++;
            }
        } else if(*p == '.' && !frac) {
            frac = 1;
        } else {
            break;
        }
    }
    if(!any)
        return 0.0;

    if(*p == 'e' || *p == 'E') {
        const char *q = p + 1;
        int         esign = 1, ea = 0;
        bool        edig = false;

        if(*q == '+' || *q == '-') {
            esign = (*q == '-') ? -1 : 1;
            q++;
        }
        for(; *q >= '0' && *q <= '9'; q++) {
            if(ea < 100000)
                ea = ea * 10 + (*q - '0');
            edig = true;
        }
        /* "1e" is 1 followed by junk, not an error: the prefix rule again. */
        if(edig) {
            exp10 += esign * ea;
            p = q;
        }
    }

    if(exp10 > 400)
        exp10 = 400;
    if(exp10 < -400)
        exp10 = -400;
    m *= pow(10.0, (double)exp10);
    if(neg)
        m = -m;
    if(end)
        *end = p;
    /* The exponent clamp above bounds the magnitude but not the overflow: a
     * mantissa of eighteen digits times 1e400 is infinite, and it is left
     * infinite. See the note on non-finites at the head of this file. */
    return m;
}

/* Number to string. Integers print as integers, which matters more than it
 * sounds: a SWF 4 frame counter concatenated into a URL must read "12" and not
 * "12.000000", and the only numeric Push type in SWF 4 is a float, so every
 * integer in a file arrives as one. */
static const char *num_text(double d, char *buf, size_t n)
{
    /* Flash's spellings, not the C library's. "%g" gives "nan" and "inf", and
     * a script that concatenates one into a variable or a URL would carry that
     * text off the machine - so the one place a non-finite becomes visible is
     * the one place it has to agree with the reference player exactly. */
    if(d != d)
        return "NaN";
    if(d > DBL_MAX)
        return "Infinity";
    if(d < -DBL_MAX)
        return "-Infinity";
    if(d == 0.0)                       /* also catches -0, which prints "-0" */
        return "0";
    if(d == floor(d) && d >= -1e15 && d <= 1e15) {
        snprintf(buf, n, "%.0f", d);
        return buf;
    }
    snprintf(buf, n, "%.15g", d);
    return buf;
}

double ps_swf_value_number(const ps_swf_value *v)
{
    switch(v->kind) {
    case PS_SWF_V_NUM:
    case PS_SWF_V_BOOL:
        return v->num;
    case PS_SWF_V_STR:
        return scan_number(v->str, nullptr);
    default:
        return 0.0;
    }
}

const char *ps_swf_value_text(const ps_swf_value *v, char *buf, size_t buflen)
{
    switch(v->kind) {
    case PS_SWF_V_STR:
        return v->str ? v->str : "";
    case PS_SWF_V_NUM:
        return num_text(v->num, buf, buflen);
    case PS_SWF_V_BOOL:
        /* "1" and "0", not "true" and "false". SWF 4 has no boolean, so the
         * only way one reaches a string context is a SWF 5 Push tag in a file
         * this reads anyway, and 1/0 is what Flash gives below version 5. */
        return v->num != 0.0 ? "1" : "0";
    default:
        return "";
    }
}

static bool truthy(const ps_swf_value *v)
{
    /* Below SWF 7 a string's truth is its numeric value, so "0" and "" and
     * "hello" are all false and "1" and "0.5" are true. That is one rule, not
     * two, which is why there is no special case for strings here. */
    return ps_swf_value_number(v) != 0.0;
}

/* True when `s` is exactly how this machine would print the number it holds -
 * so converting it to a number and back is lossless. Used on values coming
 * back from the host: see push_host_text. */
static bool numeric_canonical(const char *s, double *out)
{
    char        buf[PS_SWF_VM_TEXT];
    const char *end;
    double      d;

    if(!s || !*s)
        return false;
    d = scan_number(s, &end);
    if(end == s || *end != '\0')
        return false;
    if(strcmp(num_text(d, buf, sizeof buf), s) != 0)
        return false;
    *out = d;
    return true;
}

/* --- stack and arena ------------------------------------------------------ */

static void fault(ps_swf_vm *vm, ps_swf_act_status s, size_t pc)
{
    if(vm->fault == PS_SWF_ACT_OK) {
        vm->fault    = s;
        vm->fault_pc = pc;
    }
}

[[nodiscard]] static bool push_value(ps_swf_vm *vm, ps_swf_value v)
{
    if(vm->sp >= PS_SWF_VM_STACK) {
        fault(vm, PS_SWF_ACT_STACK_OVERFLOW, vm->fault_pc);
        return false;
    }
    vm->stack[vm->sp++] = v;
    return true;
}

[[nodiscard]] static bool push_number(ps_swf_vm *vm, double d)
{
    ps_swf_value v = { PS_SWF_V_NUM, d, nullptr };
    return push_value(vm, v);
}

/* SWF 4 comparisons yield the numbers 1 and 0, not a boolean - the boolean
 * type and the Equals2/Less2 that produce it are both SWF 5. */
[[nodiscard]] static bool push_flag(ps_swf_vm *vm, bool b)
{
    return push_number(vm, b ? 1.0 : 0.0);
}

/* `s` is borrowed and must outlive the run: the action block or the arena.
 * Null means an earlier step already failed, and is passed through so the
 * failure surfaces at one place instead of at every producer. */
[[nodiscard]] static bool push_text(ps_swf_vm *vm, const char *s)
{
    ps_swf_value v = { PS_SWF_V_STR, 0.0, s };

    if(!s)
        return false;
    return push_value(vm, v);
}

[[nodiscard]] static bool push_kind(ps_swf_vm *vm, ps_swf_vkind k)
{
    ps_swf_value v = { k, 0.0, nullptr };
    return push_value(vm, v);
}

[[nodiscard]] static ps_swf_value pop_value(ps_swf_vm *vm)
{
    ps_swf_value v = { PS_SWF_V_UNDEF, 0.0, nullptr };

    if(vm->sp > 0)
        return vm->stack[--vm->sp];
    /* Flash yields undefined rather than failing here, and content relies on
     * it - an exporter that emits a stray Pop is common and harmless. Counting
     * it keeps the information without spending the script for it. */
    vm->underflows++;
    return v;
}

/* For actions whose operands this player has nowhere to send. Separate from
 * pop_value so the [[nodiscard]] on that one stays honest: an ignored pop is a
 * bug everywhere except in the handful of places that say so. */
static void discard_values(ps_swf_vm *vm, int n)
{
    while(n-- > 0) {
        if(vm->sp > 0)
            vm->sp--;
        else
            vm->underflows++;
    }
}

/* Concatenation is the only way this machine makes a string, so it is the only
 * thing the arena has to serve. Quoted strings and constants are pointed at in
 * place inside the block and never land here. */
[[nodiscard]] static const char *arena_cat(ps_swf_vm *vm,
                                           const char *a, size_t na,
                                           const char *b, size_t nb)
{
    char  *p;
    size_t need;

    if(!a) a = "", na = 0;
    if(!b) b = "", nb = 0;
    /* Each length is checked against the whole arena before they are summed,
     * so `need` cannot wrap however large the operands claim to be - and it is
     * then compared against the space left rather than against the total, so
     * the comparison cannot wrap either. Both halves matter: the operands here
     * are ultimately file-derived string lengths. */
    if(na > PS_SWF_VM_ARENA || nb > PS_SWF_VM_ARENA) {
        fault(vm, PS_SWF_ACT_STRING_LIMIT, vm->fault_pc);
        return nullptr;
    }
    need = na + nb + 1;
    if(vm->alen > PS_SWF_VM_ARENA || need > PS_SWF_VM_ARENA - vm->alen) {
        fault(vm, PS_SWF_ACT_STRING_LIMIT, vm->fault_pc);
        return nullptr;
    }
    p = vm->arena + vm->alen;
    if(na) memcpy(p, a, na);
    if(nb) memcpy(p + na, b, nb);
    p[na + nb] = '\0';
    vm->alen += na + nb + 1;
    return p;
}

[[nodiscard]] static const char *arena_copy(ps_swf_vm *vm, const char *s)
{
    return arena_cat(vm, s, s ? strlen(s) : 0, nullptr, 0);
}

/* Text that came from outside - a variable, a property - becomes a number when
 * it is exactly one, and a copied string otherwise.
 *
 * That is not an optimisation for its own sake. `i = i + 1` in a loop reads i
 * back from the host every iteration, and without this each read would cost
 * arena bytes; a hundred-iteration counter loop would run the arena out and
 * fail a script that is doing nothing wrong. The canonical-round-trip test is
 * what makes it safe: "007" and " 1" are not canonical and stay strings, so
 * nothing a script could observe changes. */
[[nodiscard]] static bool push_host_text(ps_swf_vm *vm, const char *s)
{
    double d;

    if(numeric_canonical(s, &d))
        return push_number(vm, d);
    return push_text(vm, arena_copy(vm, s));
}

/* --- reading a block ------------------------------------------------------ */

/* A NUL-terminated string out of an action body, borrowed in place. Null means
 * the terminator is not inside the body, which makes the action truncated -
 * and the caller must say so, which is what the attribute is for. */
[[nodiscard]] static const char *read_str(ps_bits *b)
{
    size_t i;

    ps_bits_align(b);
    for(i = b->pos; i < b->len; i++) {
        if(b->data[i] == 0) {
            const char *s = (const char *)b->data + b->pos;
            b->pos = i + 1;
            return s;
        }
    }
    b->pos  = b->len;
    b->over = 1;
    return nullptr;
}

/* SWF stores a DOUBLE as two little-endian 32-bit words with the halves the
 * other way round from the machine's own layout - high word first. Nothing
 * else in the format does this and it is the single easiest thing to get wrong
 * about Push, because a wrong-endian double is not garbage, it is a different
 * plausible number. */
static double read_double(ps_bits *b)
{
    uint32_t hi = ps_bits_u32(b);
    uint32_t lo = ps_bits_u32(b);
    uint64_t bits = ((uint64_t)hi << 32) | (uint64_t)lo;
    double   d;

    memcpy(&d, &bits, sizeof d);
    return d;
}

static double read_float(ps_bits *b)
{
    uint32_t bits = ps_bits_u32(b);
    float    f;

    memcpy(&f, &bits, sizeof f);
    return (double)f;
}

/* The one place a branch target is turned into a position. The offset is
 * signed and relative to the end of the branch action, and landing exactly on
 * the end of the block is legal - that is how a loop's exit branch is written.
 * Everything else outside [0, len] is a malformed file and stops the script:
 * an unchecked offset here is an arbitrary read, and this is the only action
 * that can move the program counter anywhere at all. */
[[nodiscard]] static bool jump_target(size_t from, int off, size_t len,
                                      size_t *out)
{
    if(off < 0) {
        size_t back = (size_t)(-(long)off);

        if(back > from)
            return false;
        *out = from - back;
    } else {
        if((size_t)off > len - from)
            return false;
        *out = from + (size_t)off;
    }
    return true;
}

/* Steps over n whole actions, for WaitForFrame's skip count. Decoding is the
 * only way to do it - actions are not fixed width - and it is bounded by the
 * block, so a skip count of 255 past the end of a short block simply arrives
 * at the end. */
[[nodiscard]] static size_t skip_actions(const uint8_t *code, size_t len,
                                         size_t pc, unsigned n)
{
    while(n-- > 0 && pc < len) {
        uint8_t op = code[pc];

        if(op == A_END)
            return len;
        if(op < 0x80) {
            pc += 1;
        } else {
            uint32_t alen;

            if(len - pc < 3)
                return len;
            alen = (uint32_t)code[pc + 1] | ((uint32_t)code[pc + 2] << 8);
            if(alen > len - pc - 3)
                return len;
            pc += 3 + alen;
        }
    }
    return pc;
}

/* --- host helpers --------------------------------------------------------- */

static const char *const ps_swf_props[] = {
    "_x", "_y", "_xscale", "_yscale", "_currentframe", "_totalframes",
    "_alpha", "_visible", "_width", "_height", "_rotation", "_target",
    "_framesloaded", "_name", "_droptarget", "_url", "_highquality",
    "_focusrect", "_soundbuftime", "_quality", "_xmouse", "_ymouse"
};

const char *ps_swf_property_name(int prop)
{
    if(prop < 0 || (size_t)prop >= sizeof ps_swf_props / sizeof ps_swf_props[0])
        return nullptr;
    return ps_swf_props[prop];
}

/* The frame operand shared by GotoFrame2, WaitForFrame2 and Call: either a
 * number or a label. Returns a zero-based frame, or -1 for "no such frame",
 * which every caller treats as "do nothing" rather than as an error - a goto
 * to a label a truncated file never defined should not kill the script.
 *
 * The one-based-ness is the trap. GotoFrame's operand is a zero-based frame
 * index in the tag body, while everything that takes a frame off the *stack*
 * is one-based, because that is what the author wrote in gotoAndPlay(). Both
 * arrive at the host as zero-based. The spec is explicit for GotoFrame2 and
 * silent for Call; they are treated alike here, because being off by one in
 * one action and not the other is a worse thing to be wrong about than being
 * off by one in both.
 *
 * The arithmetic stays in double until the range test, because the inputs are
 * a file-supplied scene bias and a file-supplied number: adding them as ints
 * is exactly the overflow this is meant to be immune to. */
[[nodiscard]] static int resolve_frame(ps_swf_vm *vm, const ps_swf_value *v,
                                       int bias)
{
    /* No SWF has more frames than this, and it keeps the result inside an int
     * on every target with room to spare. */
    constexpr double frame_max = 1000000.0;
    double           n;

    if(v->kind == PS_SWF_V_STR) {
        const char *s = v->str ? v->str : "";
        const char *end;

        if(vm->host->find_label) {
            int f = vm->host->find_label(vm->host->user, s);

            if(f >= 0)
                return f;
        }
        /* A label that is not defined may still be a frame number written as
         * text, which is what a script that computed it produces. */
        n = scan_number(s, &end);
        if(end == s)
            return -1;
    } else if(v->kind == PS_SWF_V_NUM || v->kind == PS_SWF_V_BOOL) {
        n = v->num;
    } else {
        return -1;
    }

    n = floor(n) + (double)bias - 1.0;
    if(!(n >= 0.0 && n <= frame_max))
        return -1;
    return (int)n;
}

/* --- the machine ---------------------------------------------------------- */

[[nodiscard]] static bool run_block(ps_swf_vm *vm, const uint8_t *code,
                                    size_t len);

/* After a nested Call returns, its block is about to go out of scope as far as
 * this VM is concerned, and anything the callee left on the shared stack that
 * points into it would dangle. There is no ownership to consult - a value's
 * string is a bare pointer - so the range is tested directly and the survivors
 * are copied into the arena, which outlives every block. */
static void relocate(ps_swf_vm *vm, const uint8_t *base, size_t len)
{
    uintptr_t lo = (uintptr_t)base, hi = (uintptr_t)base + len;
    int       i;

    for(i = 0; i < vm->sp; i++) {
        ps_swf_value *v = &vm->stack[i];
        uintptr_t     p;

        if(v->kind != PS_SWF_V_STR || !v->str)
            continue;
        p = (uintptr_t)v->str;
        if(p < lo || p >= hi)
            continue;
        v->str = arena_copy(vm, v->str);
        if(!v->str)
            v->kind = PS_SWF_V_UNDEF;      /* arena_cat already faulted */
    }
}

static const char *pool_get(const ps_swf_vm *vm, unsigned i)
{
    if(i >= (unsigned)vm->npool)
        return "";
    return vm->pool[i];
}

/* Push's payload is a sequence of tagged values, however many fit in the
 * action's length. Split out because it is the only action with an inner loop
 * and the only one whose payload can be self-describing garbage. */
[[nodiscard]] static bool run_push(ps_swf_vm *vm, ps_bits *b)
{
    bool ok = true;

    while(ok && !b->over && b->pos < b->len) {
        uint8_t t = ps_bits_u8(b);

        switch(t) {
        case 0: ok = push_text(vm, read_str(b));                       break;
        case 1: ok = push_number(vm, read_float(b));                   break;
        case 2: ok = push_kind(vm, PS_SWF_V_NULL);                     break;
        case 3: ok = push_kind(vm, PS_SWF_V_UNDEF);                    break;
        case 4:
            /* A register reference. Registers arrive with SWF 5's
             * StoreRegister and DefineFunction2, neither of which is here, so
             * the index is read to keep the stream aligned and the value is
             * undefined - which is what an unwritten register holds anyway. */
            (void)ps_bits_u8(b);
            ok = push_kind(vm, PS_SWF_V_UNDEF);
            break;
        case 5: ok = push_number(vm, ps_bits_u8(b) ? 1.0 : 0.0);       break;
        case 6: ok = push_number(vm, read_double(b));                  break;
        case 7: ok = push_number(vm, (double)(int32_t)ps_bits_u32(b)); break;
        case 8: ok = push_text(vm, pool_get(vm, ps_bits_u8(b)));       break;
        case 9: ok = push_text(vm, pool_get(vm, ps_bits_u16(b)));      break;
        default:
            /* An unknown type tag has an unknown width, so where the next
             * value starts is unknowable. The action's own length still says
             * where the next action starts, so the block continues. */
            return true;
        }
    }
    return ok;
}

[[nodiscard]] static bool run_block(ps_swf_vm *vm, const uint8_t *code,
                                    size_t len)
{
    size_t pc = 0;

    while(pc < len) {
        ps_bits      b;
        size_t       body, next;
        uint32_t     alen;
        uint8_t      op = code[pc];
        bool         ok = true;

        if(op == A_END)
            return true;
        if(vm->steps >= vm->max_steps) {
            fault(vm, PS_SWF_ACT_STEP_LIMIT, pc);
            return false;
        }
        vm->steps++;
        vm->fault_pc = pc;

        if(op < 0x80) {
            alen = 0;
            body = pc + 1;
        } else {
            if(len - pc < 3) {
                fault(vm, PS_SWF_ACT_TRUNCATED, pc);
                return false;
            }
            alen = (uint32_t)code[pc + 1] | ((uint32_t)code[pc + 2] << 8);
            body = pc + 3;
            if(alen > len - body) {
                fault(vm, PS_SWF_ACT_TRUNCATED, pc);
                return false;
            }
        }
        next = body + alen;

        /* An empty stack is the only moment at which nothing can be pointing
         * into the arena, so it is the only moment at which it can be reclaimed
         * - and it is a moment that arrives between every pair of statements in
         * a well-formed script, because a statement balances the stack. That is
         * what lets a loop concatenate strings a thousand times inside four
         * kilobytes, and it is the reason variables live in the host rather
         * than in the VM: a variable table here would hold arena pointers and
         * this reset could never happen. */
        if(vm->sp == 0)
            vm->alen = 0;

        ps_bits_init(&b, code + body, alen);

        switch((ps_action)op) {

        /* --- flow ------------------------------------------------------- */
        case A_PLAY:
            if(vm->host->set_play)
                vm->host->set_play(vm->host->user, true);
            break;
        case A_STOP:
            if(vm->host->set_play)
                vm->host->set_play(vm->host->user, false);
            break;
        case A_NEXT_FRAME:
            if(vm->host->step_frame)
                vm->host->step_frame(vm->host->user, 1);
            break;
        case A_PREV_FRAME:
            if(vm->host->step_frame)
                vm->host->step_frame(vm->host->user, -1);
            break;
        case A_GOTO_FRAME: {
            /* Zero-based here, unlike every frame that arrives on the stack. */
            uint16_t f = ps_bits_u16(&b);

            if(b.over) {
                fault(vm, PS_SWF_ACT_TRUNCATED, pc);
                return false;
            }
            if(vm->host->goto_frame)
                vm->host->goto_frame(vm->host->user, (int)f);
            break;
        }
        case A_GOTO_LABEL: {
            const char *label = read_str(&b);
            int         f;

            if(!label) {
                fault(vm, PS_SWF_ACT_TRUNCATED, pc);
                return false;
            }
            f = vm->host->find_label
                    ? vm->host->find_label(vm->host->user, label) : -1;
            if(f >= 0 && vm->host->goto_frame)
                vm->host->goto_frame(vm->host->user, f);
            break;
        }
        case A_GOTO_FRAME2: {
            uint8_t      flags = ps_bits_u8(&b);
            int          bias  = (flags & 2) ? (int)ps_bits_u16(&b) : 0;
            ps_swf_value v     = pop_value(vm);
            int          f;

            if(b.over) {
                fault(vm, PS_SWF_ACT_TRUNCATED, pc);
                return false;
            }
            f = resolve_frame(vm, &v, bias);
            if(f >= 0 && vm->host->goto_frame)
                vm->host->goto_frame(vm->host->user, f);
            /* The play flag applies whether or not the frame resolved: it is
             * the gotoAndPlay/gotoAndStop distinction and it is not
             * conditional on the target existing. */
            if(vm->host->set_play)
                vm->host->set_play(vm->host->user, (flags & 1) != 0);
            break;
        }
        case A_WAIT_FOR_FRAME: {
            uint16_t f    = ps_bits_u16(&b);
            uint8_t  skip = ps_bits_u8(&b);

            if(b.over) {
                fault(vm, PS_SWF_ACT_TRUNCATED, pc);
                return false;
            }
            /* No host to ask means everything is loaded, which is the right
             * default: a preloader that never advances is worse than one that
             * advances early. */
            if(vm->host->frame_ready &&
               !vm->host->frame_ready(vm->host->user, (int)f))
                next = skip_actions(code, len, next, skip);
            break;
        }
        case A_WAIT_FOR_FRAME2: {
            uint8_t      skip = ps_bits_u8(&b);
            ps_swf_value v    = pop_value(vm);
            int          f;

            if(b.over) {
                fault(vm, PS_SWF_ACT_TRUNCATED, pc);
                return false;
            }
            f = resolve_frame(vm, &v, 0);
            if(f >= 0 && vm->host->frame_ready &&
               !vm->host->frame_ready(vm->host->user, f))
                next = skip_actions(code, len, next, skip);
            break;
        }
        case A_JUMP: {
            int16_t off = (int16_t)ps_bits_u16(&b);

            if(b.over) {
                fault(vm, PS_SWF_ACT_TRUNCATED, pc);
                return false;
            }
            if(!jump_target(next, off, len, &next)) {
                fault(vm, PS_SWF_ACT_BAD_JUMP, pc);
                return false;
            }
            break;
        }
        case A_IF: {
            int16_t      off = (int16_t)ps_bits_u16(&b);
            ps_swf_value v   = pop_value(vm);

            if(b.over) {
                fault(vm, PS_SWF_ACT_TRUNCATED, pc);
                return false;
            }
            if(truthy(&v) && !jump_target(next, off, len, &next)) {
                fault(vm, PS_SWF_ACT_BAD_JUMP, pc);
                return false;
            }
            break;
        }
        case A_CALL: {
            ps_swf_value   v = pop_value(vm);
            int            f = resolve_frame(vm, &v, 0);
            const uint8_t *sub;
            size_t         slen;

            if(f < 0 || !vm->host->frame_code)
                break;
            if(!vm->host->frame_code(vm->host->user, f, &sub, &slen))
                break;
            if(vm->depth >= vm->max_depth) {
                fault(vm, PS_SWF_ACT_DEPTH_LIMIT, pc);
                return false;
            }
            vm->depth++;
            ok = run_block(vm, sub, slen);
            vm->depth--;
            /* Even on failure: the stack is inspected afterwards and must not
             * hold pointers into a block that has gone. */
            relocate(vm, sub, slen);
            if(vm->pool_owner == sub) {
                vm->npool      = 0;
                vm->pool_owner = nullptr;
            }
            break;
        }

        /* --- values ----------------------------------------------------- */
        case A_PUSH:
            ok = run_push(vm, &b);
            /* Tested before `ok`, not after: a Push whose string has no
             * terminator fails by producing no string, and the reason it
             * produced none is here rather than in the push. */
            if(b.over) {
                fault(vm, PS_SWF_ACT_TRUNCATED, pc);
                return false;
            }
            break;
        case A_POP:
            discard_values(vm, 1);
            break;
        case A_CONSTANT_POOL: {
            uint32_t n = ps_bits_u16(&b), i;

            vm->npool      = 0;
            vm->pool_owner = code;
            for(i = 0; i < n && !b.over; i++) {
                const char *s = read_str(&b);

                if(!s)
                    break;
                /* Entries past the cap are still read - that is what keeps the
                 * rest of the pool findable - and simply not retained. */
                if(vm->npool < PS_SWF_VM_POOL)
                    vm->pool[vm->npool++] = s;
            }
            break;
        }

        /* --- arithmetic ------------------------------------------------- */
        case A_ADD: {
            /* Arithmetic, not concatenation. See the header: the overloaded
             * add is Add2 and it is SWF 5. */
            ps_swf_value bv = pop_value(vm), av = pop_value(vm);

            ok = push_number(vm, ps_swf_value_number(&av) +
                                 ps_swf_value_number(&bv));
            break;
        }
        case A_SUBTRACT: {
            ps_swf_value bv = pop_value(vm), av = pop_value(vm);

            ok = push_number(vm, ps_swf_value_number(&av) -
                                 ps_swf_value_number(&bv));
            break;
        }
        case A_MULTIPLY: {
            ps_swf_value bv = pop_value(vm), av = pop_value(vm);

            ok = push_number(vm, ps_swf_value_number(&av) *
                                 ps_swf_value_number(&bv));
            break;
        }
        case A_DIVIDE: {
            ps_swf_value bv = pop_value(vm), av = pop_value(vm);
            double       d  = ps_swf_value_number(&bv);

            /* Division by zero produces a string rather than an infinity, and
             * scripts print it. Getting this wrong is invisible until a movie
             * divides by zero, at which point every later use of the result is
             * arithmetic on a number that should have been text.
             *
             * Only by zero. A divisor that is infinite or NaN goes through the
             * ordinary path, because "#ERROR#" is the format's answer to a
             * division that has no value, not to one whose value is unusual -
             * 3/Infinity is 0 and 3/NaN is NaN in the reference player. */
            if(d == 0.0)
                ok = push_text(vm, "#ERROR#");
            else
                ok = push_number(vm, ps_swf_value_number(&av) / d);
            break;
        }
        case A_EQUALS: {
            ps_swf_value bv = pop_value(vm), av = pop_value(vm);

            ok = push_flag(vm, ps_swf_value_number(&av) ==
                               ps_swf_value_number(&bv));
            break;
        }
        case A_LESS: {
            ps_swf_value bv = pop_value(vm), av = pop_value(vm);

            ok = push_flag(vm, ps_swf_value_number(&av) <
                               ps_swf_value_number(&bv));
            break;
        }
        case A_AND: {
            ps_swf_value bv = pop_value(vm), av = pop_value(vm);

            /* Logical, not bitwise, and it does not short-circuit: both
             * operands are already on the stack by the time this runs. */
            ok = push_flag(vm, truthy(&av) && truthy(&bv));
            break;
        }
        case A_OR: {
            ps_swf_value bv = pop_value(vm), av = pop_value(vm);

            ok = push_flag(vm, truthy(&av) || truthy(&bv));
            break;
        }
        case A_NOT: {
            ps_swf_value v = pop_value(vm);

            ok = push_flag(vm, !truthy(&v));
            break;
        }
        case A_TO_INTEGER: {
            ps_swf_value v = pop_value(vm);

            ok = push_number(vm, to_int32(ps_swf_value_number(&v)));
            break;
        }

        /* --- strings ---------------------------------------------------- */
        case A_STRING_ADD: {
            ps_swf_value bv = pop_value(vm), av = pop_value(vm);
            const char  *a  = ps_swf_value_text(&av, vm->tmp[0], PS_SWF_VM_TEXT);
            const char  *bs = ps_swf_value_text(&bv, vm->tmp[1], PS_SWF_VM_TEXT);

            ok = push_text(vm, arena_cat(vm, a, strlen(a), bs, strlen(bs)));
            break;
        }
        case A_STRING_EQUALS: {
            ps_swf_value bv = pop_value(vm), av = pop_value(vm);
            const char  *a  = ps_swf_value_text(&av, vm->tmp[0], PS_SWF_VM_TEXT);
            const char  *bs = ps_swf_value_text(&bv, vm->tmp[1], PS_SWF_VM_TEXT);

            ok = push_flag(vm, strcmp(a, bs) == 0);
            break;
        }
        case A_STRING_LESS: {
            ps_swf_value bv = pop_value(vm), av = pop_value(vm);
            const char  *a  = ps_swf_value_text(&av, vm->tmp[0], PS_SWF_VM_TEXT);
            const char  *bs = ps_swf_value_text(&bv, vm->tmp[1], PS_SWF_VM_TEXT);

            ok = push_flag(vm, strcmp(a, bs) < 0);
            break;
        }
        case A_STRING_LENGTH:
        case A_MB_STRING_LENGTH: {
            /* The MB form counts characters in the player's code page and this
             * one counts bytes. They are the same function here because
             * nothing in this player knows what code page a file is in yet -
             * and for the Latin-1 content this targets they are also the same
             * answer. A Shift-JIS movie would read long. */
            ps_swf_value v = pop_value(vm);
            const char  *s = ps_swf_value_text(&v, vm->tmp[0], PS_SWF_VM_TEXT);

            ok = push_number(vm, (double)strlen(s));
            break;
        }
        case A_STRING_EXTRACT:
        case A_MB_STRING_EXTRACT: {
            ps_swf_value cv = pop_value(vm);
            ps_swf_value iv = pop_value(vm);
            ps_swf_value sv = pop_value(vm);
            const char  *s  = ps_swf_value_text(&sv, vm->tmp[0], PS_SWF_VM_TEXT);
            size_t       slen = strlen(s);
            long         i1 = clamp_long(ps_swf_value_number(&iv));
            long         cn = clamp_long(ps_swf_value_number(&cv));
            size_t       start, count;

            /* The index is one-based - the only one-based index in the whole
             * action set apart from the frame operands. Out of range clamps
             * rather than failing, because Flash returns a short string and a
             * script that walks off the end of one expects to get "". */
            if(i1 < 1)
                i1 = 1;
            start = (size_t)(i1 - 1);
            if(start > slen)
                start = slen;
            count = cn < 0 ? 0 : (size_t)cn;
            if(count > slen - start)
                count = slen - start;
            ok = push_text(vm, arena_cat(vm, s + start, count, nullptr, 0));
            break;
        }
        case A_CHAR_TO_ASCII:
        case A_MB_CHAR_TO_ASCII: {
            ps_swf_value v = pop_value(vm);
            const char  *s = ps_swf_value_text(&v, vm->tmp[0], PS_SWF_VM_TEXT);

            ok = push_number(vm, (double)(unsigned char)s[0]);
            break;
        }
        case A_ASCII_TO_CHAR:
        case A_MB_ASCII_TO_CHAR: {
            ps_swf_value v = pop_value(vm);
            unsigned     c = (unsigned)clamp_long(ps_swf_value_number(&v)) & 0xffu;

            /* Code 0 would be a string containing a terminator, which this
             * representation cannot hold and which no SWF string can either -
             * Push's own string type is NUL-terminated. It becomes "", and it
             * costs no arena to say so. */
            if(c == 0) {
                ok = push_text(vm, "");
            } else {
                char one[2] = { (char)c, '\0' };

                ok = push_text(vm, arena_copy(vm, one));
            }
            break;
        }

        /* --- variables and properties ----------------------------------- */
        case A_GET_VARIABLE: {
            ps_swf_value v    = pop_value(vm);
            const char  *name = ps_swf_value_text(&v, vm->tmp[0], PS_SWF_VM_TEXT);

            if(vm->host->get_var &&
               vm->host->get_var(vm->host->user, name, vm->io, PS_SWF_VM_TEXT)) {
                vm->io[PS_SWF_VM_TEXT - 1] = '\0';   /* a careless host */
                ok = push_host_text(vm, vm->io);
            } else {
                ok = push_kind(vm, PS_SWF_V_UNDEF);
            }
            break;
        }
        case A_SET_VARIABLE: {
            ps_swf_value vv = pop_value(vm), nv = pop_value(vm);
            const char  *name = ps_swf_value_text(&nv, vm->tmp[0], PS_SWF_VM_TEXT);
            const char  *val  = ps_swf_value_text(&vv, vm->tmp[1], PS_SWF_VM_TEXT);

            if(vm->host->set_var)
                vm->host->set_var(vm->host->user, name, val);
            break;
        }
        case A_GET_PROPERTY: {
            ps_swf_value iv = pop_value(vm), tv = pop_value(vm);
            const char  *target = ps_swf_value_text(&tv, vm->tmp[0],
                                                    PS_SWF_VM_TEXT);
            int          prop = (int)clamp_long(ps_swf_value_number(&iv));

            if(vm->host->get_prop &&
               vm->host->get_prop(vm->host->user, target, prop, vm->io,
                                  PS_SWF_VM_TEXT)) {
                vm->io[PS_SWF_VM_TEXT - 1] = '\0';
                ok = push_host_text(vm, vm->io);
            } else {
                ok = push_kind(vm, PS_SWF_V_UNDEF);
            }
            break;
        }
        case A_SET_PROPERTY: {
            ps_swf_value vv = pop_value(vm), iv = pop_value(vm), tv = pop_value(vm);
            const char  *target = ps_swf_value_text(&tv, vm->tmp[0],
                                                    PS_SWF_VM_TEXT);
            const char  *val    = ps_swf_value_text(&vv, vm->tmp[1],
                                                    PS_SWF_VM_TEXT);
            int          prop   = (int)clamp_long(ps_swf_value_number(&iv));

            if(vm->host->set_prop)
                vm->host->set_prop(vm->host->user, target, prop, val);
            break;
        }

        /* --- movie control ---------------------------------------------- */
        case A_SET_TARGET: {
            const char *t = read_str(&b);

            if(!t) {
                fault(vm, PS_SWF_ACT_TRUNCATED, pc);
                return false;
            }
            if(vm->host->set_target)
                vm->host->set_target(vm->host->user, t);
            vm->retargeted = (*t != '\0');
            break;
        }
        case A_SET_TARGET2: {
            ps_swf_value v = pop_value(vm);
            const char  *t = ps_swf_value_text(&v, vm->tmp[0], PS_SWF_VM_TEXT);

            if(vm->host->set_target)
                vm->host->set_target(vm->host->user, t);
            vm->retargeted = (*t != '\0');
            break;
        }
        case A_GET_URL: {
            const char *url    = read_str(&b);
            const char *target = url ? read_str(&b) : nullptr;

            if(!url || !target) {
                fault(vm, PS_SWF_ACT_TRUNCATED, pc);
                return false;
            }
            if(vm->host->get_url)
                vm->host->get_url(vm->host->user, url, target, 0);
            break;
        }
        case A_GET_URL2: {
            unsigned     flags  = ps_bits_u8(&b);
            ps_swf_value tv     = pop_value(vm);
            ps_swf_value uv     = pop_value(vm);
            const char  *target = ps_swf_value_text(&tv, vm->tmp[0],
                                                    PS_SWF_VM_TEXT);
            const char  *url    = ps_swf_value_text(&uv, vm->tmp[1],
                                                    PS_SWF_VM_TEXT);

            if(b.over) {
                fault(vm, PS_SWF_ACT_TRUNCATED, pc);
                return false;
            }
            if(vm->host->get_url)
                vm->host->get_url(vm->host->user, url, target, flags);
            break;
        }
        case A_TRACE: {
            ps_swf_value v = pop_value(vm);
            const char  *s = ps_swf_value_text(&v, vm->tmp[0], PS_SWF_VM_TEXT);

            if(vm->host->trace)
                vm->host->trace(vm->host->user, s);
            break;
        }
        case A_STOP_SOUNDS:
            if(vm->host->stop_sounds)
                vm->host->stop_sounds(vm->host->user);
            break;
        case A_TOGGLE_QUALITY:
            if(vm->host->toggle_quality)
                vm->host->toggle_quality(vm->host->user);
            break;
        case A_RANDOM_NUMBER: {
            ps_swf_value v = pop_value(vm);
            long         m = clamp_long(ps_swf_value_number(&v));
            int          r = 0;

            if(m > 0 && vm->host->rand_below)
                r = vm->host->rand_below(vm->host->user,
                                         m > 0x7fffffff ? 0x7fffffff : (int)m);
            ok = push_number(vm, (double)r);
            break;
        }
        case A_GET_TIME:
            ok = push_number(vm, vm->host->time_ms
                                     ? vm->host->time_ms(vm->host->user) : 0.0);
            break;

        /* --- display list actions, parsed but not performed --------------
         *
         * These four move sprites about, and this player has no display list
         * to move them in. Their operands are still popped, exactly and in the
         * right order, because the alternative is not "the action does
         * nothing" - it is every subsequent action in the block reading the
         * wrong operands off a stack that no longer lines up. */
        case A_CLONE_SPRITE:
            discard_values(vm, 3);
            break;
        case A_REMOVE_SPRITE:
            discard_values(vm, 1);
            break;
        case A_START_DRAG: {
            ps_swf_value cons;

            discard_values(vm, 2);              /* target, lock centre */
            cons = pop_value(vm);
            if(truthy(&cons))
                discard_values(vm, 4);          /* y2, x2, y1, x1 */
            break;
        }
        case A_END_DRAG:
            break;

        /* --- refused ----------------------------------------------------- */
        case A_DEFINE_FUNCTION:
        case A_DEFINE_FUNCTION2:
        case A_WITH:
            /* The only actions in the format whose length field does not cover
             * their whole body: each is followed by a code block whose size is
             * stated *inside* the body, and the bytes of it sit after the
             * action rather than in it. Skipping the stated length would
             * therefore start executing the function body inline. Since none
             * of them can be run either - they are SWF 5, and this has no
             * scope chain to run them in - the block stops here, which is the
             * one honest outcome. */
            fault(vm, PS_SWF_ACT_UNSUPPORTED, pc);
            return false;

        default:
            /* Everything else is skippable and is skipped. Below 0x80 an
             * action has no body at all, and above it the length field says
             * where the next one starts, so stepping over an unknown opcode is
             * always structurally safe - it is only the stack effect that is
             * lost. That is the better trade for a browser: the unknown codes
             * a SWF 4 target actually meets are SWF 5 and later ones in a file
             * that overstates its needs, and running most of such a file beats
             * running none of it. The count and the first code are reported so
             * it is never a silent trade. */
            vm->skipped++;
            if(vm->skipped == 1)
                vm->first_skipped = op;
            break;
        }

        /* Two conditions, because they catch different mistakes. `ok` is the
         * value a case chose to return; the fault test catches a case that
         * recorded a reason and then carried on - relocate below is the one
         * that genuinely can, since it runs after its caller has finished. The
         * fault() call is a last net for a case that did neither, which would
         * otherwise stop the block and report success. */
        if(!ok || vm->fault != PS_SWF_ACT_OK) {
            fault(vm, PS_SWF_ACT_TRUNCATED, pc);
            return false;
        }
        pc = next;
    }
    return true;                                /* ran off the end: normal */
}

/* --- entry points --------------------------------------------------------- */

void ps_swf_action_init(ps_swf_vm *vm, const ps_swf_host *host)
{
    memset(vm, 0, sizeof *vm);
    vm->host      = host ? host : &ps_swf_host_none;
    vm->max_steps = PS_SWF_VM_STEPS;
    vm->max_depth = PS_SWF_VM_DEPTH;
}

bool ps_swf_action_run(ps_swf_vm *vm, const uint8_t *code, size_t len,
                       ps_swf_action_result *res)
{
    bool ok;

    /* Cleared per block, not per VM. A block that faulted with six values on
     * the stack must not hand them to the next one - the two are separate
     * programs that happen to share a machine. */
    vm->sp            = 0;
    vm->alen          = 0;
    vm->steps         = 0;
    vm->depth         = 0;
    vm->skipped       = 0;
    vm->underflows    = 0;
    vm->first_skipped = 0;
    vm->retargeted    = false;
    vm->npool         = 0;
    vm->pool_owner    = nullptr;
    vm->fault         = PS_SWF_ACT_OK;
    vm->fault_pc      = 0;

    ok = (code && len) ? run_block(vm, code, len) : true;

    /* A SetTarget lasts to the end of its block and no further. Leaving it set
     * would silently redirect the next block's variables into some sprite. */
    if(vm->retargeted && vm->host->set_target)
        vm->host->set_target(vm->host->user, "");
    vm->retargeted = false;

    if(res) {
        res->status        = vm->fault;
        res->pc            = vm->fault_pc;
        res->steps         = vm->steps;
        res->skipped       = vm->skipped;
        res->underflows    = vm->underflows;
        res->first_skipped = vm->first_skipped;
    }
    return ok;
}

const char *ps_swf_action_status_name(ps_swf_act_status s)
{
    switch(s) {
    case PS_SWF_ACT_OK:             return "ok";
    case PS_SWF_ACT_TRUNCATED:      return "truncated action";
    case PS_SWF_ACT_BAD_JUMP:       return "jump outside the block";
    case PS_SWF_ACT_STACK_OVERFLOW: return "operand stack overflow";
    case PS_SWF_ACT_STEP_LIMIT:     return "action limit reached";
    case PS_SWF_ACT_DEPTH_LIMIT:    return "call depth limit reached";
    case PS_SWF_ACT_STRING_LIMIT:   return "string space exhausted";
    case PS_SWF_ACT_UNSUPPORTED:    return "action whose body cannot be skipped";
    }
    return "?";
}

int ps_swf_action_depth(const ps_swf_vm *vm)
{
    return vm->sp;
}

const ps_swf_value *ps_swf_action_peek(const ps_swf_vm *vm, int from_top)
{
    if(from_top < 0 || from_top >= vm->sp)
        return nullptr;
    return &vm->stack[vm->sp - 1 - from_top];
}
