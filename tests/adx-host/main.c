/* Host-side proof that the ADX decoder, and the ring it feeds, are correct.
 *
 * The page that motivated ADX support references an asset the server no longer
 * has, so there is no real file to test against and no way to get one that is
 * both permissively licensed and known-good. What can be built instead is the
 * stronger test anyway: an encoder written from the same format description,
 * run over signals whose exact shape is known, decoded back, and compared.
 *
 * A round trip alone would pass even if both halves shared a mistake - if both
 * packed the low nibble first, say - so it is not the only check here:
 *
 *   1. The predictor coefficients are pinned to values derivable by hand from
 *      the published formula, independently of any code in this project.
 *   2. One block is decoded whose first four output samples were worked out on
 *      paper. That fixes nibble order, sign extension, scale endianness and the
 *      Q12 predictor, which are exactly the conventions a symmetric bug would
 *      hide.
 *   3. Chunked decoding is compared against one-shot decoding, because the
 *      Dreamcast path never decodes a file in one call - it refills a ring
 *      buffer a few thousand samples at a time, and a decoder that carries
 *      state wrongly across calls would sound fine here and broken there.
 *   4. Headers that must be refused are checked one at a time, since on the
 *      console the alternative to a clean refusal is playing noise at volume.
 *
 * The second half of the file goes further and runs ps_adxstream against a
 * stub voice backend (fakevoice.c), stepping a simulated play cursor and
 * reading back what a speaker on the ring would have heard. That is where the
 * genuinely hard-to-see bugs live: the ring arithmetic decides what gets
 * overwritten while the hardware is reading it, and on a console every one of
 * its failure modes sounds like a periodic click that could just as easily be
 * the decoder or the pitch. It has already earned its keep - it caught a loop
 * seam that ended a track after one pass whenever the file was not a whole
 * number of blocks long, which is nearly every file.
 *
 * Build and run:  cd tests/adx-host && make && ./adxtest
 */
#include "ps_adx.h"
#include "ps_adxstream.h"
#include "fakevoice.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;

static void check(int ok, const char *what)
{
    printf("%-58s %s\n", what, ok ? "ok" : "FAIL");
    if(!ok)
        g_fail++;
}

/* ------------------------------------------------------------- encoder ---- */

/* A minimal ADX writer, here and not in the library: nothing on a Dreamcast
 * needs to produce ADX, and a test that shares code with the thing it is
 * testing proves less. It mirrors only the format description, not ps_adx.c. */

#define ENC_BLOCK      18
#define ENC_BLOCK_FRM  32
#define ENC_DATA_OFF   0x34

static void put_be16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* The same coefficients the decoder derives, computed here from the published
 * formula rather than by calling into ps_adx.c. */
static void enc_coefs(uint32_t rate, uint32_t hpf, int32_t *c1, int32_t *c2)
{
    double z = cos(2.0 * 3.14159265358979323846 * hpf / rate);
    double a = sqrt(2.0) - z;
    double b = sqrt(2.0) - 1.0;
    double c = (a - sqrt((a + b) * (a - b))) / b;

    *c1 = (int32_t)(c * 2.0 * 4096.0);
    *c2 = (int32_t)(-(c * c) * 4096.0);
}

typedef struct {
    int32_t h1, h2;
} enc_hist;

/* Closed-loop quantisation of one block: each residual is measured against the
 * prediction the decoder will actually make, so encoder and decoder stay in
 * step instead of drifting apart over the file. */
static int encode_block(const int16_t *src, uint32_t n, int32_t c1, int32_t c2,
                        enc_hist *hist, uint8_t *out)
{
    int32_t  scale, best_scale = 1, maxabs = 0;
    uint32_t i;
    int      attempt;

    /* Opening estimate from the open-loop error, then widened until nothing
     * saturates. Sixteen tries is far more than any real signal needs. */
    {
        int32_t h1 = hist->h1, h2 = hist->h2;

        for(i = 0; i < n; i++) {
            int32_t pred = (c1 * h1 + c2 * h2) >> 12;
            int32_t e    = src[i] - pred;

            if(e < 0)
                e = -e;
            if(e > maxabs)
                maxabs = e;
            h2 = h1;
            h1 = src[i];
        }
    }
    best_scale = maxabs / 7 + 1;

    for(attempt = 0; attempt < 16; attempt++) {
        int32_t h1 = hist->h1, h2 = hist->h2;
        int     saturated = 0;

        scale = best_scale;
        if(scale > 32767)
            scale = 32767;

        memset(out, 0, ENC_BLOCK);
        put_be16(out, (uint32_t)(scale & 0xffff));

        for(i = 0; i < ENC_BLOCK_FRM; i++) {
            int32_t want = (i < n) ? src[i] : 0;
            int32_t pred = (c1 * h1 + c2 * h2) >> 12;
            int32_t d    = want - pred;
            int32_t nib  = (d >= 0) ? (d + scale / 2) / scale
                                    : -((-d + scale / 2) / scale);
            int32_t v;

            if(nib > 7)  { nib = 7;  saturated = 1; }
            if(nib < -8) { nib = -8; saturated = 1; }

            v = nib * scale + pred;
            if(v > 32767)
                v = 32767;
            else if(v < -32768)
                v = -32768;

            h2 = h1;
            h1 = v;

            out[2 + (i >> 1)] |= (uint8_t)((nib & 0x0f) << ((i & 1) ? 0 : 4));
        }

        if(!saturated || scale >= 32767) {
            hist->h1 = h1;
            hist->h2 = h2;
            return 0;
        }
        best_scale = best_scale * 5 / 4 + 1;
    }
    return -1;
}

/* Writes a whole file. src is deinterleaved: src[ch][frame]. loop_end 0 means
 * no loop record. Returns a malloc'd buffer. */
static uint8_t *encode_adx(int16_t *const *src, uint32_t frames,
                           uint32_t channels, uint32_t rate,
                           uint32_t loop_start, uint32_t loop_end,
                           size_t *out_len)
{
    uint32_t blocks = (frames + ENC_BLOCK_FRM - 1) / ENC_BLOCK_FRM;
    size_t   len    = ENC_DATA_OFF + (size_t)blocks * ENC_BLOCK * channels;
    uint8_t *buf    = (uint8_t *)calloc(1, len);
    enc_hist hist[PS_ADX_MAX_CHANNELS];
    int32_t  c1, c2;
    uint32_t b, ch;

    if(!buf)
        return NULL;

    enc_coefs(rate, 500, &c1, &c2);
    memset(hist, 0, sizeof hist);

    put_be16(buf + 0x00, 0x8000);
    put_be16(buf + 0x02, ENC_DATA_OFF - 4);
    buf[0x04] = PS_ADX_ENC_STANDARD;
    buf[0x05] = ENC_BLOCK;
    buf[0x06] = 4;
    buf[0x07] = (uint8_t)channels;
    put_be32(buf + 0x08, rate);
    put_be32(buf + 0x0c, frames);
    put_be16(buf + 0x10, 500);
    buf[0x12] = 3;
    buf[0x13] = 0;

    if(loop_end) {
        put_be16(buf + 0x14, 0);
        put_be16(buf + 0x16, 1);
        put_be32(buf + 0x18, 1);
        put_be32(buf + 0x1c, loop_start);
        put_be32(buf + 0x20, ENC_DATA_OFF +
                             (loop_start / ENC_BLOCK_FRM) * ENC_BLOCK * channels);
        put_be32(buf + 0x24, loop_end);
        put_be32(buf + 0x28, ENC_DATA_OFF +
                             (loop_end / ENC_BLOCK_FRM) * ENC_BLOCK * channels);
    }

    memcpy(buf + ENC_DATA_OFF - 6, "(c)CRI", 6);

    for(b = 0; b < blocks; b++) {
        uint32_t at = b * ENC_BLOCK_FRM;
        uint32_t n  = frames - at;

        if(n > ENC_BLOCK_FRM)
            n = ENC_BLOCK_FRM;

        for(ch = 0; ch < channels; ch++) {
            uint8_t *dst = buf + ENC_DATA_OFF +
                           ((size_t)b * channels + ch) * ENC_BLOCK;

            if(encode_block(src[ch] + at, n, c1, c2, &hist[ch], dst) != 0) {
                free(buf);
                return NULL;
            }
        }
    }

    *out_len = len;
    return buf;
}

/* ------------------------------------------------------------ signal ------ */

static double snr_db(const int16_t *ref, const int16_t *got, uint32_t n)
{
    double sig = 0.0, err = 0.0;
    uint32_t i;

    for(i = 0; i < n; i++) {
        double e = (double)ref[i] - (double)got[i];

        sig += (double)ref[i] * (double)ref[i];
        err += e * e;
    }
    if(err <= 0.0)
        return 999.0;
    if(sig <= 0.0)
        return -999.0;
    return 10.0 * log10(sig / err);
}

/* ------------------------------------------------------------- cases ------ */

static void test_coefficients(void)
{
    ps_adx_info info;
    int16_t    *sig;
    int16_t    *chans[1];
    uint8_t    *file;
    size_t      len;
    uint32_t    i;

    sig = (int16_t *)calloc(64, sizeof(int16_t));
    for(i = 0; i < 64; i++)
        sig[i] = 0;
    chans[0] = sig;

    file = encode_adx(chans, 64, 1, 44100, 0, 0, &len);
    check(file != NULL, "encode a 44100Hz mono file");
    if(!file)
        return;

    check(ps_adx_parse(file, len, &info) == PS_ADX_OK, "parse it");

    /* Worked out by hand from the published formula at 44100Hz with the
     * standard 500Hz cutoff:
     *   z = cos(2*pi*500/44100)   = 0.9974637
     *   a = sqrt(2) - z           = 0.4167499
     *   b = sqrt(2) - 1           = 0.4142136
     *   c = (a - sqrt(a^2-b^2))/b = 0.8952965
     *   c1 = 2c * 4096 = 7334.1,  c2 = -c^2 * 4096 = -3283.2
     * Truncation and rounding-to-nearest differ by one here, so one step of
     * slack is allowed; anything further out means the formula is wrong, not
     * the rounding. */
    printf("    coefficients: c1=%d c2=%d\n", (int)info.coef1, (int)info.coef2);
    check(info.coef1 >= 7333 && info.coef1 <= 7335, "c1 matches the hand value");
    check(info.coef2 >= -3285 && info.coef2 <= -3282, "c2 matches the hand value");

    free(file);
    free(sig);
}

/* One block built by hand, with output computed on paper. Depends on the
 * coefficients above being 7334 and -3283:
 *
 *   scale = 256, nibbles = 1, 0, 0, 0, ...
 *   s0 = 1*256 + 0                              = 256
 *   s1 = 0*256 + (7334*256 - 3283*0)   >> 12    = 458
 *   s2 = 0*256 + (7334*458 - 3283*256) >> 12    = 614
 *   s3 = 0*256 + (7334*614 - 3283*458) >> 12    = 732
 */
static void test_hand_decoded_block(void)
{
    uint8_t     file[ENC_DATA_OFF + ENC_BLOCK];
    ps_adx_info info;
    ps_adx_dec  dec;
    int16_t     pcm[ENC_BLOCK_FRM];
    int16_t    *out[1];
    uint32_t    n;

    memset(file, 0, sizeof file);
    put_be16(file + 0x00, 0x8000);
    put_be16(file + 0x02, ENC_DATA_OFF - 4);
    file[0x04] = PS_ADX_ENC_STANDARD;
    file[0x05] = ENC_BLOCK;
    file[0x06] = 4;
    file[0x07] = 1;
    put_be32(file + 0x08, 44100);
    put_be32(file + 0x0c, ENC_BLOCK_FRM);
    put_be16(file + 0x10, 500);
    file[0x12] = 3;
    memcpy(file + ENC_DATA_OFF - 6, "(c)CRI", 6);

    put_be16(file + ENC_DATA_OFF, 256);
    file[ENC_DATA_OFF + 2] = 0x10;   /* first nibble 1, second 0 */

    check(ps_adx_parse(file, sizeof file, &info) == PS_ADX_OK,
          "parse the hand-built file");

    ps_adx_dec_init(&dec, &info, file, sizeof file);
    out[0] = pcm;
    n = ps_adx_decode(&dec, out, ENC_BLOCK_FRM);

    check(n == ENC_BLOCK_FRM, "one block yields 32 samples");
    printf("    decoded: %d %d %d %d\n", pcm[0], pcm[1], pcm[2], pcm[3]);
    check(pcm[0] == 256 && pcm[1] == 458 && pcm[2] == 614 && pcm[3] == 732,
          "first four samples match the hand calculation");
}

static void test_roundtrip(uint32_t channels, uint32_t rate, uint32_t frames,
                           const char *label)
{
    int16_t    *src[PS_ADX_MAX_CHANNELS];
    int16_t    *got[PS_ADX_MAX_CHANNELS];
    uint8_t    *file;
    size_t      len;
    ps_adx_info info;
    ps_adx_dec  dec;
    uint32_t    ch, i, n;
    char        msg[128];
    int         ok = 1;

    for(ch = 0; ch < channels; ch++) {
        src[ch] = (int16_t *)calloc(frames, sizeof(int16_t));
        got[ch] = (int16_t *)calloc(frames, sizeof(int16_t));
    }

    /* A different tone per channel, so a decoder that crossed the two would
     * fail rather than average out. */
    for(ch = 0; ch < channels; ch++) {
        double f = (ch == 0) ? 440.0 : 660.0;

        for(i = 0; i < frames; i++)
            src[ch][i] = (int16_t)(12000.0 *
                                   sin(2.0 * 3.14159265358979323846 * f * i / rate));
    }

    file = encode_adx(src, frames, channels, rate, 0, 0, &len);
    check(file != NULL, "encode");
    if(!file)
        return;

    check(ps_adx_parse(file, len, &info) == PS_ADX_OK, "parse");
    check(info.channels == channels && info.rate == rate &&
          info.frames == frames, "header round-trips");

    ps_adx_dec_init(&dec, &info, file, len);
    {
        int16_t *ptr[PS_ADX_MAX_CHANNELS];

        for(ch = 0; ch < channels; ch++)
            ptr[ch] = got[ch];
        n = ps_adx_decode(&dec, ptr, frames);
    }
    check(n == frames, "decodes every sample");

    for(ch = 0; ch < channels; ch++) {
        double s = snr_db(src[ch], got[ch], frames);

        snprintf(msg, sizeof msg, "%s ch%u SNR %.1f dB (want > 30)", label,
                 (unsigned)ch, s);
        check(s > 30.0, msg);
        if(s <= 30.0)
            ok = 0;
    }
    (void)ok;

    free(file);
    for(ch = 0; ch < channels; ch++) {
        free(src[ch]);
        free(got[ch]);
    }
}

/* The console never decodes a file in one call. This is that path. */
static void test_chunked_matches_whole(void)
{
    enum { FRAMES = 4096, CHUNK = 128 };
    int16_t    *src[2], *whole[2], *piece[2];
    uint8_t    *file;
    size_t      len;
    ps_adx_info info;
    ps_adx_dec  dec;
    uint32_t    ch, i, at;
    int         same = 1;

    for(ch = 0; ch < 2; ch++) {
        src[ch]   = (int16_t *)calloc(FRAMES, sizeof(int16_t));
        whole[ch] = (int16_t *)calloc(FRAMES, sizeof(int16_t));
        piece[ch] = (int16_t *)calloc(FRAMES, sizeof(int16_t));
    }
    for(ch = 0; ch < 2; ch++)
        for(i = 0; i < FRAMES; i++)
            src[ch][i] = (int16_t)(9000.0 * sin(i * (0.03 + 0.01 * ch)) +
                                   3000.0 * sin(i * 0.31));

    file = encode_adx(src, FRAMES, 2, 22050, 0, 0, &len);
    if(!file || ps_adx_parse(file, len, &info) != PS_ADX_OK) {
        check(0, "chunked test setup");
        return;
    }

    ps_adx_dec_init(&dec, &info, file, len);
    { int16_t *p[2] = { whole[0], whole[1] };
      ps_adx_decode(&dec, p, FRAMES); }

    ps_adx_dec_init(&dec, &info, file, len);
    for(at = 0; at < FRAMES; at += CHUNK) {
        int16_t *p[2] = { piece[0] + at, piece[1] + at };

        if(ps_adx_decode(&dec, p, CHUNK) != CHUNK) {
            same = 0;
            break;
        }
    }

    for(ch = 0; ch < 2 && same; ch++)
        if(memcmp(whole[ch], piece[ch], FRAMES * sizeof(int16_t)) != 0)
            same = 0;

    check(same, "128-sample chunks decode identically to one call");

    /* Seeking back to the start must reproduce the opening exactly, which is
     * what looping does at the end of every pass. */
    ps_adx_seek(&dec, 0);
    { int16_t *p[2] = { piece[0], piece[1] };
      ps_adx_decode(&dec, p, CHUNK); }
    check(memcmp(whole[0], piece[0], CHUNK * sizeof(int16_t)) == 0,
          "seek to 0 reproduces the opening samples");

    free(file);
    for(ch = 0; ch < 2; ch++) {
        free(src[ch]);
        free(whole[ch]);
        free(piece[ch]);
    }
}

static void test_loop_record(void)
{
    int16_t    *sig = (int16_t *)calloc(2048, sizeof(int16_t));
    int16_t    *ch[1];
    uint8_t    *file;
    size_t      len;
    ps_adx_info info;
    uint32_t    i;

    for(i = 0; i < 2048; i++)
        sig[i] = (int16_t)(8000.0 * sin(i * 0.05));
    ch[0] = sig;

    file = encode_adx(ch, 2048, 1, 22050, 640, 1920, &len);
    if(!file) {
        check(0, "loop test setup");
        return;
    }

    check(ps_adx_parse(file, len, &info) == PS_ADX_OK, "parse with loop record");
    check(info.loop_start == 640 && info.loop_end == 1920,
          "loop points read from the header");

    /* Corrupt only the byte offset. The sample indices still look perfectly
     * plausible, so this is exactly the case a header read at the wrong
     * offset would produce - and it must be rejected rather than obeyed. */
    file[0x22] ^= 0x40;
    check(ps_adx_parse(file, len, &info) == PS_ADX_OK, "still parses");
    check(info.loop_end == 0, "inconsistent loop record is discarded");

    free(file);
    free(sig);
}

static void test_refusals(void)
{
    int16_t  *sig = (int16_t *)calloc(256, sizeof(int16_t));
    int16_t  *ch[1] = { NULL };
    uint8_t  *file;
    size_t    len;
    ps_adx_info info;

    ch[0] = sig;
    file = encode_adx(ch, 256, 1, 22050, 0, 0, &len);
    if(!file) {
        check(0, "refusal test setup");
        return;
    }

    check(ps_adx_parse(file, len, &info) == PS_ADX_OK, "baseline parses");

    { uint8_t save = file[0]; file[0] = 0x00;
      check(ps_adx_parse(file, len, &info) == PS_ADX_ENOTADX, "bad magic refused");
      file[0] = save; }

    { uint8_t save = file[ENC_DATA_OFF - 6]; file[ENC_DATA_OFF - 6] = 'X';
      check(ps_adx_parse(file, len, &info) == PS_ADX_ENOTADX,
            "missing (c)CRI marker refused");
      file[ENC_DATA_OFF - 6] = save; }

    { uint8_t save = file[4]; file[4] = 4;
      check(ps_adx_parse(file, len, &info) == PS_ADX_EENCODING,
            "exponential-scale encoding refused");
      file[4] = 0x11;
      check(ps_adx_parse(file, len, &info) == PS_ADX_EENCODING, "AHX refused");
      file[4] = save; }

    { uint8_t save = file[7]; file[7] = 6;
      check(ps_adx_parse(file, len, &info) == PS_ADX_ECHANNELS,
            "6-channel file refused");
      file[7] = save; }

    { uint8_t save = file[0x13]; file[0x13] = 0x08;
      check(ps_adx_parse(file, len, &info) == PS_ADX_EENCRYPTED,
            "encrypted file refused");
      file[0x13] = save; }

    { uint8_t a = file[0x0a], b = file[0x0b];
      file[0x0a] = 0xff; file[0x0b] = 0xff;   /* 65535 Hz */
      check(ps_adx_parse(file, len, &info) == PS_ADX_EHEADER,
            "absurd sample rate refused");
      file[0x0a] = a; file[0x0b] = b; }

    check(ps_adx_parse(file, 8, &info) == PS_ADX_ENOTADX,
          "a file shorter than its header is refused");

    /* Truncation is not refusal: a download cut short should still play what
     * arrived, with the sample count clamped to it. */
    free(file);
    file = encode_adx(ch, 256, 1, 22050, 0, 0, &len);
    check(ps_adx_parse(file, len - ENC_BLOCK * 3, &info) == PS_ADX_OK,
          "truncated file still parses");
    check(info.frames == 256 - 32 * 3, "sample count clamped to what arrived");

    free(file);
    free(sig);
}

/* ------------------------------------------------------------- stream ---- */

/* Plays a file through ps_adxstream against the stub backend, recording what a
 * speaker attached to the ring would have heard.
 *
 * The stream is driven exactly as the browser drives it - tick, then let the
 * cursor advance by a frame's worth - and the cursor is deliberately stepped
 * by an amount that is not a divisor of the chunk size, so boundaries are
 * crossed at every offset within a chunk rather than at the same one each
 * lap. Returns the number of frames heard.
 */
static uint32_t play_and_listen(void *file, size_t len, int loop, uint32_t rate,
                                uint32_t channels, int16_t *const *heard,
                                uint32_t cap)
{
    ps_adxstream  *st;
    const int16_t *ring[PS_ADX_MAX_CHANNELS];
    size_t         ring_bytes = 0;
    uint32_t       ring_frames, cursor = 0, n = 0, ch;
    const int      dt = 33;

    fake_voice_reset();

    st = ps_adx_stream_create();
    if(!st || ps_adx_stream_play(st, file, len, loop) != 0) {
        ps_adx_stream_destroy(st);
        return 0;
    }

    /* One ring per channel, in the order the stream allocated them. */
    for(ch = 0; ch < channels; ch++) {
        ring[ch] = (const int16_t *)fake_voice_ring((ps_smp)(ch + 1),
                                                    &ring_bytes);
        if(!ring[ch]) {
            ps_adx_stream_destroy(st);
            return 0;
        }
    }
    ring_frames = (uint32_t)(ring_bytes / 2);

    while(n < cap) {
        uint32_t adv = rate * (uint32_t)dt / 1000, i;

        fake_pos = (int)(cursor % ring_frames);
        ps_adx_stream_tick(st, dt);

        /* The stream releases the ring when it stops, so nothing may be read
         * out of it after a tick that ended playback. */
        if(!ps_adx_stream_is_playing(st))
            break;

        if(adv > cap - n)
            adv = cap - n;

        for(ch = 0; ch < channels; ch++)
            for(i = 0; i < adv; i++)
                heard[ch][n + i] = ring[ch][(cursor + i) % ring_frames];

        n      += adv;
        cursor += adv;
    }

    ps_adx_stream_destroy(st);
    return n;
}

/* The same audio the stream should have produced, decoded straight. */
static uint32_t reference(const void *file, size_t len, int loop,
                          int16_t *const *ref, uint32_t cap)
{
    ps_adx_info info;
    ps_adx_dec  dec;
    uint32_t    n = 0;
    int         dry = 0;

    if(ps_adx_parse(file, len, &info) != PS_ADX_OK)
        return 0;

    if(loop && info.loop_end)
        info.frames = info.loop_end;

    /* The same block-boundary rule the stream applies when it loops, stated
     * here rather than borrowed, so the two have to agree on purpose. */
    if(loop)
        info.frames -= info.frames % info.block_frames;

    ps_adx_dec_init(&dec, &info, file, len);

    while(n < cap) {
        int16_t *p[PS_ADX_MAX_CHANNELS];
        uint32_t got, ch;

        for(ch = 0; ch < info.channels; ch++)
            p[ch] = ref[ch] + n;
        got = ps_adx_decode(&dec, p, cap - n);
        if(got) {
            n  += got;
            dry = 0;
            continue;
        }
        if(!loop || dry)
            break;
        ps_adx_seek(&dec, loop ? (info.loop_end ? info.loop_start : 0) : 0);
        dry = 1;
    }
    return n;
}

static void test_stream(int loop, uint32_t frames, const char *label)
{
    enum { RATE = 22050, CAP = 260000 };
    int16_t  *sig  = (int16_t *)calloc(frames, sizeof(int16_t));
    int16_t  *heard = (int16_t *)calloc(CAP, sizeof(int16_t));
    int16_t  *ref   = (int16_t *)calloc(CAP, sizeof(int16_t));
    int16_t  *ch[1];
    uint8_t  *file, *copy;
    size_t    len;
    uint32_t  i, heard_n, ref_n, bad = 0;
    char      msg[128];

    for(i = 0; i < frames; i++)
        sig[i] = (int16_t)(11000.0 * sin(i * 0.017) + 2000.0 * sin(i * 0.21));
    ch[0] = sig;

    file = encode_adx(ch, frames, 1, RATE, 0, 0, &len);
    if(!file) {
        check(0, "stream test setup");
        return;
    }
    copy = (uint8_t *)malloc(len);
    memcpy(copy, file, len);

    { int16_t *rp[1] = { ref }, *hp[1] = { heard };

      ref_n   = reference(file, len, loop, rp, CAP);
      heard_n = play_and_listen(copy, len, loop, RATE, 1, hp, CAP); }

    snprintf(msg, sizeof msg, "%s: heard %u frames", label, (unsigned)heard_n);
    check(heard_n > frames / 2, msg);

    if(ref_n > heard_n)
        ref_n = heard_n;
    for(i = 0; i < ref_n; i++)
        if(heard[i] != ref[i])
            bad++;

    snprintf(msg, sizeof msg, "%s: ring content matches the decode (%u bad)",
             label, (unsigned)bad);
    check(bad == 0, msg);

    snprintf(msg, sizeof msg, "%s: every transfer was 32-byte aligned", label);
    check(fake_bad_write == 0, msg);

    /* A one-shot must end by itself; a looping track must not. */
    snprintf(msg, sizeof msg, "%s: %s", label,
             loop ? "still playing after four ring laps" : "stops at the end");
    check(loop ? (heard_n == CAP) : (heard_n < CAP), msg);

    free(file);
    free(sig);
    free(heard);
    free(ref);
}

/* A jingle shorter than one refill chunk. The refill has to rewind several
 * times to fill a single chunk, which is the case a naive "one rewind per
 * chunk" guard turns into a track that stops after a second. */
static void test_stream_short_loop(void)
{
    enum { RATE = 22050, FRAMES = 1024, CAP = 120000 };
    int16_t  *sig   = (int16_t *)calloc(FRAMES, sizeof(int16_t));
    int16_t  *heard = (int16_t *)calloc(CAP, sizeof(int16_t));
    int16_t  *ch[1];
    uint8_t  *file;
    size_t    len;
    uint32_t  i, n, bad = 0;

    for(i = 0; i < FRAMES; i++)
        sig[i] = (int16_t)(10000.0 * sin(i * 0.04));
    ch[0] = sig;

    file = encode_adx(ch, FRAMES, 1, RATE, 0, 0, &len);
    if(!file) {
        check(0, "short loop setup");
        return;
    }

    { int16_t *hp[1] = { heard };
      n = play_and_listen(file, len, 1, RATE, 1, hp, CAP); }
    check(n == CAP, "a 1024-frame loop keeps playing");

    /* Whatever it plays must repeat with the file's period. */
    for(i = FRAMES; i < n && i < 40000; i++)
        if(heard[i] != heard[i - FRAMES])
            bad++;
    check(bad == 0, "the short loop repeats exactly");

    free(sig);
    free(heard);
}

/* Two rings, one cursor. A stream that wrote both channels from the same half
 * of the staging buffer, or swapped them, sounds like a mono file on hardware
 * and like nothing at all here unless the channels are checked apart. */
static void test_stream_stereo(void)
{
    enum { RATE = 22050, FRAMES = 60000, CAP = 100000 };
    int16_t  *src[2], *heard[2], *ref[2];
    int16_t  *file_ch[2];
    uint8_t  *file, *copy;
    size_t    len;
    uint32_t  ch, i, heard_n, ref_n, bad = 0;

    for(ch = 0; ch < 2; ch++) {
        src[ch]   = (int16_t *)calloc(FRAMES, sizeof(int16_t));
        heard[ch] = (int16_t *)calloc(CAP, sizeof(int16_t));
        ref[ch]   = (int16_t *)calloc(CAP, sizeof(int16_t));
        for(i = 0; i < FRAMES; i++)
            src[ch][i] = (int16_t)(10000.0 * sin(i * (ch ? 0.031 : 0.011)));
        file_ch[ch] = src[ch];
    }

    file = encode_adx(file_ch, FRAMES, 2, RATE, 0, 0, &len);
    if(!file) {
        check(0, "stereo stream setup");
        return;
    }
    copy = (uint8_t *)malloc(len);
    memcpy(copy, file, len);

    ref_n   = reference(file, len, 0, ref, CAP);
    heard_n = play_and_listen(copy, len, 0, RATE, 2, heard, CAP);

    check(heard_n > FRAMES, "stereo: plays past the end of the file");

    if(ref_n > heard_n)
        ref_n = heard_n;
    for(ch = 0; ch < 2; ch++)
        for(i = 0; i < ref_n; i++)
            if(heard[ch][i] != ref[ch][i])
                bad++;
    check(bad == 0, "stereo: both rings match their own channel");
    check(fake_bad_write == 0, "stereo: every transfer was 32-byte aligned");

    free(file);
    for(ch = 0; ch < 2; ch++) {
        free(src[ch]);
        free(heard[ch]);
        free(ref[ch]);
    }
}

/* Writes a stereo test track, for playing on a real console.
 *
 * The decoder is verified here on the host, but nothing about the AICA side is
 * - and the streaming path reads the hardware's play cursor over G2, which
 * this project has never done. That has to be heard, and the file it is heard
 * with should make a fault obvious rather than subtle:
 *
 *   - the two channels carry different pitches, so a crossed or collapsed
 *     stereo decode is audible immediately rather than looking like a mix
 *   - the pitch steps up in stages and then restarts, so a stall, a skipped
 *     refill or a ring lap lands on an audibly wrong step
 *   - it runs long enough to lap the ring several times, which is where the
 *     write cursor overtaking the read cursor would show up
 *
 * A steady sine would sound identical whether the stream advanced correctly or
 * repeated the same second forever, which is precisely the failure most
 * expected here. */
static int write_test_track(const char *path)
{
    enum { RATE = 22050, SECONDS = 20, FRAMES = RATE * SECONDS };
    int16_t *ch[2];
    uint8_t *file;
    size_t   len;
    FILE    *f;
    int      i, c;

    for(c = 0; c < 2; c++) {
        ch[c] = calloc(FRAMES, sizeof **ch);
        if(!ch[c])
            return -1;
    }

    for(i = 0; i < FRAMES; i++) {
        /* Eight steps of a rising scale, one per half second, repeating. */
        int    step = (i / (RATE / 2)) % 8;
        double base = 220.0 * (1.0 + step * 0.125);
        double t    = (double)i / RATE;

        /* Right an octave up, so the channels are never confusable. */
        ch[0][i] = (int16_t)(9000.0 * sin(2.0 * 3.14159265 * base * t));
        ch[1][i] = (int16_t)(9000.0 * sin(2.0 * 3.14159265 * base * 2.0 * t));
    }

    file = encode_adx(ch, FRAMES, 2, RATE, 0, 0, &len);
    free(ch[0]);
    free(ch[1]);
    if(!file)
        return -1;

    f = fopen(path, "wb");
    if(!f) {
        free(file);
        return -1;
    }
    fwrite(file, 1, len, f);
    fclose(f);
    printf("wrote %s: %zu bytes, %d s stereo at %d Hz\n",
           path, len, SECONDS, RATE);
    free(file);
    return 0;
}

int main(int argc, char **argv)
{
    if(argc == 3 && !strcmp(argv[1], "--write"))
        return write_test_track(argv[2]) == 0 ? 0 : 1;

    printf("ps_adx host tests\n\n");

    test_coefficients();
    test_hand_decoded_block();
    test_roundtrip(1, 22050, 8192, "mono 22050");
    test_roundtrip(2, 44100, 8192, "stereo 44100");
    test_chunked_matches_whole();
    test_loop_record();
    test_refusals();

    printf("\nstreaming over a simulated ring\n\n");
    test_stream(0, 120000, "one-shot");
    test_stream(1, 90000, "looping");
    test_stream_short_loop();
    test_stream_stereo();

    printf("\n%s\n", g_fail ? "FAILED" : "all passed");
    return g_fail ? 1 : 0;
}
