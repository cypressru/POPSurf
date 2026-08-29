/* Bit reader tests.
 *
 * Worth having separately because a bit-reader fault does not look like a
 * bit-reader fault. Read one bit too few from a style count and the next
 * hundred fields are still "valid" - plausible widths, plausible deltas - and
 * the symptom is a shape that comes out as noise several hundred bytes later.
 * There is no way to work back from that to the cause by looking at the
 * picture, so the reader gets pinned down here against hand-computed answers
 * before anything else is allowed to depend on it.
 */
#include "ps_swf_bits.h"

#include <stdio.h>

static int fails;

static void chk(const char *what, long got, long want)
{
    if(got != want) {
        printf("FAIL %-34s got %ld want %ld\n", what, got, want);
        fails++;
    }
}

int main(void)
{
    /* 10110010 01110001 11111111 00000001 */
    static const uint8_t d[] = { 0xb2, 0x71, 0xff, 0x01 };
    ps_bits b;

    ps_bits_init(&b, d, sizeof d);
    chk("ub(1)",  ps_bits_ub(&b, 1), 1);
    chk("ub(3)",  ps_bits_ub(&b, 3), 3);      /* 011 */
    chk("ub(0)",  ps_bits_ub(&b, 0), 0);      /* legal: RECT with nbits 0 */
    chk("ub(4)",  ps_bits_ub(&b, 4), 2);      /* 0010 */
    chk("tell",   (long)ps_bits_tell(&b), 8);
    /* Straddles the byte boundary: 0111 0001 1111 = 0x71f. */
    chk("ub(12) across bytes", ps_bits_ub(&b, 12), 0x71f);

    /* Sign extension at every awkward width. */
    {
        static const uint8_t s[] = { 0xff, 0x80, 0x00, 0x00, 0x7f, 0xff };
        ps_bits t;

        ps_bits_init(&t, s, sizeof s);
        chk("sb(1) of 1",  ps_bits_sb(&t, 1), -1);
        chk("sb(4) of f",  ps_bits_sb(&t, 4), -1);   /* 1111 */
        chk("sb(3) of 7",  ps_bits_sb(&t, 3), -1);   /* 111 */
        ps_bits_init(&t, s, sizeof s);
        chk("sb(16) 0xff80", ps_bits_sb(&t, 16), -128);
        chk("sb(16) 0x0000", ps_bits_sb(&t, 16), 0);
        chk("sb(16) 0x7fff", ps_bits_sb(&t, 16), 32767);
    }
    {
        /* 32-bit width is the case a mask-and-negate implementation gets
         * wrong, because the mask needs a shift the width of the type. */
        static const uint8_t s[] = { 0x80, 0x00, 0x00, 0x00 };
        ps_bits t;

        ps_bits_init(&t, s, sizeof s);
        chk("sb(32) 0x80000000", ps_bits_sb(&t, 32), -2147483647L - 1L);
    }

    /* Byte fields realign first, and are little endian. */
    {
        static const uint8_t s[] = { 0x80, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12 };
        ps_bits t;

        ps_bits_init(&t, s, sizeof s);
        chk("ub(1) then align", ps_bits_ub(&t, 1), 1);
        chk("u16 realigned", ps_bits_u16(&t), 0x1234);
        chk("u32", (long)ps_bits_u32(&t), 0x12345678L);
    }

    /* Overrun is sticky, yields zero, and never moves the cursor past the
     * end - that last property is what stops a parse loop driven by file
     * data from spinning forever on a truncated file. */
    {
        static const uint8_t s[] = { 0xff, 0xff };
        ps_bits t;

        ps_bits_init(&t, s, sizeof s);
        (void)ps_bits_ub(&t, 12);
        chk("not over yet", t.over, 0);
        chk("overrun reads zero", ps_bits_ub(&t, 12), 0);
        chk("over is set", t.over, 1);
        chk("stays set", (ps_bits_ub(&t, 1), t.over), 1);
        chk("pos clamped", (long)(t.pos <= t.len), 1);
        chk("u8 past end is zero", ps_bits_u8(&t), 0);
    }
    {
        ps_bits t;
        static const uint8_t s[] = { 1, 2, 3, 4 };

        ps_bits_init(&t, s, sizeof s);
        ps_bits_skip(&t, 100);
        chk("skip past end clamps", (long)t.pos, 4);
        chk("skip past end flags", t.over, 1);
    }

    /* A real RECT: nbits=15, then four 15-bit signed values 0, 11200, 0,
     * 5200 - the stage rect of a 560x260 movie. Hand-assembled so a failure
     * here is unambiguously the reader and not the encoder. */
    {
        uint8_t r[16] = { 0 };
        int     bit = 0, i, k;
        long    vals[4] = { 0, 11200, 0, 5200 };
        ps_bits t;
        int32_t x0, x1, y0, y1;

        for(k = 4; k >= 0; k--) {          /* nbits = 15, five bits, MSB first */
            if((15 >> k) & 1) r[bit >> 3] |= (uint8_t)(0x80 >> (bit & 7));
            bit++;
        }
        for(i = 0; i < 4; i++)
            for(k = 14; k >= 0; k--) {
                if((vals[i] >> k) & 1) r[bit >> 3] |= (uint8_t)(0x80 >> (bit & 7));
                bit++;
            }

        ps_bits_init(&t, r, sizeof r);
        chk("rect nbits", ps_bits_ub(&t, 5), 15);
        x0 = ps_bits_sb(&t, 15);
        x1 = ps_bits_sb(&t, 15);
        y0 = ps_bits_sb(&t, 15);
        y1 = ps_bits_sb(&t, 15);
        chk("rect xmin", x0, 0);
        chk("rect xmax", x1, 11200);
        chk("rect ymin", y0, 0);
        chk("rect ymax", y1, 5200);
        chk("rect width px", (x1 - x0) / 20, 560);
        chk("rect height px", (y1 - y0) / 20, 260);
    }

    printf(fails ? "%d failure(s)\n" : "all bit reader tests pass\n", fails);
    return fails ? 1 : 0;
}
