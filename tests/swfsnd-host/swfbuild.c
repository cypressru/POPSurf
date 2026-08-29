#include "swfbuild.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void die(const char *why)
{
    printf("swfbuild: %s\n", why);
    exit(1);
}

void bw_init(bw *w, size_t cap)
{
    w->b   = (uint8_t *)malloc(cap ? cap : 64);
    w->n   = 0;
    w->cap = cap ? cap : 64;
    w->bit = 0;
    w->acc = 0;
    if(!w->b)
        die("out of memory");
}

void bw_free(bw *w) { free(w->b); memset(w, 0, sizeof *w); }

static void bw_byte(bw *w, unsigned v)
{
    if(w->n == w->cap) {
        w->cap *= 2;
        w->b = (uint8_t *)realloc(w->b, w->cap);
        if(!w->b)
            die("out of memory");
    }
    w->b[w->n++] = (uint8_t)v;
}

void bw_bits(bw *w, uint32_t v, int n)
{
    int i;

    for(i = n - 1; i >= 0; i--) {
        w->acc = (w->acc << 1) | ((v >> i) & 1u);
        if(++w->bit == 8) {
            bw_byte(w, w->acc);
            w->acc = 0;
            w->bit = 0;
        }
    }
}

void bw_sbits(bw *w, int32_t v, int n)
{
    bw_bits(w, (uint32_t)v & (n >= 32 ? 0xffffffffu : ((1u << n) - 1u)), n);
}

void bw_flush(bw *w)
{
    if(w->bit) {
        bw_byte(w, w->acc << (8 - w->bit));
        w->acc = 0;
        w->bit = 0;
    }
}

void bw_u8(bw *w, unsigned v)  { bw_flush(w); bw_byte(w, v & 0xff); }
void bw_u16(bw *w, unsigned v) { bw_u8(w, v & 0xff); bw_u8(w, (v >> 8) & 0xff); }
void bw_u32(bw *w, uint32_t v) { bw_u16(w, v & 0xffff); bw_u16(w, v >> 16); }

void bw_bytes(bw *w, const uint8_t *p, size_t n)
{
    size_t i;

    bw_flush(w);
    for(i = 0; i < n; i++)
        bw_byte(w, p[i]);
}

int bw_sbits_needed(int32_t v)
{
    int n = 2;

    while(n < 31) {
        int32_t hi = ((int32_t)1 << (n - 1)) - 1;
        int32_t lo = -hi - 1;

        if(v >= lo && v <= hi)
            return n;
        n++;
    }
    return 31;
}

void swf_tagged(bw *out, int code, const bw *body)
{
    if(body->n < 0x3f) {
        bw_u16(out, (unsigned)((code << 6) | (int)body->n));
    }
    else {
        bw_u16(out, (unsigned)((code << 6) | 0x3f));
        bw_u32(out, (uint32_t)body->n);
    }
    bw_bytes(out, body->b, body->n);
}

void swf_showframe(bw *tags)
{
    bw e;

    bw_init(&e, 4);
    swf_tagged(tags, 1, &e);
    bw_free(&e);
}

void swf_end(bw *tags)
{
    bw e;

    bw_init(&e, 4);
    swf_tagged(tags, 0, &e);
    bw_free(&e);
}

void swf_bgcolor(bw *tags, unsigned rgb)
{
    bw body;

    bw_init(&body, 8);
    bw_u8(&body, (rgb >> 16) & 0xff);
    bw_u8(&body, (rgb >> 8) & 0xff);
    bw_u8(&body, rgb & 0xff);
    swf_tagged(tags, 9, &body);
    bw_free(&body);
}

/* A RECT, which every geometry field in this format is wrapped in: one width
 * shared by all four values, so the widest of them decides. */
static void put_rect(bw *w, int32_t x0, int32_t x1, int32_t y0, int32_t y1)
{
    int n = bw_sbits_needed(x0);
    int m;

    m = bw_sbits_needed(x1); if(m > n) n = m;
    m = bw_sbits_needed(y0); if(m > n) n = m;
    m = bw_sbits_needed(y1); if(m > n) n = m;

    bw_bits(w, (uint32_t)n, 5);
    bw_sbits(w, x0, n);
    bw_sbits(w, x1, n);
    bw_sbits(w, y0, n);
    bw_sbits(w, y1, n);
}

void swf_rect_shape(bw *tags, int id, int32_t w, int32_t h, unsigned rgb)
{
    bw  body;
    int eb;

    bw_init(&body, 128);
    bw_u16(&body, (unsigned)id);
    put_rect(&body, 0, w, 0, h);
    bw_flush(&body);

    bw_u8(&body, 1);                       /* one fill style */
    bw_u8(&body, 0x00);                    /* solid */
    bw_u8(&body, (rgb >> 16) & 0xff);
    bw_u8(&body, (rgb >> 8) & 0xff);
    bw_u8(&body, rgb & 0xff);
    bw_u8(&body, 0);                       /* no line styles */

    bw_bits(&body, 1, 4);                  /* NumFillBits */
    bw_bits(&body, 0, 4);                  /* NumLineBits */

    /* Move to the origin and select the fill on the left of the path. Which
     * side matters: a rectangle wound the other way with FillStyle1 set paints
     * its outside, which on a full-stage shape is the whole stage. */
    bw_bits(&body, 0, 1);                  /* not an edge */
    bw_bits(&body, 0, 1);                  /* no new styles */
    bw_bits(&body, 0, 1);                  /* no line style change */
    bw_bits(&body, 1, 1);                  /* fill style 1 */
    bw_bits(&body, 0, 1);                  /* no fill style 0 */
    bw_bits(&body, 1, 1);                  /* move to */
    bw_bits(&body, 5, 5);                  /* five bits of move, both zero */
    bw_sbits(&body, 0, 5);
    bw_sbits(&body, 0, 5);
    bw_bits(&body, 1, 1);                  /* fill style index 1 */

    eb = bw_sbits_needed(w);
    if(bw_sbits_needed(h) > eb)
        eb = bw_sbits_needed(h);

    /* Four straight edges, clockwise from the origin. */
    {
        const int32_t dx[4] = { w, 0, -w, 0 };
        const int32_t dy[4] = { 0, h, 0, -h };
        int           i;

        for(i = 0; i < 4; i++) {
            bw_bits(&body, 1, 1);          /* edge */
            bw_bits(&body, 1, 1);          /* straight */
            bw_bits(&body, (uint32_t)(eb - 2), 4);
            bw_bits(&body, 1, 1);          /* general line */
            bw_sbits(&body, dx[i], eb);
            bw_sbits(&body, dy[i], eb);
        }
    }

    bw_bits(&body, 0, 6);                  /* end of shape */
    bw_flush(&body);

    swf_tagged(tags, 2, &body);
    bw_free(&body);
}

void swf_place(bw *tags, int depth, int id, int32_t tx, int32_t ty, int move)
{
    bw  body;
    int n = bw_sbits_needed(tx);
    int m = bw_sbits_needed(ty);

    if(m > n)
        n = m;

    bw_init(&body, 64);
    bw_u8(&body, (unsigned)(0x04 | (id >= 0 ? 0x02 : 0) | (move ? 0x01 : 0)));
    bw_u16(&body, (unsigned)depth);
    if(id >= 0)
        bw_u16(&body, (unsigned)id);

    bw_bits(&body, 0, 1);                  /* no scale */
    bw_bits(&body, 0, 1);                  /* no rotate */
    bw_bits(&body, (uint32_t)n, 5);
    bw_sbits(&body, tx, n);
    bw_sbits(&body, ty, n);
    bw_flush(&body);

    swf_tagged(tags, 26, &body);
    bw_free(&body);
}

void swf_remove(bw *tags, int depth)
{
    bw body;

    bw_init(&body, 8);
    bw_u16(&body, (unsigned)depth);
    swf_tagged(tags, 28, &body);
    bw_free(&body);
}

/* --- sound --------------------------------------------------------------- */

void swf_define_sound(bw *tags, int id, int fmt, int rate_code, int bits16,
                      int stereo, uint32_t nsample, const uint8_t *d, size_t n)
{
    bw body;

    bw_init(&body, n + 64);
    bw_u16(&body, (unsigned)id);
    bw_bits(&body, (uint32_t)fmt, 4);
    bw_bits(&body, (uint32_t)rate_code, 2);
    bw_bits(&body, (uint32_t)(bits16 ? 1 : 0), 1);
    bw_bits(&body, (uint32_t)(stereo ? 1 : 0), 1);
    bw_u32(&body, nsample);
    bw_bytes(&body, d, n);
    swf_tagged(tags, 14, &body);
    bw_free(&body);
}

void swf_start_sound(bw *tags, int id, const swf_startinfo *si)
{
    bw  body;
    int i;

    bw_init(&body, 256);
    bw_u16(&body, (unsigned)id);
    bw_bits(&body, 0, 2);                                   /* reserved */
    bw_bits(&body, (uint32_t)(si->stop ? 1 : 0), 1);
    bw_bits(&body, (uint32_t)(si->no_multiple ? 1 : 0), 1);
    bw_bits(&body, (uint32_t)(si->nenv ? 1 : 0), 1);
    bw_bits(&body, (uint32_t)(si->loops ? 1 : 0), 1);
    bw_bits(&body, (uint32_t)(si->has_out ? 1 : 0), 1);
    bw_bits(&body, (uint32_t)(si->has_in ? 1 : 0), 1);
    if(si->has_in)  bw_u32(&body, si->in);
    if(si->has_out) bw_u32(&body, si->out);
    if(si->loops)   bw_u16(&body, (unsigned)si->loops);
    if(si->nenv) {
        bw_u8(&body, (unsigned)si->nenv);
        for(i = 0; i < si->nenv; i++) {
            bw_u32(&body, si->pos44[i]);
            bw_u16(&body, si->left[i]);
            bw_u16(&body, si->right[i]);
        }
    }
    swf_tagged(tags, 15, &body);
    bw_free(&body);
}

/* SoundStreamHead2, which differs from 18 only in which codec and sample-size
 * combinations it may declare. */
void swf_stream_head(bw *tags, int fmt, int rate_code, int stereo, int spf)
{
    bw body;

    bw_init(&body, 64);
    bw_bits(&body, 0, 4);                                   /* reserved */
    bw_bits(&body, (uint32_t)rate_code, 2);                 /* playback */
    bw_bits(&body, 1, 1);
    bw_bits(&body, (uint32_t)(stereo ? 1 : 0), 1);
    bw_bits(&body, (uint32_t)fmt, 4);                       /* stream */
    bw_bits(&body, (uint32_t)rate_code, 2);
    bw_bits(&body, 1, 1);
    bw_bits(&body, (uint32_t)(stereo ? 1 : 0), 1);
    bw_u16(&body, (unsigned)spf);
    swf_tagged(tags, 45, &body);
    bw_free(&body);
}

void swf_stream_block(bw *tags, const uint8_t *d, size_t n)
{
    bw body;

    bw_init(&body, n + 16);
    bw_bytes(&body, d, n);
    swf_tagged(tags, 19, &body);
    bw_free(&body);
}

/* --- Flash ADPCM --------------------------------------------------------- */

static const int32_t step_tab[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41,
    45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190,
    209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724,
    796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272,
    2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132,
    7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
    22385, 24623, 27086, 29794, 32767
};

static const int8_t idx_tab[4][16] = {
    { -1, 2 },
    { -1, -1, 2, 4 },
    { -1, -1, -1, -1, 2, 4, 6, 8 },
    { -1, -1, -1, -1, -1, -1, -1, -1, 1, 2, 4, 6, 8, 10, 13, 16 }
};

void swf_adpcm_stream(bw *out, const int16_t *src, uint32_t n, int nbits)
{
    uint32_t at = 0;

    bw_bits(out, (uint32_t)(nbits - 2), 2);

    while(at < n) {
        uint32_t end = at + 4096u;
        int32_t  pred, index;
        uint32_t i;

        if(end > n)
            end = n;

        /* The step index the packet starts at is a field, not a constant, and
         * writing zero into it is the mistake that makes a per-block codec
         * sound broken. Index zero is a step of seven, and the largest jump a
         * four-bit code can make is 1.875 steps - so a block that opens on the
         * steep part of a waveform is limited to about thirteen units a sample
         * until the adaptation climbs, which on a stream cut into twelve blocks
         * a second is a lag at the head of every one of them. Starting from the
         * step that can actually represent the block's largest move costs one
         * pass over the samples and removes it. */
        pred  = src[at];
        index = 0;
        for(i = at + 1u; i < end; i++) {
            int32_t d = src[i] - src[i - 1u];

            if(d < 0)
                d = -d;
            while(index < 88 && (int64_t)step_tab[index] * 15 / 8 < (int64_t)d)
                index++;
        }
        bw_sbits(out, pred, 16);
        bw_bits(out, (uint32_t)index, 6);

        for(i = at + 1u; i < end; i++) {
            int32_t  step = step_tab[index];
            int32_t  d    = src[i] - pred;
            uint32_t sign = d < 0 ? 1u : 0u;
            int32_t  a    = d < 0 ? -d : d;
            uint32_t mag  = 0;
            int32_t  s    = step;
            int32_t  diff = 0;
            uint32_t bit;

            /* Greedy, largest bit first, mirroring the decoder's weighted sum
             * of halvings exactly - including the trailing half-step that is
             * always added, which is why the residual starts one step down. */
            for(bit = 1u << (nbits - 2); bit; bit >>= 1) {
                if(a >= s) {
                    mag |= bit;
                    a   -= s;
                    diff += s;
                }
                s >>= 1;
            }
            diff += s;

            pred += sign ? -diff : diff;
            if(pred > 32767)       pred = 32767;
            else if(pred < -32768) pred = -32768;

            index += idx_tab[nbits - 2][mag];
            if(index < 0)       index = 0;
            else if(index > 88) index = 88;

            bw_bits(out, (sign << (nbits - 1)) | mag, nbits);
        }

        /* No padding for a short final packet. The sample count is nowhere in
         * the file and a reader derives it from the bits present, so padding
         * would add samples rather than align anything - and a short packet
         * can only ever be the last one, with nothing after it to desync. */
        at = end;
    }
    bw_flush(out);
}

uint8_t *swf_finish(const bw *tags, int32_t stage_w, int32_t stage_h, int fps,
                    int nframe, size_t *out_len)
{
    bw       hdr;
    uint8_t *img;
    uint32_t total;

    bw_init(&hdr, 64);
    put_rect(&hdr, 0, stage_w, 0, stage_h);
    bw_flush(&hdr);
    bw_u16(&hdr, (unsigned)(fps << 8));    /* FIXED8, fraction first */
    bw_u16(&hdr, (unsigned)nframe);

    total = (uint32_t)(8 + hdr.n + tags->n);
    img   = (uint8_t *)malloc(total);
    if(!img)
        die("out of memory");

    memcpy(img, "FWS", 3);
    img[3] = 4;
    img[4] = (uint8_t)(total & 0xff);
    img[5] = (uint8_t)((total >> 8) & 0xff);
    img[6] = (uint8_t)((total >> 16) & 0xff);
    img[7] = (uint8_t)((total >> 24) & 0xff);
    memcpy(img + 8, hdr.b, hdr.n);
    memcpy(img + 8 + hdr.n, tags->b, tags->n);
    bw_free(&hdr);

    *out_len = total;
    return img;
}
