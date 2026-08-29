/* SWF sound tests: the codec, the tags, and the frame-to-sample map.
 *
 * There is a trap in testing a codec against your own encoder, and this file is
 * arranged around avoiding it. A round trip passes whenever the two halves
 * agree, including when they agree about something wrong - swap the sign bit
 * and the meaning of the magnitude, or pack the bits backwards, and encode
 * followed by decode is still the identity. So the round trip here is only the
 * second test. The first is four hand-assembled blocks whose first few output
 * samples were worked out on paper, byte by byte, from the format description;
 * those pin the bit order, the sign extension, the step index tables and the
 * packet header independently of anything else in this repository. It is how
 * the ADX decoder next door was validated and it is the only part of this file
 * that could not be fooled by a consistent mistake.
 *
 * The SWF-level tests build whole movies in memory rather than on disk. They
 * are throwaway - four tags and a header - and the thing under test is the tag
 * walk, so there is nothing a file adds except a path to get wrong.
 *
 * What is not covered, and cannot be from here: real content. The one sample
 * this player was developed against carries no sound tags at all, so every byte
 * that reaches the decoder below was written by the test. Pass a .swf as argv[1]
 * to print its sound inventory; that is a smoke test, not a check.
 *
 *   ./sndtest [file.swf]
 */
#include "ps_swf.h"
#include "ps_swf_sound.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

static void chk(const char *what, long got, long want)
{
    if(got != want) {
        printf("FAIL %-44s got %ld want %ld\n", what, got, want);
        fails++;
    }
}

static void chk_true(const char *what, int cond)
{
    if(!cond) {
        printf("FAIL %s\n", what);
        fails++;
    }
}

/* --- the format, restated ------------------------------------------------ */

/* The step table and the index adjustments again, so the encoder below and the
 * hand-worked expectations do not simply read back whatever ps_swf_sound.c
 * happens to hold. This is a weak form of independence - the values came from
 * the same reading of the same specification - which is exactly why the
 * hand-assembled vectors exist. */
static const int32_t step_tab[89] = {
        7,     8,     9,    10,    11,    12,    13,    14,    16,    17,
       19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
       50,    55,    60,    66,    73,    80,    88,    97,   107,   118,
      130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
      337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
      876,   963,  1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
     2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
     5894,  6484,  7132,  7845,  8630,  9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static const int8_t idx_tab[4][16] = {
    { -1,  2 },
    { -1, -1,  2,  4 },
    { -1, -1, -1, -1,  2,  4,  6,  8 },
    { -1, -1, -1, -1, -1, -1, -1, -1,  1,  2,  4,  6,  8, 10, 13, 16 }
};

static int32_t recon_step(int32_t pred, int32_t *index, uint32_t code, int nbits)
{
    int32_t  step = step_tab[*index];
    uint32_t sign = 1u << (nbits - 1);
    uint32_t mag  = code & (sign - 1u);
    int32_t  diff = 0;
    uint32_t bit;

    for(bit = sign >> 1; bit; bit >>= 1) {
        if(mag & bit)
            diff += step;
        step >>= 1;
    }
    diff += step;

    pred += (code & sign) ? -diff : diff;
    if(pred > 32767)  pred = 32767;
    if(pred < -32768) pred = -32768;

    *index += idx_tab[nbits - 2][mag];
    if(*index < 0)  *index = 0;
    if(*index > 88) *index = 88;
    return pred;
}

/* --- bit writer ---------------------------------------------------------- */

typedef struct {
    uint8_t *b;
    size_t   n, cap;
    uint8_t  acc;
    int      nb;
} bw;

static void bw_init(bw *w, size_t cap)
{
    memset(w, 0, sizeof *w);
    w->b   = malloc(cap);
    w->cap = cap;
    if(!w->b) {
        printf("FAIL out of memory building a test file\n");
        exit(1);
    }
}

static void bw_free(bw *w) { free(w->b); memset(w, 0, sizeof *w); }

static void bw_byte(bw *w, unsigned v)
{
    if(w->n >= w->cap) {
        printf("FAIL test file buffer overflow\n");
        exit(1);
    }
    w->b[w->n++] = (uint8_t)v;
}

static void bw_bits(bw *w, uint32_t v, int n)
{
    while(n-- > 0) {
        w->acc = (uint8_t)((w->acc << 1) | ((v >> n) & 1u));
        if(++w->nb == 8) {
            bw_byte(w, w->acc);
            w->acc = 0;
            w->nb  = 0;
        }
    }
}

static void bw_flush(bw *w)
{
    if(w->nb) {
        bw_byte(w, (unsigned)(w->acc << (8 - w->nb)));
        w->acc = 0;
        w->nb  = 0;
    }
}

static void bw_u8(bw *w, unsigned v)  { bw_flush(w); bw_byte(w, v & 0xff); }
static void bw_u16(bw *w, unsigned v) { bw_u8(w, v & 0xff); bw_u8(w, (v >> 8) & 0xff); }
static void bw_u32(bw *w, uint32_t v) { bw_u16(w, v & 0xffff); bw_u16(w, v >> 16); }

static void bw_bytes(bw *w, const uint8_t *p, size_t n)
{
    size_t i;

    bw_flush(w);
    for(i = 0; i < n; i++)
        bw_byte(w, p[i]);
}

/* --- Flash ADPCM encoder ------------------------------------------------- */

/* Encodes `n` samples per channel at `nbits` bits, and reports through `recon`
 * the sample values a correct decoder is obliged to produce. Those are not the
 * input: ADPCM is lossy, and the encoder knows exactly how lossy because it
 * runs the reconstruction itself in order to choose the next code against it.
 * Comparing the decoder to `recon` is therefore an exact equality, not a
 * tolerance - which is the only reason a round trip is worth running at all. */
static void adpcm_encode(bw *w, const int16_t *const *src, uint32_t n,
                         int channels, int nbits, int16_t *const *recon)
{
    int32_t  pred[2] = { 0, 0 }, index[2] = { 0, 0 };
    uint32_t i;
    int      ch;

    bw_bits(w, (uint32_t)(nbits - 2), 2);

    for(i = 0; i < n; i++) {
        if(i % PS_SWF_ADPCM_PACKET == 0) {
            /* A packet header states the predictor outright, so the sample it
             * names comes back exactly. The step index carries over from the
             * previous packet rather than restarting - legal, and it is what
             * puts a non-zero index in the second packet's header where a
             * reader that assumed zero would be caught. */
            for(ch = 0; ch < channels; ch++) {
                pred[ch] = src[ch][i];
                bw_bits(w, (uint32_t)(uint16_t)pred[ch], 16);
                bw_bits(w, (uint32_t)index[ch], 6);
                recon[ch][i] = (int16_t)pred[ch];
            }
            continue;
        }
        for(ch = 0; ch < channels; ch++) {
            int32_t  step = step_tab[index[ch]];
            int32_t  d    = src[ch][i] - pred[ch];
            uint32_t sign = 1u << (nbits - 1);
            uint32_t code = 0, bit;
            int32_t  s;

            if(d < 0) {
                code = sign;
                d    = -d;
            }
            /* The decoder adds a half-step whatever the magnitude says, so the
             * search has to be for the residual above that floor. Skipping this
             * biases every sample outward by half a quantum, which a round trip
             * against the same encoder would never notice. */
            d -= step >> (nbits - 1);
            if(d < 0)
                d = 0;

            s = step;
            for(bit = sign >> 1; bit; bit >>= 1) {
                if(d >= s) {
                    code |= bit;
                    d    -= s;
                }
                s >>= 1;
            }

            pred[ch]     = recon_step(pred[ch], &index[ch], code, nbits);
            recon[ch][i] = (int16_t)pred[ch];
            bw_bits(w, code, nbits);
        }
    }
    bw_flush(w);
}

/* --- test 1: the tables -------------------------------------------------- */

/* Not a proof of the table, which is a published constant and can only really
 * be checked against real content. It is a check that no digit has been
 * transposed or dropped: the sequence is strictly increasing and each step is
 * roughly a tenth larger than the one before, so 1060 written as 1600 or a
 * missing entry both fall out. The bounds are wide because the low end of the
 * table is quantised coarsely - 14 to 16 is a jump of a seventh. */
static void test_tables(void)
{
    int i, worst_lo = 0, worst_hi = 0;

    chk("step table starts at 7", step_tab[0], 7);
    chk("step table ends at 32767", step_tab[88], 32767);

    for(i = 1; i < 89; i++) {
        double r = (double)step_tab[i] / (double)step_tab[i - 1];

        if(step_tab[i] <= step_tab[i - 1])
            worst_lo = i;
        if(r < 1.04 || r > 1.16)
            worst_hi = i;
    }
    chk("step table strictly increasing", worst_lo, 0);
    chk("step table grows by about a tenth", worst_hi, 0);
}

/* --- test 2: hand-assembled blocks --------------------------------------- */

/* Four-bit mono, six samples, assembled by hand from the format description.
 *
 *   bits  0..1   code size 2, so four-bit samples          -> 10
 *   bits  2..17  initial sample 0                          -> 0000000000000000
 *   bits 18..23  initial step index 0, so step 7           -> 000000
 *   bits 24..43  five codes: 4, 4, C, 0, F
 *
 * which packs, most significant bit first and padded to a byte, as
 * 80 00 00 44 C0 F0.
 *
 * Sample 0 is the stated one, 0. Then, with the top bit of a code as the sign
 * and diff = step/8 + (bit2 ? step : 0) + (bit1 ? step/2 : 0) + (bit0 ? step/4 : 0):
 *
 *   code 4  step  7  diff = 7 + 0        =  7   pred    7  index 0+2 = 2
 *   code 4  step  9  diff = 9 + 1        = 10   pred   17  index 2+2 = 4
 *   code C  step 11  diff = 11 + 1       = 12   pred    5  index 4+2 = 6   (sign)
 *   code 0  step 13  diff = 0 + 1        =  1   pred    6  index 6-1 = 5
 *   code F  step 12  diff = 12+6+3+1     = 22   pred  -16  index 5+8 = 13  (sign)
 *
 * Every one of those numbers is a separate claim about the decoder: the nibble
 * order, the sign bit's position, the half-step floor, the index table for four
 * bits, and that the header's sample is itself an output. */
static void test_handmade(void)
{
    static const uint8_t d4[] = { 0x80, 0x00, 0x00, 0x44, 0xc0, 0xf0 };
    static const int16_t w4[] = { 0, 7, 17, 5, 6, -16 };

    /* Two-bit mono, five samples. Only one magnitude bit exists, so the whole
     * of the reconstruction is "step or nothing, plus half a step", and the
     * index table has two entries. Nothing about this width can be right by
     * accident if the four-bit case was written first.
     *
     *   bits  0..1   code size 0                            -> 00
     *   bits  2..17  initial sample 100                     -> 0000000001100100
     *   bits 18..23  initial step index 5, so step 12       -> 000101
     *   bits 24..31  four codes: 1, 3, 0, 2
     *
     * packing as 00 19 05 72.
     *
     *   code 1  step 12  diff = 12 + 6 = 18  pred 118  index 5+2 = 7
     *   code 3  step 14  diff = 14 + 7 = 21  pred  97  index 7+2 = 9   (sign)
     *   code 0  step 17  diff =  0 + 8 =  8  pred 105  index 9-1 = 8
     *   code 2  step 16  diff =  0 + 8 =  8  pred  97  index 8-1 = 7   (sign) */
    static const uint8_t d2[] = { 0x00, 0x19, 0x05, 0x72 };
    static const int16_t w2[] = { 100, 118, 97, 105, 97 };

    struct { const uint8_t *d; size_t n; const int16_t *want; uint32_t cnt;
             const char *name; } cases[] = {
        { d4, sizeof d4, w4, 6, "4-bit" },
        { d2, sizeof d2, w2, 5, "2-bit" }
    };
    size_t c;

    for(c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        ps_swf_sound  s;
        ps_swf_snddec dec;
        int16_t       got[8];
        int16_t      *ptr[2] = { got, NULL };
        char          label[64];
        uint32_t      i, n;

        memset(&s, 0, sizeof s);
        s.format    = PS_SWF_SND_ADPCM;
        s.bits      = 16;
        s.channels  = 1;
        s.rate      = 22050;
        s.decodable = 1;
        s.nsample   = cases[c].cnt;
        s.data      = (uint8_t *)(size_t)cases[c].d;
        s.len       = (uint32_t)cases[c].n;

        snprintf(label, sizeof label, "%s hand block decodes", cases[c].name);
        chk(label, ps_swf_snddec_init(&dec, &s), 0);

        n = ps_swf_snddec_read(&dec, ptr, cases[c].cnt);
        snprintf(label, sizeof label, "%s hand block sample count",
                 cases[c].name);
        chk(label, (long)n, (long)cases[c].cnt);

        for(i = 0; i < n && i < cases[c].cnt; i++) {
            snprintf(label, sizeof label, "%s hand block sample %u",
                     cases[c].name, i);
            chk(label, got[i], cases[c].want[i]);
        }
    }
}

/* --- test 3: round trip -------------------------------------------------- */

static void fill_tone(int16_t *dst, uint32_t n, double period, double amp,
                      double phase)
{
    uint32_t i;

    for(i = 0; i < n; i++)
        dst[i] = (int16_t)(amp * sin(2.0 * 3.14159265358979323846 *
                                     ((double)i / period + phase)));
}

static void test_roundtrip(void)
{
    /* Just over a packet, so the second packet header and the sample count that
     * is nowhere in the file are both exercised. A decoder that reads 4096 as
     * 4095 or 4097 gets the whole tail wrong and nothing before it. */
    enum { N = 5000 };
    int nbits, channels;

    for(channels = 1; channels <= 2; channels++)
        for(nbits = 2; nbits <= 5; nbits++) {
            int16_t *src[2], *rec[2], *out[2];
            bw       w;
            ps_swf_sound  s;
            ps_swf_snddec dec;
            char     label[80];
            uint32_t got, i;
            int      ch;
            double   err = 0.0, sig = 0.0;
            long     mismatch = -1;

            for(ch = 0; ch < 2; ch++) {
                src[ch] = malloc(N * sizeof(int16_t));
                rec[ch] = malloc(N * sizeof(int16_t));
                out[ch] = malloc(N * sizeof(int16_t));
                memset(src[ch], 0, N * sizeof(int16_t));
                memset(rec[ch], 0, N * sizeof(int16_t));
                memset(out[ch], 0, N * sizeof(int16_t));
            }
            fill_tone(src[0], N, 137.0, 9000.0, 0.0);
            fill_tone(src[1], N, 211.0, 6000.0, 0.25);

            bw_init(&w, 1u << 20);
            adpcm_encode(&w, (const int16_t *const *)src, N, channels, nbits,
                         rec);

            memset(&s, 0, sizeof s);
            s.format    = PS_SWF_SND_ADPCM;
            s.bits      = 16;
            s.channels  = (uint8_t)channels;
            s.rate      = 22050;
            s.decodable = 1;
            s.nsample   = N;
            s.data      = w.b;
            s.len       = (uint32_t)w.n;

            snprintf(label, sizeof label, "%dch %d-bit init", channels, nbits);
            chk(label, ps_swf_snddec_init(&dec, &s), 0);

            got = ps_swf_snddec_read(&dec, out, N);
            snprintf(label, sizeof label, "%dch %d-bit sample count", channels,
                     nbits);
            chk(label, (long)got, N);

            for(ch = 0; ch < channels; ch++)
                for(i = 0; i < N; i++)
                    if(out[ch][i] != rec[ch][i] && mismatch < 0)
                        mismatch = (long)(ch * N + i);
            snprintf(label, sizeof label,
                     "%dch %d-bit decode matches the encoder", channels, nbits);
            chk(label, mismatch, -1);

            /* And that the codec actually codes something. A decoder that
             * returned the encoder's reconstruction of silence would pass every
             * check above. The bound is loose on purpose: this is here to catch
             * "unrelated to the input", not to grade the quantiser. */
            for(i = 0; i < N; i++) {
                double e = (double)rec[0][i] - (double)src[0][i];

                err += e * e;
                sig += (double)src[0][i] * (double)src[0][i];
            }
            if(nbits >= 4) {
                snprintf(label, sizeof label,
                         "%dch %d-bit tracks the input", channels, nbits);
                chk_true(label, sqrt(err / sig) < 0.15);
            }

            bw_free(&w);
            for(ch = 0; ch < 2; ch++) {
                free(src[ch]);
                free(rec[ch]);
                free(out[ch]);
            }
        }
}

/* --- test 4: seek, and the loop seam ------------------------------------- */

/* The bug this is written against was not an ADX bug. Almost no sound is a
 * whole number of decoder blocks long, so a loop that rewinds to a block
 * boundary leaves a remainder at the seam - and a refill handed less than it
 * asked for reads that as the end of the track, so a looping tune plays once
 * and stops. Five thousand samples is one full ADPCM packet and 904 of the
 * next, which is precisely that shape. */
static void test_seek_and_loop(void)
{
    enum { N = 5000, LOOPS = 3 };
    int16_t      *src = malloc(N * sizeof(int16_t));
    int16_t      *rec = malloc(N * sizeof(int16_t));
    int16_t      *ref = malloc(N * sizeof(int16_t));
    int16_t      *all = malloc((size_t)N * LOOPS * sizeof(int16_t));
    int16_t      *sp[1], *rp[1], *fp[2];
    bw            w;
    ps_swf_sound  s;
    ps_swf_snddec dec;
    ps_swf_sndstart ev;
    ps_swf_sndplay  pl;
    uint32_t      i, made;
    long          bad;

    sp[0] = src;
    rp[0] = rec;
    fill_tone(src, N, 97.0, 11000.0, 0.0);

    bw_init(&w, 1u << 20);
    adpcm_encode(&w, (const int16_t *const *)sp, N, 1, 4, rp);

    memset(&s, 0, sizeof s);
    s.format = PS_SWF_SND_ADPCM;  s.bits = 16;  s.channels = 1;
    s.rate = 22050;  s.decodable = 1;  s.nsample = N;
    s.data = w.b;    s.len = (uint32_t)w.n;

    /* Linear decode first: that is what every seek below is checked against. */
    fp[0] = ref;  fp[1] = NULL;
    chk("seam: linear init", ps_swf_snddec_init(&dec, &s), 0);
    chk("seam: linear count", (long)ps_swf_snddec_read(&dec, fp, N), N);

    /* A seek has to land on the sample, not on the packet that holds it. The
     * positions below straddle the packet boundary at 4096 deliberately. */
    bad = -1;
    for(i = 0; i < 12; i++) {
        static const uint32_t at[12] = { 0, 1, 4095, 4096, 4097, 1000,
                                         2500, 4900, 4999, 3, 4094, 2048 };
        int16_t  got[16];
        int16_t *gp[2] = { got, NULL };
        uint32_t k, n;

        if(ps_swf_snddec_init(&dec, &s) < 0 ||
           ps_swf_snddec_seek(&dec, at[i]) < 0) {
            bad = (long)at[i];
            break;
        }
        n = ps_swf_snddec_read(&dec, gp, 16);
        for(k = 0; k < n; k++)
            if(got[k] != ref[at[i] + k] && bad < 0)
                bad = (long)at[i];
    }
    chk("seam: seek lands on the sample", bad, -1);

    /* Now the loop itself, read back in a chunk size that divides neither the
     * sound nor a packet, so a seam falls inside a read rather than at its
     * edge - which is the case that used to come back short. */
    memset(&ev, 0, sizeof ev);
    ev.loops = LOOPS;
    chk("seam: total", (long)ps_swf_sndplay_total(&s, &ev), (long)N * LOOPS);
    chk("seam: play init", ps_swf_sndplay_init(&pl, &s, &ev), 0);

    made = 0;
    for(;;) {
        int16_t *ap[2] = { all + made, NULL };
        uint32_t want = 997, got;

        if(made + want > (uint32_t)(N * LOOPS))
            want = (uint32_t)(N * LOOPS) - made;
        if(!want)
            break;
        got = ps_swf_sndplay_read(&pl, ap, want);
        if(!got)
            break;
        made += got;
    }
    chk("seam: three passes come back whole", (long)made, (long)N * LOOPS);

    bad = -1;
    for(i = 0; i < made; i++)
        if(all[i] != ref[i % N] && bad < 0)
            bad = (long)i;
    chk("seam: every pass is identical", bad, -1);

    /* And with in and out points, so the pass is not a whole packet either at
     * its start or at its end. */
    memset(&ev, 0, sizeof ev);
    ev.loops = 2;  ev.has_in = 1;  ev.in = 1000;  ev.has_out = 1;  ev.out = 4500;
    chk("seam: windowed total", (long)ps_swf_sndplay_total(&s, &ev), 7000);
    chk("seam: windowed init", ps_swf_sndplay_init(&pl, &s, &ev), 0);
    {
        int16_t *ap[2] = { all, NULL };

        made = ps_swf_sndplay_read(&pl, ap, 7000);
    }
    chk("seam: windowed count", (long)made, 7000);
    bad = -1;
    for(i = 0; i < made; i++)
        if(all[i] != ref[1000 + i % 3500] && bad < 0)
            bad = (long)i;
    chk("seam: windowed passes match", bad, -1);

    bw_free(&w);
    free(src); free(rec); free(ref); free(all);
}

/* --- SWF assembly -------------------------------------------------------- */

static void tagged(bw *out, int code, const bw *body)
{
    if(body->n < 0x3f) {
        bw_u16(out, (unsigned)((code << 6) | (int)body->n));
    } else {
        bw_u16(out, (unsigned)((code << 6) | 0x3f));
        bw_u32(out, (uint32_t)body->n);
    }
    bw_bytes(out, body->b, body->n);
}

static void put_showframe(bw *tags)
{
    bw e;

    bw_init(&e, 4);
    tagged(tags, 1, &e);
    bw_free(&e);
}

static void put_end(bw *tags)
{
    bw e;

    bw_init(&e, 4);
    tagged(tags, 0, &e);
    bw_free(&e);
}

/* DefineSound: an ID, one packed byte of geometry, a sample count, the data. */
static void put_define_sound(bw *tags, int id, int fmt, int rate_code,
                             int bits16, int stereo, uint32_t nsample,
                             const uint8_t *d, size_t n)
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
    tagged(tags, 14, &body);
    bw_free(&body);
}

typedef struct {
    int      stop, no_multiple;
    int      has_in, has_out;
    uint32_t in, out;
    int      loops;              /* 0 for absent */
    int      nenv;
    uint32_t pos44[8];
    uint16_t left[8], right[8];
} sinfo;

static void put_start_sound(bw *tags, int id, const sinfo *si)
{
    bw  body;
    int i;

    bw_init(&body, 256);
    bw_u16(&body, (unsigned)id);
    bw_bits(&body, 0, 2);                              /* reserved */
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
    tagged(tags, 15, &body);
    bw_free(&body);
}

static void put_stream_head(bw *tags, int code, int fmt, int rate_code,
                            int bits16, int stereo, int spf)
{
    bw body;

    bw_init(&body, 64);
    bw_bits(&body, 0, 4);                              /* reserved */
    bw_bits(&body, (uint32_t)rate_code, 2);            /* playback */
    bw_bits(&body, 1, 1);
    bw_bits(&body, (uint32_t)(stereo ? 1 : 0), 1);
    bw_bits(&body, (uint32_t)fmt, 4);                  /* stream */
    bw_bits(&body, (uint32_t)rate_code, 2);
    bw_bits(&body, (uint32_t)(bits16 ? 1 : 0), 1);
    bw_bits(&body, (uint32_t)(stereo ? 1 : 0), 1);
    bw_u16(&body, (unsigned)spf);
    if(fmt == PS_SWF_SND_MP3)
        bw_u16(&body, 0);                              /* LatencySeek */
    tagged(tags, code, &body);
    bw_free(&body);
}

static void put_stream_block(bw *tags, const uint8_t *d, size_t n)
{
    bw body;

    bw_init(&body, n + 16);
    bw_bytes(&body, d, n);
    tagged(tags, 19, &body);
    bw_free(&body);
}

static uint8_t *make_swf(const bw *tags, size_t *out_len)
{
    bw       hdr;
    uint8_t *img;
    uint32_t total;

    bw_init(&hdr, 64);
    bw_bits(&hdr, 15, 5);                  /* RECT, fifteen bits per value */
    bw_bits(&hdr, 0, 15);
    bw_bits(&hdr, 6400, 15);
    bw_bits(&hdr, 0, 15);
    bw_bits(&hdr, 4800, 15);
    bw_flush(&hdr);
    bw_u16(&hdr, 12 << 8);                 /* FIXED8 12.0 fps, fraction first */
    bw_u16(&hdr, 1);                       /* declared frame count */

    total = (uint32_t)(8 + hdr.n + tags->n);
    img   = malloc(total);
    if(!img) {
        printf("FAIL out of memory building a test movie\n");
        exit(1);
    }
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

static int load_swf(const bw *tags, ps_swf_audio *a, const char *what)
{
    uint8_t *img;
    size_t   len;
    char     err[128];
    int      rc;

    err[0] = 0;
    img = make_swf(tags, &len);
    rc  = ps_swf_audio_load(img, len, a, err, sizeof err);
    if(rc < 0) {
        printf("FAIL %s did not load: %s\n", what, err);
        fails++;
    }
    free(img);
    return rc;
}

/* --- test 5: the tags ---------------------------------------------------- */

static void test_tags(void)
{
    enum { N = 300 };
    int16_t     *pcm = malloc(N * sizeof(int16_t));
    uint8_t     *le  = malloc(N * 2);
    uint8_t     *u8  = malloc(N);
    ps_swf_audio a;
    bw           tags;
    sinfo        si;
    uint32_t     i;

    fill_tone(pcm, N, 53.0, 20000.0, 0.0);
    for(i = 0; i < N; i++) {
        le[i * 2]     = (uint8_t)(pcm[i] & 0xff);
        le[i * 2 + 1] = (uint8_t)((pcm[i] >> 8) & 0xff);
        u8[i]         = (uint8_t)(i * 7u);
    }

    /* One movie carrying every event-sound shape at once: 16-bit little-endian
     * PCM under both codec numbers, 8-bit PCM, and a stereo pair. Codec 0 and
     * codec 3 hold identical bytes and must decode identically - the format has
     * no field saying which way round codec 0's samples are, so a reader that
     * guesses differently for the two produces a movie where the same sound
     * defined twice sounds different. */
    bw_init(&tags, 1u << 16);
    put_define_sound(&tags, 1, PS_SWF_SND_PCM_LE, 2, 1, 0, N, le, N * 2);
    put_define_sound(&tags, 2, PS_SWF_SND_PCM,    2, 1, 0, N, le, N * 2);
    put_define_sound(&tags, 3, PS_SWF_SND_PCM_LE, 1, 0, 0, N, u8, N);
    put_define_sound(&tags, 4, PS_SWF_SND_PCM_LE, 2, 1, 1, N / 2, le, N * 2);

    memset(&si, 0, sizeof si);
    put_start_sound(&tags, 1, &si);        /* frame 0, nothing optional */
    put_showframe(&tags);

    si.has_in = 1;  si.in = 40;
    si.has_out = 1; si.out = 260;
    si.loops = 5;
    si.no_multiple = 1;
    si.nenv = 3;
    si.pos44[0] = 0;    si.left[0] = 32768; si.right[0] = 0;
    si.pos44[1] = 4410; si.left[1] = 16384; si.right[1] = 16384;
    si.pos44[2] = 8820; si.left[2] = 0;     si.right[2] = 32768;
    put_start_sound(&tags, 3, &si);        /* frame 1, everything */
    put_showframe(&tags);

    memset(&si, 0, sizeof si);
    si.stop = 1;
    put_start_sound(&tags, 1, &si);        /* frame 2, a stop */
    put_showframe(&tags);
    put_end(&tags);

    if(load_swf(&tags, &a, "event sound movie") == 0) {
        const ps_swf_sound *s;

        chk("tags: four sounds", (long)a.nsound, 4);
        chk("tags: three cues", (long)a.nstart, 3);
        chk("tags: three frames", (long)a.nframe, 3);
        chk("tags: fps", (long)(a.fps * 100.0f + 0.5f), 1200);

        s = ps_swf_find_sound(&a, 1);
        chk_true("tags: sound 1 found", s != NULL);
        if(s) {
            ps_swf_snddec dec;
            int16_t      *out = malloc(N * sizeof(int16_t));
            int16_t      *op[2] = { out, NULL };
            long          bad = -1;

            chk("tags: sound 1 rate", (long)s->rate, 22050);
            chk("tags: sound 1 bits", s->bits, 16);
            chk("tags: sound 1 channels", s->channels, 1);
            chk("tags: sound 1 samples", (long)s->nsample, N);
            chk("tags: sound 1 decodable", s->decodable, 1);
            chk("tags: sound 1 init", ps_swf_snddec_init(&dec, s), 0);
            chk("tags: sound 1 count",
                (long)ps_swf_snddec_read(&dec, op, N), N);
            for(i = 0; i < N; i++)
                if(out[i] != pcm[i] && bad < 0)
                    bad = (long)i;
            chk("tags: 16-bit PCM is exact", bad, -1);
            free(out);
        }

        /* Codec 0 against codec 3, same bytes. */
        {
            const ps_swf_sound *a0 = ps_swf_find_sound(&a, 2);
            ps_swf_snddec dec;
            int16_t      *out = malloc(N * sizeof(int16_t));
            int16_t      *op[2] = { out, NULL };
            long          bad = -1;

            if(a0 && ps_swf_snddec_init(&dec, a0) == 0) {
                (void)ps_swf_snddec_read(&dec, op, N);
                for(i = 0; i < N; i++)
                    if(out[i] != pcm[i] && bad < 0)
                        bad = (long)i;
            }
            chk("tags: codec 0 reads as little-endian too", bad, -1);
            free(out);
        }

        /* Eight-bit PCM is unsigned and centred on 128. */
        {
            const ps_swf_sound *s8 = ps_swf_find_sound(&a, 3);
            ps_swf_snddec dec;
            int16_t      *out = malloc(N * sizeof(int16_t));
            int16_t      *op[2] = { out, NULL };
            long          bad = -1;

            chk_true("tags: sound 3 found", s8 != NULL);
            if(s8) {
                chk("tags: sound 3 bits", s8->bits, 8);
                chk("tags: sound 3 rate", (long)s8->rate, 11025);
                if(ps_swf_snddec_init(&dec, s8) == 0) {
                    (void)ps_swf_snddec_read(&dec, op, N);
                    for(i = 0; i < N; i++)
                        if(out[i] != (int16_t)(((int32_t)u8[i] - 128) * 256) &&
                           bad < 0)
                            bad = (long)i;
                }
            }
            chk("tags: 8-bit PCM widens around 128", bad, -1);
            free(out);
        }

        /* Stereo deinterleaves: the file holds L,R,L,R and the decoder hands
         * back two buffers, which is what the AICA's mono voices want. */
        {
            const ps_swf_sound *st = ps_swf_find_sound(&a, 4);
            ps_swf_snddec dec;
            int16_t      *l = malloc(N * sizeof(int16_t));
            int16_t      *r = malloc(N * sizeof(int16_t));
            int16_t      *op[2] = { l, r };
            long          bad = -1;

            chk_true("tags: sound 4 found", st != NULL);
            if(st) {
                chk("tags: sound 4 channels", st->channels, 2);
                chk("tags: sound 4 samples", (long)st->nsample, N / 2);
                if(ps_swf_snddec_init(&dec, st) == 0) {
                    (void)ps_swf_snddec_read(&dec, op, N / 2);
                    for(i = 0; i < N / 2; i++) {
                        if(l[i] != pcm[i * 2] && bad < 0)
                            bad = (long)i;
                        if(r[i] != pcm[i * 2 + 1] && bad < 0)
                            bad = (long)i;
                    }
                }
            }
            chk("tags: stereo deinterleaves", bad, -1);
            free(l);
            free(r);
        }

        /* The cues, and the frames they landed on. */
        if(a.nstart == 3) {
            chk("tags: cue 0 frame", (long)a.starts[0].frame, 0);
            chk("tags: cue 0 timeline", a.starts[0].timeline, 0);
            chk("tags: cue 0 loops", a.starts[0].loops, 0);
            chk("tags: cue 1 frame", (long)a.starts[1].frame, 1);
            chk("tags: cue 1 id", a.starts[1].id, 3);
            chk("tags: cue 1 in", (long)a.starts[1].in, 40);
            chk("tags: cue 1 out", (long)a.starts[1].out, 260);
            chk("tags: cue 1 loops", a.starts[1].loops, 5);
            chk("tags: cue 1 no_multiple", a.starts[1].no_multiple, 1);
            chk("tags: cue 1 envelope points", a.starts[1].nenv, 3);
            chk("tags: cue 1 env[1] pos", (long)a.starts[1].env[1].pos44, 4410);
            chk("tags: cue 1 env[2] right", a.starts[1].env[2].right, 32768);
            chk("tags: cue 2 is a stop", a.starts[2].stop, 1);
            chk("tags: cue 2 frame", (long)a.starts[2].frame, 2);
        }
        ps_swf_audio_free(&a);
    }
    bw_free(&tags);

    /* A cue inside a sprite belongs to the sprite's timeline and counts the
     * sprite's own frames. Getting this wrong makes every sound in a movie clip
     * fire against the root's clock, which on a clip that loops is a cue that
     * never repeats. */
    bw_init(&tags, 1u << 16);
    put_define_sound(&tags, 7, PS_SWF_SND_PCM_LE, 2, 1, 0, N, le, N * 2);
    put_showframe(&tags);
    {
        bw sprite, inner;

        bw_init(&inner, 1u << 12);
        memset(&si, 0, sizeof si);
        put_showframe(&inner);
        put_showframe(&inner);
        put_start_sound(&inner, 7, &si);    /* sprite frame 2 */
        put_showframe(&inner);
        put_end(&inner);

        bw_init(&sprite, 1u << 12);
        bw_u16(&sprite, 42);                /* sprite character ID */
        bw_u16(&sprite, 3);                 /* declared frames */
        bw_bytes(&sprite, inner.b, inner.n);
        tagged(&tags, 39, &sprite);
        bw_free(&sprite);
        bw_free(&inner);
    }
    put_showframe(&tags);
    put_end(&tags);

    if(load_swf(&tags, &a, "sprite cue movie") == 0) {
        chk("sprite: one cue", (long)a.nstart, 1);
        if(a.nstart == 1) {
            chk("sprite: cue timeline", a.starts[0].timeline, 42);
            chk("sprite: cue frame is the sprite's", (long)a.starts[0].frame, 2);
        }
        chk("sprite: root frames", (long)a.nframe, 2);
        ps_swf_audio_free(&a);
    }
    bw_free(&tags);

    free(pcm);
    free(le);
    free(u8);
}

/* --- test 6: the frame-to-sample map ------------------------------------- */

/* At 22050Hz and 12fps a frame is 1837.5 samples, which is the interesting case
 * and the usual one: the head's StreamSoundSampleCount is an integer, so it
 * cannot be the truth, and an encoder alternates 1837 and 1838 to stay on rate.
 * Frame N therefore begins at floor(N * 22050 / 12) and nowhere else, and the
 * whole point of keeping a block table is that this stays exact where
 * accumulating the declared rate does not. */
static void test_stream_map(void)
{
    enum { RATE = 22050, FPS = 12, FRAMES = 8, SPF = RATE / FPS };
    ps_swf_audio a;
    bw           tags;
    uint8_t     *blk = malloc(4096 * 2);
    uint32_t     f;

    memset(blk, 0, 4096 * 2);

    bw_init(&tags, 1u << 18);
    put_stream_head(&tags, 18, PS_SWF_SND_PCM_LE, 2, 1, 0, SPF);
    for(f = 0; f < FRAMES; f++) {
        uint32_t n = (uint32_t)(((uint64_t)(f + 1) * RATE) / FPS) -
                     (uint32_t)(((uint64_t)f * RATE) / FPS);

        put_stream_block(&tags, blk, n * 2u);
        put_showframe(&tags);
    }
    put_end(&tags);

    if(load_swf(&tags, &a, "stream map movie") == 0) {
        chk("map: one stream", (long)a.nstream, 1);
        if(a.nstream == 1) {
            const ps_swf_sndstream *st = &a.streams[0];
            long drift = 0;

            chk("map: head tag", st->head_tag, 18);
            chk("map: declared rate", (long)st->spf, SPF);
            chk("map: stream rate", (long)st->rate, RATE);
            chk("map: blocks", (long)st->nblock, FRAMES);
            chk("map: frames", (long)st->nframe, FRAMES);
            chk("map: no gaps", (long)st->gaps, 0);
            chk("map: no duplicates", (long)st->dups, 0);
            chk("map: no short blocks", (long)st->short_blocks, 0);

            for(f = 0; f < FRAMES; f++) {
                uint32_t want = (uint32_t)(((uint64_t)f * RATE) / FPS);
                uint32_t got  = 0xffffffffu;
                char     label[64];
                int      rc;

                snprintf(label, sizeof label, "map: frame %u starts at %u",
                         f, want);
                rc = ps_swf_stream_frame_sample(st, f, &got);
                chk("map: every frame is mapped", rc, 0);
                chk(label, (long)got, (long)want);
            }
            chk("map: total samples",
                (long)st->nsample,
                (long)(((uint64_t)FRAMES * RATE) / FPS));

            /* And the reason the block table exists rather than a multiply: by
             * frame seven, accumulating the declared integer rate is already
             * three samples adrift, and the error only grows. */
            drift = (long)(((uint64_t)(FRAMES - 1) * RATE) / FPS) -
                    (long)((FRAMES - 1) * SPF);
            chk("map: declared rate has drifted by frame 7", drift, 3);
        }
        ps_swf_audio_free(&a);
    }
    bw_free(&tags);
    free(blk);
}

/* A stream in ADPCM, where each block is a complete little ADPCM stream of its
 * own - code size, packet header and all. That is what makes seeking to a frame
 * possible at all, and a reader that treats the blocks as one continuous stream
 * decodes each block's header as audio. */
static void test_stream_adpcm(void)
{
    enum { RATE = 22050, FPS = 12, FRAMES = 5, SPF = 1837 };
    ps_swf_audio a;
    bw           tags;
    int16_t     *src = malloc(SPF * FRAMES * sizeof(int16_t));
    int16_t     *rec = malloc(SPF * FRAMES * sizeof(int16_t));
    uint32_t     f;

    fill_tone(src, SPF * FRAMES, 61.0, 12000.0, 0.0);

    bw_init(&tags, 1u << 18);
    put_stream_head(&tags, 45, PS_SWF_SND_ADPCM, 2, 1, 0, SPF);
    for(f = 0; f < FRAMES; f++) {
        bw       w;
        int16_t *sp[1] = { src + f * SPF };
        int16_t *rp[1] = { rec + f * SPF };

        bw_init(&w, 1u << 16);
        adpcm_encode(&w, (const int16_t *const *)sp, SPF, 1, 4, rp);
        put_stream_block(&tags, w.b, w.n);
        put_showframe(&tags);
        bw_free(&w);
    }
    put_end(&tags);

    if(load_swf(&tags, &a, "adpcm stream movie") == 0 && a.nstream == 1) {
        const ps_swf_sndstream *st = &a.streams[0];
        long bad = -1;

        chk("adpcm stream: head tag", st->head_tag, 45);
        chk("adpcm stream: blocks", (long)st->nblock, FRAMES);
        chk("adpcm stream: bits forced to 16", st->bits, 16);
        chk("adpcm stream: total", (long)st->nsample, SPF * FRAMES);
        chk("adpcm stream: no short blocks", (long)st->short_blocks, 0);

        for(f = 0; f < FRAMES && f < st->nblock; f++) {
            ps_swf_snddec dec;
            int16_t      *out = malloc(SPF * sizeof(int16_t));
            int16_t      *op[2] = { out, NULL };
            uint32_t      k, n;

            chk("adpcm stream: block start",
                (long)st->blocks[f].first, (long)(f * SPF));
            if(ps_swf_snddec_init_block(&dec, st, f) == 0) {
                n = ps_swf_snddec_read(&dec, op, SPF);
                if(n != SPF && bad < 0)
                    bad = (long)f;
                for(k = 0; k < n; k++)
                    if(out[k] != rec[f * SPF + k] && bad < 0)
                        bad = (long)f;
            } else if(bad < 0) {
                bad = (long)f;
            }
            free(out);
        }
        chk("adpcm stream: every block decodes on its own", bad, -1);
        ps_swf_audio_free(&a);
    }
    bw_free(&tags);
    free(src);
    free(rec);
}

/* --- test 7: refusals ---------------------------------------------------- */

static void test_refusals(void)
{
    enum { N = 400, SPF = 1837 };
    ps_swf_audio a;
    bw           tags;
    uint8_t     *blk = malloc(SPF * 2);

    memset(blk, 0, SPF * 2);

    /* A codec we decline, as an event sound and as a stream. Both parse - the
     * geometry, the sample count and the frame map all come out - and both
     * refuse to decode, which is the whole distinction between "we did not read
     * this tag" and "we read it and will not guess at the audio". */
    bw_init(&tags, 1u << 16);
    {
        uint8_t mp3[64];

        memset(mp3, 0xff, sizeof mp3);
        put_define_sound(&tags, 1, PS_SWF_SND_MP3, 3, 1, 1, 1152, mp3,
                         sizeof mp3);
        put_define_sound(&tags, 2, PS_SWF_SND_NELLY, 2, 1, 0, 256, mp3,
                         sizeof mp3);
    }
    put_showframe(&tags);
    put_end(&tags);

    if(load_swf(&tags, &a, "declined codec movie") == 0) {
        const ps_swf_sound *s = ps_swf_find_sound(&a, 1);
        ps_swf_snddec dec;

        chk("refuse: two sounds parsed", (long)a.nsound, 2);
        chk_true("refuse: mp3 sound found", s != NULL);
        if(s) {
            chk("refuse: mp3 not decodable", s->decodable, 0);
            chk("refuse: mp3 rate still read", (long)s->rate, 44100);
            chk("refuse: mp3 channels still read", s->channels, 2);
            chk("refuse: mp3 samples still read", (long)s->nsample, 1152);
            chk("refuse: mp3 decoder declines",
                ps_swf_snddec_init(&dec, s), -1);
            chk_true("refuse: mp3 says why",
                     strstr(ps_swf_sound_fmtname(s->format), "declined") != NULL);
        }
        s = ps_swf_find_sound(&a, 2);
        if(s)
            chk("refuse: nellymoser declines too", s->decodable, 0);
        ps_swf_audio_free(&a);
    }
    bw_free(&tags);

    /* An MP3 stream: still declined, but its blocks state their own sample
     * counts, so the frame map is exact even though not one sample can be
     * decoded. That map is what places captions and cue points. */
    bw_init(&tags, 1u << 16);
    put_stream_head(&tags, 18, PS_SWF_SND_MP3, 2, 1, 0, SPF);
    {
        uint8_t body[64];
        uint32_t f;

        for(f = 0; f < 4; f++) {
            memset(body, 0, sizeof body);
            body[0] = (uint8_t)(1152u & 0xff);       /* SampleCount */
            body[1] = (uint8_t)(1152u >> 8);
            put_stream_block(&tags, body, sizeof body);
            put_showframe(&tags);
        }
    }
    put_end(&tags);

    if(load_swf(&tags, &a, "mp3 stream movie") == 0 && a.nstream == 1) {
        const ps_swf_sndstream *st = &a.streams[0];
        uint32_t at = 0;

        chk("refuse: mp3 stream not decodable", st->decodable, 0);
        chk("refuse: mp3 stream blocks", (long)st->nblock, 4);
        chk("refuse: mp3 stream maps frames",
            ps_swf_stream_frame_sample(st, 2, &at), 0);
        chk("refuse: mp3 frame 2 sample", (long)at, 2304);
        chk("refuse: mp3 total", (long)st->nsample, 4608);
        ps_swf_audio_free(&a);
    }
    bw_free(&tags);

    /* A truncated block, and a stream whose block count disagrees with its
     * frame count. One movie carries both because they are the same kind of
     * damage: frame 1 gets a block a tenth of the size it should be and an odd
     * number of bytes with it, frame 3 gets none at all, and frame 4 gets two.
     * Five blocks across five frames, which is the right count for entirely the
     * wrong reasons - which is why the count alone is not the check. */
    bw_init(&tags, 1u << 18);
    put_stream_head(&tags, 18, PS_SWF_SND_PCM_LE, 2, 1, 0, SPF);
    put_stream_block(&tags, blk, SPF * 2);           /* frame 0, whole */
    put_showframe(&tags);
    put_stream_block(&tags, blk, 180 * 2 + 1);       /* frame 1, cut short */
    put_showframe(&tags);
    put_stream_block(&tags, blk, SPF * 2);           /* frame 2, whole */
    put_showframe(&tags);
    put_showframe(&tags);                            /* frame 3, silent */
    put_stream_block(&tags, blk, SPF * 2);           /* frame 4, two of them */
    put_stream_block(&tags, blk, SPF * 2);
    put_showframe(&tags);
    put_end(&tags);

    if(load_swf(&tags, &a, "damaged stream movie") == 0 && a.nstream == 1) {
        const ps_swf_sndstream *st = &a.streams[0];
        uint32_t at = 0;

        chk("damage: blocks", (long)st->nblock, 5);
        chk("damage: frames", (long)st->nframe, 5);
        chk("damage: one short block", (long)st->short_blocks, 1);
        chk("damage: the short one is frame 1's", st->blocks[1].damaged, 1);
        chk("damage: the whole ones are not flagged",
            st->blocks[0].damaged + st->blocks[2].damaged, 0);
        chk("damage: one gap", (long)st->gaps, 1);
        chk("damage: one duplicate", (long)st->dups, 1);
        chk("damage: the short block decoded what it had",
            (long)st->blocks[1].nsample, 180);
        chk("damage: frame 3 has no block",
            ps_swf_stream_frame_sample(st, 3, &at), -1);
        chk("damage: frame 2 still maps",
            ps_swf_stream_frame_sample(st, 2, &at), 0);
        chk("damage: frame 2 sample", (long)at, SPF + 180);
        ps_swf_audio_free(&a);
    }
    bw_free(&tags);

    /* A DefineSound claiming far more samples than its bytes can hold. Believing
     * it would have the decoder walk off the end of the buffer; the bytes that
     * arrived are what gets played, which is the same call ps_adx_parse makes
     * about a download cut off mid-file. */
    bw_init(&tags, 1u << 16);
    {
        uint8_t small[64];

        memset(small, 0, sizeof small);
        put_define_sound(&tags, 9, PS_SWF_SND_PCM_LE, 2, 1, 0, 100000, small,
                         sizeof small);
    }
    put_showframe(&tags);
    put_end(&tags);

    if(load_swf(&tags, &a, "overclaiming movie") == 0) {
        const ps_swf_sound *s = ps_swf_find_sound(&a, 9);

        chk_true("clamp: sound found", s != NULL);
        if(s) {
            ps_swf_snddec dec;
            int16_t       out[64];
            int16_t      *op[2] = { out, NULL };

            chk("clamp: declared count kept", (long)s->declared, 100000);
            chk("clamp: usable count is the bytes present", (long)s->nsample, 32);
            chk("clamp: flagged", s->clamped, 1);
            chk("clamp: init", ps_swf_snddec_init(&dec, s), 0);
            chk("clamp: reads only what is there",
                (long)ps_swf_snddec_read(&dec, op, 64), 32);
        }
        ps_swf_audio_free(&a);
    }
    bw_free(&tags);

    /* Not a SWF at all, and a truncated header. */
    {
        static const uint8_t junk[16] = { 'N', 'O', 'P', 'E' };
        char err[64];

        chk("refuse: not a SWF",
            ps_swf_audio_load(junk, sizeof junk, &a, err, sizeof err), -1);
        chk("refuse: short buffer",
            ps_swf_audio_load(junk, 4, &a, err, sizeof err), -1);
    }
    {
        static const uint8_t cws[16] = { 'C', 'W', 'S', 6, 0, 0, 0, 0, 0 };
        char err[64];

        chk("refuse: compressed SWF",
            ps_swf_audio_load(cws, sizeof cws, &a, err, sizeof err), -1);
    }

    free(blk);
}

/* --- test 8: the envelope ------------------------------------------------ */

/* Envelope positions are stated at 44100Hz whatever the sound's own rate is.
 * The sound below runs at 11025, so its sample 2756 is 44100-sample 11024 -
 * just short of the second point at 11025 - and a reader that skipped the
 * conversion would have finished the whole curve by then. */
static void test_envelope(void)
{
    ps_swf_sndstart ev;
    uint16_t        l, r;

    memset(&ev, 0, sizeof ev);
    chk("env: no envelope is full level",
        (ps_swf_sndplay_envelope(&ev, 11025, 500, &l, &r), l), 32768);

    ev.nenv = 3;
    ev.env[0].pos44 = 0;      ev.env[0].left = 32768; ev.env[0].right = 0;
    ev.env[1].pos44 = 11025;  ev.env[1].left = 16384; ev.env[1].right = 16384;
    ev.env[2].pos44 = 22050;  ev.env[2].left = 0;     ev.env[2].right = 32768;

    /* At 44100 a sample position and an envelope position are the same number,
     * so the nodes land exactly and every level below is arithmetic. */
    ps_swf_sndplay_envelope(&ev, 44100, 0, &l, &r);
    chk("env: at the first point, left", l, 32768);
    chk("env: at the first point, right", r, 0);

    ps_swf_sndplay_envelope(&ev, 44100, 11025, &l, &r);
    chk("env: at the middle point, left", l, 16384);
    chk("env: at the middle point, right", r, 16384);

    /* Half way along the second segment, at 16537 of 11025..22050: the levels
     * are 16384 - 16384*5512/11025 and 16384 + 16384*5512/11025, truncated. */
    ps_swf_sndplay_envelope(&ev, 44100, 16537, &l, &r);
    chk("env: mid second segment, left", l, 8193);
    chk("env: mid second segment, right", r, 24575);

    ps_swf_sndplay_envelope(&ev, 44100, 100000, &l, &r);
    chk("env: past the end holds the last, left", l, 0);
    chk("env: past the end holds the last, right", r, 32768);

    /* And now the conversion that is the whole point. Sample 2756 of an
     * 11025Hz sound is 44100-sample 11024, one short of the middle node, so the
     * curve is all but finished with its first segment: 32768 - 16384*11024/11025.
     * The same sample number of a 44100Hz sound is only a quarter of the way
     * along that segment. A reader that ignored the rate would report the first
     * of these for both. */
    ps_swf_sndplay_envelope(&ev, 11025, 2756, &l, &r);
    chk("env: 11025Hz sample 2756 is nearly at the node", l, 16386);

    ps_swf_sndplay_envelope(&ev, 44100, 2756, &l, &r);
    chk("env: 44100Hz sample 2756 is a quarter along", l, 28673);
}

/* --- real content -------------------------------------------------------- */

static void inventory(const char *path)
{
    FILE     *f = fopen(path, "rb");
    uint8_t  *buf;
    long      len;
    ps_swf_audio a;
    char      err[128];
    uint32_t  i;

    if(!f) {
        printf("cannot open %s\n", path);
        return;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)len);
    if(!buf || fread(buf, 1, (size_t)len, f) != (size_t)len) {
        printf("cannot read %s\n", path);
        fclose(f);
        free(buf);
        return;
    }
    fclose(f);

    err[0] = 0;
    if(ps_swf_audio_load(buf, (size_t)len, &a, err, sizeof err) < 0) {
        printf("%s: %s\n", path, err);
        free(buf);
        return;
    }

    printf("\n--- %s: %ld bytes, SWF %d, %.2f fps, %u frames\n", path, len,
           a.version, (double)a.fps, a.nframe);
    printf("    %u sounds, %u cues, %u streams\n", a.nsound, a.nstart,
           a.nstream);
    for(i = 0; i < a.nsound; i++) {
        const ps_swf_sound *s = &a.sounds[i];

        printf("    sound %-5u %-34s %5u Hz %2u-bit %s  %u samples%s\n",
               s->id, ps_swf_sound_fmtname(s->format), s->rate, s->bits,
               s->channels == 2 ? "stereo" : "mono  ", s->nsample,
               s->clamped ? " (clamped)" : "");
    }
    for(i = 0; i < a.nstream; i++) {
        const ps_swf_sndstream *st = &a.streams[i];

        printf("    stream on timeline %-5u %-34s %5u Hz %s  %u blocks, "
               "%u samples, %u gaps, %u dups, %u short\n",
               st->timeline, ps_swf_sound_fmtname(st->format), st->rate,
               st->channels == 2 ? "stereo" : "mono  ", st->nblock,
               st->nsample, st->gaps, st->dups, st->short_blocks);
    }
    printf("    held %zu bytes\n", ps_swf_mem_live());
    ps_swf_audio_free(&a);
    free(buf);
}

int main(int argc, char **argv)
{
    test_tables();
    test_handmade();
    test_roundtrip();
    test_seek_and_loop();
    test_tags();
    test_stream_map();
    test_stream_adpcm();
    test_refusals();
    test_envelope();

    chk("everything the tests loaded was freed", (long)ps_swf_mem_live(), 0);
    printf("sound: peak %zu bytes held\n", ps_swf_mem_peak());

    if(argc > 1)
        inventory(argv[1]);

    printf(fails ? "%d failure(s)\n" : "all sound tests pass\n", fails);
    return fails ? 1 : 0;
}
