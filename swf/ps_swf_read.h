/* The record primitives shared by every tag parser.
 *
 * These lived inside ps_swf_parse.c until a second and a third parser needed
 * them. A morph shape reads two RECTs and, per gradient fill, two MATRIXes and
 * two colours per stop; an edit text reads a RECT and a colour. Copying them
 * would create three places where the MATRIX optional-field rule could drift
 * apart, and that is the rule whose failure is silent - see read_matrix.
 *
 * static inline rather than linked because each is a handful of instructions
 * and every one of them sits inside a parse loop. The duplicate copies cost
 * less than the call would.
 */
#ifndef PS_SWF_READ_H
#define PS_SWF_READ_H

#include "ps_swf.h"
#include "ps_swf_bits.h"
#include "ps_swf_mem.h"

#include <string.h>

/* Style indices are stored in sixteen bits per edge, and edges are by far the
 * largest thing a shape holds - widening them to be safe would cost a quarter
 * of the shape's memory to describe files that do not exist. Capping the
 * table instead keeps every index inside the type by construction. A shape
 * with 65535 distinct fills is not artwork, it is an attack. */
#define MAX_STYLES 65535u

/* Growable array. Doubling rather than exact sizing because the element
 * counts are not known until the records have been read, and re-reading the
 * stream to count first would double the parse cost to save a fraction of the
 * memory. `cap_limit` is the input-derived ceiling: no allocation is ever
 * sized by a field, only by the number of bytes that actually arrived. */
typedef struct {
    void   *base;
    uint32_t n, cap;
    size_t   esz;
} vec;

static inline int vec_push(vec *v, const void *elem, uint32_t cap_limit)
{
    if(v->n == v->cap) {
        uint32_t want = v->cap ? v->cap * 2 : 16;
        void    *nb;

        if(want > cap_limit)
            want = cap_limit;
        if(want <= v->n)
            return -1;
        nb = ps_swf_realloc(v->base, (size_t)want * v->esz);
        if(!nb)
            return -1;
        v->base = nb;
        v->cap  = want;
    }
    memcpy((unsigned char *)v->base + (size_t)v->n * v->esz, elem, v->esz);
    v->n++;
    return 0;
}

static inline void read_rect(ps_bits *b, int32_t *x0, int32_t *x1,
                             int32_t *y0, int32_t *y1)
{
    int n = (int)ps_bits_ub(b, 5);

    *x0 = ps_bits_sb(b, n);
    *x1 = ps_bits_sb(b, n);
    *y0 = ps_bits_sb(b, n);
    *y1 = ps_bits_sb(b, n);
    ps_bits_align(b);
}

/* MATRIX, into the a,b,c,d,tx,ty order ps_swf_gradient documents. Scale and
 * rotate are 16.16 fixed point and optional, translate is in twips and always
 * present; the identity defaults matter because a file that omits the scale
 * block is asking for 1, not 0.
 *
 * Stepping this exactly is what the rest of the fill style array depends on.
 * Every style after this one is read from the bit position this leaves behind,
 * so an error of one bit here does not corrupt one fill, it corrupts all of
 * them from here to the end of the array - and since the styles are then
 * indexed by edges that parsed fine, the result is a correctly shaped picture
 * in wrong colours, which reads as a compositing bug. */
static inline void read_matrix(ps_bits *b, float *m)
{
    m[0] = m[3] = 1.0f;
    m[1] = m[2] = 0.0f;
    m[4] = m[5] = 0.0f;

    if(ps_bits_ub(b, 1)) {                 /* HasScale */
        int n = (int)ps_bits_ub(b, 5);
        m[0] = ps_bits_fb(b, n);           /* ScaleX */
        m[3] = ps_bits_fb(b, n);           /* ScaleY */
    }
    if(ps_bits_ub(b, 1)) {                 /* HasRotate */
        int n = (int)ps_bits_ub(b, 5);
        m[1] = ps_bits_fb(b, n);           /* RotateSkew0 */
        m[2] = ps_bits_fb(b, n);           /* RotateSkew1 */
    }
    {
        int n = (int)ps_bits_ub(b, 5);
        m[4] = (float)ps_bits_sb(b, n);
        m[5] = (float)ps_bits_sb(b, n);
    }
    ps_bits_align(b);
}

static inline ps_swf_rgba read_color(ps_bits *b, int with_alpha)
{
    ps_swf_rgba c;

    c.r = ps_bits_u8(b);
    c.g = ps_bits_u8(b);
    c.b = ps_bits_u8(b);
    c.a = with_alpha ? ps_bits_u8(b) : 255;
    return c;
}

/* An index is only meaningful against the style table in force when the edge
 * was read: 1-based, and no further than the end of the array as it stood.
 * Out of range means the file is malformed, and drawing nothing is the better
 * failure - a wrong colour looks like a renderer bug and gets debugged as
 * one, whereas a missing piece points at the file. */
static inline uint16_t style_index(uint32_t base, uint32_t local,
                                   uint32_t count)
{
    uint32_t g;

    if(local == 0)
        return 0;
    g = base + local;
    if(g > count || g > MAX_STYLES)
        return 0;
    return (uint16_t)g;
}

#endif /* PS_SWF_READ_H */
