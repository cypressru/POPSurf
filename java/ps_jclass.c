#include "ps_jclass.h"

#include <stdlib.h>
#include <string.h>

/* --- bounded reader ------------------------------------------------------
 *
 * Every field of a class file is read through this. A class arriving off the
 * open web is hostile input, and the failure mode a length-prefixed format
 * invites - a count that says 60000 entries in a 400 byte file - has to become
 * a clean parse failure rather than a walk off the end of the buffer.
 */
typedef struct {
    const uint8_t *p, *end;
    int            bad;
} rd;

static uint8_t r8(rd *r)
{
    if(r->p + 1 > r->end) { r->bad = 1; return 0; }
    return *r->p++;
}

static uint16_t r16(rd *r)
{
    uint16_t v;

    if(r->p + 2 > r->end) { r->bad = 1; return 0; }
    v = (uint16_t)((r->p[0] << 8) | r->p[1]);
    r->p += 2;
    return v;
}

static uint32_t r32(rd *r)
{
    uint32_t v;

    if(r->p + 4 > r->end) { r->bad = 1; return 0; }
    v = ((uint32_t)r->p[0] << 24) | ((uint32_t)r->p[1] << 16) |
        ((uint32_t)r->p[2] << 8)  |  (uint32_t)r->p[3];
    r->p += 4;
    return v;
}

static int64_t r64(rd *r)
{
    uint32_t hi = r32(r), lo = r32(r);

    return (int64_t)(((uint64_t)hi << 32) | lo);
}

static const uint8_t *rskip(rd *r, uint32_t n)
{
    const uint8_t *at = r->p;

    if(n > (uint32_t)(r->end - r->p)) { r->bad = 1; return NULL; }
    r->p += n;
    return at;
}

/* --- descriptors --------------------------------------------------------- */

uint8_t ps_jdesc_kind(const char *d)
{
    if(!d)
        return PS_T_VOID;

    switch(*d) {
    case 'V': return PS_T_VOID;
    case 'J': return PS_T_LONG;
    case 'F': return PS_T_FLOAT;
    case 'D': return PS_T_DOUBLE;
    case 'B': return PS_T_BYTE;
    case 'C': return PS_T_CHAR;
    case 'S': return PS_T_SHORT;
    case 'Z': return PS_T_BOOL;
    case 'I': return PS_T_INT;
    case 'L': case '[': return PS_T_REF;
    default:  return PS_T_INT;
    }
}

int ps_jdesc_arg_slots(const char *desc, uint8_t *ret_kind)
{
    int n = 0;

    if(!desc || *desc != '(')
        return -1;
    desc++;

    while(*desc && *desc != ')') {
        switch(*desc) {
        case 'J': case 'D':
            /* Category 2. Two slots, which is why an interpreter cannot just
             * count arguments. */
            n += 2;
            desc++;
            break;
        case 'L':
            n++;
            while(*desc && *desc != ';')
                desc++;
            if(*desc)
                desc++;
            break;
        case '[':
            while(*desc == '[')
                desc++;
            if(*desc == 'L') {
                while(*desc && *desc != ';')
                    desc++;
                if(*desc)
                    desc++;
            }
            else if(*desc) {
                desc++;
            }
            n++;
            break;
        default:
            n++;
            desc++;
            break;
        }
    }

    if(*desc != ')')
        return -1;
    desc++;

    if(ret_kind)
        *ret_kind = ps_jdesc_kind(desc);
    return n;
}

/* --- constant pool ------------------------------------------------------- */

const char *ps_jcp_utf8(const ps_jclass *c, uint16_t i)
{
    if(i == 0 || i >= c->cp_count || c->cp[i].tag != PS_CP_UTF8)
        return NULL;
    return c->cp[i].u.utf8.s;
}

const char *ps_jcp_class_name(const ps_jclass *c, uint16_t i)
{
    if(i == 0 || i >= c->cp_count || c->cp[i].tag != PS_CP_CLASS)
        return NULL;
    return ps_jcp_utf8(c, c->cp[i].u.index);
}

int ps_jcp_ref(const ps_jclass *c, uint16_t i, const char **cls,
               const char **name, const char **desc)
{
    uint16_t nt;

    if(i == 0 || i >= c->cp_count)
        return -1;
    if(c->cp[i].tag != PS_CP_FIELD && c->cp[i].tag != PS_CP_METHOD &&
       c->cp[i].tag != PS_CP_IMETHOD)
        return -1;

    nt = c->cp[i].u.pair.b;
    if(nt == 0 || nt >= c->cp_count || c->cp[nt].tag != PS_CP_NAMETYPE)
        return -1;

    if(cls)  *cls  = ps_jcp_class_name(c, c->cp[i].u.pair.a);
    if(name) *name = ps_jcp_utf8(c, c->cp[nt].u.pair.a);
    if(desc) *desc = ps_jcp_utf8(c, c->cp[nt].u.pair.b);

    return (cls && !*cls) || (name && !*name) || (desc && !*desc) ? -1 : 0;
}

static int parse_pool(ps_jclass *c, rd *r, const char **err)
{
    uint16_t i;

    c->cp_count = r16(r);
    if(r->bad || c->cp_count == 0) {
        *err = "bad constant pool count";
        return -1;
    }

    c->cp = (ps_jconst *)calloc(c->cp_count, sizeof(ps_jconst));
    if(!c->cp) {
        *err = "out of memory";
        return -1;
    }

    /* Index 0 is unused, by definition. */
    for(i = 1; i < c->cp_count; i++) {
        uint8_t tag = r8(r);

        if(r->bad) {
            *err = "truncated constant pool";
            return -1;
        }

        c->cp[i].tag = tag;

        switch(tag) {
        case PS_CP_UTF8: {
            uint16_t       n = r16(r);
            const uint8_t *s = rskip(r, n);

            if(!s) {
                *err = "truncated utf8";
                return -1;
            }
            c->cp[i].u.utf8.s   = (const char *)s;
            c->cp[i].u.utf8.len = n;
            break;
        }
        case PS_CP_INTEGER: c->cp[i].u.i = (int32_t)r32(r); break;
        case PS_CP_FLOAT: {
            uint32_t bits = r32(r);
            memcpy(&c->cp[i].u.f, &bits, 4);
            break;
        }
        case PS_CP_LONG:
            c->cp[i].u.j = r64(r);
            i++;   /* JVMS §4.4.5: takes two pool entries. */
            break;
        case PS_CP_DOUBLE: {
            int64_t bits = r64(r);
            memcpy(&c->cp[i].u.d, &bits, 8);
            i++;
            break;
        }
        case PS_CP_CLASS:
        case PS_CP_STRING:
        case PS_CP_MTYPE:
        case PS_CP_MODULE:
        case PS_CP_PACKAGE:
            c->cp[i].u.index = r16(r);
            break;
        case PS_CP_FIELD:
        case PS_CP_METHOD:
        case PS_CP_IMETHOD:
        case PS_CP_NAMETYPE:
        case PS_CP_DYNAMIC:
        case PS_CP_INVOKEDYN:
            c->cp[i].u.pair.a = r16(r);
            c->cp[i].u.pair.b = r16(r);
            break;
        case PS_CP_MHANDLE:
            c->cp[i].u.pair.a = r8(r);
            c->cp[i].u.pair.b = r16(r);
            break;
        default:
            /* A tag we do not know is not survivable: entries are variable
             * width, so we no longer know where the next one starts. */
            *err = "unknown constant pool tag";
            return -1;
        }
    }

    /* The UTF8 entries point into the file image, which is not NUL
     * terminated between fields. Terminating them in place is safe - the byte
     * after a UTF8 entry's data is the next entry's tag, which has already
     * been read - and it lets every name and descriptor be used as an ordinary
     * C string for the life of the class. */
    for(i = 1; i < c->cp_count; i++) {
        if(c->cp[i].tag == PS_CP_UTF8) {
            char *s = (char *)c->cp[i].u.utf8.s;

            s[c->cp[i].u.utf8.len] = '\0';
        }
    }

    return 0;
}

/* --- attributes ---------------------------------------------------------- */

/* Skips an attribute block, capturing Code when it appears. Everything else -
 * LineNumberTable, StackMapTable, Signature, annotations - is stepped over by
 * its own declared length, which is why unknown attributes cost nothing. */
static int parse_attrs(ps_jclass *c, rd *r, ps_jmethod *m, const char **err)
{
    uint16_t n = r16(r), k;

    for(k = 0; k < n && !r->bad; k++) {
        uint16_t       ni  = r16(r);
        uint32_t       len = r32(r);
        const char    *an  = ps_jcp_utf8(c, ni);
        const uint8_t *at;

        if(r->bad) {
            *err = "truncated attribute";
            return -1;
        }

        if(m && an && !strcmp(an, "Code")) {
            rd cr;

            at = rskip(r, len);
            if(!at) {
                *err = "truncated Code";
                return -1;
            }

            cr.p = at;
            cr.end = at + len;
            cr.bad = 0;

            m->max_stack  = r16(&cr);
            m->max_locals = r16(&cr);
            m->code_len   = r32(&cr);
            m->code       = rskip(&cr, m->code_len);

            if(cr.bad || !m->code) {
                *err = "bad Code attribute";
                return -1;
            }

            /* The exception table follows the code. Without it a throw has
             * nowhere to land and every try/catch in the method is a lie. */
            m->etab_len = r16(&cr);
            m->etab     = rskip(&cr, (uint32_t)m->etab_len * 8);
            if(cr.bad || !m->etab) {
                m->etab     = NULL;
                m->etab_len = 0;
            }
            continue;
        }

        if(!rskip(r, len)) {
            *err = "attribute length past end of file";
            return -1;
        }
    }

    return r->bad ? -1 : 0;
}

/* --- members ------------------------------------------------------------- */

static int parse_members(ps_jclass *c, rd *r, int is_method, const char **err)
{
    uint16_t n = r16(r), i;

    if(r->bad) {
        *err = "truncated member count";
        return -1;
    }

    if(is_method) {
        c->method_count = n;
        c->methods = n ? (ps_jmethod *)calloc(n, sizeof(ps_jmethod)) : NULL;
        if(n && !c->methods) { *err = "out of memory"; return -1; }
    }
    else {
        c->field_count = n;
        c->fields = n ? (ps_jfield *)calloc(n, sizeof(ps_jfield)) : NULL;
        if(n && !c->fields) { *err = "out of memory"; return -1; }
    }

    for(i = 0; i < n; i++) {
        uint16_t acc  = r16(r);
        uint16_t ni   = r16(r);
        uint16_t di   = r16(r);
        const char *nm = ps_jcp_utf8(c, ni);
        const char *ds = ps_jcp_utf8(c, di);

        if(r->bad || !nm || !ds) {
            *err = "bad member entry";
            return -1;
        }

        if(is_method) {
            ps_jmethod *m = &c->methods[i];
            int         slots;

            m->access = acc;
            m->name   = nm;
            m->desc   = ds;

            slots = ps_jdesc_arg_slots(ds, &m->ret_kind);
            if(slots < 0) {
                *err = "bad method descriptor";
                return -1;
            }
            /* Instance methods receive `this` in local 0. */
            if(!(acc & PS_ACC_STATIC))
                slots++;
            m->arg_slots = (uint16_t)slots;

            if(parse_attrs(c, r, m, err) != 0)
                return -1;
        }
        else {
            ps_jfield *f = &c->fields[i];

            f->access = acc;
            f->name   = nm;
            f->desc   = ds;
            f->kind   = ps_jdesc_kind(ds);

            if(parse_attrs(c, r, NULL, err) != 0)
                return -1;
        }
    }

    return 0;
}

/* --- entry point --------------------------------------------------------- */

ps_jclass *ps_jclass_parse(uint8_t *raw, size_t len, const char **err)
{
    ps_jclass *c;
    rd         r;
    uint16_t   minor, major, this_i, super_i, ifaces, i;
    const char *dummy;

    if(!err)
        err = &dummy;
    *err = NULL;

    if(!raw || len < 10) {
        *err = "too short to be a class file";
        return NULL;
    }

    c = (ps_jclass *)calloc(1, sizeof *c);
    if(!c) {
        *err = "out of memory";
        return NULL;
    }

    r.p   = raw;
    r.end = raw + len;
    r.bad = 0;

    if(r32(&r) != 0xCAFEBABEu) {
        *err = "not a class file";
        free(c);
        return NULL;
    }

    minor = r16(&r);
    major = r16(&r);
    (void)minor;

    /* 45 is JDK 1.1, which is what applets of the period were compiled as.
     * 52 is Java 8, the oldest a current javac will emit and what the tests
     * are built with. Above that only brings constant pool tags we would
     * reject anyway, and rejecting up front gives a better message than
     * failing later on an unknown opcode. */
    if(major < 45 || major > 52) {
        *err = (major > 52) ? "class file too new (needs -release 8 or lower)"
                            : "class file too old";
        free(c);
        return NULL;
    }

    if(parse_pool(c, &r, err) != 0)
        goto fail;

    c->access = r16(&r);
    this_i    = r16(&r);
    super_i   = r16(&r);

    c->name = ps_jcp_class_name(c, this_i);
    if(!c->name) {
        *err = "bad this_class";
        goto fail;
    }
    /* java/lang/Object alone has no superclass. */
    c->super_name = super_i ? ps_jcp_class_name(c, super_i) : NULL;

    ifaces = r16(&r);
    for(i = 0; i < ifaces; i++)
        (void)r16(&r);

    if(r.bad) {
        *err = "truncated header";
        goto fail;
    }

    if(parse_members(c, &r, 0, err) != 0)
        goto fail;
    if(parse_members(c, &r, 1, err) != 0)
        goto fail;
    if(parse_attrs(c, &r, NULL, err) != 0)
        goto fail;

    c->raw     = raw;
    c->raw_len = len;
    return c;

fail:
    free(c->cp);
    free(c->fields);
    free(c->methods);
    free(c);
    return NULL;
}

void ps_jclass_free(ps_jclass *c)
{
    if(!c)
        return;

    free(c->cp);
    free(c->fields);
    free(c->methods);
    free(c->statics);
    free(c->raw);
    free(c);
}

ps_jmethod *ps_jclass_find_method(ps_jclass *c, const char *name,
                                  const char *desc)
{
    uint16_t i;

    for(; c; c = c->super) {
        for(i = 0; i < c->method_count; i++) {
            if(strcmp(c->methods[i].name, name))
                continue;
            /* A NULL descriptor matches on name alone, which is what the
             * runtime wants when it is looking up paint() and there is only
             * one of them. */
            if(desc && strcmp(c->methods[i].desc, desc))
                continue;
            return &c->methods[i];
        }
    }
    return NULL;
}

ps_jfield *ps_jclass_find_field(ps_jclass *c, const char *name)
{
    uint16_t i;

    for(; c; c = c->super) {
        for(i = 0; i < c->field_count; i++) {
            if(!strcmp(c->fields[i].name, name))
                return &c->fields[i];
        }
    }
    return NULL;
}
