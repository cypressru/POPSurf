/* SWF bit reader.
 *
 * SWF stores almost every geometric quantity in a variable-width bitfield
 * whose width is itself read from the stream a few bits earlier, MSB first,
 * with byte alignment reasserted before every fixed-width field. Getting this
 * wrong does not produce an error, it produces plausible-looking garbage that
 * only shows up as a mangled shape hundreds of bytes later - so this is worth
 * having on its own and worth testing on its own (see bittest.c).
 *
 * The overrun policy is the other reason this is a separate unit. This parses
 * data pulled off the network, so every read has to be bounds-checked, but
 * threading a status return through fifty call sites guarantees one of them
 * gets forgotten. Instead an overrun sets a sticky flag and yields zero, and
 * callers test the flag at the granularity that suits them - once per tag,
 * once per shape-record loop. The invariant that makes this safe is that
 * zero-and-sticky can never advance the read position past the end, so a loop
 * driven by parsed data always terminates once `over` is set.
 */
#ifndef PS_SWF_BITS_H
#define PS_SWF_BITS_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint8_t *data;
    size_t         len;
    size_t         pos;   /* byte holding the next unread bit */
    int            bit;   /* bits already taken from data[pos], 0..7 */
    int            over;  /* sticky: some read wanted bytes that are not there */
} ps_bits;

static inline void ps_bits_init(ps_bits *b, const uint8_t *data, size_t len)
{
    b->data = data;
    b->len  = len;
    b->pos  = 0;
    b->bit  = 0;
    b->over = 0;
}

/* Bits already consumed. Tag parsers use this to tell "I stopped where the
 * tag ended" from "I stopped early and the rest is something I misread". */
static inline size_t ps_bits_tell(const ps_bits *b)
{
    return b->pos * 8u + (size_t)b->bit;
}

static inline uint32_t ps_bits_ub(ps_bits *b, int n)
{
    uint32_t v = 0;

    /* Zero-width fields are legal and common: a RECT whose corners are all
     * zero encodes nbits=0 and stores no value bits at all. */
    if(n <= 0)
        return 0;
    if(n > 32) {
        b->over = 1;
        return 0;
    }

    while(n--) {
        if(b->pos >= b->len) {
            b->over = 1;
            return 0;
        }
        v = (v << 1) | (uint32_t)((b->data[b->pos] >> (7 - b->bit)) & 1);
        if(++b->bit == 8) {
            b->bit = 0;
            b->pos++;
        }
    }
    return v;
}

static inline int32_t ps_bits_sb(ps_bits *b, int n)
{
    uint32_t v = ps_bits_ub(b, n);
    int64_t  s = (int64_t)v;

    /* Subtracting 2^n rather than masking with ~0 keeps this defined for
     * n == 32, where a 32-bit shift would not be. */
    if(n > 0 && n <= 32 && (v & (1u << (n - 1))))
        s -= ((int64_t)1 << n);
    return (int32_t)s;
}

/* 16.16 signed fixed point, as used by MATRIX scale and skew. */
static inline float ps_bits_fb(ps_bits *b, int n)
{
    return (float)ps_bits_sb(b, n) / 65536.0f;
}

static inline void ps_bits_align(ps_bits *b)
{
    if(b->bit) {
        b->bit = 0;
        b->pos++;
    }
}

static inline uint8_t ps_bits_u8(ps_bits *b)
{
    ps_bits_align(b);
    if(b->pos >= b->len) {
        b->over = 1;
        return 0;
    }
    return b->data[b->pos++];
}

static inline uint16_t ps_bits_u16(ps_bits *b)
{
    uint16_t lo = ps_bits_u8(b);
    return (uint16_t)(lo | ((uint16_t)ps_bits_u8(b) << 8));
}

static inline uint32_t ps_bits_u32(ps_bits *b)
{
    uint32_t lo = ps_bits_u16(b);
    return lo | ((uint32_t)ps_bits_u16(b) << 16);
}

/* Steps over a field this phase does not interpret. Clamped rather than
 * wrapped so a bogus length cannot move the cursor backwards. */
static inline void ps_bits_skip(ps_bits *b, size_t bytes)
{
    ps_bits_align(b);
    if(bytes > b->len - b->pos) {
        b->pos  = b->len;
        b->over = 1;
        return;
    }
    b->pos += bytes;
}

#endif /* PS_SWF_BITS_H */
