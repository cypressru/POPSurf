/* ActionScript tests, and a fuzzer for the same machine.
 *
 * Every block below is assembled by hand, byte by byte, and every expected
 * answer is worked out on paper. That is deliberate and it is the whole point
 * of the file. The obvious alternative - write an assembler, emit a program,
 * run it, check the result - tests the assembler and the interpreter against
 * each other, and a misunderstanding shared by both (the operand order of
 * StringExtract, say, or which end of a SWF double comes first) cancels out
 * and passes. So the emitters here do nothing but lay down bytes: no opcode
 * knows what it means, and `br_to` computes a branch offset from two positions
 * rather than from anything the VM believes.
 *
 * What gets tested hardest is the SWF 4 value model, because that is the part
 * that fails quietly. A wrong Add does not crash a movie, it silently turns
 * arithmetic into string concatenation and the movie plays on with nonsense in
 * it - so there is a case here for every combination of number and string
 * operands on both adds, and on both comparisons.
 *
 *   ./actiontest             the tests
 *   ./actiontest fuzz [n] [seed]
 *                            random and truncated blocks; build it with
 *                            -fsanitize=address,undefined first - see the
 *                            Makefile's `fuzz` target
 */
#include "ps_swf_action.h"
#include "ps_swf.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

static void chk(const char *what, long got, long want)
{
    if(got != want) {
        printf("FAIL %-40s got %ld want %ld\n", what, got, want);
        fails++;
    }
}

static void chks(const char *what, const char *got, const char *want)
{
    if(strcmp(got, want) != 0) {
        printf("FAIL %-40s got \"%s\" want \"%s\"\n", what, got, want);
        fails++;
    }
}

/* --- block builder -------------------------------------------------------
 *
 * Bytes only. Nothing here shares a table, a constant or an assumption with
 * the interpreter; the opcode numbers are written at the call sites so that a
 * test reads as the byte sequence it is. */

typedef struct {
    uint8_t b[16384];
    size_t  n;
} blk;

/* Silently dropping bytes at the end of the buffer would make a test assert
 * against a program that is not the one it wrote, so overflow is a failure
 * here rather than a truncation. */
static void e8(blk *k, unsigned v)
{
    if(k->n >= sizeof k->b) {
        printf("FAIL test builder: block over %zu bytes\n", sizeof k->b);
        fails++;
        return;
    }
    k->b[k->n++] = (uint8_t)v;
}

/* Patching a length or a branch offset writes backwards into the buffer, so
 * every such site checks that the field it is aiming at is really there. */
static bool patch16(blk *k, size_t at, unsigned long v)
{
    if(at + 2 > k->n || at + 2 > sizeof k->b) {
        printf("FAIL test builder: patch at %zu outside the block\n", at);
        fails++;
        return false;
    }
    k->b[at]     = (uint8_t)(v & 0xff);
    k->b[at + 1] = (uint8_t)((v >> 8) & 0xff);
    return true;
}

static void e16(blk *k, unsigned v)
{
    e8(k, v & 0xff);
    e8(k, (v >> 8) & 0xff);
}

static void e32(blk *k, uint32_t v)
{
    e16(k, v & 0xffff);
    e16(k, v >> 16);
}

static void ebytes(blk *k, const void *p, size_t n)
{
    size_t i;

    for(i = 0; i < n; i++)
        e8(k, ((const unsigned char *)p)[i]);
}

/* An action with no body. Only legal below 0x80, which is the format's rule
 * and not this file's, so it is asserted rather than assumed. */
static void op0(blk *k, unsigned code)
{
    if(code >= 0x80) {
        printf("FAIL test builder: op0 on 0x%02x, which carries a length\n", code);
        fails++;
    }
    e8(k, code);
}

/* An action with a body: emit the opcode and a placeholder length, write the
 * body, then patch. Returns the position of the length field. */
static size_t opn(blk *k, unsigned code)
{
    size_t at;

    e8(k, code);
    at = k->n;
    e16(k, 0);
    return at;
}

/* An action at or above 0x80 with an empty body - Call is the only one. It
 * still carries a length field, which is exactly the sort of thing this file
 * exists to not assume. */
static void opz(blk *k, unsigned code);

static void opn_end(blk *k, size_t at)
{
    if(at + 2 <= k->n)
        (void)patch16(k, at, (unsigned long)(k->n - (at + 2)));
    else
        (void)patch16(k, at, 0);
}

static void opz(blk *k, unsigned code)
{
    if(code < 0x80) {
        printf("FAIL test builder: opz on 0x%02x, which has no length\n", code);
        fails++;
    }
    e8(k, code);
    e16(k, 0);
}

static void estr(blk *k, const char *s)
{
    ebytes(k, s, strlen(s) + 1);
}

/* Push, one value per call. Types are written as the raw tag byte so a reader
 * can check them against the spec table without leaving the line. */
static void push_str(blk *k, const char *s)
{
    size_t at = opn(k, 0x96);

    e8(k, 0);
    estr(k, s);
    opn_end(k, at);
}

/* Type 1, a 32-bit float - the only number a genuine SWF 4 file can push. */
static void push_f32(blk *k, double d)
{
    float    f = (float)d;
    uint32_t u;
    size_t   at = opn(k, 0x96);

    memcpy(&u, &f, sizeof u);
    e8(k, 1);
    e32(k, u);
    opn_end(k, at);
}

/* Type 6, a 64-bit double, written the way SWF writes one: two little-endian
 * 32-bit words with the high word first. Needed here for values a float cannot
 * hold exactly - 2^32 + 2 rounds to 2^32 in a float, which would make a test
 * of ToInteger's wrap prove nothing at all. */
static void push_f64(blk *k, double d)
{
    uint64_t u;
    size_t   at = opn(k, 0x96);

    memcpy(&u, &d, sizeof u);
    e8(k, 6);
    e32(k, (uint32_t)(u >> 32));
    e32(k, (uint32_t)(u & 0xffffffffu));
    opn_end(k, at);
}

/* A branch: opcode, length 2, and a placeholder offset. Returns the position
 * of the offset field so it can be aimed afterwards. */
static size_t br(blk *k, unsigned code)
{
    size_t at;

    e8(k, code);
    e16(k, 2);
    at = k->n;
    e16(k, 0);
    return at;
}

static void br_to(blk *k, size_t at, size_t target)
{
    /* Relative to the end of the branch action, which is two bytes past the
     * offset field. Computed from positions, never from a count of actions. */
    long off = (long)target - (long)(at + 2);

    (void)patch16(k, at, (unsigned long)off);
}

static void br_here(blk *k, size_t at)
{
    br_to(k, at, k->n);
}

/* --- the host ------------------------------------------------------------
 *
 * A variable table, a frame label table, and a log. The log is the real
 * assertion surface: it records what the script *asked the world to do*, in
 * order, which is exactly what a player has to get right and is nothing to do
 * with what any renderer would then draw. */

constexpr int  H_VARS  = 24;
constexpr int  H_LOG   = 64;
constexpr size_t H_TEXT = PS_SWF_VM_TEXT;

typedef struct {
    char   name[H_VARS][64];
    char   val[H_VARS][H_TEXT];
    int    nvar;

    char   log[H_LOG][160];
    int    nlog;

    const char *label[8];
    int         label_frame[8];
    int         nlabel;

    const uint8_t *fcode[8];
    size_t         flen[8];

    int  ready_below;     /* frames below this are loaded */
    int  rand_fixed;
    double now;
} host_t;

static host_t     H;
static ps_swf_host HOST;
static ps_swf_vm   VM;

static void hlog(const char *fmt, ...)
{
    va_list ap;

    if(H.nlog >= H_LOG)
        return;
    va_start(ap, fmt);
    vsnprintf(H.log[H.nlog], sizeof H.log[0], fmt, ap);
    va_end(ap);
    H.nlog++;
}

/* The whole log as one semicolon-joined string, so a test states the expected
 * sequence in one place and a missing or extra call is a visible diff. */
static const char *logtext(void)
{
    static char out[H_LOG * 40];
    int         i;
    size_t      n = 0;

    out[0] = '\0';
    for(i = 0; i < H.nlog; i++)
        n += (size_t)snprintf(out + n, sizeof out - n, "%s%s",
                              i ? ";" : "", H.log[i]);
    return out;
}

static int findvar(const char *name)
{
    int i;

    for(i = 0; i < H.nvar; i++)
        if(strcmp(H.name[i], name) == 0)
            return i;
    return -1;
}

static bool h_get_var(void *u, const char *name, char *out, size_t outlen)
{
    int i = findvar(name);

    (void)u;
    if(i < 0)
        return false;
    snprintf(out, outlen, "%s", H.val[i]);
    return true;
}

static void h_set_var(void *u, const char *name, const char *value)
{
    int i = findvar(name);

    (void)u;
    if(i < 0) {
        if(H.nvar >= H_VARS)
            return;
        i = H.nvar++;
        snprintf(H.name[i], sizeof H.name[0], "%s", name);
    }
    snprintf(H.val[i], sizeof H.val[0], "%s", value);
}

static bool h_get_prop(void *u, const char *target, int prop, char *out,
                       size_t outlen)
{
    (void)u;
    (void)target;
    /* _x reads 120.5 and _currentframe reads 7; everything else is absent, so
     * the undefined path gets exercised too. */
    if(prop == 0) {
        snprintf(out, outlen, "120.5");
        return true;
    }
    if(prop == 4) {
        snprintf(out, outlen, "7");
        return true;
    }
    return false;
}

static void h_set_prop(void *u, const char *target, int prop, const char *value)
{
    const char *name = ps_swf_property_name(prop);

    (void)u;
    hlog("setprop %s.%s=%s", target, name ? name : "?", value);
}

static void h_goto(void *u, int frame)      { (void)u; hlog("goto %d", frame); }
static void h_step(void *u, int d)          { (void)u; hlog("step %d", d); }
static void h_play(void *u, bool p)         { (void)u; hlog("play %d", p ? 1 : 0); }
static void h_target(void *u, const char *t){ (void)u; hlog("target %s", t); }
static void h_trace(void *u, const char *s) { (void)u; hlog("trace %s", s); }
static void h_sounds(void *u)               { (void)u; hlog("stopsounds"); }
static void h_quality(void *u)              { (void)u; hlog("quality"); }

static void h_url(void *u, const char *url, const char *target, unsigned flags)
{
    (void)u;
    hlog("url %s|%s|%u", url, target, flags);
}

static int h_label(void *u, const char *label)
{
    int i;

    (void)u;
    for(i = 0; i < H.nlabel; i++)
        if(strcmp(H.label[i], label) == 0)
            return H.label_frame[i];
    return -1;
}

static bool h_ready(void *u, int frame)
{
    (void)u;
    return frame < H.ready_below;
}

static bool h_fcode(void *u, int frame, const uint8_t **code, size_t *len)
{
    (void)u;
    if(frame < 0 || frame >= 8 || !H.fcode[frame])
        return false;
    *code = H.fcode[frame];
    *len  = H.flen[frame];
    return true;
}

static int h_rand(void *u, int max)
{
    (void)u;
    return H.rand_fixed % (max > 0 ? max : 1);
}

static double h_time(void *u) { (void)u; return H.now; }

static void reset(void)
{
    memset(&H, 0, sizeof H);
    H.now = 1234.0;

    memset(&HOST, 0, sizeof HOST);
    HOST.user           = &H;
    HOST.get_var        = h_get_var;
    HOST.set_var        = h_set_var;
    HOST.get_prop       = h_get_prop;
    HOST.set_prop       = h_set_prop;
    HOST.goto_frame     = h_goto;
    HOST.step_frame     = h_step;
    HOST.set_play       = h_play;
    HOST.find_label     = h_label;
    HOST.frame_ready    = h_ready;
    HOST.frame_code     = h_fcode;
    HOST.set_target     = h_target;
    HOST.get_url        = h_url;
    HOST.trace          = h_trace;
    HOST.stop_sounds    = h_sounds;
    HOST.toggle_quality = h_quality;
    HOST.rand_below     = h_rand;
    HOST.time_ms        = h_time;

    ps_swf_action_init(&VM, &HOST);
}

static ps_swf_action_result run(const blk *k)
{
    ps_swf_action_result r;

    (void)ps_swf_action_run(&VM, k->b, k->n, &r);
    return r;
}

/* --- reading results back ------------------------------------------------ */

static double topnum(int from_top)
{
    const ps_swf_value *v = ps_swf_action_peek(&VM, from_top);

    return v ? ps_swf_value_number(v) : -99999.0;
}

static const char *toptext(int from_top)
{
    static char buf[PS_SWF_VM_TEXT];
    const ps_swf_value *v = ps_swf_action_peek(&VM, from_top);

    return v ? ps_swf_value_text(v, buf, sizeof buf) : "<empty>";
}

static const char *varval(const char *name)
{
    int i = findvar(name);

    return i < 0 ? "<unset>" : H.val[i];
}

/* --- the SWF 4 value model ----------------------------------------------- */

/* One binary operator over two operands, each supplied as either a pushed
 * float or a pushed string, so the four combinations of a duality test are one
 * line each at the call site. */
static double binop_num(unsigned code, int a_is_str, const char *a_s, double a_n,
                        int b_is_str, const char *b_s, double b_n)
{
    blk k = { 0 };

    if(a_is_str) push_str(&k, a_s); else push_f32(&k, a_n);
    if(b_is_str) push_str(&k, b_s); else push_f32(&k, b_n);
    op0(&k, code);
    reset();
    (void)run(&k);
    return topnum(0);
}

static const char *binop_text(unsigned code,
                              int a_is_str, const char *a_s, double a_n,
                              int b_is_str, const char *b_s, double b_n)
{
    blk k = { 0 };

    if(a_is_str) push_str(&k, a_s); else push_f32(&k, a_n);
    if(b_is_str) push_str(&k, b_s); else push_f32(&k, b_n);
    op0(&k, code);
    reset();
    (void)run(&k);
    return toptext(0);
}

static void test_duality(void)
{
    /* Add, 0x0a. Arithmetic in every combination - this is the single most
     * important set of assertions in the file. If a "3" + "4" here ever reads
     * 34, the machine has been given SWF 5's Add2 semantics and every numeric
     * expression in every Flash 4 file is quietly wrong. */
    chk("add num+num",  (long)binop_num(0x0a, 0, "", 3, 0, "", 4),      7);
    chk("add str+str",  (long)binop_num(0x0a, 1, "3", 0, 1, "4", 0),    7);
    chk("add num+str",  (long)binop_num(0x0a, 0, "", 3, 1, "4", 0),     7);
    chk("add str+num",  (long)binop_num(0x0a, 1, "3", 0, 0, "", 4),     7);
    chk("add nonnum",   (long)binop_num(0x0a, 1, "abc", 0, 1, "def", 0), 0);
    /* Flash 4 takes the numeric prefix where ECMAScript would give NaN. */
    chk("add prefix",   (long)binop_num(0x0a, 1, "12abc", 0, 0, "", 1), 13);
    chk("add trailing", (long)binop_num(0x0a, 1, "  7  ", 0, 0, "", 1),  8);

    /* StringAdd, 0x21, is the one that concatenates - in every combination. */
    chks("cat num&num", binop_text(0x21, 0, "", 3, 0, "", 4),      "34");
    chks("cat str&str", binop_text(0x21, 1, "3", 0, 1, "4", 0),    "34");
    chks("cat num&str", binop_text(0x21, 0, "", 3, 1, "4", 0),     "34");
    chks("cat str&num", binop_text(0x21, 1, "ab", 0, 0, "", 4),    "ab4");
    /* An integral float must print without a decimal point, or every number
     * concatenated into a URL comes out as "12.000000". */
    chks("cat integral", binop_text(0x21, 0, "", 12, 1, "px", 0),  "12px");
    chks("cat fraction", binop_text(0x21, 0, "", 0.5, 1, "!", 0),  "0.5!");

    /* Comparison splits the same way, and the two disagree on exactly the
     * inputs where it matters: "10" and "9". */
    chk("less numeric 10<9",  (long)binop_num(0x0f, 1, "10", 0, 1, "9", 0), 0);
    chk("less string 10<9",   (long)binop_num(0x29, 1, "10", 0, 1, "9", 0), 1);
    chk("less numeric 9<10",  (long)binop_num(0x0f, 1, "9", 0, 1, "10", 0), 1);
    chk("equals numeric",     (long)binop_num(0x0e, 1, "1.0", 0, 0, "", 1), 1);
    chk("stringequals differ", (long)binop_num(0x13, 1, "1.0", 0, 0, "", 1), 0);
    chk("stringequals same",  (long)binop_num(0x13, 1, "ab", 0, 1, "ab", 0), 1);

    /* Comparisons yield the numbers 1 and 0, not a boolean: SWF 4 has no
     * boolean type at all. */
    chks("less yields a number", binop_text(0x0f, 0, "", 1, 0, "", 2), "1");

    chk("sub",  (long)binop_num(0x0b, 0, "", 10, 0, "", 4),  6);
    chk("mul",  (long)binop_num(0x0c, 0, "", 10, 0, "", 4), 40);
    chk("div",  (long)binop_num(0x0d, 0, "", 10, 0, "", 4),  2);  /* 2.5 -> 2 */

    /* Division by zero is a *string*, because SWF 4 has no way to write an
     * infinity down. That is not the same as having no infinities. */
    chks("div by zero", binop_text(0x0d, 0, "", 1, 0, "", 0), "#ERROR#");
    chks("0/0",         binop_text(0x0d, 0, "", 0, 0, "", 0), "#ERROR#");

    /* A non-finite cannot be written and can still be reached - Push's FLOAT
     * tag is SWF 4 and carries one directly, which is how these operands get
     * onto the stack. Every line below is a case from Ruffle's
     * avm1/divide_swf4, whose expected output is a real player's trace of a
     * version 4 file that Ruffle does not list as a known failure. Clamping to
     * DBL_MAX instead gave 1.6688053938804e-308 for the first of them. */
    {
        double inf = (double)INFINITY;
        double nan = (double)NAN;

        chks("3/inf",    binop_text(0x0d, 0, "", 3, 0, "", inf),   "0");
        chks("inf/3",    binop_text(0x0d, 0, "", inf, 0, "", 3),   "Infinity");
        chks("3/-inf",   binop_text(0x0d, 0, "", 3, 0, "", -inf),  "0");
        chks("-inf/3",   binop_text(0x0d, 0, "", -inf, 0, "", 3),  "-Infinity");
        chks("3/nan",    binop_text(0x0d, 0, "", 3, 0, "", nan),   "NaN");
        chks("nan/3",    binop_text(0x0d, 0, "", nan, 0, "", 3),   "NaN");
        chks("inf/inf",  binop_text(0x0d, 0, "", inf, 0, "", inf), "NaN");
        chks("inf/-inf", binop_text(0x0d, 0, "", inf, 0, "", -inf), "NaN");
        /* Not from the corpus but from the rule the corpus confirms: the
         * string is what division *by zero* yields, so a non-finite dividend
         * over zero is still "#ERROR#" and not "Infinity". */
        chks("inf/0",    binop_text(0x0d, 0, "", inf, 0, "", 0),   "#ERROR#");

        /* NaN compares false against everything including itself, which is
         * IEEE and is also what a SWF 4 Equals does once a NaN can exist. */
        chk("nan==nan",  (long)binop_num(0x0e, 0, "", nan, 0, "", nan), 0);
        chk("nan<3",     (long)binop_num(0x0f, 0, "", nan, 0, "", 3),   0);
    }

    /* The conversion is unchanged and is the half that was never in question:
     * Flash 4 reads a numeric prefix or zero, so the *spellings* of the
     * non-finites are not numbers on the way in even though they are on the
     * way out. "Infinity" + 1 is 1, not infinite. */
    chk("\"Infinity\"+1", (long)binop_num(0x0a, 1, "Infinity", 0, 0, "", 1), 1);
    chk("\"NaN\"+1",      (long)binop_num(0x0a, 1, "NaN", 0, 0, "", 1),      1);

    chk("and t,t", (long)binop_num(0x10, 0, "", 1, 0, "", 2), 1);
    chk("and t,f", (long)binop_num(0x10, 0, "", 1, 0, "", 0), 0);
    chk("or f,f",  (long)binop_num(0x11, 0, "", 0, 0, "", 0), 0);
    chk("or f,t",  (long)binop_num(0x11, 0, "", 0, 0, "", 3), 1);
    /* A string's truth is its number below SWF 7, so "0" and "abc" are false. */
    chk("and \"0\"",   (long)binop_num(0x10, 1, "0", 0, 0, "", 1), 0);
    chk("and \"abc\"", (long)binop_num(0x10, 1, "abc", 0, 0, "", 1), 0);
    chk("and \"1\"",   (long)binop_num(0x10, 1, "1", 0, 0, "", 1), 1);
}

/* --- Push, every type tag ------------------------------------------------ */

static void test_push_types(void)
{
    blk k = { 0 };
    size_t at;
    uint32_t f32bits;
    float    f = 2.5f;
    /* 1.5 as an IEEE double is 0x3ff8000000000000. SWF writes it as two
     * little-endian 32-bit words with the halves swapped, so the high word
     * 0x3ff80000 comes first. Written out rather than computed, because the
     * swap is the thing under test. */
    static const uint8_t d_1p5[8] = { 0x00, 0x00, 0xf8, 0x3f,
                                      0x00, 0x00, 0x00, 0x00 };

    memcpy(&f32bits, &f, sizeof f32bits);

    /* A constant pool first, so the constant8 and constant16 tags have
     * something to index. */
    at = opn(&k, 0x88);
    e16(&k, 2);
    estr(&k, "alpha");
    estr(&k, "beta");
    opn_end(&k, at);

    /* One Push action carrying nine values, which also exercises the inner
     * loop: the action's length is the only thing saying where it stops. */
    at = opn(&k, 0x96);
    e8(&k, 0); estr(&k, "hi");          /* string        */
    e8(&k, 1); e32(&k, f32bits);        /* float 2.5     */
    e8(&k, 2);                          /* null          */
    e8(&k, 3);                          /* undefined     */
    e8(&k, 4); e8(&k, 3);               /* register 3    */
    e8(&k, 5); e8(&k, 1);               /* boolean true  */
    e8(&k, 6); ebytes(&k, d_1p5, 8);    /* double 1.5    */
    e8(&k, 7); e32(&k, 0xfffffff6u);    /* int32 -10     */
    e8(&k, 8); e8(&k, 1);               /* constant "beta" */
    e8(&k, 9); e16(&k, 0);              /* constant "alpha" */
    opn_end(&k, at);

    reset();
    (void)run(&k);

    chk("push depth", ps_swf_action_depth(&VM), 10);
    chks("push const16", toptext(0), "alpha");
    chks("push const8",  toptext(1), "beta");
    chk("push int32",    (long)topnum(2), -10);
    chks("push double",  toptext(3), "1.5");
    chks("push bool",    toptext(4), "1");
    chks("push register is undefined", toptext(5), "");
    chks("push undefined", toptext(6), "");
    chks("push null",      toptext(7), "");
    chks("push float",     toptext(8), "2.5");
    chks("push string",    toptext(9), "hi");

    /* Both absent types read as zero and as the empty string, which is what a
     * SWF 4 expression would have done with an unset variable. */
    chk("null is 0",      (long)topnum(7), 0);
    chk("undefined is 0", (long)topnum(6), 0);
}

/* --- strings -------------------------------------------------------------- */

static void test_strings(void)
{
    blk k = { 0 };

    push_str(&k, "hello");
    op0(&k, 0x14);                        /* StringLength */
    reset();
    (void)run(&k);
    chk("stringlength", (long)topnum(0), 5);

    /* StringExtract's index is one-based - the only one-based index in the
     * action set apart from the frame operands taken off the stack. */
    memset(&k, 0, sizeof k);
    push_str(&k, "hello");
    push_f32(&k, 2);
    push_f32(&k, 3);
    op0(&k, 0x15);
    reset();
    (void)run(&k);
    chks("stringextract 2,3", toptext(0), "ell");

    memset(&k, 0, sizeof k);
    push_str(&k, "hello");
    push_f32(&k, 0);                      /* below the start: clamps to 1 */
    push_f32(&k, 99);                     /* past the end: clamps to 5 */
    op0(&k, 0x15);
    reset();
    (void)run(&k);
    chks("stringextract clamps", toptext(0), "hello");

    memset(&k, 0, sizeof k);
    push_str(&k, "hello");
    push_f32(&k, 9);                      /* start past the end */
    push_f32(&k, 2);
    op0(&k, 0x15);
    reset();
    (void)run(&k);
    chks("stringextract past end", toptext(0), "");

    memset(&k, 0, sizeof k);
    push_str(&k, "hello");
    op0(&k, 0x31);                        /* MBStringLength */
    push_str(&k, "wor");
    push_f32(&k, 1);
    push_f32(&k, 2);
    op0(&k, 0x35);                        /* MBStringExtract */
    reset();
    (void)run(&k);
    chks("mbstringextract", toptext(0), "wo");
    chk("mbstringlength", (long)topnum(1), 5);

    memset(&k, 0, sizeof k);
    push_str(&k, "Abc");
    op0(&k, 0x32);                        /* CharToAscii */
    push_f32(&k, 66);
    op0(&k, 0x33);                        /* AsciiToChar */
    push_str(&k, "");
    op0(&k, 0x32);                        /* CharToAscii of "" */
    reset();
    (void)run(&k);
    chk("chartoascii empty", (long)topnum(0), 0);
    chks("asciitochar 66",   toptext(1), "B");
    chk("chartoascii A",     (long)topnum(2), 65);

    /* ToInteger truncates towards zero and then wraps modulo 2^32, which is
     * ECMA's ToInt32 and is what the action actually is. */
    memset(&k, 0, sizeof k);
    push_f32(&k, 3.75);
    op0(&k, 0x18);
    push_f32(&k, -3.75);
    op0(&k, 0x18);
    push_f64(&k, 4294967298.0);           /* 2^32 + 2, needs a double */
    op0(&k, 0x18);
    reset();
    (void)run(&k);
    chk("tointeger wraps", (long)topnum(0), 2);
    chk("tointeger -3.75", (long)topnum(1), -3);
    chk("tointeger 3.75",  (long)topnum(2), 3);
}

/* --- variables, properties, and the world -------------------------------- */

static void test_variables(void)
{
    blk k = { 0 };

    push_str(&k, "x");
    push_f32(&k, 42);
    op0(&k, 0x1d);                        /* SetVariable */
    push_str(&k, "x");
    op0(&k, 0x1c);                        /* GetVariable */
    push_str(&k, "nope");
    op0(&k, 0x1c);
    reset();
    (void)run(&k);
    chks("setvariable stored", varval("x"), "42");
    chks("unset reads empty",  toptext(0), "");
    chk("unset reads zero",    (long)topnum(0), 0);
    chk("getvariable",         (long)topnum(1), 42);

    memset(&k, 0, sizeof k);
    push_str(&k, "/clip");
    push_f32(&k, 0);                      /* _x */
    op0(&k, 0x22);                        /* GetProperty */
    push_str(&k, "/clip");
    push_f32(&k, 6);                      /* _alpha, which this host lacks */
    op0(&k, 0x22);
    push_str(&k, "/clip");
    push_f32(&k, 1);                      /* _y */
    push_f32(&k, 17);
    op0(&k, 0x23);                        /* SetProperty */
    reset();
    (void)run(&k);
    chks("absent property is undefined", toptext(0), "");
    chks("getproperty _x", toptext(1), "120.5");
    chks("setproperty log", logtext(), "setprop /clip._y=17");
}

static void test_movie_control(void)
{
    blk k = { 0 };

    op0(&k, 0x06);                        /* Play */
    op0(&k, 0x07);                        /* Stop */
    op0(&k, 0x04);                        /* NextFrame */
    op0(&k, 0x05);                        /* PreviousFrame */
    op0(&k, 0x08);                        /* ToggleQuality */
    op0(&k, 0x09);                        /* StopSounds */
    push_str(&k, "hi there");
    op0(&k, 0x26);                        /* Trace */
    reset();
    (void)run(&k);
    chks("simple controls", logtext(),
         "play 1;play 0;step 1;step -1;quality;stopsounds;trace hi there");

    /* GotoFrame's operand is zero-based; GotoFrame2's, taken off the stack, is
     * one-based. Both reach the host as zero-based, and asserting them in one
     * block is the only way that asymmetry stays fixed. */
    memset(&k, 0, sizeof k);
    {
        size_t at = opn(&k, 0x81);        /* GotoFrame 4 */
        e16(&k, 4);
        opn_end(&k, at);

        push_f32(&k, 5);                  /* gotoAndPlay(5) -> frame 4 */
        at = opn(&k, 0x9f);               /* GotoFrame2, PlayFlag */
        e8(&k, 1);
        opn_end(&k, at);

        push_str(&k, "intro");
        at = opn(&k, 0x9f);               /* GotoFrame2 by label, stop */
        e8(&k, 0);
        opn_end(&k, at);

        at = opn(&k, 0x8c);               /* GotoLabel */
        estr(&k, "outro");
        opn_end(&k, at);

        at = opn(&k, 0x8c);               /* GotoLabel, undefined */
        estr(&k, "missing");
        opn_end(&k, at);
    }
    reset();
    H.label[0] = "intro"; H.label_frame[0] = 2;
    H.label[1] = "outro"; H.label_frame[1] = 9;
    H.nlabel = 2;
    (void)run(&k);
    chks("frame targets", logtext(),
         "goto 4;goto 4;play 1;goto 2;play 0;goto 9");

    /* GotoFrame2 with a scene bias, which is added only to a numeric frame. */
    memset(&k, 0, sizeof k);
    {
        size_t at;

        push_f32(&k, 3);
        at = opn(&k, 0x9f);
        e8(&k, 3);                        /* PlayFlag | SceneBiasFlag */
        e16(&k, 10);
        opn_end(&k, at);
    }
    reset();
    (void)run(&k);
    chks("scene bias", logtext(), "goto 12;play 1");

    /* GetURL carries both strings in the tag; GetURL2 takes them off the stack
     * with the target on top, and its flags byte reaches the host intact. */
    memset(&k, 0, sizeof k);
    {
        size_t at = opn(&k, 0x83);
        estr(&k, "page.html");
        estr(&k, "_blank");
        opn_end(&k, at);

        push_str(&k, "post.cgi");
        push_str(&k, "_self");
        at = opn(&k, 0x9a);
        e8(&k, 0x42);                     /* POST | LoadTarget */
        opn_end(&k, at);
    }
    reset();
    (void)run(&k);
    chks("geturl", logtext(), "url page.html|_blank|0;url post.cgi|_self|66");

    /* SetTarget lasts to the end of the block and no further: the trailing
     * restore is the VM's, not the script's. */
    memset(&k, 0, sizeof k);
    {
        size_t at = opn(&k, 0x8b);
        estr(&k, "/sprite");
        opn_end(&k, at);
        push_str(&k, "/other");
        op0(&k, 0x20);                    /* SetTarget2 */
    }
    reset();
    (void)run(&k);
    chks("settarget", logtext(), "target /sprite;target /other;target ");

    /* Both are impure, so both come from the host - a script that uses them
     * has to stay assertable. */
    memset(&k, 0, sizeof k);
    push_f32(&k, 100);
    op0(&k, 0x30);                        /* RandomNumber */
    op0(&k, 0x34);                        /* GetTime */
    reset();
    H.rand_fixed = 37;
    (void)run(&k);
    chk("gettime", (long)topnum(0), 1234);
    chk("randomnumber", (long)topnum(1), 37);
}

/* --- flow ----------------------------------------------------------------- */

static void test_flow(void)
{
    blk    k = { 0 };
    size_t j;
    ps_swf_action_result r;

    /* An unconditional jump over a Push. If the offset were mismeasured by so
     * much as a byte the skipped Push would still decode - as something - so
     * the assertion is on the depth as well as the value. */
    push_f32(&k, 1);
    j = br(&k, 0x99);
    push_f32(&k, 999);
    br_here(&k, j);
    push_f32(&k, 2);
    reset();
    (void)run(&k);
    chk("jump depth", ps_swf_action_depth(&VM), 2);
    chk("jump top",   (long)topnum(0), 2);
    chk("jump under", (long)topnum(1), 1);

    /* If, both ways, with an operand that is a string - because truthiness
     * below SWF 7 is the numeric reading of the string and not its emptiness. */
    memset(&k, 0, sizeof k);
    push_str(&k, "0");                    /* false: numerically zero */
    j = br(&k, 0x9d);
    push_f32(&k, 111);
    br_here(&k, j);
    push_str(&k, "0.5");                  /* true */
    j = br(&k, 0x9d);
    push_f32(&k, 222);
    br_here(&k, j);
    reset();
    (void)run(&k);
    chk("if depth", ps_swf_action_depth(&VM), 1);
    chk("if not taken pushed", (long)topnum(0), 111);

    /* A countdown loop, which is the only thing in the action set that can
     * fail to terminate. 200 iterations of eleven actions after a three-action
     * prologue is 2203, and asserting that exactly is what proves the loop ran
     * the number of times arithmetic says it should. */
    {
        size_t top;

        memset(&k, 0, sizeof k);
        push_str(&k, "i");
        push_f32(&k, 200);
        op0(&k, 0x1d);                    /* i = 200 */

        top = k.n;
        push_str(&k, "i");                /* name for the store */
        push_str(&k, "i");
        op0(&k, 0x1c);                    /* GetVariable */
        push_f32(&k, 1);
        op0(&k, 0x0b);                    /* Subtract */
        op0(&k, 0x1d);                    /* i = i - 1 */

        push_f32(&k, 0);
        push_str(&k, "i");
        op0(&k, 0x1c);
        op0(&k, 0x0f);                    /* 0 < i */
        j = br(&k, 0x9d);
        br_to(&k, j, top);

        reset();
        r = run(&k);
        chk("loop status", r.status, PS_SWF_ACT_OK);
        chk("loop steps",  (long)r.steps, 3 + 200 * 11);
        chks("loop result", varval("i"), "0");
        chk("loop leaves nothing", ps_swf_action_depth(&VM), 0);
        /* Two hundred round trips through the host and the arena is untouched:
         * a canonical number comes back as a number, not as a copied string. */
        chk("counter loop costs no arena", (long)VM.alen, 0);
    }

    /* WaitForFrame skips whole actions, not bytes, when the frame is not in
     * yet - so the count has to be measured by decoding them. */
    memset(&k, 0, sizeof k);
    {
        size_t at = opn(&k, 0x8a);
        e16(&k, 5);                       /* wait for frame 5 */
        e8(&k, 2);                        /* skip two actions */
        opn_end(&k, at);
        push_f32(&k, 1);                  /* skipped */
        op0(&k, 0x06);                    /* skipped: Play */
        push_f32(&k, 2);                  /* not skipped */
    }
    reset();
    H.ready_below = 3;                    /* frame 5 is not loaded */
    (void)run(&k);
    chk("waitforframe skipped", ps_swf_action_depth(&VM), 1);
    chk("waitforframe top", (long)topnum(0), 2);
    chks("waitforframe skipped the Play", logtext(), "");

    reset();
    H.ready_below = 9;                    /* now it is */
    (void)run(&k);
    chk("waitforframe ran", ps_swf_action_depth(&VM), 2);
    chks("waitforframe ran the Play", logtext(), "play 1");

    /* WaitForFrame2 takes its frame off the stack, one-based. */
    memset(&k, 0, sizeof k);
    {
        size_t at;

        push_f32(&k, 6);                  /* frame 5, zero-based */
        at = opn(&k, 0x8d);
        e8(&k, 1);
        opn_end(&k, at);
        op0(&k, 0x06);                    /* Play, skipped if not loaded */
    }
    reset();
    H.ready_below = 3;
    (void)run(&k);
    chks("waitforframe2 skipped", logtext(), "");
}

static void test_call(void)
{
    static blk sub;
    blk        k = { 0 };
    ps_swf_action_result r;

    /* The called block leaves a string on the shared stack. Its bytes live in
     * `sub`, which the VM must not still be pointing at afterwards - so the
     * assertion is on where the pointer ends up, not only on its contents. */
    memset(&sub, 0, sizeof sub);
    push_f32(&sub, 42);
    push_str(&sub, "from the sub");

    push_f32(&k, 3);                      /* one-based: frame 2 */
    opz(&k, 0x9e);                        /* Call */

    reset();
    H.fcode[2] = sub.b;
    H.flen[2]  = sub.n;
    r = run(&k);
    chk("call status", r.status, PS_SWF_ACT_OK);
    chk("call depth",  ps_swf_action_depth(&VM), 2);
    chks("call left a string", toptext(0), "from the sub");
    chk("call left a number",  (long)topnum(1), 42);
    {
        const ps_swf_value *v = ps_swf_action_peek(&VM, 0);
        int inside = v && v->str >= VM.arena && v->str < VM.arena + PS_SWF_VM_ARENA;

        chk("callee string was relocated into the arena", inside, 1);
    }

    /* A frame with no code is not an error: a Call to a frame the file never
     * defined should leave the script running. */
    reset();
    r = run(&k);
    chk("call to nothing is ok", r.status, PS_SWF_ACT_OK);
    chk("call to nothing pushes nothing", ps_swf_action_depth(&VM), 0);
}

/* --- the limits ----------------------------------------------------------- */

static void test_limits(void)
{
    blk    k = { 0 };
    size_t j;
    int    i;
    ps_swf_action_result r;

    /* The one that matters: a script that never finishes. On a console with no
     * preemption this is not a slow page, it is a machine that never runs
     * anything again. */
    memset(&k, 0, sizeof k);
    j = br(&k, 0x99);
    br_to(&k, j, 0);                      /* jump to itself */
    reset();
    r = run(&k);
    chk("infinite loop status", r.status, PS_SWF_ACT_STEP_LIMIT);
    chk("infinite loop stopped at the budget", (long)r.steps,
        (long)PS_SWF_VM_STEPS);
    chks("infinite loop reported", ps_swf_action_status_name(r.status),
         "action limit reached");

    /* A backward jump past the start of the block, and a forward jump past its
     * end. Both are arbitrary reads if they are not checked. */
    memset(&k, 0, sizeof k);
    push_f32(&k, 1);
    j = br(&k, 0x99);
    (void)patch16(&k, j, 0x8000);         /* -32768 */
    reset();
    r = run(&k);
    chk("backward jump out of range", r.status, PS_SWF_ACT_BAD_JUMP);

    memset(&k, 0, sizeof k);
    j = br(&k, 0x99);
    (void)patch16(&k, j, 0x7fff);         /* +32767 */
    reset();
    r = run(&k);
    chk("forward jump out of range", r.status, PS_SWF_ACT_BAD_JUMP);

    /* Landing exactly on the end of the block is legal - it is how a loop's
     * exit branch is written - and must not be mistaken for the case above. */
    memset(&k, 0, sizeof k);
    j = br(&k, 0x99);
    br_to(&k, j, k.n);
    reset();
    r = run(&k);
    chk("jump to the end is fine", r.status, PS_SWF_ACT_OK);

    /* An action claiming more body than the block holds. */
    memset(&k, 0, sizeof k);
    e8(&k, 0x96);
    e16(&k, 400);
    e8(&k, 0);
    reset();
    r = run(&k);
    chk("overlong action", r.status, PS_SWF_ACT_TRUNCATED);

    /* A block that ends in the middle of an action header. */
    memset(&k, 0, sizeof k);
    e8(&k, 0x96);
    e8(&k, 1);
    reset();
    r = run(&k);
    chk("truncated header", r.status, PS_SWF_ACT_TRUNCATED);

    /* Push's own payload running off the end of an otherwise valid action. */
    memset(&k, 0, sizeof k);
    {
        size_t at = opn(&k, 0x96);
        e8(&k, 0);
        ebytes(&k, "unterminated", 12);   /* no NUL */
        opn_end(&k, at);
    }
    reset();
    r = run(&k);
    chk("unterminated push string", r.status, PS_SWF_ACT_TRUNCATED);

    /* More values than the stack holds. */
    memset(&k, 0, sizeof k);
    for(i = 0; i < PS_SWF_VM_STACK + 4; i++)
        push_f32(&k, i);
    reset();
    r = run(&k);
    chk("stack overflow status", r.status, PS_SWF_ACT_STACK_OVERFLOW);
    chk("stack stayed inside its bound", ps_swf_action_depth(&VM),
        PS_SWF_VM_STACK);

    /* Concatenation that is never allowed to reach an empty stack, so the
     * arena can never be reclaimed. This is the case the reclaim scheme cannot
     * serve, and it has to fail cleanly rather than overrun. */
    memset(&k, 0, sizeof k);
    {
        char big[201];

        memset(big, 'x', 200);
        big[200] = '\0';
        push_str(&k, big);
        for(i = 0; i < 40; i++) {
            push_str(&k, big);
            op0(&k, 0x21);                /* StringAdd */
        }
    }
    reset();
    r = run(&k);
    chk("arena exhaustion status", r.status, PS_SWF_ACT_STRING_LIMIT);
    chk("arena stayed inside its bound", (long)(VM.alen <= PS_SWF_VM_ARENA), 1);

    /* The same work, but storing to a variable so the stack empties between
     * statements. Two hundred bytes of result out of a four kilobyte arena. */
    memset(&k, 0, sizeof k);
    {
        size_t top;

        push_str(&k, "s");
        push_str(&k, "");
        op0(&k, 0x1d);
        push_str(&k, "n");
        push_f32(&k, 0);
        op0(&k, 0x1d);

        top = k.n;
        push_str(&k, "s");
        push_str(&k, "s");
        op0(&k, 0x1c);
        push_str(&k, "ab");
        op0(&k, 0x21);                    /* s = s & "ab" */
        op0(&k, 0x1d);

        push_str(&k, "n");
        push_str(&k, "n");
        op0(&k, 0x1c);
        push_f32(&k, 1);
        op0(&k, 0x0a);
        op0(&k, 0x1d);                    /* n = n + 1 */

        push_str(&k, "n");
        op0(&k, 0x1c);
        push_f32(&k, 100);
        op0(&k, 0x0f);                    /* n < 100 */
        j = br(&k, 0x9d);
        br_to(&k, j, top);

        push_str(&k, "s");
        op0(&k, 0x1c);
        op0(&k, 0x14);                    /* StringLength */
    }
    reset();
    r = run(&k);
    chk("reclaimed loop status", r.status, PS_SWF_ACT_OK);
    chk("reclaimed loop built 200 chars", (long)topnum(0), 200);

    /* Call recursion. Each level costs a C stack frame, and the depth cap is
     * what stops a file choosing how deep that goes. */
    {
        static blk rec;

        memset(&rec, 0, sizeof rec);
        push_f32(&rec, 1);                /* frame 0 */
        opz(&rec, 0x9e);                  /* Call, recursing into itself */

        reset();
        H.fcode[0] = rec.b;
        H.flen[0]  = rec.n;
        r = run(&rec);
        chk("call recursion status", r.status, PS_SWF_ACT_DEPTH_LIMIT);
    }

    /* The three actions whose length does not cover their own body. Skipping
     * one would start executing a function body as if it were top-level code. */
    memset(&k, 0, sizeof k);
    {
        size_t at = opn(&k, 0x9b);        /* DefineFunction */
        estr(&k, "f");
        e16(&k, 0);
        e16(&k, 4);                       /* code size, outside this action */
        opn_end(&k, at);
        push_f32(&k, 1);
    }
    reset();
    r = run(&k);
    chk("definefunction refused", r.status, PS_SWF_ACT_UNSUPPORTED);

    /* Everything else unknown is stepped over, because below 0x80 there is no
     * body and above it the length says where the next action is. */
    memset(&k, 0, sizeof k);
    push_f32(&k, 1);
    op0(&k, 0x60);                        /* unknown, no body */
    {
        size_t at = opn(&k, 0xa0);        /* unknown, with a body */
        ebytes(&k, "junk", 4);
        opn_end(&k, at);
    }
    push_f32(&k, 2);
    reset();
    r = run(&k);
    chk("unknown opcodes skipped", r.status, PS_SWF_ACT_OK);
    chk("skipped count", (long)r.skipped, 2);
    chk("first skipped reported", r.first_skipped, 0x60);
    chk("skipping kept the stream aligned", ps_swf_action_depth(&VM), 2);
    chk("skipping kept the values", (long)topnum(0), 2);

    /* Popping an empty stack yields undefined and is counted, not fatal:
     * exporters emit stray pops and Flash tolerates them. */
    memset(&k, 0, sizeof k);
    op0(&k, 0x17);
    op0(&k, 0x0a);                        /* Add on an empty stack */
    reset();
    r = run(&k);
    chk("underflow is not fatal", r.status, PS_SWF_ACT_OK);
    chk("underflow counted", (long)r.underflows, 3);
    chk("underflow yields zero", (long)topnum(0), 0);

    /* An empty block, and a block that is nothing but the End action. */
    reset();
    chk("empty block", ps_swf_action_run(&VM, nullptr, 0, &r) ? 1 : 0, 1);
    {
        static const uint8_t just_end[1] = { 0x00 };

        reset();
        chk("end action only",
            ps_swf_action_run(&VM, just_end, sizeof just_end, &r) ? 1 : 0, 1);
    }

    /* A VM with no host at all must run a script that uses every capability
     * and simply drop the effects - that is what makes the callbacks optional
     * rather than merely nullable. */
    memset(&k, 0, sizeof k);
    push_str(&k, "x");
    op0(&k, 0x1c);
    push_f32(&k, 1);
    opz(&k, 0x9e);
    op0(&k, 0x06);
    push_str(&k, "hi");
    op0(&k, 0x26);
    ps_swf_action_init(&VM, nullptr);
    r.status = PS_SWF_ACT_OK;
    chk("no host at all", ps_swf_action_run(&VM, k.b, k.n, &r) ? 1 : 0, 1);
    chk("no host status", r.status, PS_SWF_ACT_OK);
}

/* --- the tags ------------------------------------------------------------- */

/* A tag header, in whichever of the two forms fits. The short form packs the
 * length into the low six bits of the same word as the code, and 0x3f is the
 * escape to a separate 32-bit length rather than a length of 63. */
static void tag(blk *k, unsigned code, size_t len)
{
    if(len < 0x3f) {
        e16(k, (code << 6) | (unsigned)len);
    } else {
        e16(k, (code << 6) | 0x3f);
        e32(k, (uint32_t)len);
    }
}

/* One block of ActionScript that traces a distinguishing string, so the log
 * says which of several blocks ran. */
static void trace_block(blk *k, const char *what)
{
    memset(k, 0, sizeof *k);
    push_str(k, what);
    op0(k, 0x26);
}

/* A whole SWF, built here rather than by mkswf, because what is under test is
 * that DoAction and DoInitAction come out of the tag stream attached to the
 * right frame and the right timeline - not anything about shapes.
 *
 * The sprite is the point of the file. A DefineSprite carries its own tag
 * stream with its own ShowFrames, so a script inside one is on frame 2 of the
 * sprite and not on frame 2 of anything else. Collecting scripts anywhere but
 * in the timeline being walked would number them against the root's frames,
 * and the resulting movie would run the right code at the wrong time - which
 * is not a crash and would not show up anywhere else. */
static void test_tags(void)
{
    blk          f = { 0 };
    blk          a = { 0 };
    blk          b = { 0 };
    blk          c = { 0 };
    blk          sp = { 0 };
    ps_swf_movie m;
    char         err[128] = "";

    trace_block(&a, "root frame 1");
    trace_block(&b, "sprite frame 2");
    trace_block(&c, "init of 77");

    /* The sprite's own tag stream: two frames, then a script, then its End. */
    e16(&sp, 88);                         /* sprite id */
    e16(&sp, 2);                          /* declared frame count */
    tag(&sp, 1, 0);                       /* ShowFrame */
    tag(&sp, 1, 0);                       /* ShowFrame */
    tag(&sp, 12, b.n);
    ebytes(&sp, b.b, b.n);
    tag(&sp, 0, 0);                       /* End of sprite */

    ebytes(&f, "FWS", 3);
    e8(&f, 4);                            /* version */
    e32(&f, 0);                           /* file length, not checked */
    e8(&f, 0x00);                         /* RECT, nbits = 0 */
    e16(&f, 0x0c00);                      /* 12 fps */
    e16(&f, 2);                           /* frame count */

    tag(&f, 1, 0);                        /* ShowFrame: root frame 0 done */
    tag(&f, 12, a.n);                     /* DoAction, so root frame 1 */
    ebytes(&f, a.b, a.n);
    tag(&f, 59, c.n + 2);                 /* DoInitAction for sprite 77 */
    e16(&f, 77);
    ebytes(&f, c.b, c.n);
    tag(&f, 39, sp.n);                    /* DefineSprite 88 */
    ebytes(&f, sp.b, sp.n);
    tag(&f, 1, 0);                        /* ShowFrame */
    tag(&f, 0, 0);                        /* End */

    chk("movie parses", ps_swf_load(f.b, f.n, &m, err, sizeof err), 0);

    chk("one script on the root", (long)m.root.nact, 1);
    if(m.root.nact == 1) {
        chk("root script frame", m.root.acts[0].frame, 1);
        chk("root script is not an init", m.root.acts[0].sprite, 0);
        chk("root block length", (long)m.root.acts[0].len, (long)a.n);
        reset();
        (void)ps_swf_action_run(&VM, m.root.acts[0].code, m.root.acts[0].len,
                                nullptr);
        chks("root block runs", logtext(), "trace root frame 1");
    }

    /* Numbered against the sprite's two ShowFrames, not against the root's
     * one. If these ever agree by accident, raise the sprite's frame count. */
    chk("one sprite defined", (long)m.nsprite, 1);
    if(m.nsprite == 1) {
        chk("one script on the sprite", (long)m.sprites[0].nact, 1);
        chk("sprite has its own frames", (long)m.sprites[0].nframe, 2);
        if(m.sprites[0].nact == 1) {
            chk("sprite script frame", m.sprites[0].acts[0].frame, 2);
            reset();
            (void)ps_swf_action_run(&VM, m.sprites[0].acts[0].code,
                                    m.sprites[0].acts[0].len, nullptr);
            chks("sprite block runs", logtext(), "trace sprite frame 2");
        }
    }

    chk("one init block", (long)m.ninit, 1);
    if(m.ninit == 1) {
        chk("init has no frame", m.inits[0].frame, -1);
        chk("init names its sprite", m.inits[0].sprite, 77);
        reset();
        (void)ps_swf_action_run(&VM, m.inits[0].code, m.inits[0].len, nullptr);
        chks("init block runs", logtext(), "trace init of 77");
    }

    ps_swf_free(&m);
}

/* Scripts that act on a timeline other than the one they are written in, which
 * is the combination test_tags does not reach.
 *
 * test_tags proves a block is filed against the right frame of the right
 * timeline. This proves the other half: that a block filed against a sprite
 * stops that sprite, and that a block on the root which retargets first acts on
 * the sprite and then puts the target back. Those are the same two opcodes in
 * the two arrangements real Flash 4 content uses, and until now each end was
 * tested and the join was not.
 *
 * The retarget is asserted as a *sequence* rather than as a final state,
 * because the order is the whole content of it. "target box; goto 1; play 0;
 * target " says the Stop landed while the target was the sprite; the same three
 * calls in any other order describe a movie that stops the root instead, which
 * is a difference nothing downstream could recover from.
 *
 * The name is a string and stays one all the way through, and that is worth
 * stating rather than glossing: ps_swf_parse.c reads PlaceObject2's Name field
 * and discards it, so nothing in the parsed movie can resolve "box" to the
 * sprite this file places. The VM is right - a slash path is the host's to
 * resolve, per ps_swf_action.h - but the host has nothing to resolve it
 * against, and a real player will need the parser to keep that field. */
static void test_composite_scripts(void)
{
    blk          f = { 0 };
    blk          root = { 0 };
    blk          spr = { 0 };
    blk          sp = { 0 };
    ps_swf_movie m;
    char         err[128] = "";
    size_t       at;

    /* The root's script: step into the sprite, wind it to its second frame,
     * stop it there, and come back out. SetTarget's restore at the end of the
     * block is the VM's own, which is why there are four calls and not three. */
    at = opn(&root, 0x8b);                /* SetTarget "box" */
    estr(&root, "box");
    opn_end(&root, at);
    at = opn(&root, 0x81);                /* GotoFrame 1, zero-based */
    e16(&root, 1);
    opn_end(&root, at);
    op0(&root, 0x07);                     /* Stop */

    /* The sprite's own script, on its second frame: stop, with no target at
     * all. A block inside a sprite acts on the sprite it is in. */
    op0(&spr, 0x07);

    e16(&sp, 42);                         /* sprite id */
    e16(&sp, 2);                          /* declared frame count */
    tag(&sp, 1, 0);                       /* ShowFrame: sprite frame 0 done */
    tag(&sp, 12, spr.n);                  /* so this is sprite frame 1 */
    ebytes(&sp, spr.b, spr.n);
    tag(&sp, 1, 0);
    tag(&sp, 0, 0);

    ebytes(&f, "FWS", 3);
    e8(&f, 4);
    e32(&f, 0);
    e8(&f, 0x00);                         /* RECT, nbits = 0 */
    e16(&f, 0x0c00);
    e16(&f, 1);

    tag(&f, 39, sp.n);                    /* DefineSprite 42 */
    ebytes(&f, sp.b, sp.n);
    /* PlaceObject2 with HasCharacter and HasName, naming the instance "box".
     * The flag byte is written here rather than by a helper for the reason
     * mkswf.c gives: a generator sharing a table with the reader can agree with
     * it while both are wrong. */
    {
        blk p = { 0 };

        e8(&p, 0x22);                     /* HasName | HasCharacter */
        e16(&p, 1);                       /* depth */
        e16(&p, 42);                      /* character */
        estr(&p, "box");
        tag(&f, 26, p.n);
        ebytes(&f, p.b, p.n);
    }
    tag(&f, 12, root.n);                  /* DoAction on root frame 0 */
    ebytes(&f, root.b, root.n);
    tag(&f, 1, 0);                        /* ShowFrame */
    tag(&f, 0, 0);

    chk("composite movie parses", ps_swf_load(f.b, f.n, &m, err, sizeof err), 0);

    chk("the named sprite is placed", (long)m.root.nop, 1);
    chk("one script on the root", (long)m.root.nact, 1);
    if(m.root.nact == 1) {
        chk("root script is on frame 0", m.root.acts[0].frame, 0);
        reset();
        (void)ps_swf_action_run(&VM, m.root.acts[0].code, m.root.acts[0].len,
                                nullptr);
        chks("retarget, wind, stop, restore", logtext(),
             "target box;goto 1;play 0;target ");
    }

    chk("one sprite defined", (long)m.nsprite, 1);
    if(m.nsprite == 1 && m.sprites[0].nact == 1) {
        chk("the sprite's own script is on its frame 1",
            m.sprites[0].acts[0].frame, 1);
        reset();
        (void)ps_swf_action_run(&VM, m.sprites[0].acts[0].code,
                                m.sprites[0].acts[0].len, nullptr);
        /* No target call: a script inside a sprite is already there. */
        chks("the sprite stops itself", logtext(), "play 0");
    } else {
        chk("one script on the sprite",
            m.nsprite ? (long)m.sprites[0].nact : -1, 1);
    }

    ps_swf_free(&m);
}

/* --- fuzz ---------------------------------------------------------------- */

/* Deterministic, so a failure is reproducible from the seed printed in the
 * banner rather than from luck. */
static uint32_t rng;

static uint32_t rnd(void)
{
    rng = rng * 1664525u + 1013904223u;
    return rng >> 8;
}

/* Every run, whatever it was given, has to leave the machine inside its own
 * bounds. Checking that here rather than only trusting the sanitizers means a
 * bound that is merely nearly right still fails. */
static void fuzz_run(const uint8_t *code, size_t len)
{
    ps_swf_action_result r;

    reset();
    /* A frame that calls into itself, so Call is actually reachable during
     * fuzzing instead of always returning "no such frame". */
    H.fcode[0]    = code;
    H.flen[0]     = len;
    H.ready_below = 2;

    (void)ps_swf_action_run(&VM, code, len, &r);

    if(VM.sp < 0 || VM.sp > PS_SWF_VM_STACK) {
        printf("FAIL fuzz: stack pointer %d outside 0..%d\n", VM.sp,
               PS_SWF_VM_STACK);
        fails++;
    }
    if(VM.alen > PS_SWF_VM_ARENA) {
        printf("FAIL fuzz: arena %zu over %zu\n", VM.alen, PS_SWF_VM_ARENA);
        fails++;
    }
    if(r.steps > PS_SWF_VM_STEPS) {
        printf("FAIL fuzz: %u steps over the budget\n", r.steps);
        fails++;
    }
    if(VM.npool < 0 || VM.npool > PS_SWF_VM_POOL) {
        printf("FAIL fuzz: pool %d outside 0..%d\n", VM.npool, PS_SWF_VM_POOL);
        fails++;
    }
}

/* A block whose actions are structurally plausible - real opcodes, honest
 * lengths - with random operands. Purely random bytes spend most of their time
 * being rejected at the first action, so this is what actually reaches the
 * arithmetic, the arena and the branch checker. */
static void fuzz_plausible(blk *k)
{
    static const uint8_t codes[] = {
        0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e,
        0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x17, 0x18, 0x1c, 0x1d,
        0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x30,
        0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
        0x81, 0x83, 0x88, 0x8a, 0x8b, 0x8c, 0x8d, 0x96, 0x99, 0x9a, 0x9d,
        0x9e, 0x9f
    };
    unsigned n = rnd() % 40u + 1u;
    unsigned i;

    memset(k, 0, sizeof *k);
    for(i = 0; i < n && k->n < sizeof k->b - 64; i++) {
        unsigned c = codes[rnd() % (sizeof codes / sizeof codes[0])];

        if(c < 0x80) {
            e8(k, c);
        } else {
            size_t   at = opn(k, c);
            unsigned b  = rnd() % 12u;

            while(b--)
                e8(k, (uint8_t)rnd());
            /* Half the time, end the body with a terminator, so the string
             * readers sometimes succeed instead of always reporting a
             * truncated action. */
            if(rnd() & 1)
                e8(k, 0);
            opn_end(k, at);
        }
    }
}

/* A whole SWF whose tag stream is mostly the two script tags, plus sprites to
 * nest them in and ShowFrames to number them against.
 *
 * The interpreter fuzzing above never reaches ps_swf_load, and the collection
 * side has its own hazards that a block-level fuzzer cannot see: a block is
 * the only thing in the parser that owns heap of its own, it is freed from
 * three different arrays, and a sprite's blocks are freed through a different
 * path from the root's. A DoInitAction tag of exactly one byte - shorter than
 * the sprite ID it must carry - is the sort of thing that finds the rest. */
static void fuzz_movie(blk *k)
{
    unsigned ntag = rnd() % 24u + 1u;
    unsigned i;

    memset(k, 0, sizeof *k);
    ebytes(k, "FWS", 3);
    e8(k, (unsigned)(rnd() % 10u));       /* version */
    e32(k, 0);
    e8(k, 0x00);                          /* RECT, nbits = 0 */
    e16(k, 0x0c00);
    e16(k, (unsigned)(rnd() % 8u));

    for(i = 0; i < ntag && k->n < sizeof k->b - 128; i++) {
        static const unsigned tags[] = { 12, 12, 12, 59, 59, 1, 1, 39, 9, 2 };
        unsigned code = tags[rnd() % (sizeof tags / sizeof tags[0])];
        blk      body = { 0 };
        unsigned n;

        if(code == 39) {
            /* A sprite: an id, a frame count, then a couple of script tags of
             * its own, and sometimes no End tag at all. */
            unsigned j, inner = rnd() % 3u;

            e16(&body, (unsigned)(rnd() % 200u));
            e16(&body, (unsigned)(rnd() % 4u));
            for(j = 0; j < inner; j++) {
                unsigned m = rnd() % 8u;

                tag(&body, (rnd() & 1) ? 12u : 1u, m);
                while(m--)
                    e8(&body, (uint8_t)rnd());
            }
            if(rnd() & 1)
                tag(&body, 0, 0);
        } else {
            /* Deliberately including 0 and 1, which are shorter than the
             * sprite ID a DoInitAction is required to carry. */
            n = rnd() % 10u;
            while(n--)
                e8(&body, (uint8_t)rnd());
        }
        if(fails)
            return;                       /* the builder overflowed */
        tag(k, code, body.n);
        ebytes(k, body.b, body.n);
    }
    tag(k, 0, 0);
}

/* Load it, run everything it collected, and free it. Under ASan the free is as
 * much of the test as the load: the blocks are the only heap the parser hands
 * out per tag, and a sprite's are reached through a different pointer. */
static void fuzz_movie_run(const uint8_t *data, size_t len)
{
    ps_swf_movie m;
    char         err[128] = "";
    uint32_t     i;

    if(ps_swf_load(data, len, &m, err, sizeof err) < 0)
        return;                           /* refused, and nothing to free */

    for(i = 0; i < m.root.nact; i++) {
        reset();
        (void)ps_swf_action_run(&VM, m.root.acts[i].code, m.root.acts[i].len,
                                nullptr);
    }
    for(i = 0; i < m.nsprite; i++) {
        uint32_t j;

        for(j = 0; j < m.sprites[i].nact; j++) {
            reset();
            (void)ps_swf_action_run(&VM, m.sprites[i].acts[j].code,
                                    m.sprites[i].acts[j].len, nullptr);
        }
    }
    for(i = 0; i < m.ninit; i++) {
        reset();
        (void)ps_swf_action_run(&VM, m.inits[i].code, m.inits[i].len, nullptr);
    }
    ps_swf_free(&m);
}

static int fuzz_main(int iters, uint32_t seed)
{
    blk   k;
    int   i;
    long  t;

    rng = seed;
    printf("fuzzing %d blocks, seed %u\n", iters, seed);

    for(i = 0; i < iters; i++) {
        /* Uniformly random bytes: nonsense, and the first thing to try. */
        {
            size_t  n = rnd() % 512u;
            uint8_t buf[512];
            size_t  j;

            for(j = 0; j < n; j++)
                buf[j] = (uint8_t)rnd();
            fuzz_run(buf, n);
        }

        /* Plausible structure, random operands. */
        fuzz_plausible(&k);
        fuzz_run(k.b, k.n);

        /* Every truncation of that same block. A parser that reads one byte
         * past a length shows up here and nowhere else, because the bytes just
         * past the end are the ones the allocator did not give us. */
        for(t = (long)k.n; t >= 0; t--) {
            uint8_t *copy = malloc((size_t)t ? (size_t)t : 1);

            if(!copy)
                break;
            memcpy(copy, k.b, (size_t)t);
            fuzz_run(copy, (size_t)t);
            free(copy);
        }

        /* And a single flipped byte, which is how a length or a type tag
         * becomes something the generator would never have chosen. */
        if(k.n) {
            uint8_t save;
            size_t  at = rnd() % k.n;

            save = k.b[at];
            k.b[at] = (uint8_t)rnd();
            fuzz_run(k.b, k.n);
            k.b[at] = save;
        }

        /* A whole file, and every truncation of it, through the tag walker and
         * out again through ps_swf_free. */
        fuzz_movie(&k);
        if(fails)
            break;
        fuzz_movie_run(k.b, k.n);
        for(t = (long)k.n; t >= 0; t--) {
            uint8_t *copy = malloc((size_t)t ? (size_t)t : 1);

            if(!copy)
                break;
            memcpy(copy, k.b, (size_t)t);
            fuzz_movie_run(copy, (size_t)t);
            free(copy);
        }

        if(fails)
            break;
    }

    printf(fails ? "%d fuzz failure(s)\n" : "fuzz clean\n", fails);
    return fails ? 1 : 0;
}

int main(int argc, char **argv)
{
    if(argc > 1 && strcmp(argv[1], "fuzz") == 0)
        return fuzz_main(argc > 2 ? atoi(argv[2]) : 200,
                         argc > 3 ? (uint32_t)strtoul(argv[3], nullptr, 0)
                                  : 20260807u);

    test_push_types();
    test_duality();
    test_strings();
    test_variables();
    test_movie_control();
    test_flow();
    test_call();
    test_limits();
    test_tags();
    test_composite_scripts();

    printf(fails ? "%d failure(s)\n" : "all action tests pass\n", fails);
    return fails ? 1 : 0;
}
