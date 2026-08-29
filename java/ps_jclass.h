/* Java class file reader.
 *
 * Parses the format JVMS §4 describes, far enough to execute: constant pool,
 * fields, methods, and each method's Code attribute. Everything else in a
 * class file - debug tables, annotations, signatures, the stack map - is
 * skipped by length, which is what makes this small.
 *
 * Verification is deliberately absent. A verifier exists to let a JVM trust
 * bytecode it did not produce, and this one does not trust it: the interpreter
 * bounds-checks every stack push, local index and pool reference as it runs.
 * That is slower per instruction and much smaller, which is the right trade on
 * a machine with 12MB - and the safety property is the same, since an applet
 * off the open web is hostile by default either way.
 *
 * Class file versions: 45 (JDK 1.1, what applets of the period were compiled
 * as) through 52 (Java 8, what a modern javac -release 8 emits, and how the
 * tests here are built). The bytecode for the constructs applets use is
 * identical across that range; only the constant pool grew tags, and the ones
 * it grew - method handles, invokedynamic - only appear if you use lambdas.
 */
#ifndef PS_JCLASS_H
#define PS_JCLASS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* JVMS table 4.4-A. */
enum {
    PS_CP_UTF8    = 1,
    PS_CP_INTEGER = 3,
    PS_CP_FLOAT   = 4,
    PS_CP_LONG    = 5,
    PS_CP_DOUBLE  = 6,
    PS_CP_CLASS   = 7,
    PS_CP_STRING  = 8,
    PS_CP_FIELD   = 9,
    PS_CP_METHOD  = 10,
    PS_CP_IMETHOD = 11,
    PS_CP_NAMETYPE = 12,
    PS_CP_MHANDLE = 15,
    PS_CP_MTYPE   = 16,
    PS_CP_DYNAMIC = 17,
    PS_CP_INVOKEDYN = 18,
    PS_CP_MODULE  = 19,
    PS_CP_PACKAGE = 20
};

typedef struct {
    uint8_t tag;
    union {
        struct { const char *s; uint16_t len; } utf8;
        int32_t  i;
        float    f;
        int64_t  j;
        double   d;
        uint16_t index;                       /* Class, String: name/value */
        struct { uint16_t a, b; } pair;       /* ref: class + nameandtype,
                                                 nameandtype: name + desc */
    } u;
} ps_jconst;

/* Access flags, the handful that change behaviour. */
#define PS_ACC_STATIC    0x0008
#define PS_ACC_FINAL     0x0010
#define PS_ACC_NATIVE    0x0100
#define PS_ACC_ABSTRACT  0x0400
#define PS_ACC_INTERFACE 0x0200

typedef struct {
    const char *name;
    const char *desc;
    uint16_t    access;

    /* Code attribute. NULL for abstract and native methods. */
    const uint8_t *code;
    uint32_t       code_len;
    uint16_t       max_stack;
    uint16_t       max_locals;

    /* Exception table, pointing into the class image.
     *
     * Four big-endian u2s per entry: start_pc, end_pc, handler_pc,
     * catch_type. A catch_type of zero is `finally`, or a catch-all - which
     * is what javac emits for a synchronized block's cleanup and what most
     * applet try/catch blocks reduce to once the type is one this VM treats
     * as universal. */
    const uint8_t *etab;
    uint16_t       etab_len;    /* entries, not bytes */

    /* Argument slots the caller must supply, `this` included for instance
     * methods. Computed from the descriptor once at load, because working it
     * out per call is the single hottest piece of parsing in an interpreter. */
    uint16_t arg_slots;
    uint8_t  ret_kind;      /* one of PS_T_*, PS_T_VOID for none */
} ps_jmethod;

typedef struct {
    const char *name;
    const char *desc;
    uint16_t    access;
    uint16_t    slot;       /* index into the object's or class's field block */
    uint8_t     kind;
} ps_jfield;

/* Descriptor kinds. Two slots for J and D, per JVMS §2.6.1. */
enum {
    PS_T_VOID = 0, PS_T_INT, PS_T_LONG, PS_T_FLOAT, PS_T_DOUBLE, PS_T_REF,
    PS_T_BYTE, PS_T_CHAR, PS_T_SHORT, PS_T_BOOL
};

typedef struct ps_jclass {
    /* The file bytes, owned. Names and descriptors point into it rather than
     * being copied out - a class file is already a string table and building
     * a second one would double the resident cost of every loaded class. */
    uint8_t *raw;
    size_t   raw_len;

    ps_jconst *cp;
    uint16_t   cp_count;

    const char *name;
    const char *super_name;
    uint16_t    access;

    ps_jfield  *fields;
    uint16_t    field_count;
    ps_jmethod *methods;
    uint16_t    method_count;

    /* Slots for instance fields declared here plus everything inherited, and
     * separately for statics. Resolved at link time. */
    uint16_t inst_slots;
    uint16_t static_slots;

    struct ps_jclass *super;    /* linked at load */

    /* Set for the classes the runtime implements in C rather than bytecode -
     * java/awt/Graphics and friends. Their methods have no Code attribute and
     * are dispatched by name. */
    int native;

    /* Whether <clinit> has been run, or was decided not to need running.
     *
     * Set before the initialiser is entered rather than after: a static
     * initialiser that touches its own class must find it already in progress
     * and carry on, which is what the JVM specifies and what stops a class
     * whose <clinit> reads one of its own statics from recursing forever. */
    uint8_t inited;

    void *statics;              /* ps_jslot[static_slots], owned by the vm */
} ps_jclass;

/* Parses a .class image. The buffer is taken over on success and freed by
 * ps_jclass_free; on failure it is left to the caller.
 *
 * err, when non-NULL, receives a short static string naming what was wrong.
 * A browser loading applets off the open web sees malformed class files as a
 * matter of course, and "it failed" is not enough to tell a truncated download
 * from a file we simply cannot read yet. */
ps_jclass *ps_jclass_parse(uint8_t *raw, size_t len, const char **err);
void       ps_jclass_free(ps_jclass *c);

ps_jmethod *ps_jclass_find_method(ps_jclass *c, const char *name,
                                  const char *desc);
ps_jfield  *ps_jclass_find_field(ps_jclass *c, const char *name);

/* Constant pool accessors that range-check. Every one of these can be reached
 * from a pool index embedded in bytecode, so none of them may trust it. */
const char *ps_jcp_utf8(const ps_jclass *c, uint16_t i);
const char *ps_jcp_class_name(const ps_jclass *c, uint16_t i);

/* For Fieldref/Methodref/InterfaceMethodref: splits out the three strings.
 * Returns 0 on success. */
int ps_jcp_ref(const ps_jclass *c, uint16_t i, const char **cls,
               const char **name, const char **desc);

/* Slots a descriptor's arguments occupy, and the return kind. */
int ps_jdesc_arg_slots(const char *desc, uint8_t *ret_kind);
uint8_t ps_jdesc_kind(const char *desc);

#ifdef __cplusplus
}
#endif

#endif /* PS_JCLASS_H */
