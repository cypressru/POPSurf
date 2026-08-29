/* java.lang and java.util, in C.
 *
 * Everything here is written from the documented behaviour of the JDK 1.0/1.1
 * API and checked against a desktop JVM, applet by applet, under java/test.
 * That is not a formality: the interesting parts of these classes are the
 * edges - which index substring refuses, what parseInt does with a leading
 * space, where lastIndexOf counts from - and an applet that catches
 * StringIndexOutOfBoundsException is depending on the edge rather than the
 * middle. Guessing them produces something that works on the test page and
 * quietly draws the wrong thing on a real one, so each of them has been run
 * both ways and diffed.
 *
 * Three things about this file are worth knowing before changing it.
 *
 * The classes are registered here, not in ps_jvm.c. The interpreter resolves a
 * class name by looking through the ones already loaded before it asks its
 * loader, so putting a placeholder in vm->classes is enough to make
 * `new java/util/Vector` resolve - and it means the whole of java.util can be
 * added without touching the interpreter. Registration piggy-backs on the
 * super-constructor call every object creation makes; see register_classes.
 *
 * State lives in real field slots, not in ps_jobj::native. The collector marks
 * an instance by walking its class's field table and following the reference
 * ones, and it does not follow the native pointer at all - so a Vector holding
 * its elements in a malloc'd array would have them collected out from under
 * it. The classes registered here carry a synthetic field layout for exactly
 * that reason, and every container's storage is an ordinary Java array the
 * collector already knows how to trace.
 *
 * Arguments are pinned across allocation. A native call's arguments sit above
 * the caller frame's recorded stack pointer, which is what the collector
 * scans, so an object that exists only as an argument is not a root - and
 * `v.addElement(new Foo())` allocates while growing. The pin set is a handful
 * of static reference fields on a private class, which the collector does
 * scan.
 */
#include "ps_jlib.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _arch_dreamcast
#include <arch/timer.h>
#else
#include <sys/time.h>
#endif

/* Matches on name and, when it matters, descriptor. Passing NULL for the
 * descriptor accepts any overload. Same shape as the one in ps_jre.c - the two
 * files are dispatch tables and share the idiom rather than a symbol. */
static int is(const char *name, const char *desc, const char *n,
              const char *d)
{
    if(strcmp(name, n))
        return 0;
    return d ? !strcmp(desc, d) : 1;
}

/* What a handler returns: claimed and done, claimed and raised, or not mine.
 * Kept separate from *handled so a handler can never forget to clear it. */
#define LIB_NO      0
#define LIB_OK      1
#define LIB_RAISED  (-1)

/* --- the classes ---------------------------------------------------------
 *
 * A layout string gives each class its instance fields, one character each:
 *
 *   R  a reference       - traced by the collector, which is the point
 *   I  an int
 *   J  a long or double  - two slots, per JVMS 2.6.1
 *   S  a static reference
 *
 * Counts and cursors go in the object's own `len` field, which instances of a
 * native class have no other use for, rather than costing a slot each.
 */
static const struct { const char *name; const char *layout; } g_lib[] = {
    /* The pin set. Registered first, and its presence is what says the rest
     * are here too. The name is not one any class file can spell, so nothing
     * an applet ships can collide with it or reach it. */
    { "ps/jlib/Pins", "SSSSSSSSSSSSSSSS" },

    { "java/lang/Integer",   "J" },
    { "java/lang/Long",      "J" },
    { "java/lang/Short",     "J" },
    { "java/lang/Byte",      "J" },
    { "java/lang/Character", "J" },
    { "java/lang/Boolean",   "J" },
    { "java/lang/Double",    "J" },
    { "java/lang/Float",     "J" },

    /* Thrown from here, so they have to resolve for a handler to accept
     * one. */
    { "java/lang/StringIndexOutOfBoundsException", "" },
    { "java/util/NoSuchElementException",          "" },

    { "java/util/Vector",          "R" },   /* the backing Object[] */
    { "java/util/Hashtable",       "RR" },  /* keys[], values[] */
    { "java/util/Enumeration",     "RI" },  /* source, which view */
    { "java/util/StringTokenizer", "RRI" }, /* text, delimiters, flags */
    { "java/util/Random",          "JJI" }, /* seed, spare gaussian, have it */
    { "java/util/Date",            "J" },   /* milliseconds */
    { NULL, NULL }
};

#define PINS_CLASS   "ps/jlib/Pins"
#define PIN_ARGS     0     /* .. 7, the incoming arguments */
#define PIN_SCRATCH  8     /* .. 11, whatever a handler is holding */
#define PIN_INTERN   12    /* String.intern's table */
#define PIN_TRUE     13
#define PIN_FALSE    14

/* Looked up directly rather than through ps_jvm_class, which would hand an
 * unknown name to the class loader - and the loader is a network fetch. */
static ps_jclass *loaded(ps_jvm *vm, const char *name)
{
    int i;

    for(i = 0; i < vm->nclasses; i++) {
        if(!strcmp(vm->classes[i]->name, name))
            return vm->classes[i];
    }
    return NULL;
}

static ps_jclass *make_lib_class(ps_jvm *vm, const char *name,
                                 const char *layout)
{
    ps_jclass *c;
    size_t     n = strlen(layout);
    uint16_t   inst = 0, stat = 0;
    size_t     i;

    if(vm->nclasses >= PS_JVM_MAX_CLASSES)
        return NULL;

    c = (ps_jclass *)calloc(1, sizeof *c);
    if(!c)
        return NULL;

    /* The name is copied, as it is for any class with no file image behind
     * it, and freed with the class. */
    c->raw = (uint8_t *)malloc(strlen(name) + 1);
    if(!c->raw) {
        free(c);
        return NULL;
    }
    strcpy((char *)c->raw, name);
    c->name   = (const char *)c->raw;
    c->native = 1;

    if(n) {
        c->fields = (ps_jfield *)calloc(n, sizeof(ps_jfield));
        if(!c->fields) {
            free(c->raw);
            free(c);
            return NULL;
        }
        c->field_count = (uint16_t)n;

        for(i = 0; i < n; i++) {
            ps_jfield *f = &c->fields[i];

            /* Never looked up by name: bytecode cannot name these, and the
             * only thing that reads the table is the collector. */
            f->name = "$";
            f->desc = "";

            switch(layout[i]) {
            case 'R': f->kind = PS_T_REF;  f->slot = inst++;        break;
            case 'I': f->kind = PS_T_INT;  f->slot = inst++;        break;
            case 'J': f->kind = PS_T_LONG; f->slot = inst; inst += 2; break;
            case 'S':
                f->kind   = PS_T_REF;
                f->access = PS_ACC_STATIC;
                f->slot   = stat++;
                break;
            default:
                break;
            }
        }
    }

    c->inst_slots   = inst;
    c->static_slots = stat;

    if(stat) {
        c->statics = calloc(stat, sizeof(ps_jslot));
        if(!c->statics) {
            free(c->fields);
            free(c->raw);
            free(c);
            return NULL;
        }
    }

    vm->classes[vm->nclasses++] = c;
    return c;
}

/* Puts the whole set in the class table, once per VM.
 *
 * The trigger is any constructor reaching a class implemented in C, which for
 * an applet means `invokespecial java/applet/Applet.<init>` - the first
 * instruction of the first method a browser runs, before any field
 * initialiser and long before anything says `new Vector`. javac emits the
 * super call first in every constructor it writes, so there is no order in
 * which an applet can beat this.
 *
 * The pin class going in first is what makes the check cheap: if it is there
 * the rest are, so the common case is one failed name comparison per
 * construction.
 */
static void register_classes(ps_jvm *vm)
{
    int i;

    if(loaded(vm, PINS_CLASS))
        return;

    for(i = 0; g_lib[i].name; i++) {
        if(!loaded(vm, g_lib[i].name))
            make_lib_class(vm, g_lib[i].name, g_lib[i].layout);
    }
}

static ps_jslot *pins(ps_jvm *vm)
{
    ps_jclass *c = loaded(vm, PINS_CLASS);

    if(!c) {
        register_classes(vm);
        c = loaded(vm, PINS_CLASS);
    }
    return c ? (ps_jslot *)c->statics : NULL;
}

static void pin(ps_jvm *vm, int slot, ps_jobj *o)
{
    ps_jslot *p = pins(vm);

    if(p)
        p[slot].o = o;
}

/* Non-zero if a descriptor's result is a reference. Also answers for a field
 * descriptor, which is what a getstatic arrives with. */
static int desc_ref_result(const char *desc)
{
    const char *q = strchr(desc, ')');

    q = q ? q + 1 : desc;
    return *q == 'L' || *q == '[';
}

/* Roots this call's arguments for as long as the call runs.
 *
 * Which slots hold references is read off the descriptor rather than guessed.
 * It has to be exact: the collector follows a static reference field without
 * checking it first, so an int parked in the pin set that happened to look
 * like an address would be dereferenced. The interpreter's own frame scan can
 * afford to be conservative because it checks; this cannot.
 *
 * The pins are not cleared on the way out on purpose. The result of a native
 * call is pushed onto an operand stack the collector is not scanning either,
 * so leaving the last call's objects pinned carries them as far as the next
 * one - by which time the interpreter has recorded a stack pointer that covers
 * them. The cost is a dozen objects retained one call too long.
 */
static void pin_args(ps_jvm *vm, const char *desc, ps_jslot *args, int nargs)
{
    ps_jslot   *p = pins(vm);
    const char *q;
    int         slot = 0, i, want;
    uint8_t     rk;

    if(!p)
        return;

    for(i = 0; i < 12; i++)
        p[PIN_ARGS + i].o = NULL;

    if(!args || nargs <= 0 || !desc)
        return;

    want = ps_jdesc_arg_slots(desc, &rk);
    if(want < 0)
        return;

    /* An instance method has `this` in slot zero, which the descriptor does
     * not mention - so the slot count is how we tell the two apart. */
    if(nargs > want) {
        p[PIN_ARGS].o = args[0].o;
        slot = 1;
    }

    q = strchr(desc, '(');
    if(!q)
        return;

    for(q++; *q && *q != ')' && slot < 8; slot++) {
        int ref = 0;

        while(*q == '[') {
            ref = 1;
            q++;
        }
        if(*q == 'L') {
            ref = 1;
            while(*q && *q != ';')
                q++;
            if(*q)
                q++;
        }
        else {
            if(!ref && (*q == 'J' || *q == 'D'))
                slot++;                       /* two slots, neither a ref */
            q++;
        }
        if(ref && slot < nargs)
            p[PIN_ARGS + slot].o = args[slot].o;
    }
}

/* --- strings -------------------------------------------------------------
 *
 * A String here is what ps_jvm_new_string makes: bytes in `native`, length in
 * `len`, and PS_T_CHAR in `elem` to mark it as one. Text is UTF-8 and indices
 * are byte indices, so everything below is correct for ASCII - which is what
 * an applet's parameters, tokens and labels are - and would need a real char
 * array to be correct for anything else. That is a deliberate limit, not an
 * oversight: the rasteriser draws the same bytes.
 */

static int is_string(const ps_jobj *o)
{
    return o && o->kind == PS_OBJ_INSTANCE && o->elem == PS_T_CHAR && o->native;
}

static const char *sbytes(const ps_jobj *o, size_t *len)
{
    if(!o || !o->native) {
        if(len)
            *len = 0;
        return NULL;
    }
    if(len)
        *len = (size_t)o->len;
    return (const char *)o->native;
}

static ps_jobj *mkstr(ps_jvm *vm, const char *s, size_t len)
{
    return ps_jvm_new_string(vm, s, len);
}

static ps_jobj *mkcstr(ps_jvm *vm, const char *s)
{
    return ps_jvm_new_string(vm, s, strlen(s));
}

/* Two pieces at once. ps_jvm_new_string copies exactly the bytes it is given,
 * so a join has to be assembled before the String is made rather than after -
 * asking it for a longer string than the source is a read off the end. */
static ps_jobj *mkstr2(ps_jvm *vm, const char *a, size_t al, const char *b,
                       size_t bl)
{
    ps_jobj *o;
    char    *tmp = (char *)malloc(al + bl + 1);

    if(!tmp)
        return NULL;
    if(al)
        memcpy(tmp, a, al);
    if(bl)
        memcpy(tmp + al, b, bl);
    tmp[al + bl] = '\0';

    o = ps_jvm_new_string(vm, tmp, al + bl);
    free(tmp);
    return o;
}

static int throw_at(ps_jvm *vm, const char *cls, const char *what, long idx)
{
    char m[80];

    snprintf(m, sizeof m, "%s: %ld", what, idx);
    ps_jvm_throw(vm, cls, m);
    return LIB_RAISED;
}

static int throw_str_index(ps_jvm *vm, long idx)
{
    return throw_at(vm, "java/lang/StringIndexOutOfBoundsException",
                    "String index out of range", idx);
}

static int throw_arr_index(ps_jvm *vm, long idx)
{
    return throw_at(vm, "java/lang/ArrayIndexOutOfBoundsException",
                    "index", idx);
}

static int throw_nfe(ps_jvm *vm, const char *s, size_t len)
{
    char m[80];

    snprintf(m, sizeof m, "For input string: \"%.*s\"", (int)(len > 40 ? 40
                                                              : len),
             s ? s : "null");
    ps_jvm_throw(vm, "java/lang/NumberFormatException", m);
    return LIB_RAISED;
}

static int throw_npe(ps_jvm *vm, const char *what)
{
    ps_jvm_throw(vm, "java/lang/NullPointerException", what);
    return LIB_RAISED;
}

static int throw_nse(ps_jvm *vm)
{
    ps_jvm_throw(vm, "java/util/NoSuchElementException", NULL);
    return LIB_RAISED;
}

/* --- numbers to text -----------------------------------------------------
 *
 * Double.toString is a specified format, not whatever printf does with %g, and
 * the difference shows on screen: Java writes 1.0 where %g writes 1, and
 * 1.0E7 where it writes 1e+07. An applet drawing a coordinate or a score is
 * drawing this function's output.
 *
 * Two rules, both from the API documentation. The digits are the fewest that
 * distinguish the value from its neighbours, which is found here by asking
 * printf for successively more of them until the result reads back as the same
 * number. The layout is plain decimal when the magnitude is in [1e-3, 1e7) and
 * scientific otherwise, with at least one digit each side of the point either
 * way.
 */
static void fmt_fp(char *out, size_t osz, double v, int is_float)
{
    char   buf[64], digits[32];
    int    nd = 0, e10 = 0, neg, i, p;
    int    pmax = is_float ? 9 : 17;
    double a;
    size_t n = 0;
    const char *q;

    if(v != v) {                                   /* NaN has no sign here */
        snprintf(out, osz, "NaN");
        return;
    }

    neg = (v < 0.0) || (v == 0.0 && 1.0 / v < 0.0);
    a   = neg ? -v : v;

    if(a > (is_float ? (double)FLT_MAX : DBL_MAX)) {
        snprintf(out, osz, neg ? "-Infinity" : "Infinity");
        return;
    }
    if(a == 0.0) {
        snprintf(out, osz, neg ? "-0.0" : "0.0");
        return;
    }

    for(p = 1; p < pmax; p++) {
        snprintf(buf, sizeof buf, "%.*e", p - 1, a);
        if(is_float) {
            if((float)strtod(buf, NULL) == (float)a)
                break;
        }
        else if(strtod(buf, NULL) == a) {
            break;
        }
    }
    snprintf(buf, sizeof buf, "%.*e", p - 1, a);

    for(q = buf; *q && *q != 'e' && *q != 'E'; q++) {
        if(*q >= '0' && *q <= '9' && nd < (int)sizeof digits)
            digits[nd++] = *q;
    }
    e10 = *q ? atoi(q + 1) : 0;

    while(nd > 1 && digits[nd - 1] == '0')
        nd--;
    if(nd == 0) {
        digits[nd++] = '0';
        e10 = 0;
    }

    if(osz < 48) {              /* every caller gives more; be sure anyway */
        snprintf(out, osz, "?");
        return;
    }

    if(neg)
        out[n++] = '-';

    if(e10 >= -3 && e10 < 7) {
        if(e10 >= 0) {
            for(i = 0; i <= e10; i++)
                out[n++] = i < nd ? digits[i] : '0';
            out[n++] = '.';
            if(nd > e10 + 1) {
                for(i = e10 + 1; i < nd; i++)
                    out[n++] = digits[i];
            }
            else {
                out[n++] = '0';
            }
        }
        else {
            out[n++] = '0';
            out[n++] = '.';
            for(i = 0; i < -e10 - 1; i++)
                out[n++] = '0';
            for(i = 0; i < nd; i++)
                out[n++] = digits[i];
        }
    }
    else {
        out[n++] = digits[0];
        out[n++] = '.';
        if(nd > 1) {
            for(i = 1; i < nd; i++)
                out[n++] = digits[i];
        }
        else {
            out[n++] = '0';
        }
        out[n++] = 'E';
        out[n] = '\0';
        n += (size_t)snprintf(out + n, osz - n, "%d", e10);
    }
    out[n] = '\0';
}

static const char g_digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";

/* Signed, in any radix Java accepts. Anything outside 2..36 means 10, which is
 * what Integer.toString(int, int) documents rather than an error. */
static size_t fmt_long(char *out, size_t osz, int64_t v, int radix)
{
    char     tmp[72];
    int      n = 0;
    size_t   k = 0;
    uint64_t u;

    if(radix < 2 || radix > 36)
        radix = 10;

    /* Negated as unsigned: -INT64_MIN is not representable as int64_t. */
    u = (v < 0) ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;

    do {
        tmp[n++] = g_digits[u % (unsigned)radix];
        u /= (unsigned)radix;
    } while(u && n < (int)sizeof tmp);

    if(v < 0 && k + 1 < osz)
        out[k++] = '-';
    while(n > 0 && k + 1 < osz)
        out[k++] = tmp[--n];
    out[k] = '\0';
    return k;
}

static size_t fmt_ulong(char *out, size_t osz, uint64_t u, int radix)
{
    char   tmp[72];
    int    n = 0;
    size_t k = 0;

    do {
        tmp[n++] = g_digits[u % (unsigned)radix];
        u /= (unsigned)radix;
    } while(u && n < (int)sizeof tmp);

    while(n > 0 && k + 1 < osz)
        out[k++] = tmp[--n];
    out[k] = '\0';
    return k;
}

static int digit_of(int c, int radix)
{
    int v;

    if(c >= '0' && c <= '9')      v = c - '0';
    else if(c >= 'a' && c <= 'z') v = c - 'a' + 10;
    else if(c >= 'A' && c <= 'Z') v = c - 'A' + 10;
    else                          return -1;

    return v < radix ? v : -1;
}

/* Java's parse, which is stricter than strtol in the ways that matter: no
 * leading space, no trailing anything, no empty string, and an overflow is a
 * NumberFormatException rather than a clamp. Applets rely on all four to
 * validate a <param> value inside a try. */
static int parse_radix(const char *s, size_t len, int radix, int bits,
                       int64_t *out)
{
    uint64_t acc = 0, limit;
    size_t   i = 0;
    int      neg = 0;

    if(!s || len == 0 || radix < 2 || radix > 36)
        return -1;

    if(s[0] == '-' || s[0] == '+') {
        neg = (s[0] == '-');
        i = 1;
    }
    if(i >= len)
        return -1;

    limit = (bits == 32) ? (neg ? 2147483648u : 2147483647u)
                         : (neg ? 9223372036854775808ULL
                                : 9223372036854775807ULL);

    for(; i < len; i++) {
        int d = digit_of((unsigned char)s[i], radix);

        if(d < 0)
            return -1;
        if(acc > (limit - (uint64_t)d) / (uint64_t)radix)
            return -1;
        acc = acc * (uint64_t)radix + (uint64_t)d;
    }

    *out = neg ? -(int64_t)acc : (int64_t)acc;
    return 0;
}

/* Double.parseDouble. strtod does the conversion; this decides what counts as
 * a number, which is the part Java specifies differently - a trailing d or f
 * suffix is allowed, and anything else left over is a NumberFormatException
 * where strtod would simply stop. */
static int parse_double(const char *s, size_t len, double *out)
{
    char   buf[80];
    char  *end;
    size_t i = 0, j;
    double v;

    if(!s || len == 0 || len >= sizeof buf)
        return -1;

    /* Java trims whitespace here, unlike the integer parsers. */
    while(i < len && (unsigned char)s[i] <= ' ')
        i++;
    j = len;
    while(j > i && (unsigned char)s[j - 1] <= ' ')
        j--;
    if(j == i)
        return -1;

    memcpy(buf, s + i, j - i);
    buf[j - i] = '\0';

    /* strtod takes hex floats and "infinity"; Java's grammar takes neither
     * spelling of the first and only "Infinity" for the second. */
    if(strchr(buf, 'x') || strchr(buf, 'X'))
        return -1;

    if(!strcmp(buf, "NaN") || !strcmp(buf, "-NaN") || !strcmp(buf, "+NaN")) {
        *out = 0.0 / 0.0;
        return 0;
    }
    if(!strcmp(buf, "Infinity") || !strcmp(buf, "+Infinity")) {
        *out = DBL_MAX * 2.0;
        return 0;
    }
    if(!strcmp(buf, "-Infinity")) {
        *out = -DBL_MAX * 2.0;
        return 0;
    }

    v = strtod(buf, &end);
    if(end == buf)
        return -1;
    if(*end == 'd' || *end == 'D' || *end == 'f' || *end == 'F')
        end++;
    if(*end)
        return -1;

    *out = v;
    return 0;
}

/* --- java.lang.Object.toString, for the cases reachable from here ---------
 *
 * String concatenation compiles to StringBuilder.append(Object), so this is
 * what `"x = " + obj` produces. Native code cannot call back into bytecode, so
 * an applet class that overrides toString() gets the inherited form instead of
 * its own - which is visibly an object identity rather than a plausible wrong
 * label, and is the honest answer available from here.
 */
static ps_jobj *obj_string(ps_jvm *vm, ps_jobj *o);

/* --- the wrappers --------------------------------------------------------
 *
 * One class each, one value each, and the value lives in field slot 0: as a
 * long for the integral kinds and a double for the floating ones.
 */
enum { BOX_INT, BOX_LONG, BOX_SHORT, BOX_BYTE, BOX_CHAR, BOX_BOOL,
       BOX_DOUBLE, BOX_FLOAT, BOX_NONE };

static int box_kind(const char *cls)
{
    if(strncmp(cls, "java/lang/", 10))
        return BOX_NONE;
    cls += 10;
    if(!strcmp(cls, "Integer"))   return BOX_INT;
    if(!strcmp(cls, "Long"))      return BOX_LONG;
    if(!strcmp(cls, "Short"))     return BOX_SHORT;
    if(!strcmp(cls, "Byte"))      return BOX_BYTE;
    if(!strcmp(cls, "Character")) return BOX_CHAR;
    if(!strcmp(cls, "Boolean"))   return BOX_BOOL;
    if(!strcmp(cls, "Double"))    return BOX_DOUBLE;
    if(!strcmp(cls, "Float"))     return BOX_FLOAT;
    return BOX_NONE;
}

static int box_kind_of(const ps_jobj *o)
{
    return (o && o->cls && !o->cls->native) ? BOX_NONE
         : (o && o->cls) ? box_kind(o->cls->name) : BOX_NONE;
}

static ps_jobj *make_box(ps_jvm *vm, int kind, int64_t j, double d)
{
    static const char *names[] = {
        "java/lang/Integer", "java/lang/Long", "java/lang/Short",
        "java/lang/Byte", "java/lang/Character", "java/lang/Boolean",
        "java/lang/Double", "java/lang/Float"
    };
    ps_jobj *o;

    if(kind < 0 || kind >= BOX_NONE)
        return NULL;

    o = ps_jvm_new(vm, loaded(vm, names[kind]));
    if(!o || !o->fields)
        return o;

    if(kind == BOX_DOUBLE || kind == BOX_FLOAT)
        o->fields[0].d = d;
    else
        o->fields[0].j = j;
    return o;
}

static int64_t box_j(const ps_jobj *o)
{
    return (o && o->fields) ? o->fields[0].j : 0;
}

static double box_d(const ps_jobj *o)
{
    return (o && o->fields) ? o->fields[0].d : 0.0;
}

/* The value of any box as a double, whichever kind it is. */
static double box_value(const ps_jobj *o, int kind)
{
    if(kind == BOX_DOUBLE || kind == BOX_FLOAT)
        return box_d(o);
    return (double)box_j(o);
}

static void box_text(char *out, size_t osz, const ps_jobj *o, int kind)
{
    switch(kind) {
    case BOX_DOUBLE: fmt_fp(out, osz, box_d(o), 0); break;
    case BOX_FLOAT:  fmt_fp(out, osz, box_d(o), 1); break;
    case BOX_BOOL:   snprintf(out, osz, "%s", box_j(o) ? "true" : "false");
                     break;
    case BOX_CHAR:   out[0] = (char)box_j(o); out[1] = '\0'; break;
    default:         fmt_long(out, osz, box_j(o), 10); break;
    }
}

/* --- object equality -----------------------------------------------------
 *
 * What Vector.contains and Hashtable.get mean by "equal". Strings compare by
 * value, which is the case that actually gets used - an applet looking up a
 * key it built by concatenation - and boxes compare by value and class, as
 * Integer.equals specifies.
 *
 * Anything else compares by identity, because equals() on an applet's own
 * class is bytecode and this is a native call: there is no way to run it from
 * here. That is exactly java.lang.Object's own equals for a class that does
 * not override it, and wrong only for one that does.
 */
static int obj_equals(const ps_jobj *a, const ps_jobj *b)
{
    int ka, kb;

    if(a == b)
        return 1;
    if(!a || !b)
        return 0;

    if(is_string(a) && is_string(b))
        return a->len == b->len && !memcmp(a->native, b->native,
                                           (size_t)a->len);

    ka = box_kind_of(a);
    kb = box_kind_of(b);
    if(ka != BOX_NONE && ka == kb) {
        if(ka == BOX_DOUBLE || ka == BOX_FLOAT)
            return box_d(a) == box_d(b);
        return box_j(a) == box_j(b);
    }
    return 0;
}

static int32_t str_hash(const char *s, size_t len)
{
    int32_t  h = 0;
    size_t   i;

    /* s[0]*31^(n-1) + s[1]*31^(n-2) + ... , wrapping at 32 bits. */
    for(i = 0; i < len; i++)
        h = (int32_t)((uint32_t)h * 31u + (unsigned char)s[i]);
    return h;
}

/* --- reference arrays ----------------------------------------------------
 *
 * The containers are all built on one of these, so growth and shifting happen
 * in one place. An Object[] is an ordinary Java array: the collector traces
 * it, and copyInto can hand it straight to an applet.
 */
static ps_jobj *arr_grow(ps_jvm *vm, ps_jobj *old, int32_t want)
{
    ps_jobj *fresh;
    int32_t  cap = old ? old->len : 0;
    int32_t  i;

    if(cap >= want)
        return old;

    cap = cap ? cap * 2 : 8;
    while(cap < want)
        cap *= 2;

    fresh = ps_jvm_new_array(vm, PS_T_REF, cap);
    if(!fresh)
        return NULL;

    if(old) {
        for(i = 0; i < old->len; i++)
            ((ps_jobj **)fresh->data)[i] = ((ps_jobj **)old->data)[i];
    }
    return fresh;
}

static ps_jobj *arr_at(ps_jobj *a, int32_t i)
{
    if(!a || i < 0 || i >= a->len)
        return NULL;
    return ((ps_jobj **)a->data)[i];
}

static void arr_set(ps_jobj *a, int32_t i, ps_jobj *v)
{
    if(a && i >= 0 && i < a->len)
        ((ps_jobj **)a->data)[i] = v;
}

/* --- java.lang.String ---------------------------------------------------- */

/* Where a substring of s starts, at or after `from`, or -1. Both the
 * character and the string forms reduce to this. */
static int32_t str_find(const char *s, size_t slen, const char *t, size_t tlen,
                        int32_t from)
{
    size_t i;

    if(from < 0)
        from = 0;
    if(tlen == 0)
        return (size_t)from > slen ? (int32_t)slen : from;
    if((size_t)from + tlen > slen)
        return -1;

    for(i = (size_t)from; i + tlen <= slen; i++) {
        if(!memcmp(s + i, t, tlen))
            return (int32_t)i;
    }
    return -1;
}

static int32_t str_rfind(const char *s, size_t slen, const char *t,
                         size_t tlen, int32_t from)
{
    int32_t i;

    if(from < 0)
        return -1;
    if(tlen == 0)
        return (size_t)from > slen ? (int32_t)slen : from;
    if((size_t)from + tlen > slen)
        from = (int32_t)(slen - tlen);
    if(from < 0)
        return -1;

    for(i = from; i >= 0; i--) {
        if(!memcmp(s + i, t, tlen))
            return i;
    }
    return -1;
}

static int up(int c)   { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
static int down(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* String.intern.
 *
 * Kept honest rather than cheap: the point of intern is that two equal strings
 * come back as one object, so returning `this` would be a plausible-looking
 * lie. The table is a growable Object[] rooted in the pin class, fed only by
 * explicit intern() calls, so it costs nothing until an applet asks. */
static ps_jobj *str_intern(ps_jvm *vm, ps_jobj *s)
{
    ps_jslot *p = pins(vm);
    ps_jobj  *tab, *fresh;
    int32_t   i;

    if(!p || !s)
        return s;

    tab = p[PIN_INTERN].o;
    for(i = 0; tab && i < tab->len; i++) {
        ps_jobj *e = arr_at(tab, i);

        if(!e)
            break;
        if(obj_equals(e, s))
            return e;
    }

    fresh = arr_grow(vm, tab, i + 1);
    if(!fresh)
        return s;
    p[PIN_INTERN].o = fresh;
    arr_set(fresh, i, s);
    return s;
}

static int jl_string(ps_jvm *vm, const char *n, const char *d, ps_jslot *args,
                     int nargs, ps_jslot *ret)
{
    char        buf[80];
    size_t      len = 0;
    const char *s;
    ps_jobj    *self = nargs > 0 ? args[0].o : NULL;

    if(nargs <= 0)
        return LIB_NO;             /* a static field read; String has none */

    /* The statics first: they have no receiver, and valueOf's argument sits
     * where an instance method's `this` would. */
    if(!strcmp(n, "valueOf") || !strcmp(n, "copyValueOf")) {
        const char *a = d + 1;

        if(!strncmp(a, "I)", 2) || !strncmp(a, "S)", 2) ||
           !strncmp(a, "B)", 2)) {
            fmt_long(buf, sizeof buf, args[0].i, 10);
        }
        else if(!strncmp(a, "J)", 2)) {
            fmt_long(buf, sizeof buf, args[0].j, 10);
        }
        else if(!strncmp(a, "Z)", 2)) {
            snprintf(buf, sizeof buf, "%s", args[0].i ? "true" : "false");
        }
        else if(!strncmp(a, "C)", 2)) {
            buf[0] = (char)args[0].i;
            buf[1] = '\0';
        }
        else if(!strncmp(a, "D)", 2)) {
            fmt_fp(buf, sizeof buf, args[0].d, 0);
        }
        else if(!strncmp(a, "F)", 2)) {
            fmt_fp(buf, sizeof buf, (double)args[0].f, 1);
        }
        else if(a[0] == '[' && a[1] == 'C') {
            ps_jobj *ca = args[0].o;
            int32_t  i, from = 0, cnt = ca ? ca->len : 0;
            char    *tmp;

            if(nargs >= 3) {
                from = args[1].i;
                cnt  = args[2].i;
            }
            if(!ca)
                return throw_npe(vm, "char[]");
            if(from < 0 || cnt < 0 || from + cnt > ca->len)
                return throw_str_index(vm, from);

            tmp = (char *)malloc((size_t)cnt + 1);
            if(!tmp)
                return LIB_OK;
            for(i = 0; i < cnt; i++)
                tmp[i] = (char)((uint16_t *)ca->data)[from + i];
            ret->o = ps_jvm_new_string(vm, tmp, (size_t)cnt);
            free(tmp);
            return LIB_OK;
        }
        else {
            /* valueOf(Object), which is also what a null concatenation goes
             * through. */
            ret->o = obj_string(vm, nargs > 0 ? args[0].o : NULL);
            return LIB_OK;
        }

        ret->o = mkcstr(vm, buf);
        return LIB_OK;
    }

    /* A String built with `new`. The object the interpreter allocated is an
     * empty instance of the placeholder class; this turns it into the shape
     * the rest of the runtime recognises, which is the same shape
     * ps_jvm_new_string produces. */
    if(is(n, d, "<init>", NULL)) {
        const char *src = NULL;
        size_t      sl  = 0;
        int32_t     i;
        ps_jobj    *ca  = NULL;

        if(!self)
            return throw_npe(vm, "String");

        if(nargs >= 2 && d[1] == 'L') {
            src = sbytes(args[1].o, &sl);
        }
        else if(nargs >= 2 && d[1] == '[' && args[1].o) {
            ca = args[1].o;
            sl = (size_t)ca->len;
        }

        if(ca) {
            int32_t from = 0, cnt = ca->len;

            if(nargs >= 4) {
                from = args[2].i;
                cnt  = args[3].i;
            }
            if(from < 0 || cnt < 0 || from + cnt > ca->len)
                return throw_str_index(vm, from);
            sl = (size_t)cnt;

            self->native = malloc(sl + 1);
            if(!self->native)
                return LIB_OK;
            for(i = 0; i < (int32_t)sl; i++) {
                ((char *)self->native)[i] = (ca->elem == PS_T_CHAR)
                    ? (char)((uint16_t *)ca->data)[from + i]
                    : ((char *)ca->data)[from + i];
            }
        }
        else {
            self->native = malloc(sl + 1);
            if(!self->native)
                return LIB_OK;
            if(src)
                memcpy(self->native, src, sl);
        }

        ((char *)self->native)[sl] = '\0';
        self->elem       = PS_T_CHAR;
        self->len        = (int32_t)sl;
        self->owns_native = 1;
        vm->bytes += (long)sl + 1;
        return LIB_OK;
    }

    if(!self)
        return throw_npe(vm, "String");
    s = sbytes(self, &len);
    if(!s)
        return throw_npe(vm, "String");

    if(is(n, d, "length", NULL))  { ret->i = (int32_t)len; return LIB_OK; }
    if(is(n, d, "isEmpty", NULL)) { ret->i = len == 0;     return LIB_OK; }
    if(is(n, d, "hashCode", NULL)) {
        ret->i = str_hash(s, len);
        return LIB_OK;
    }
    if(is(n, d, "toString", NULL)) { ret->o = self; return LIB_OK; }
    if(is(n, d, "intern", NULL))   { ret->o = str_intern(vm, self);
                                     return LIB_OK; }

    if(is(n, d, "charAt", NULL)) {
        int32_t i = nargs > 1 ? args[1].i : 0;

        if(i < 0 || (size_t)i >= len)
            return throw_str_index(vm, i);
        ret->i = (unsigned char)s[i];
        return LIB_OK;
    }

    if(is(n, d, "substring", NULL)) {
        int32_t b = nargs > 1 ? args[1].i : 0;
        int32_t e = nargs > 2 ? args[2].i : (int32_t)len;

        /* Which index a bad pair reports is not arbitrary: the JDK names the
         * begin index when it is out of range, and the difference otherwise.
         * Applets do not read the message, but the tests diff it. */
        if(b < 0)
            return throw_str_index(vm, b);
        if(e > (int32_t)len)
            return throw_str_index(vm, e);
        if(b > e)
            return throw_str_index(vm, e - b);

        ret->o = mkstr(vm, s + b, (size_t)(e - b));
        return LIB_OK;
    }

    if(is(n, d, "indexOf", NULL) || is(n, d, "lastIndexOf", NULL)) {
        int         last = (n[0] == 'l');
        const char *t;
        size_t      tl;
        char        one;
        int32_t     from;

        if(d[1] == 'I') {                              /* the char forms */
            one = (char)args[1].i;
            t   = &one;
            tl  = 1;
            from = (nargs > 2) ? args[2].i
                 : (last ? (int32_t)len : 0);
        }
        else {
            t = sbytes(nargs > 1 ? args[1].o : NULL, &tl);
            if(!t)
                return throw_npe(vm, "indexOf");
            from = (nargs > 2) ? args[2].i : (last ? (int32_t)len : 0);
        }

        ret->i = last ? str_rfind(s, len, t, tl, from)
                      : str_find(s, len, t, tl, from);
        return LIB_OK;
    }

    if(is(n, d, "equals", NULL)) {
        ps_jobj *o = nargs > 1 ? args[1].o : NULL;

        ret->i = (o && is_string(o) && o->len == self->len &&
                  !memcmp(o->native, s, len)) ? 1 : 0;
        return LIB_OK;
    }

    if(is(n, d, "equalsIgnoreCase", NULL)) {
        size_t      ol = 0;
        const char *o  = sbytes(nargs > 1 ? args[1].o : NULL, &ol);
        size_t      i;

        ret->i = 0;
        if(!o || ol != len)
            return LIB_OK;
        for(i = 0; i < len; i++) {
            if(up((unsigned char)s[i]) != up((unsigned char)o[i]))
                return LIB_OK;
        }
        ret->i = 1;
        return LIB_OK;
    }

    if(is(n, d, "compareTo", NULL) || is(n, d, "compareToIgnoreCase", NULL)) {
        int         fold = (n[9] == 'I');
        size_t      ol = 0;
        const char *o  = sbytes(nargs > 1 ? args[1].o : NULL, &ol);
        size_t      i, m;

        if(!o)
            return throw_npe(vm, "compareTo");

        m = len < ol ? len : ol;
        for(i = 0; i < m; i++) {
            int a = (unsigned char)s[i], b = (unsigned char)o[i];

            if(fold) { a = down(up(a)); b = down(up(b)); }
            if(a != b) {
                ret->i = a - b;
                return LIB_OK;
            }
        }
        ret->i = (int32_t)len - (int32_t)ol;
        return LIB_OK;
    }

    if(is(n, d, "trim", NULL)) {
        size_t b = 0, e = len;

        /* Everything at or below U+0020, which is what trim documents - not
         * "whitespace", so a control character goes too. */
        while(b < e && (unsigned char)s[b] <= ' ')
            b++;
        while(e > b && (unsigned char)s[e - 1] <= ' ')
            e--;
        ret->o = mkstr(vm, s + b, e - b);
        return LIB_OK;
    }

    if(is(n, d, "toUpperCase", NULL) || is(n, d, "toLowerCase", NULL)) {
        int      upper = (n[2] == 'U');
        ps_jobj *o = mkstr(vm, s, len);
        size_t   i;

        if(o) {
            char *p = (char *)o->native;

            for(i = 0; i < len; i++)
                p[i] = (char)(upper ? up((unsigned char)p[i])
                                    : down((unsigned char)p[i]));
        }
        ret->o = o;
        return LIB_OK;
    }

    if(is(n, d, "startsWith", NULL)) {
        size_t      tl = 0;
        const char *t  = sbytes(nargs > 1 ? args[1].o : NULL, &tl);
        int32_t     at = nargs > 2 ? args[2].i : 0;

        if(!t)
            return throw_npe(vm, "startsWith");
        ret->i = (at >= 0 && (size_t)at + tl <= len &&
                  !memcmp(s + at, t, tl)) ? 1 : 0;
        return LIB_OK;
    }

    if(is(n, d, "endsWith", NULL)) {
        size_t      tl = 0;
        const char *t  = sbytes(nargs > 1 ? args[1].o : NULL, &tl);

        if(!t)
            return throw_npe(vm, "endsWith");
        ret->i = (tl <= len && !memcmp(s + len - tl, t, tl)) ? 1 : 0;
        return LIB_OK;
    }

    if(is(n, d, "replace", "(CC)Ljava/lang/String;")) {
        char     from = (char)args[1].i, to = (char)args[2].i;
        ps_jobj *o;
        size_t   i;

        if(!memchr(s, from, len)) {
            ret->o = self;         /* replace returns this when nothing moves */
            return LIB_OK;
        }
        o = mkstr(vm, s, len);
        if(o) {
            char *p = (char *)o->native;

            for(i = 0; i < len; i++) {
                if(p[i] == from)
                    p[i] = to;
            }
        }
        ret->o = o;
        return LIB_OK;
    }

    if(is(n, d, "concat", NULL)) {
        size_t      ol = 0;
        const char *o  = sbytes(nargs > 1 ? args[1].o : NULL, &ol);

        if(!o)
            return throw_npe(vm, "concat");
        ret->o = mkstr2(vm, s, len, o, ol);
        return LIB_OK;
    }

    if(is(n, d, "toCharArray", NULL)) {
        ps_jobj *a = ps_jvm_new_array(vm, PS_T_CHAR, (int32_t)len);
        size_t   i;

        if(a) {
            s = sbytes(self, &len);
            for(i = 0; i < len; i++)
                ((uint16_t *)a->data)[i] = (unsigned char)s[i];
        }
        ret->o = a;
        return LIB_OK;
    }

    if(is(n, d, "getChars", NULL)) {
        int32_t  b = args[1].i, e = args[2].i, at = args[4].i;
        ps_jobj *a = args[3].o;
        int32_t  i;

        if(!a)
            return throw_npe(vm, "getChars");
        if(b < 0 || e > (int32_t)len || b > e)
            return throw_str_index(vm, b);
        if(at < 0 || at + (e - b) > a->len)
            return throw_arr_index(vm, at);
        for(i = b; i < e; i++)
            ((uint16_t *)a->data)[at + i - b] = (unsigned char)s[i];
        return LIB_OK;
    }

    return LIB_NO;
}

/* --- the primitive wrappers ---------------------------------------------- */

static int jl_box(ps_jvm *vm, int kind, const char *cls, const char *n,
                  const char *d, ps_jslot *args, int nargs, ps_jslot *ret)
{
    char        buf[80];
    ps_jobj    *self = nargs > 0 ? args[0].o : NULL;
    size_t      sl = 0;
    const char *sp;

    (void)cls;

    /* --- the constants, read through getstatic --- */
    if(!strcmp(n, "MIN_VALUE") || !strcmp(n, "MAX_VALUE")) {
        int hi = (n[1] == 'A');

        switch(kind) {
        case BOX_INT:    ret->i = hi ? 2147483647 : (-2147483647 - 1); break;
        case BOX_LONG:   ret->j = hi ? 9223372036854775807LL
                                     : (-9223372036854775807LL - 1); break;
        case BOX_SHORT:  ret->i = hi ? 32767 : -32768; break;
        case BOX_BYTE:   ret->i = hi ? 127 : -128; break;
        case BOX_CHAR:   ret->i = hi ? 65535 : 0; break;
        /* Double.MIN_VALUE is the smallest positive value, not the most
         * negative one - the one place the naming does not mean what the
         * integral wrappers mean by it. */
        case BOX_DOUBLE: ret->d = hi ? DBL_MAX : 4.9406564584124654e-324;
                         break;
        case BOX_FLOAT:  ret->f = hi ? FLT_MAX : 1.4012984643e-45f; break;
        default:         return LIB_NO;
        }
        return LIB_OK;
    }
    if(kind == BOX_BOOL && (!strcmp(n, "TRUE") || !strcmp(n, "FALSE"))) {
        int       want = (n[0] == 'T');
        ps_jslot *p    = pins(vm);
        int       slot = want ? PIN_TRUE : PIN_FALSE;

        /* Cached so `b == Boolean.TRUE` behaves, which is how applets of the
         * period test a flag they stored in a Hashtable. */
        if(p && !p[slot].o)
            p[slot].o = make_box(vm, BOX_BOOL, want, 0.0);
        ret->o = p ? p[slot].o : make_box(vm, BOX_BOOL, want, 0.0);
        return LIB_OK;
    }
    if(kind == BOX_DOUBLE || kind == BOX_FLOAT) {
        if(!strcmp(n, "POSITIVE_INFINITY") || !strcmp(n, "NEGATIVE_INFINITY")) {
            double v = (n[0] == 'P') ? DBL_MAX * 2.0 : -DBL_MAX * 2.0;

            if(kind == BOX_FLOAT) ret->f = (float)v; else ret->d = v;
            return LIB_OK;
        }
        if(!strcmp(n, "NaN")) {
            double v = 0.0 / 0.0;

            if(kind == BOX_FLOAT) ret->f = (float)v; else ret->d = v;
            return LIB_OK;
        }
    }

    /* --- parsing --- */
    if(!strcmp(n, "parseInt") || !strcmp(n, "parseLong") ||
       !strcmp(n, "parseShort") || !strcmp(n, "parseByte")) {
        int64_t v;
        int     radix = (nargs > 1 && strstr(d, "I)")) ? args[1].i : 10;
        int     bits  = !strcmp(n, "parseLong") ? 64 : 32;

        sp = sbytes(nargs > 0 ? args[0].o : NULL, &sl);
        if(!sp || parse_radix(sp, sl, radix, bits, &v) != 0)
            return throw_nfe(vm, sp, sl);
        if(bits == 32) {
            if(!strcmp(n, "parseShort") && (v < -32768 || v > 32767))
                return throw_nfe(vm, sp, sl);
            if(!strcmp(n, "parseByte") && (v < -128 || v > 127))
                return throw_nfe(vm, sp, sl);
            ret->i = (int32_t)v;
        }
        else {
            ret->j = v;
        }
        return LIB_OK;
    }
    if(!strcmp(n, "parseDouble") || !strcmp(n, "parseFloat")) {
        double v;

        sp = sbytes(nargs > 0 ? args[0].o : NULL, &sl);
        if(!sp || parse_double(sp, sl, &v) != 0)
            return throw_nfe(vm, sp, sl);
        if(n[5] == 'F')
            ret->f = (float)v;
        else
            ret->d = v;
        return LIB_OK;
    }
    if(kind == BOX_BOOL && (!strcmp(n, "parseBoolean") ||
                            is(n, d, "valueOf", "(Ljava/lang/String;)"
                                                "Ljava/lang/Boolean;"))) {
        int v;

        sp = sbytes(nargs > 0 ? args[0].o : NULL, &sl);
        v = (sp && sl == 4 && up((unsigned char)sp[0]) == 'T' &&
             up((unsigned char)sp[1]) == 'R' && up((unsigned char)sp[2]) == 'U'
             && up((unsigned char)sp[3]) == 'E');
        if(n[0] == 'p')
            ret->i = v;
        else
            ret->o = make_box(vm, BOX_BOOL, v, 0.0);
        return LIB_OK;
    }

    /* --- static toString, in a radix or not --- */
    if(!strcmp(n, "toString") && d[1] != ')') {
        /* Integer.toString(int) and Integer.toString(int, radix): static, so
         * the value is argument zero rather than a receiver. */
        int radix = 10;

        switch(kind) {
        case BOX_LONG:
            radix = (nargs > 2) ? args[2].i : 10;
            fmt_long(buf, sizeof buf, args[0].j, radix);
            break;
        case BOX_DOUBLE: fmt_fp(buf, sizeof buf, args[0].d, 0); break;
        case BOX_FLOAT:  fmt_fp(buf, sizeof buf, (double)args[0].f, 1); break;
        case BOX_CHAR:   buf[0] = (char)args[0].i; buf[1] = '\0'; break;
        case BOX_BOOL:   snprintf(buf, sizeof buf, "%s",
                                  args[0].i ? "true" : "false"); break;
        default:
            radix = (nargs > 1) ? args[1].i : 10;
            fmt_long(buf, sizeof buf, args[0].i, radix);
            break;
        }
        ret->o = mkcstr(vm, buf);
        return LIB_OK;
    }
    if(!strcmp(n, "toHexString") || !strcmp(n, "toOctalString") ||
       !strcmp(n, "toBinaryString")) {
        int radix = (n[2] == 'H') ? 16 : (n[2] == 'O') ? 8 : 2;

        /* Unsigned, which is why -1 comes out as ffffffff rather than -1. */
        if(kind == BOX_LONG)
            fmt_ulong(buf, sizeof buf, (uint64_t)args[0].j, radix);
        else
            fmt_ulong(buf, sizeof buf, (uint32_t)args[0].i, radix);
        ret->o = mkcstr(vm, buf);
        return LIB_OK;
    }

    /* --- valueOf, boxing or parsing --- */
    if(!strcmp(n, "valueOf")) {
        const char *a = d + 1;

        if(*a == 'L') {                                /* valueOf(String) */
            int64_t v;
            double  dv;
            int     radix = (nargs > 1) ? args[1].i : 10;

            sp = sbytes(args[0].o, &sl);
            if(!sp)
                return throw_nfe(vm, NULL, 0);
            if(kind == BOX_DOUBLE || kind == BOX_FLOAT) {
                if(parse_double(sp, sl, &dv) != 0)
                    return throw_nfe(vm, sp, sl);
                ret->o = make_box(vm, kind, 0, dv);
            }
            else {
                if(parse_radix(sp, sl, radix,
                               kind == BOX_LONG ? 64 : 32, &v) != 0)
                    return throw_nfe(vm, sp, sl);
                ret->o = make_box(vm, kind, v, 0.0);
            }
            return LIB_OK;
        }
        switch(kind) {
        case BOX_LONG:   ret->o = make_box(vm, kind, args[0].j, 0.0); break;
        case BOX_DOUBLE: ret->o = make_box(vm, kind, 0, args[0].d);   break;
        case BOX_FLOAT:  ret->o = make_box(vm, kind, 0,
                                           (double)args[0].f);        break;
        default:         ret->o = make_box(vm, kind, args[0].i, 0.0); break;
        }
        return LIB_OK;
    }

    /* --- java.lang.Character's static tests ---
     *
     * ASCII only, and deliberately: a String here is UTF-8 bytes, so a
     * character above 127 is half of something rather than a letter, and
     * answering for it would be answering about the wrong thing. */
    if(kind == BOX_CHAR && nargs >= 1) {
        int c = args[0].i;

        if(!strcmp(n, "isDigit"))    { ret->i = (c >= '0' && c <= '9');
                                       return LIB_OK; }
        if(!strcmp(n, "isLetter"))   { ret->i = (up(c) >= 'A' && up(c) <= 'Z');
                                       return LIB_OK; }
        if(!strcmp(n, "isLetterOrDigit")) {
            ret->i = (c >= '0' && c <= '9') || (up(c) >= 'A' && up(c) <= 'Z');
            return LIB_OK;
        }
        if(!strcmp(n, "isUpperCase")) { ret->i = (c >= 'A' && c <= 'Z');
                                        return LIB_OK; }
        if(!strcmp(n, "isLowerCase")) { ret->i = (c >= 'a' && c <= 'z');
                                        return LIB_OK; }
        if(!strcmp(n, "isWhitespace") || !strcmp(n, "isSpace")) {
            ret->i = (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
                      c == '\f' || c == 0x0b || (c >= 0x1c && c <= 0x1f));
            return LIB_OK;
        }
        if(!strcmp(n, "isSpaceChar")) { ret->i = (c == ' '); return LIB_OK; }
        if(!strcmp(n, "toUpperCase") && d[1] == 'C') { ret->i = up(c);
                                                       return LIB_OK; }
        if(!strcmp(n, "toLowerCase") && d[1] == 'C') { ret->i = down(c);
                                                       return LIB_OK; }
        if(!strcmp(n, "digit")) {
            ret->i = digit_of(c, nargs > 1 ? args[1].i : 10);
            return LIB_OK;
        }
        if(!strcmp(n, "forDigit")) {
            int radix = nargs > 1 ? args[1].i : 10;

            ret->i = (c >= 0 && radix >= 2 && radix <= 36 && c < radix)
                   ? g_digits[c] : 0;
            return LIB_OK;
        }
    }

    /* --- instance methods --- */
    if(is(n, d, "<init>", NULL)) {
        if(!self)
            return throw_npe(vm, "wrapper");
        if(!self->fields)
            return LIB_OK;

        if(nargs < 2)
            return LIB_OK;

        /* Read off the descriptor, never off the slot. An argument slot holds
         * whatever its type says and nothing marks which - treating the 7 in
         * `new Integer(7)` as a possible String is a dereference of 7. */
        switch(d[1]) {
        case 'L': {
            int64_t v;
            double  dv;

            sp = sbytes(args[1].o, &sl);
            if(!sp)
                return throw_nfe(vm, NULL, 0);
            if(kind == BOX_DOUBLE || kind == BOX_FLOAT) {
                if(parse_double(sp, sl, &dv) != 0)
                    return throw_nfe(vm, sp, sl);
                self->fields[0].d = dv;
            }
            else {
                if(parse_radix(sp, sl, 10, kind == BOX_LONG ? 64 : 32,
                               &v) != 0)
                    return throw_nfe(vm, sp, sl);
                self->fields[0].j = v;
            }
            break;
        }
        /* new Float(double) is a real overload, so the widening is the
         * descriptor's business rather than the wrapper's. */
        case 'J': self->fields[0].j = args[1].j;         break;
        case 'D': self->fields[0].d = args[1].d;         break;
        case 'F': self->fields[0].d = (double)args[1].f; break;
        default:  self->fields[0].j = args[1].i;         break;
        }
        return LIB_OK;
    }

    if(!self)
        return LIB_NO;

    if(is(n, d, "intValue", NULL))    { ret->i = (int32_t)box_value(self, kind);
                                        return LIB_OK; }
    if(is(n, d, "shortValue", NULL))  { ret->i = (int16_t)box_value(self, kind);
                                        return LIB_OK; }
    if(is(n, d, "byteValue", NULL))   { ret->i = (int8_t)box_value(self, kind);
                                        return LIB_OK; }
    if(is(n, d, "longValue", NULL))   {
        ret->j = (kind == BOX_DOUBLE || kind == BOX_FLOAT)
               ? (int64_t)box_d(self) : box_j(self);
        return LIB_OK;
    }
    if(is(n, d, "doubleValue", NULL)) { ret->d = box_value(self, kind);
                                        return LIB_OK; }
    if(is(n, d, "floatValue", NULL))  { ret->f = (float)box_value(self, kind);
                                        return LIB_OK; }
    if(is(n, d, "charValue", NULL))   { ret->i = (int32_t)box_j(self);
                                        return LIB_OK; }
    if(is(n, d, "booleanValue", NULL)){ ret->i = box_j(self) ? 1 : 0;
                                        return LIB_OK; }
    if(is(n, d, "toString", "()Ljava/lang/String;")) {
        box_text(buf, sizeof buf, self, kind);
        ret->o = mkcstr(vm, buf);
        return LIB_OK;
    }
    if(is(n, d, "equals", NULL)) {
        ret->i = obj_equals(self, nargs > 1 ? args[1].o : NULL);
        return LIB_OK;
    }
    if(is(n, d, "hashCode", NULL)) {
        /* Integer's is the value itself, which is the one applets can
         * observe; the rest are folded the same way rather than reproducing
         * each wrapper's own recipe. */
        int64_t v = box_j(self);

        if(kind == BOX_DOUBLE || kind == BOX_FLOAT)
            v = (int64_t)box_d(self);
        ret->i = (int32_t)(v ^ (v >> 32));
        return LIB_OK;
    }
    if(is(n, d, "compareTo", NULL)) {
        double a = box_value(self, kind);
        double b = box_value(nargs > 1 ? args[1].o : NULL, kind);

        ret->i = a < b ? -1 : (a > b ? 1 : 0);
        return LIB_OK;
    }

    return LIB_NO;
}

/* --- java.lang.StringBuffer / StringBuilder ------------------------------
 *
 * `"Score: " + n` compiles to one of these, so an applet that draws a number
 * is using it whether or not its source mentions it. The buffer is a plain C
 * string in the object's native pointer with the length in `len` - the same
 * representation ps_jre.c uses, so the two cannot disagree about an object
 * that passes between them.
 */
static int sb_room(ps_jvm *vm, ps_jobj *self, size_t extra)
{
    char *grown = (char *)realloc(self->native, (size_t)self->len + extra + 1);

    if(!grown)
        return -1;
    self->native      = grown;
    self->owns_native = 1;
    vm->bytes += (long)extra;
    return 0;
}

static int sb_add(ps_jvm *vm, ps_jobj *self, const char *s, size_t n)
{
    if(!self || sb_room(vm, self, n) != 0)
        return -1;
    memcpy((char *)self->native + self->len, s, n);
    self->len = (int32_t)((size_t)self->len + n);
    ((char *)self->native)[self->len] = '\0';
    return 0;
}

/* The text a value appends, for every descriptor javac emits. Returns the
 * bytes and their length, using buf when it has to build them. */
static const char *sb_text(ps_jvm *vm, const char *d, ps_jslot *args,
                           int nargs, char *buf, size_t bsz, size_t *out_len)
{
    const char *a = strchr(d, '(');

    a = a ? a + 1 : d;

    if(!strncmp(a, "Ljava/lang/String;", 18)) {
        /* A null String appends the four letters, not nothing - which is what
         * makes `"x " + s` readable when s was never set. */
        if(nargs < 2 || !args[1].o) {
            *out_len = 4;
            return "null";
        }
        return sbytes(args[1].o, out_len);
    }
    if(*a == 'I' || *a == 'S' || *a == 'B') {
        *out_len = fmt_long(buf, bsz, args[1].i, 10);
        return buf;
    }
    if(*a == 'J') {
        *out_len = fmt_long(buf, bsz, args[1].j, 10);
        return buf;
    }
    if(*a == 'C') {
        buf[0] = (char)args[1].i;
        buf[1] = '\0';
        *out_len = 1;
        return buf;
    }
    if(*a == 'Z') {
        const char *t = args[1].i ? "true" : "false";

        *out_len = strlen(t);
        return t;
    }
    if(*a == 'D' || *a == 'F') {
        fmt_fp(buf, bsz, *a == 'D' ? args[1].d : (double)args[1].f,
               *a == 'F');
        *out_len = strlen(buf);
        return buf;
    }
    if(!strncmp(a, "[C", 2)) {
        ps_jobj *ca = nargs > 1 ? args[1].o : NULL;
        int32_t  i;

        *out_len = 0;
        if(!ca)
            return NULL;
        for(i = 0; i < ca->len && (size_t)i + 1 < bsz; i++)
            buf[i] = (char)((uint16_t *)ca->data)[i];
        buf[i] = '\0';
        *out_len = (size_t)i;
        return buf;
    }

    /* append(Object), which is also how a null gets here. */
    {
        ps_jobj *o = obj_string(vm, nargs > 1 ? args[1].o : NULL);

        return sbytes(o, out_len);
    }
}

static int jl_sbuf(ps_jvm *vm, const char *n, const char *d, ps_jslot *args,
                   int nargs, ps_jslot *ret)
{
    ps_jobj *self = nargs > 0 ? args[0].o : NULL;
    char     buf[512];
    size_t   al = 0;

    if(!self)
        return (nargs <= 0) ? LIB_NO : throw_npe(vm, "StringBuffer");

    if(is(n, d, "<init>", NULL)) {
        size_t      il = 0;
        /* new StringBuffer(16) reserves; new StringBuffer("x") seeds. The
         * descriptor is the only thing that says which, and reading the slot
         * to find out would dereference the 16. */
        const char *init = (nargs >= 2 && d[1] == 'L')
                         ? sbytes(args[1].o, &il) : NULL;

        self->native = malloc(il + 1);
        if(!self->native)
            return LIB_OK;
        self->owns_native = 1;
        if(init)
            memcpy(self->native, init, il);
        ((char *)self->native)[il] = '\0';
        self->len = (int32_t)il;
        vm->bytes += (long)il + 1;
        return LIB_OK;
    }

    if(!self->native) {
        /* Constructed by ps_jre.c's block, or not at all. Either way there is
         * a buffer to make before anything can be added to it. */
        self->native = malloc(1);
        if(!self->native)
            return LIB_OK;
        ((char *)self->native)[0] = '\0';
        self->owns_native = 1;
        self->len = 0;
        vm->bytes += 1;
    }

    if(!strcmp(n, "append")) {
        const char *add = sb_text(vm, d, args, nargs, buf, sizeof buf, &al);

        if(add)
            sb_add(vm, self, add, al);
        ret->o = self;                    /* append returns this, to chain */
        return LIB_OK;
    }

    if(is(n, d, "toString", NULL)) {
        ret->o = mkstr(vm, (const char *)self->native, (size_t)self->len);
        return LIB_OK;
    }
    if(is(n, d, "length", NULL))   { ret->i = self->len; return LIB_OK; }
    if(is(n, d, "capacity", NULL)) { ret->i = self->len; return LIB_OK; }
    if(is(n, d, "ensureCapacity", NULL)) { return LIB_OK; }

    if(is(n, d, "charAt", NULL)) {
        int32_t i = args[1].i;

        if(i < 0 || i >= self->len)
            return throw_str_index(vm, i);
        ret->i = (unsigned char)((char *)self->native)[i];
        return LIB_OK;
    }
    if(is(n, d, "setCharAt", NULL)) {
        int32_t i = args[1].i;

        if(i < 0 || i >= self->len)
            return throw_str_index(vm, i);
        ((char *)self->native)[i] = (char)args[2].i;
        return LIB_OK;
    }
    if(is(n, d, "setLength", NULL)) {
        int32_t want = args[1].i;

        if(want < 0)
            return throw_str_index(vm, want);
        if(want > self->len) {
            /* Padded with NUL, which is what the spec says and what an applet
             * measuring a fixed-width field is relying on. */
            size_t grow = (size_t)(want - self->len);

            if(sb_room(vm, self, grow) != 0)
                return LIB_OK;
            memset((char *)self->native + self->len, 0, grow);
        }
        else {
            vm->bytes -= (long)(self->len - want);
        }
        self->len = want;
        ((char *)self->native)[want] = '\0';
        return LIB_OK;
    }
    if(is(n, d, "reverse", NULL)) {
        char   *p = (char *)self->native;
        int32_t i, j;

        for(i = 0, j = self->len - 1; i < j; i++, j--) {
            char t = p[i];

            p[i] = p[j];
            p[j] = t;
        }
        ret->o = self;
        return LIB_OK;
    }
    if(is(n, d, "deleteCharAt", NULL) || is(n, d, "delete", NULL)) {
        int32_t b = args[1].i;
        int32_t e = (n[0] == 'd' && n[6] == 'C') ? b + 1 : args[2].i;

        if(e > self->len)
            e = self->len;
        if(b < 0 || b > e)
            return throw_str_index(vm, b);
        memmove((char *)self->native + b, (char *)self->native + e,
                (size_t)(self->len - e) + 1);
        vm->bytes -= (long)(e - b);
        self->len -= (e - b);
        ret->o = self;
        return LIB_OK;
    }
    if(!strcmp(n, "insert")) {
        int32_t     at = args[1].i;
        size_t      il = 0;
        const char *ins;
        ps_jslot    shifted[4];

        if(at < 0 || at > self->len)
            return throw_str_index(vm, at);

        /* insert's value is one argument further along than append's, so the
         * descriptor is read past the leading int and the slots are shifted
         * to match. */
        shifted[0] = args[0];
        shifted[1] = args[2];
        if(nargs > 3)
            shifted[2] = args[3];
        ins = sb_text(vm, d + 2, shifted, nargs > 3 ? 3 : 2, buf, sizeof buf,
                      &il);
        if(!ins)
            return LIB_OK;

        if(sb_room(vm, self, il) != 0)
            return LIB_OK;
        memmove((char *)self->native + at + il, (char *)self->native + at,
                (size_t)(self->len - at) + 1);
        memcpy((char *)self->native + at, ins, il);
        self->len = (int32_t)((size_t)self->len + il);
        ret->o = self;
        return LIB_OK;
    }
    if(is(n, d, "indexOf", NULL)) {
        size_t      tl = 0;
        const char *t  = sbytes(nargs > 1 ? args[1].o : NULL, &tl);

        if(!t)
            return throw_npe(vm, "indexOf");
        ret->i = str_find((const char *)self->native, (size_t)self->len, t, tl,
                          nargs > 2 ? args[2].i : 0);
        return LIB_OK;
    }

    return LIB_NO;
}

/* --- java.util.Vector ----------------------------------------------------
 *
 * The collection of the 1.0 era: no interfaces, no iterators, and every applet
 * that keeps a list of anything keeps it here. Elements live in an Object[]
 * in field slot 0 and the count in the object's own `len`, so the array is
 * both the storage and the thing copyInto hands back.
 */
static ps_jobj *vec_data(ps_jobj *v) { return v && v->fields ? v->fields[0].o
                                                             : NULL; }

static int vec_reserve(ps_jvm *vm, ps_jobj *v, int32_t want)
{
    ps_jobj *grown = arr_grow(vm, vec_data(v), want);

    if(!grown)
        return -1;
    v->fields[0].o = grown;
    return 0;
}

static int32_t vec_index(ps_jobj *v, ps_jobj *want, int32_t from)
{
    ps_jobj *a = vec_data(v);
    int32_t  i;

    for(i = from < 0 ? 0 : from; i < v->len; i++) {
        if(obj_equals(arr_at(a, i), want))
            return i;
    }
    return -1;
}

static int ju_vector(ps_jvm *vm, const char *n, const char *d, ps_jslot *args,
                     int nargs, ps_jslot *ret)
{
    ps_jobj *self = nargs > 0 ? args[0].o : NULL;
    ps_jobj *a;
    int32_t  i;

    if(!self || !self->fields)
        return (nargs <= 0) ? LIB_NO : throw_npe(vm, "Vector");

    if(is(n, d, "<init>", NULL)) {
        /* The initial capacity is a hint and nothing more: it cannot be
         * observed except through capacity(), and growth is doubling either
         * way. */
        int32_t cap = (nargs > 1 && d[1] == 'I') ? args[1].i : 10;

        self->len = 0;
        if(cap > 0)
            vec_reserve(vm, self, cap);
        return LIB_OK;
    }

    if(is(n, d, "size", NULL))    { ret->i = self->len;      return LIB_OK; }
    if(is(n, d, "isEmpty", NULL)) { ret->i = self->len == 0; return LIB_OK; }
    if(is(n, d, "capacity", NULL)) {
        a = vec_data(self);
        ret->i = a ? a->len : 0;
        return LIB_OK;
    }

    if(is(n, d, "addElement", NULL)) {
        if(vec_reserve(vm, self, self->len + 1) != 0)
            return LIB_OK;
        arr_set(vec_data(self), self->len, nargs > 1 ? args[1].o : NULL);
        self->len++;
        return LIB_OK;
    }

    if(is(n, d, "elementAt", NULL)) {
        i = args[1].i;
        if(i < 0 || i >= self->len)
            return throw_arr_index(vm, i);
        ret->o = arr_at(vec_data(self), i);
        return LIB_OK;
    }

    if(is(n, d, "setElementAt", NULL)) {
        i = args[2].i;
        if(i < 0 || i >= self->len)
            return throw_arr_index(vm, i);
        arr_set(vec_data(self), i, args[1].o);
        return LIB_OK;
    }

    if(is(n, d, "insertElementAt", NULL)) {
        i = args[2].i;
        if(i < 0 || i > self->len)
            return throw_arr_index(vm, i);
        if(vec_reserve(vm, self, self->len + 1) != 0)
            return LIB_OK;
        a = vec_data(self);
        {
            int32_t k;

            for(k = self->len; k > i; k--)
                arr_set(a, k, arr_at(a, k - 1));
        }
        arr_set(a, i, args[1].o);
        self->len++;
        return LIB_OK;
    }

    if(is(n, d, "removeElementAt", NULL)) {
        i = args[1].i;
        if(i < 0 || i >= self->len)
            return throw_arr_index(vm, i);
        a = vec_data(self);
        for(; i < self->len - 1; i++)
            arr_set(a, i, arr_at(a, i + 1));
        arr_set(a, self->len - 1, NULL);
        self->len--;
        return LIB_OK;
    }

    if(is(n, d, "removeElement", NULL)) {
        i = vec_index(self, nargs > 1 ? args[1].o : NULL, 0);

        ret->i = 0;
        if(i < 0)
            return LIB_OK;
        a = vec_data(self);
        for(; i < self->len - 1; i++)
            arr_set(a, i, arr_at(a, i + 1));
        arr_set(a, self->len - 1, NULL);
        self->len--;
        ret->i = 1;
        return LIB_OK;
    }

    if(is(n, d, "removeAllElements", NULL)) {
        a = vec_data(self);
        for(i = 0; i < self->len; i++)
            arr_set(a, i, NULL);
        self->len = 0;
        return LIB_OK;
    }

    if(is(n, d, "contains", NULL)) {
        ret->i = vec_index(self, args[1].o, 0) >= 0;
        return LIB_OK;
    }
    if(is(n, d, "indexOf", NULL)) {
        ret->i = vec_index(self, args[1].o, nargs > 2 ? args[2].i : 0);
        return LIB_OK;
    }
    if(is(n, d, "lastIndexOf", NULL)) {
        int32_t from = nargs > 2 ? args[2].i : self->len - 1;

        if(from >= self->len)
            from = self->len - 1;
        a = vec_data(self);
        ret->i = -1;
        for(i = from; i >= 0; i--) {
            if(obj_equals(arr_at(a, i), args[1].o)) {
                ret->i = i;
                break;
            }
        }
        return LIB_OK;
    }

    if(is(n, d, "firstElement", NULL) || is(n, d, "lastElement", NULL)) {
        if(self->len == 0)
            return throw_nse(vm);
        ret->o = arr_at(vec_data(self), n[0] == 'f' ? 0 : self->len - 1);
        return LIB_OK;
    }

    if(is(n, d, "copyInto", NULL)) {
        ps_jobj *dst = nargs > 1 ? args[1].o : NULL;

        if(!dst)
            return throw_npe(vm, "copyInto");
        if(dst->len < self->len)
            return throw_arr_index(vm, self->len);
        a = vec_data(self);
        for(i = 0; i < self->len; i++)
            ((ps_jobj **)dst->data)[i] = arr_at(a, i);
        return LIB_OK;
    }

    if(is(n, d, "elements", NULL)) {
        ps_jobj *e = ps_jvm_new(vm, loaded(vm, "java/util/Enumeration"));

        if(e && e->fields) {
            e->fields[0].o = self;
            e->fields[1].i = 0;             /* a vector's own elements */
            e->len = 0;
        }
        ret->o = e;
        return LIB_OK;
    }

    if(is(n, d, "setSize", NULL)) {
        int32_t want = args[1].i;

        if(want < 0)
            return throw_arr_index(vm, want);
        if(want > self->len && vec_reserve(vm, self, want) != 0)
            return LIB_OK;
        a = vec_data(self);
        for(i = want; i < self->len; i++)
            arr_set(a, i, NULL);
        self->len = want;
        return LIB_OK;
    }
    if(is(n, d, "trimToSize", NULL) || is(n, d, "ensureCapacity", NULL))
        return LIB_OK;

    return LIB_NO;
}

/* --- java.util.Hashtable -------------------------------------------------
 *
 * Two parallel Object[] and a linear scan, not a hash table.
 *
 * The name is the API's, not a description of the implementation. What makes
 * the scan the right call is what an applet puts in one: a dozen parameters
 * keyed by name, or a handful of cached images. At that size the compare is
 * cheaper than the hash, insertion order makes the enumerations reproducible,
 * and there is no rehash or tombstone to get wrong. An applet that puts
 * thousands of entries in a Hashtable would want the real thing, and would be
 * the first one ever to do so on this machine.
 */
static ps_jobj *ht_keys(ps_jobj *h) { return h && h->fields ? h->fields[0].o
                                                            : NULL; }
static ps_jobj *ht_vals(ps_jobj *h) { return h && h->fields ? h->fields[1].o
                                                            : NULL; }

static int32_t ht_find(ps_jobj *h, ps_jobj *key)
{
    ps_jobj *k = ht_keys(h);
    int32_t  i;

    for(i = 0; i < h->len; i++) {
        if(obj_equals(arr_at(k, i), key))
            return i;
    }
    return -1;
}

static int ju_hashtable(ps_jvm *vm, const char *n, const char *d,
                        ps_jslot *args, int nargs, ps_jslot *ret)
{
    ps_jobj *self = nargs > 0 ? args[0].o : NULL;
    int32_t  i;

    if(!self || !self->fields)
        return (nargs <= 0) ? LIB_NO : throw_npe(vm, "Hashtable");

    if(is(n, d, "<init>", NULL)) {
        self->len = 0;
        return LIB_OK;
    }

    if(is(n, d, "size", NULL))    { ret->i = self->len;      return LIB_OK; }
    if(is(n, d, "isEmpty", NULL)) { ret->i = self->len == 0; return LIB_OK; }

    if(is(n, d, "put", NULL)) {
        ps_jobj *key = args[1].o, *val = args[2].o;
        ps_jobj *ka, *va;

        /* Both are specified as throwing rather than being stored, and an
         * applet that hits it has a bug this makes visible. */
        if(!key || !val)
            return throw_npe(vm, "Hashtable.put");

        i = ht_find(self, key);
        if(i >= 0) {
            ret->o = arr_at(ht_vals(self), i);
            arr_set(ht_vals(self), i, val);
            return LIB_OK;
        }

        ka = arr_grow(vm, ht_keys(self), self->len + 1);
        if(!ka)
            return LIB_OK;
        self->fields[0].o = ka;
        va = arr_grow(vm, ht_vals(self), self->len + 1);
        if(!va)
            return LIB_OK;
        self->fields[1].o = va;

        arr_set(ka, self->len, key);
        arr_set(va, self->len, val);
        self->len++;
        ret->o = NULL;
        return LIB_OK;
    }

    if(is(n, d, "get", NULL)) {
        i = ht_find(self, args[1].o);
        ret->o = i >= 0 ? arr_at(ht_vals(self), i) : NULL;
        return LIB_OK;
    }
    if(is(n, d, "containsKey", NULL)) {
        ret->i = ht_find(self, args[1].o) >= 0;
        return LIB_OK;
    }
    if(is(n, d, "contains", NULL) || is(n, d, "containsValue", NULL)) {
        ps_jobj *v = ht_vals(self);

        ret->i = 0;
        for(i = 0; i < self->len; i++) {
            if(obj_equals(arr_at(v, i), args[1].o)) {
                ret->i = 1;
                break;
            }
        }
        return LIB_OK;
    }

    if(is(n, d, "remove", NULL)) {
        ps_jobj *ka = ht_keys(self), *va = ht_vals(self);

        i = ht_find(self, args[1].o);
        ret->o = NULL;
        if(i < 0)
            return LIB_OK;
        ret->o = arr_at(va, i);
        for(; i < self->len - 1; i++) {
            arr_set(ka, i, arr_at(ka, i + 1));
            arr_set(va, i, arr_at(va, i + 1));
        }
        arr_set(ka, self->len - 1, NULL);
        arr_set(va, self->len - 1, NULL);
        self->len--;
        return LIB_OK;
    }

    if(is(n, d, "clear", NULL)) {
        ps_jobj *ka = ht_keys(self), *va = ht_vals(self);

        for(i = 0; i < self->len; i++) {
            arr_set(ka, i, NULL);
            arr_set(va, i, NULL);
        }
        self->len = 0;
        return LIB_OK;
    }

    if(is(n, d, "keys", NULL) || is(n, d, "elements", NULL)) {
        ps_jobj *e = ps_jvm_new(vm, loaded(vm, "java/util/Enumeration"));

        if(e && e->fields) {
            e->fields[0].o = self;
            e->fields[1].i = (n[0] == 'k') ? 1 : 2;
            e->len = 0;
        }
        ret->o = e;
        return LIB_OK;
    }

    return LIB_NO;
}

/* --- java.util.Enumeration -----------------------------------------------
 *
 * A cursor over whichever container made it, read live rather than from a
 * snapshot - so an applet that removes as it enumerates sees what a real JVM's
 * Enumeration would see. The view (elements, keys or values) is field slot 1.
 */
static int ju_enum(ps_jvm *vm, const char *n, const char *d, ps_jslot *args,
                   int nargs, ps_jslot *ret)
{
    ps_jobj *self = nargs > 0 ? args[0].o : NULL;
    ps_jobj *src;
    int      which;

    if(!self || !self->fields)
        return (nargs <= 0) ? LIB_NO : throw_npe(vm, "Enumeration");

    src   = self->fields[0].o;
    which = self->fields[1].i;

    if(is(n, d, "hasMoreElements", NULL)) {
        ret->i = src && self->len < src->len;
        return LIB_OK;
    }
    if(is(n, d, "nextElement", NULL)) {
        if(!src || self->len >= src->len)
            return throw_nse(vm);

        ret->o = arr_at(which == 2 ? ht_vals(src)
                      : which == 1 ? ht_keys(src) : vec_data(src),
                        self->len);
        self->len++;
        return LIB_OK;
    }
    return LIB_NO;
}

/* --- java.util.StringTokenizer -------------------------------------------
 *
 * How a <param> value gets taken apart. The delimiters are a set of
 * characters, not a separator string, and runs of them collapse - which is
 * why "a,,b" with a comma delimiter yields two tokens and not three.
 */
static int stok_is_delim(const char *dl, size_t dn, int c)
{
    size_t i;

    for(i = 0; i < dn; i++) {
        if((unsigned char)dl[i] == (unsigned char)c)
            return 1;
    }
    return 0;
}

/* The token starting at or after `from`, or -1. Its end goes to *end. */
static int32_t stok_next(const char *s, size_t sl, const char *dl, size_t dn,
                         int32_t from, int32_t *end)
{
    int32_t b = from;

    while(b < (int32_t)sl && stok_is_delim(dl, dn, (unsigned char)s[b]))
        b++;
    if(b >= (int32_t)sl)
        return -1;

    *end = b;
    while(*end < (int32_t)sl && !stok_is_delim(dl, dn, (unsigned char)s[*end]))
        (*end)++;
    return b;
}

static int ju_stok(ps_jvm *vm, const char *n, const char *d, ps_jslot *args,
                   int nargs, ps_jslot *ret)
{
    ps_jobj    *self = nargs > 0 ? args[0].o : NULL;
    const char *s, *dl;
    size_t      sl = 0, dn = 0;
    int32_t     b, e;

    if(!self || !self->fields)
        return (nargs <= 0) ? LIB_NO : throw_npe(vm, "StringTokenizer");

    if(is(n, d, "<init>", NULL)) {
        ps_jobj *text = nargs > 1 ? args[1].o : NULL;

        if(!text)
            return throw_npe(vm, "StringTokenizer");
        self->fields[0].o = text;
        self->fields[1].o = (nargs > 2 && args[2].o)
                          ? args[2].o : mkcstr(vm, " \t\n\r\f");
        self->fields[2].i = (nargs > 3) ? args[3].i : 0;
        self->len = 0;
        return LIB_OK;
    }

    s  = sbytes(self->fields[0].o, &sl);
    dl = sbytes(self->fields[1].o, &dn);
    if(!s || !dl)
        return throw_npe(vm, "StringTokenizer");

    if(is(n, d, "hasMoreTokens", NULL) || is(n, d, "hasMoreElements", NULL)) {
        ret->i = stok_next(s, sl, dl, dn, self->len, &e) >= 0;
        return LIB_OK;
    }
    if(is(n, d, "countTokens", NULL)) {
        int32_t at = self->len, count = 0;

        while((b = stok_next(s, sl, dl, dn, at, &e)) >= 0) {
            count++;
            at = e;
        }
        ret->i = count;
        return LIB_OK;
    }
    if(is(n, d, "nextToken", NULL) || is(n, d, "nextElement", NULL)) {
        /* nextToken(String) changes the delimiter set from here on, which is
         * how a parser switches from splitting on commas to splitting on
         * equals signs without building a second tokenizer. */
        if(nargs > 1 && args[1].o) {
            self->fields[1].o = args[1].o;
            dl = sbytes(args[1].o, &dn);
        }

        b = stok_next(s, sl, dl, dn, self->len, &e);
        if(b < 0)
            return throw_nse(vm);
        self->len = e;
        ret->o = mkstr(vm, s + b, (size_t)(e - b));
        return LIB_OK;
    }
    return LIB_NO;
}

/* --- java.util.Random ----------------------------------------------------
 *
 * The one class here whose algorithm is published in the API documentation
 * rather than only in an implementation: a 48-bit linear congruential
 * generator, seed scrambled on the way in, and each method defined in terms of
 * how many bits it draws. That makes a seeded sequence portable, which is not
 * a nicety - an applet that scatters stars or raindrops from `new Random(7)`
 * draws a specific picture, and drawing a different one would be a bug nobody
 * could see was a bug.
 *
 * Verified against a desktop JVM for every method here, seeded; see
 * java/test/applets/LibRand.java.
 */
#define RNG_MUL   0x5DEECE66DLL
#define RNG_ADD   0xBLL
#define RNG_MASK  ((1LL << 48) - 1)

/* Field slots, not field numbers: the layout is "JJI" and a long takes two.
 * Confusing the two is silent - the seed still advances, just not from where
 * the constructor put it. */
#define RNG_SEED   0
#define RNG_SPARE  2      /* nextGaussian's second value, held for next time */
#define RNG_HAVE   4

static void rng_seed(ps_jobj *r, int64_t seed)
{
    if(!r->fields)
        return;
    r->fields[RNG_SEED].j = (seed ^ RNG_MUL) & RNG_MASK;
    r->fields[RNG_HAVE].i = 0;             /* the spare gaussian is stale */
}

static int32_t rng_next(ps_jobj *r, int bits)
{
    int64_t s;

    if(!r->fields)
        return 0;
    s = (r->fields[RNG_SEED].j * RNG_MUL + RNG_ADD) & RNG_MASK;
    r->fields[RNG_SEED].j = s;
    return (int32_t)((uint64_t)s >> (48 - bits));
}

static double rng_double(ps_jobj *r)
{
    /* 53 bits, drawn 26 then 27, exactly as the documentation spells it. */
    int64_t hi = (int64_t)rng_next(r, 26) << 27;

    return (double)(hi + rng_next(r, 27)) * (1.0 / 9007199254740992.0);
}

static int ju_random(ps_jvm *vm, const char *n, const char *d, ps_jslot *args,
                     int nargs, ps_jslot *ret)
{
    ps_jobj *self = nargs > 0 ? args[0].o : NULL;

    if(nargs <= 0)
        return LIB_NO;
    if(!self || !self->fields)
        return throw_npe(vm, "Random");

    if(is(n, d, "<init>", NULL)) {
        /* An unseeded generator has to differ between runs, and the only
         * clock this machine has is the one below. */
        int64_t seed = (nargs > 1 && d[1] == 'J')
                     ? args[1].j
                     : (int64_t)time(NULL) * 1000 + (int64_t)(size_t)self;

        rng_seed(self, seed);
        return LIB_OK;
    }
    if(is(n, d, "setSeed", NULL)) {
        rng_seed(self, args[1].j);
        return LIB_OK;
    }

    if(is(n, d, "nextInt", "()I")) {
        ret->i = rng_next(self, 32);
        return LIB_OK;
    }
    if(is(n, d, "nextInt", "(I)I")) {
        int32_t bound = args[1].i, bits, val;

        if(bound <= 0) {
            ps_jvm_throw(vm, "java/lang/IllegalArgumentException",
                         "bound must be positive");
            return LIB_RAISED;
        }
        /* A power of two is taken from the high bits, because the low bits of
         * an LCG are the weak ones - which is why the documented algorithm
         * has this case at all. */
        if((bound & -bound) == bound) {
            ret->i = (int32_t)(((int64_t)bound *
                                (int64_t)rng_next(self, 31)) >> 31);
            return LIB_OK;
        }

        do {
            bits = rng_next(self, 31);
            val  = bits % bound;
        } while(bits - val + (bound - 1) < 0);   /* the rejection step */
        ret->i = val;
        return LIB_OK;
    }
    if(is(n, d, "nextLong", NULL)) {
        int64_t hi = (int64_t)rng_next(self, 32) << 32;

        ret->j = hi + (int64_t)rng_next(self, 32);
        return LIB_OK;
    }
    if(is(n, d, "nextBoolean", NULL)) {
        ret->i = rng_next(self, 1) != 0;
        return LIB_OK;
    }
    if(is(n, d, "nextDouble", NULL)) {
        ret->d = rng_double(self);
        return LIB_OK;
    }
    if(is(n, d, "nextFloat", NULL)) {
        ret->f = (float)rng_next(self, 24) / (float)(1 << 24);
        return LIB_OK;
    }
    if(is(n, d, "nextGaussian", NULL)) {
        double v1, v2, s, mul;

        if(self->fields[RNG_HAVE].i) {
            self->fields[RNG_HAVE].i = 0;
            ret->d = self->fields[RNG_SPARE].d;
            return LIB_OK;
        }
        /* The polar method, as the documentation spells it: two values come
         * out of it and the second is kept for the next call, so the
         * sequence a seeded applet sees depends on that being held. */
        do {
            v1 = 2.0 * rng_double(self) - 1.0;
            v2 = 2.0 * rng_double(self) - 1.0;
            s  = v1 * v1 + v2 * v2;
        } while(s >= 1.0 || s == 0.0);

        mul = sqrt(-2.0 * log(s) / s);
        self->fields[RNG_SPARE].d = v2 * mul;
        self->fields[RNG_HAVE].i  = 1;
        ret->d = v1 * mul;
        return LIB_OK;
    }

    return LIB_NO;
}

/* --- the clock -----------------------------------------------------------
 *
 * Milliseconds since the epoch. An applet uses it two ways: as a timestamp,
 * and as a frame timer where only the difference matters, so the resolution
 * has to be better than the second that time() offers or every animation
 * driven by it moves in one-second jumps.
 */
static int64_t now_ms(void)
{
#ifdef _arch_dreamcast
    /* The console's uptime counter, offset by the RTC so the absolute value
     * is a real date as well as a usable delta. */
    static int64_t base;

    if(!base)
        base = (int64_t)time(NULL) * 1000 - (int64_t)timer_ms_gettime64();
    return base + (int64_t)timer_ms_gettime64();
#else
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + (int64_t)tv.tv_usec / 1000;
#endif
}

static int elem_bytes(uint8_t k)
{
    switch(k) {
    case PS_T_BOOL: case PS_T_BYTE:   return 1;
    case PS_T_CHAR: case PS_T_SHORT:  return 2;
    case PS_T_LONG: case PS_T_DOUBLE: return 8;
    case PS_T_REF:                    return (int)sizeof(void *);
    default:                          return 4;
    }
}

static int jl_system(ps_jvm *vm, const char *n, const char *d, ps_jslot *args,
                     int nargs, ps_jslot *ret)
{
    (void)d;

    if(!strcmp(n, "currentTimeMillis")) {
        ret->j = now_ms();
        return LIB_OK;
    }
    if(!strcmp(n, "arraycopy") && nargs >= 5) {
        ps_jobj *src = args[0].o, *dst = args[2].o;
        int32_t  sp = args[1].i, dp = args[3].i, cnt = args[4].i;
        int      es;

        if(!src || !dst)
            return throw_npe(vm, "arraycopy");
        if(src->kind != PS_OBJ_ARRAY || dst->kind != PS_OBJ_ARRAY ||
           src->elem != dst->elem) {
            ps_jvm_throw(vm, "java/lang/ArrayStoreException", "arraycopy");
            return LIB_RAISED;
        }
        if(cnt < 0 || sp < 0 || dp < 0 ||
           sp + cnt > src->len || dp + cnt > dst->len)
            return throw_arr_index(vm, cnt);

        /* memmove, not memcpy: the spec says it behaves as if the source went
         * through a temporary, and `arraycopy(a, 0, a, 1, n)` to shift an
         * array along is a thing applets genuinely write. */
        es = elem_bytes(src->elem);
        memmove((char *)dst->data + (size_t)dp * es,
                (char *)src->data + (size_t)sp * es, (size_t)cnt * es);
        return LIB_OK;
    }
    if(!strcmp(n, "nanoTime")) {
        ret->j = now_ms() * 1000000;
        return LIB_OK;
    }
    if(!strcmp(n, "identityHashCode")) {
        ret->i = (int32_t)(size_t)(nargs > 0 ? args[0].o : NULL);
        return LIB_OK;
    }
    return LIB_NO;
}

/* --- java.util.Date ------------------------------------------------------
 *
 * Only the part a clock applet uses. The calendar fields come from the C
 * library's local time, which on the console is the RTC the user set - so a
 * clock reads right rather than reading UTC. The 1.0 accessors are deprecated
 * everywhere else and are the only ones an applet of this vintage calls.
 */
static int ju_date(ps_jvm *vm, const char *n, const char *d, ps_jslot *args,
                   int nargs, ps_jslot *ret)
{
    ps_jobj  *self = nargs > 0 ? args[0].o : NULL;
    time_t    t;
    struct tm *lt;

    if(!self || !self->fields)
        return (nargs <= 0) ? LIB_NO : throw_npe(vm, "Date");

    if(is(n, d, "<init>", NULL)) {
        self->fields[0].j = (nargs > 1 && d[1] == 'J') ? args[1].j : now_ms();
        return LIB_OK;
    }
    if(is(n, d, "getTime", NULL)) { ret->j = self->fields[0].j;
                                    return LIB_OK; }
    if(is(n, d, "setTime", NULL)) { self->fields[0].j = args[1].j;
                                    return LIB_OK; }

    t  = (time_t)(self->fields[0].j / 1000);
    lt = localtime(&t);
    if(!lt)
        return LIB_NO;

    if(is(n, d, "getYear", NULL))    { ret->i = lt->tm_year;  return LIB_OK; }
    if(is(n, d, "getMonth", NULL))   { ret->i = lt->tm_mon;   return LIB_OK; }
    if(is(n, d, "getDate", NULL))    { ret->i = lt->tm_mday;  return LIB_OK; }
    if(is(n, d, "getDay", NULL))     { ret->i = lt->tm_wday;  return LIB_OK; }
    if(is(n, d, "getHours", NULL))   { ret->i = lt->tm_hour;  return LIB_OK; }
    if(is(n, d, "getMinutes", NULL)) { ret->i = lt->tm_min;   return LIB_OK; }
    if(is(n, d, "getSeconds", NULL)) { ret->i = lt->tm_sec;   return LIB_OK; }

    return LIB_NO;
}

/* --- Object.toString ----------------------------------------------------- */

static ps_jobj *obj_string(ps_jvm *vm, ps_jobj *o)
{
    char buf[80];
    int  kind;

    if(!o)
        return mkcstr(vm, "null");
    if(is_string(o))
        return o;

    kind = box_kind_of(o);
    if(kind != BOX_NONE) {
        box_text(buf, sizeof buf, o, kind);
        return mkcstr(vm, buf);
    }

    /* A StringBuffer, which concatenation does reach when an applet builds one
     * and appends it to another. */
    if(o->cls && o->native && o->kind == PS_OBJ_INSTANCE &&
       (!strcmp(o->cls->name, "java/lang/StringBuffer") ||
        !strcmp(o->cls->name, "java/lang/StringBuilder")))
        return mkstr(vm, (const char *)o->native, (size_t)o->len);

    /* The object's own toString, if it wrote one.
     *
     * This is what makes `"total: " + thing` print the thing rather than its
     * address, and applets concatenate their own objects constantly. It means
     * re-entering the interpreter from inside a native call, which ps_jvm_call
     * supports - it runs a nested method to completion - but only for a method
     * with bytecode. A native toString would come back through ps_jre_call
     * with the same object and recurse, so those keep the default below.
     *
     * A toString that fails leaves the failure standing and falls back rather
     * than taking the applet down in the middle of a concatenation. */
    if(o->cls && !o->cls->native) {
        ps_jmethod *ts = ps_jclass_find_method(o->cls, "toString",
                                               "()Ljava/lang/String;");

        if(ts && ts->code) {
            ps_jslot argv[1], r;

            memset(&r, 0, sizeof r);
            argv[0].o = o;
            if(ps_jvm_call(vm, o->cls, ts, argv, 1, &r) == 0 &&
               r.o && is_string(r.o))
                return r.o;
            vm->failed = 0;
        }
    }

    snprintf(buf, sizeof buf, "%s@%lx",
             o->cls ? o->cls->name : "java.lang.Object",
             (unsigned long)(size_t)o);
    return mkcstr(vm, buf);
}

/* --- dispatch ------------------------------------------------------------ */

/* Cheap enough to run on every native call, which is what it does. Most of
 * them are java/awt and stop at the second comparison. */
static int owns(const char *cls)
{
    if(strncmp(cls, "java/", 5))
        return 0;

    switch(cls[5]) {
    case 'u':
        return !strcmp(cls, "java/util/Vector") ||
               !strcmp(cls, "java/util/Hashtable") ||
               !strcmp(cls, "java/util/Enumeration") ||
               !strcmp(cls, "java/util/StringTokenizer") ||
               !strcmp(cls, "java/util/Random") ||
               !strcmp(cls, "java/util/Date");
    case 'l':
        return !strcmp(cls, "java/lang/String") ||
               !strcmp(cls, "java/lang/StringBuffer") ||
               !strcmp(cls, "java/lang/StringBuilder") ||
               !strcmp(cls, "java/lang/System") ||
               box_kind(cls) != BOX_NONE;
    default:
        return 0;
    }
}

int ps_jlib_call(ps_jvm *vm, const char *cls, const char *name,
                 const char *desc, ps_jslot *args, int nargs, ps_jslot *ret,
                 int *handled)
{
    int r, kind;

    /* Registration rides on the super-constructor call, which every object an
     * applet creates makes before it can do anything else - so the classes are
     * in the table before the first `new Vector` needs to resolve. See
     * register_classes. */
    if(name[0] == '<')
        register_classes(vm);

    if(!owns(cls))
        return 0;

    pin_args(vm, desc, args, nargs);

    if(!strcmp(cls, "java/lang/String"))
        r = jl_string(vm, name, desc, args, nargs, ret);
    else if(!strcmp(cls, "java/lang/System"))
        r = jl_system(vm, name, desc, args, nargs, ret);
    else if(!strcmp(cls, "java/lang/StringBuffer") ||
            !strcmp(cls, "java/lang/StringBuilder"))
        r = jl_sbuf(vm, name, desc, args, nargs, ret);
    else if(!strcmp(cls, "java/util/Vector"))
        r = ju_vector(vm, name, desc, args, nargs, ret);
    else if(!strcmp(cls, "java/util/Hashtable"))
        r = ju_hashtable(vm, name, desc, args, nargs, ret);
    else if(!strcmp(cls, "java/util/Enumeration"))
        r = ju_enum(vm, name, desc, args, nargs, ret);
    else if(!strcmp(cls, "java/util/StringTokenizer"))
        r = ju_stok(vm, name, desc, args, nargs, ret);
    else if(!strcmp(cls, "java/util/Random"))
        r = ju_random(vm, name, desc, args, nargs, ret);
    else if(!strcmp(cls, "java/util/Date"))
        r = ju_date(vm, name, desc, args, nargs, ret);
    else if((kind = box_kind(cls)) != BOX_NONE)
        r = jl_box(vm, kind, cls, name, desc, args, nargs, ret);
    else
        return 0;

    /* A method this file does not know is left alone rather than absorbed, so
     * the interpreter reports it by name - which is the message that says what
     * to write next. */
    if(r == LIB_NO)
        return 0;

    if(r == LIB_OK && desc_ref_result(desc))
        pin(vm, PIN_SCRATCH, ret->o);
    *handled = (r == LIB_OK);
    return 1;
}
