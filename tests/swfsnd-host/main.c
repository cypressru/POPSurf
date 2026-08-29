/* Host test for swf/ps_swf_track.c: the ring arithmetic, the timeline pacing
 * and the event-sound voice policy, against a simulated play cursor.
 *
 * Why this is a separate program from tests/swf-host/sndtest, which already
 * covers the decoder: what is under test here is not whether a sample decodes
 * but whether the right sample reaches the right byte of a ring buffer at the
 * right moment relative to a play cursor the software does not control. That
 * is arithmetic, the AICA contributes nothing to it but the cursor, and on
 * real hardware every way of getting it wrong sounds like a periodic click
 * that could equally be the decoder, the pitch or the transfer. tests/adx-host
 * exists for exactly this reason and caught exactly this class of bug; this is
 * the same idea pointed at the other streamer.
 *
 * The fixture is what makes the assertions arithmetic rather than recorded.
 * Every stream sample carries its own index as its value, so the question "is
 * the ring correct" has a closed-form answer at every byte:
 *
 *     ring16[pos] == (int16_t)(chunk_first[pos / 4096] + pos % 4096)
 *
 * and that expected value is checked against the track's own idea of where the
 * cursor is, so the two have to agree with each other AND with the arithmetic.
 * A round trip through the same code cannot pass that: an off-by-one in the
 * chunk map moves the ring content and the reported position by different
 * amounts.
 *
 *   ./swfsndtest              the whole suite
 *   ./swfsndtest some.swf     what one real file would cost on the console
 *   ./swfsndcap               the same source with the retention cap turned
 *                             down, which is the one thing that cannot be
 *                             tested at the real ceiling without building a
 *                             sixteen megabyte movie
 */
#include "ps_swf_track.h"
#include "ps_swf_sound.h"
#include "ps_swf.h"
#include "ps_audio.h"
#include "ps_config.h"
#include "ps_voice.h"

#include "fakevoice.h"
#include "swfbuild.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Must match ps_swf_track.c. Restated rather than exported because the test
 * asserting the geometry it was told is no test at all - if these ever
 * disagree with the implementation, the ring checks below fail loudly, which
 * is the intended behaviour. */
#define RING_FRAMES  32768u
#define RING_CHUNKS  8u
#define CHUNK_FRAMES (RING_FRAMES / RING_CHUNKS)

/* Where ps_swf_track_create puts its voices, worked out the same way it does:
 * off the top of the pool, below the two the ADX streamer holds. */
#define BASE_SLOT (PS_CFG_AUDIO_VOICES - PS_AUDIO_STREAM_SLOTS - \
                   PS_AUDIO_SWF_SLOTS)
#define RING_SLOT (BASE_SLOT)
#define EV_SLOT0  (BASE_SLOT + 2)
#define EV_SLOT1  (BASE_SLOT + 3)

static int fails;

static void chk(const char *what, long got, long want)
{
    if(got == want) {
        printf("  %-58s ok\n", what);
    }
    else {
        printf("  %-58s FAIL got %ld want %ld\n", what, got, want);
        fails++;
    }
}

static void chk_true(const char *what, int cond)
{
    if(cond) {
        printf("  %-58s ok\n", what);
    }
    else {
        printf("  %-58s FAIL\n", what);
        fails++;
    }
}

static void chk_range(const char *what, long got, long lo, long hi)
{
    if(got >= lo && got <= hi) {
        printf("  %-58s ok (%ld)\n", what, got);
    }
    else {
        printf("  %-58s FAIL %ld not in [%ld,%ld]\n", what, got, lo, hi);
        fails++;
    }
}

/* --- the tags this file needs, over swfbuild ----------------------------- */

/* Thin wrappers rather than direct calls, because every fixture here is
 * uncompressed little-endian PCM on the same 320x240 twelve-frame stage: the
 * codec and the stage are constants of this suite, and repeating them at two
 * hundred call sites would only make it easier for one of them to disagree. */
static void put_define_sound(bw *tags, int id, int rate_code, int bits16,
                             int stereo, uint32_t nsample, const uint8_t *d,
                             size_t n)
{
    swf_define_sound(tags, id, PS_SWF_SND_PCM_LE, rate_code, bits16, stereo,
                     nsample, d, n);
}

static void put_stream_head(bw *tags, int rate_code, int stereo, int spf)
{
    swf_stream_head(tags, PS_SWF_SND_PCM_LE, rate_code, stereo, spf);
}

static uint8_t *make_swf(const bw *tags, size_t *out_len)
{
    return swf_finish(tags, 6400, 4800, 12, 1, out_len);
}

/* --- the ramp fixture ---------------------------------------------------- */

/* Sample n of the stream is n. Sixteen-bit little-endian, mono, so a byte pair
 * at ring offset 2n either says n or says where the arithmetic went wrong. */
#define SND_RATE      22050u
#define SND_RATE_CODE 2          /* 0=5512, 1=11025, 2=22050, 3=44100 */
#define SND_SPF       1837u      /* 22050/12, which is 1837.5 and so not exact */
#define SND_FRAMES    140u

static uint32_t ramp_block_len(uint32_t frame)
{
    /* Alternating, so `first` is genuinely a running total of what the blocks
     * decode to and not frame * spf. A player that accumulated the head's
     * average instead would be half a sample per frame out here, which is the
     * whole reason ps_swf_stream_frame_sample exists. */
    return SND_SPF + (frame & 1u);
}

static uint32_t ramp_first(uint32_t frame)
{
    uint32_t i, at = 0;

    for(i = 0; i < frame; i++)
        at += ramp_block_len(i);
    return at;
}

static uint32_t ramp_total(void) { return ramp_first(SND_FRAMES); }

static uint8_t *build_stream_movie(size_t *len)
{
    bw       tags;
    uint8_t *blk;
    uint32_t f, i;

    bw_init(&tags, 1 << 20);
    put_stream_head(&tags, SND_RATE_CODE, 0, (int)SND_SPF);

    blk = (uint8_t *)malloc((SND_SPF + 1u) * 2u);
    for(f = 0; f < SND_FRAMES; f++) {
        uint32_t n     = ramp_block_len(f);
        uint32_t first = ramp_first(f);

        for(i = 0; i < n; i++) {
            uint16_t v = (uint16_t)(first + i);

            blk[i * 2]     = (uint8_t)(v & 0xff);
            blk[i * 2 + 1] = (uint8_t)(v >> 8);
        }
        swf_stream_block(&tags, blk, n * 2u);
        swf_showframe(&tags);
    }
    free(blk);
    swf_end(&tags);

    {
        uint8_t *img = make_swf(&tags, len);

        bw_free(&tags);
        return img;
    }
}

/* --- the simulated console ----------------------------------------------- */

typedef struct {
    ps_swf_track *t;
    uint32_t      rate;
    int           us_per_frame;
    int           acc_us;
    uint32_t      frame, nframe;
    uint64_t      played;      /* absolute frames the AICA has consumed */
    int           bad_ring;    /* ring bytes that did not match the arithmetic */
    int           keys;        /* re-keys, which are ring reprimes */
    long          worst_drift;
} sim;

static const int16_t *ring16(void)
{
    size_t len;
    const uint8_t *p = fake_voice_ring(fake_voice[RING_SLOT].smp, &len);

    return (const int16_t *)(const void *)p;
}

/* The one assertion the whole file is built around, applied at the byte the
 * hardware is reading this instant.
 *
 * Two independent things have to line up: the track's own report of where the
 * cursor is in the source's sample space, and the sample actually sitting in
 * the ring at that offset. Checking only one of them would pass with a
 * consistently shifted chunk map. */
static void sim_probe(sim *s, uint32_t total)
{
    ps_swf_track_stats st;
    const int16_t     *r = ring16();
    uint32_t           expect;
    int16_t            want;

    if(!r)
        return;

    ps_swf_track_get_stats(s->t, &st);
    expect = st.cursor_sample;
    want   = expect < total ? (int16_t)(uint16_t)expect : 0;

    if(r[fake_pos] != want)
        s->bad_ring++;

    /* And the report itself has to be the arithmetic, not just self-consistent
     * with the ring: the sample under the cursor is where the timeline says
     * the movie has reached, within the slack the resync rule allows. */
    if(st.drift > s->worst_drift)
        s->worst_drift = st.drift;
    if(-(long)st.drift > s->worst_drift)
        s->worst_drift = -(long)st.drift;
}

static void sim_step(sim *s, int dt_ms, int run_tick)
{
    int keyed_before = fake_keyed;

    s->played += (uint64_t)dt_ms * s->rate / 1000u;
    fake_pos   = (int)(s->played % RING_FRAMES);

    s->acc_us += dt_ms * 1000;
    while(s->acc_us >= s->us_per_frame) {
        s->acc_us -= s->us_per_frame;
        s->frame++;
    }
    s->frame %= s->nframe;

    if(run_tick)
        ps_swf_track_tick(s->t, dt_ms, s->frame);

    /* A key event restarts the sample from its start address, so the hardware
     * cursor goes back to zero. The stub backend does not model that - it only
     * counts keys - so the simulation has to, or a reprimed ring would be read
     * from wherever the old cursor happened to be. */
    if(fake_keyed != keyed_before) {
        s->played = 0;
        fake_pos  = 0;
        s->keys++;
    }
}

/* --- test 1: what the ring holds before anything has played -------------- */

[[maybe_unused]] static void test_prime(void)
{
    uint8_t      *img;
    size_t        len;
    ps_swf_track *t;
    char          err[128];
    const int16_t *r;
    uint32_t      i;
    int           bad = 0;

    printf("--- priming\n");
    fake_voice_reset();

    img = build_stream_movie(&len);
    t   = ps_swf_track_create();
    chk("the file walks", ps_swf_track_load(t, img, len, err, sizeof err), 0);
    free(img);

    chk_true("the movie is its own soundtrack",
             ps_swf_track_has_soundtrack(t));
    chk_true("and it is sounding", ps_swf_track_is_playing(t));

    chk("the ring voice loops", fake_voice[RING_SLOT].loop, 1);
    chk("over the whole ring", (long)fake_voice[RING_SLOT].frames,
        (long)RING_FRAMES);
    chk("at the stream's own rate", (long)fake_voice[RING_SLOT].freq,
        (long)SND_RATE);
    chk("as PCM16", fake_voice[RING_SLOT].fmt, PS_FMT_PCM16);
    chk("keyed once, not twice", fake_keyed, 1);
    chk("no transfer the AICA would have dropped", fake_bad_write, 0);

    r = ring16();
    chk_true("the ring exists", r != NULL);
    if(r) {
        for(i = 0; i < RING_FRAMES; i++)
            if(r[i] != (int16_t)(uint16_t)i)
                bad++;
        chk("every one of 32768 frames is the sample it should be", bad, 0);

        /* Two values worked out on paper rather than derived from the loop
         * above, because a loop that agrees with itself proves less than one
         * number does. Chunk 3 starts at 3 * 4096 = 12288 = 0x3000, and the
         * last frame of the ring is 32767 = 0x7fff. */
        chk("chunk 3 begins at sample 0x3000", r[3 * CHUNK_FRAMES], 0x3000);
        chk("the last frame of the ring is 0x7fff", r[RING_FRAMES - 1], 0x7fff);
    }

    ps_swf_track_destroy(t);
}

/* --- test 1b: the same, through the codec real content uses -------------- */

/* Flash ADPCM, one packet per block, of a tone the ring can be checked
 * against.
 *
 * The ramp fixture cannot be used here and it is worth saying why: a sawtooth
 * that wraps from 32767 to -32768 every two seconds is a step a four-bit
 * differential codec cannot follow at all, so the error would be enormous and
 * would say nothing about whether the blocks landed in the right place. A tone
 * is what this codec was designed for, and a tone that is wrong is obvious.
 *
 * 1837 samples a block, which is odd on purpose: a block is 2 bits of code
 * width, 22 of packet header and four per delta, so it ends on a byte boundary
 * only for an odd count. An even one leaves four spare bits a reader has no
 * way to know are padding, and decodes them as one more sample. */
#define ADP_BLOCKS 40u
#define ADP_SPF    1837u

static int16_t adp_sample(uint32_t i)
{
    /* 440Hz at 22050, in integer phase so the expectation needs no float. */
    static const double k = 2.0 * 3.14159265358979 * 440.0 / 22050.0;

    return (int16_t)(12000.0 * __builtin_sin(k * (double)i));
}

static uint8_t *build_adpcm_movie(size_t *len)
{
    bw       tags;
    int16_t *src;
    uint32_t f, i;

    src = (int16_t *)malloc((size_t)ADP_SPF * sizeof *src);

    bw_init(&tags, 1 << 18);
    swf_stream_head(&tags, PS_SWF_SND_ADPCM, SND_RATE_CODE, 0, (int)ADP_SPF);

    for(f = 0; f < ADP_BLOCKS; f++) {
        bw blk;

        for(i = 0; i < ADP_SPF; i++)
            src[i] = adp_sample(f * ADP_SPF + i);

        bw_init(&blk, ADP_SPF);
        swf_adpcm_stream(&blk, src, ADP_SPF, 4);
        swf_stream_block(&tags, blk.b, blk.n);
        bw_free(&blk);

        swf_showframe(&tags);
    }
    free(src);
    swf_end(&tags);

    {
        uint8_t *img = make_swf(&tags, len);

        bw_free(&tags);
        return img;
    }
}

[[maybe_unused]] static void test_adpcm(void)
{
    uint8_t       *img;
    size_t         len;
    ps_swf_track  *t;
    char           err[128];
    const int16_t *r;
    uint32_t       i;
    long           worst = 0, sum = 0;
    long           worst_seam = 0;

    printf("--- Flash ADPCM, block by block, into the ring\n");
    fake_voice_reset();

    img = build_adpcm_movie(&len);
    t   = ps_swf_track_create();
    chk("the file walks", ps_swf_track_load(t, img, len, err, sizeof err), 0);
    free(img);

    r = ring16();
    chk_true("the ring exists", r != NULL);
    if(!r) {
        ps_swf_track_destroy(t);
        return;
    }

    for(i = 0; i < RING_FRAMES; i++) {
        long d = (long)r[i] - adp_sample(i);

        if(d < 0)
            d = -d;
        if(d > worst)
            worst = d;
        sum += d;
    }

    /* A four-bit differential codec on a smooth tone tracks it closely; what a
     * misplaced block looks like is an error of the tone's own amplitude,
     * which is twelve thousand. Two hundred says the blocks are where they
     * should be and the predictor is carried correctly inside each one. */
    chk_range("worst error over the whole ring", worst, 0, 400);

    /* Four bits buy about 24dB of range below whatever step the adaptation has
     * settled on, so a mean error under one percent of the tone's own
     * amplitude is what this codec does when it is working. It is not a tight
     * bound and is not meant to be: what it is here to catch is a block placed
     * in the wrong part of the ring, which puts the error at the amplitude
     * itself. */
    chk_range("mean error", sum / (long)RING_FRAMES, 0, 12000 / 100);

    /* And the seams. A block boundary decoded with the previous block's
     * predictor - or a block placed one sample out - puts a step here, and a
     * step at 12 a second is a buzz rather than a wrong note. Neighbouring
     * samples of a 440Hz tone at 22050Hz differ by at most 12000 * 2*pi*440 /
     * 22050, which is 1504. */
    for(i = 1; i < ADP_BLOCKS && i * ADP_SPF < RING_FRAMES; i++) {
        long d = (long)r[i * ADP_SPF] - r[i * ADP_SPF - 1];

        if(d < 0)
            d = -d;
        if(d > worst_seam)
            worst_seam = d;
    }
    chk_range("worst step across a block boundary", worst_seam, 0, 1800);

    ps_swf_track_destroy(t);
}

/* --- test 2: steady state, where nothing should happen ------------------- */

[[maybe_unused]] static void test_steady(void)
{
    uint8_t      *img;
    size_t        len;
    char          err[128];
    sim           s;
    ps_swf_track_stats st;
    int           i;

    printf("--- steady state\n");
    fake_voice_reset();
    memset(&s, 0, sizeof s);

    img = build_stream_movie(&len);
    s.t = ps_swf_track_create();
    chk("the file walks", ps_swf_track_load(s.t, img, len, err, sizeof err), 0);
    free(img);

    s.rate         = SND_RATE;
    s.us_per_frame = 1000000 / 12;
    s.nframe       = SND_FRAMES;

    /* Eight seconds at 60Hz, which is five laps of the ring. */
    for(i = 0; i < 480; i++) {
        sim_step(&s, 16, 1);
        sim_probe(&s, ramp_total());
    }

    ps_swf_track_get_stats(s.t, &st);
    chk("the ring always held the sample it reported", s.bad_ring, 0);
    chk("nothing needed resyncing", (long)st.resyncs, 0);
    chk("nothing ran dry", (long)st.underruns, 0);
    chk("no transfer the AICA would have dropped", fake_bad_write, 0);
    /* 480 ticks of 16ms is 7.68 seconds, which at 22050Hz is 169344 frames and
     * so 41 whole chunks of 4096, on top of the 8 the priming wrote. One
     * either side for where the cursor happened to stop. */
    chk_range("chunks written in eight seconds", (long)st.refills, 48, 50);

    /* A quarter of a second is the threshold; a tenth is what unavoidable
     * frame quantisation costs. Anything in between would be a slow leak. */
    chk_range("worst drift stayed inside a tenth of a second",
              s.worst_drift, 0, (long)SND_RATE / 10);

    ps_swf_track_destroy(s.t);
}

/* --- test 3: the loader held the frame ----------------------------------- */

[[maybe_unused]] static void test_stall(void)
{
    uint8_t      *img;
    size_t        len;
    char          err[128];
    sim           s;
    ps_swf_track_stats st;
    int           i;
    long          after;

    printf("--- a stall, and getting back in step\n");
    fake_voice_reset();
    memset(&s, 0, sizeof s);

    img = build_stream_movie(&len);
    s.t = ps_swf_track_create();
    chk("the file walks", ps_swf_track_load(s.t, img, len, err, sizeof err), 0);
    free(img);

    s.rate         = SND_RATE;
    s.us_per_frame = 1000000 / 12;
    s.nframe       = SND_FRAMES;

    for(i = 0; i < 120; i++) {
        sim_step(&s, 16, 1);
        sim_probe(&s, ramp_total());
    }
    ps_swf_track_get_stats(s.t, &st);
    chk("settled with no resyncs", (long)st.resyncs, 0);

    /* A second and a half with the frame held: the cursor keeps moving and the
     * timeline keeps moving, and nothing refills. This is a page fetching an
     * image, and it is the case the resync exists for. */
    sim_step(&s, 1500, 0);

    for(i = 0; i < 120; i++) {
        sim_step(&s, 16, 1);
        sim_probe(&s, ramp_total());
    }

    ps_swf_track_get_stats(s.t, &st);
    chk_true("the stall forced a resync", st.resyncs >= 1);
    after = st.drift < 0 ? -(long)st.drift : (long)st.drift;
    chk_range("and the stream came back in step", after, 0,
              (long)SND_RATE / 4);
    chk("the ring still held what it reported", s.bad_ring, 0);
    chk("no transfer the AICA would have dropped", fake_bad_write, 0);
    chk_true("and it is still playing", ps_swf_track_is_playing(s.t));

    ps_swf_track_destroy(s.t);
}

/* --- test 4: the movie wraps --------------------------------------------- */

[[maybe_unused]] static void test_wrap(void)
{
    uint8_t      *img;
    size_t        len;
    char          err[128];
    sim           s;
    ps_swf_track_stats st;
    int           i;
    uint32_t      seen_high = 0;
    uint32_t      lowest_after = 0xffffffffu;
    int           wrapped = 0;

    printf("--- the movie loops, and so must its soundtrack\n");
    fake_voice_reset();
    memset(&s, 0, sizeof s);

    img = build_stream_movie(&len);
    s.t = ps_swf_track_create();
    chk("the file walks", ps_swf_track_load(s.t, img, len, err, sizeof err), 0);
    free(img);

    s.rate         = SND_RATE;
    s.us_per_frame = 1000000 / 12;
    s.nframe       = SND_FRAMES;

    /* 140 frames at 12fps is eleven and two thirds seconds, so twenty is a
     * lap and a half of the timeline. */
    for(i = 0; i < 1250; i++) {
        uint32_t before = s.frame;

        sim_step(&s, 16, 1);
        sim_probe(&s, ramp_total());

        ps_swf_track_get_stats(s.t, &st);
        if(!wrapped && st.cursor_sample > ramp_total() / 2u)
            seen_high = st.cursor_sample;
        if(s.frame < before)
            wrapped = 1;
        if(wrapped && st.cursor_sample < lowest_after)
            lowest_after = st.cursor_sample;
    }

    chk_true("the timeline wrapped", wrapped);
    chk_true("the soundtrack had reached the far end", seen_high > 0);
    chk_true("without the ring ever being stopped",
             ps_swf_track_is_playing(s.t));
    chk("the ring always held what it reported", s.bad_ring, 0);
    chk("no transfer the AICA would have dropped", fake_bad_write, 0);

    /* The whole point of throwing the queue away on a backward jump: the new
     * pass has to start at the top of the track, not most of a second into it.
     * A ring left to play out its lookahead rejoins at RING_FRAMES minus the
     * cursor offset - well over twenty thousand samples - and that is what
     * this number would be if the reprime were removed. */
    chk_range("the new pass began at the top of the track",
              (long)lowest_after, 0, 2048);
    chk_true("which took a re-key", s.keys >= 1);

    /* And the loop-seam bug this design is a response to: a track that stops
     * after one pass because a short read at the seam read as end-of-track. If
     * the ring had been let stop, is_playing above would be false. */
    ps_swf_track_get_stats(s.t, &st);
    chk_true("the wrap cost at least one resync", st.resyncs >= 1);

    ps_swf_track_destroy(s.t);
}

/* --- test 5: a resync lands on the sample it was asked for --------------- */

[[maybe_unused]] static void test_resync_exact(void)
{
    uint8_t      *img;
    size_t        len;
    char          err[128];
    sim           s;
    ps_swf_track_stats st;
    const int16_t *r;
    uint32_t      chunk, at;
    int           i;

    printf("--- a resync is exact, not approximate\n");
    fake_voice_reset();
    memset(&s, 0, sizeof s);

    img = build_stream_movie(&len);
    s.t = ps_swf_track_create();
    chk("the file walks", ps_swf_track_load(s.t, img, len, err, sizeof err), 0);
    free(img);

    s.rate         = SND_RATE;
    s.us_per_frame = 1000000 / 12;
    s.nframe       = SND_FRAMES;

    /* Move the cursor into chunk 1 so chunk 0 is the one due for a refill, and
     * jump the timeline to frame 60 in the same breath. Frame 60's first
     * sample is a running total the fixture can state exactly, and the chunk
     * about to be written is 7 chunks minus the cursor's offset ahead of it. */
    s.played = CHUNK_FRAMES + 100u;
    fake_pos = (int)s.played;
    s.frame  = 60;
    ps_swf_track_tick(s.t, 16, s.frame);

    ps_swf_track_get_stats(s.t, &st);
    chk_true("the jump forced a resync", st.resyncs >= 1);

    chunk = 0;
    at    = ramp_first(60) + (RING_FRAMES - (uint32_t)fake_pos);
    r     = ring16();
    chk_true("the ring exists", r != NULL);
    if(r) {
        chk("chunk 0 now begins where the timeline plus the queue says",
            r[chunk * CHUNK_FRAMES], (int16_t)(uint16_t)at);
        chk("and runs on from there",
            r[chunk * CHUNK_FRAMES + 999], (int16_t)(uint16_t)(at + 999u));
    }

    /* And having jumped, it must settle rather than oscillate. */
    for(i = 0; i < 240; i++) {
        sim_step(&s, 16, 1);
        sim_probe(&s, ramp_total());
    }
    ps_swf_track_get_stats(s.t, &st);
    chk("the ring held what it reported throughout", s.bad_ring, 0);
    chk_range("and it did not go on resyncing", (long)st.resyncs, 1, 3);

    ps_swf_track_destroy(s.t);
}

/* --- event sounds -------------------------------------------------------- */

/* A DefineSound whose sample n is n * mul, so an upload can be checked the
 * same way the ring is. */
static uint8_t *ramp_pcm(uint32_t n, int stereo, int mul, size_t *len)
{
    uint32_t ch = stereo ? 2u : 1u;
    uint8_t *d  = (uint8_t *)malloc((size_t)n * 2u * ch);
    uint32_t i, c;

    for(i = 0; i < n; i++) {
        for(c = 0; c < ch; c++) {
            uint16_t v = (uint16_t)((int)i * mul + (int)c * 1000);
            size_t   o = ((size_t)i * ch + c) * 2u;

            d[o]     = (uint8_t)(v & 0xff);
            d[o + 1] = (uint8_t)(v >> 8);
        }
    }
    *len = (size_t)n * 2u * ch;
    return d;
}

static ps_swf_track *load_tags(bw *tags, const char *what)
{
    ps_swf_track *t = ps_swf_track_create();
    uint8_t      *img;
    size_t        len;
    char          err[128];

    err[0] = '\0';
    img    = make_swf(tags, &len);
    if(ps_swf_track_load(t, img, len, err, sizeof err) < 0) {
        printf("  %-58s FAIL %s\n", what, err);
        fails++;
    }
    free(img);
    bw_free(tags);
    return t;
}

[[maybe_unused]] static void test_event_basic(void)
{
    bw            tags;
    uint8_t      *pcm;
    size_t        n;
    swf_startinfo         si;
    ps_swf_track *t;
    ps_swf_track_stats st;
    const int16_t *up;
    size_t        ulen;

    printf("--- an event sound reaches a voice\n");
    fake_voice_reset();

    /* 1000 frames at 22050Hz is 45ms, played three times. */
    pcm = ramp_pcm(1000, 0, 3, &n);
    bw_init(&tags, 4096);
    put_define_sound(&tags, 7, SND_RATE_CODE, 1, 0, 1000, pcm, n);
    swf_showframe(&tags);                       /* frame 0 */
    memset(&si, 0, sizeof si);
    si.loops = 3;
    swf_start_sound(&tags, 7, &si);
    swf_showframe(&tags);                       /* frame 1 */
    swf_showframe(&tags);
    swf_end(&tags);
    free(pcm);

    t = load_tags(&tags, "a movie with one event sound");
    chk_true("it has audio", ps_swf_track_has_audio(t));
    chk_true("but does not claim the page's music",
             !ps_swf_track_has_soundtrack(t));

    ps_swf_track_tick(t, 16, 0);
    ps_swf_track_get_stats(t, &st);
    chk("nothing fires on frame 0", (long)st.ev_started, 0);

    ps_swf_track_tick(t, 16, 1);
    ps_swf_track_get_stats(t, &st);
    chk("the cue on frame 1 fires", (long)st.ev_started, 1);
    chk("one sample went into sound memory", fake_uploads, 1);
    chk("on one voice", fake_voice[EV_SLOT0].live, 1);
    chk("the second is untouched", fake_voice[EV_SLOT1].live, 0);
    chk("the voice spans one pass", (long)fake_voice[EV_SLOT0].frames, 1000);
    chk("at the sound's own rate", (long)fake_voice[EV_SLOT0].freq,
        (long)SND_RATE);
    chk("and loops, because it plays three times",
        fake_voice[EV_SLOT0].loop, 1);
    chk("centred, because it is mono", fake_voice[EV_SLOT0].pan, 128);

    up = (const int16_t *)(const void *)
             fake_voice_ring(fake_voice[EV_SLOT0].smp, &ulen);
    chk_true("the sample is in sound memory", up != NULL);
    if(up) {
        chk("1000 frames of it", (long)ulen, 2000);
        chk("first frame", up[0], 0);
        chk("hundredth frame", up[100], 300);
        chk("last frame", up[999], 2997);
    }

    /* Three passes of 1000 frames at 22050Hz is 136ms. At 100ms it is still
     * sounding; at 200ms it must be gone, and the voice with it. */
    ps_swf_track_tick(t, 100, 1);
    chk("still sounding at 100ms", fake_voice[EV_SLOT0].live, 1);
    ps_swf_track_tick(t, 100, 1);
    chk("finished by 200ms", fake_voice[EV_SLOT0].live, 0);
    chk_true("and the track knows it is idle", !ps_swf_track_is_playing(t));

    ps_swf_track_destroy(t);
}

[[maybe_unused]] static void test_event_policy(void)
{
    bw            tags;
    uint8_t      *pcm;
    size_t        n;
    swf_startinfo         si;
    ps_swf_track *t;
    ps_swf_track_stats st;

    printf("--- in and out points, stereo, and what happens when it runs out\n");
    fake_voice_reset();

    pcm = ramp_pcm(4000, 0, 1, &n);
    bw_init(&tags, 65536);
    put_define_sound(&tags, 1, SND_RATE_CODE, 1, 0, 4000, pcm, n);
    free(pcm);
    pcm = ramp_pcm(4000, 1, 1, &n);
    put_define_sound(&tags, 2, SND_RATE_CODE, 1, 1, 4000, pcm, n);
    free(pcm);
    pcm = ramp_pcm(4000, 0, 1, &n);
    put_define_sound(&tags, 3, SND_RATE_CODE, 1, 0, 4000, pcm, n);
    free(pcm);
    swf_showframe(&tags);                       /* frame 0 */

    memset(&si, 0, sizeof si);
    si.has_in  = 1;
    si.in      = 500;
    si.has_out = 1;
    si.out     = 1500;
    swf_start_sound(&tags, 1, &si);
    swf_showframe(&tags);                       /* frame 1 */

    memset(&si, 0, sizeof si);
    swf_start_sound(&tags, 2, &si);             /* stereo: takes both slots */
    swf_showframe(&tags);                       /* frame 2 */

    memset(&si, 0, sizeof si);
    swf_start_sound(&tags, 3, &si);
    swf_showframe(&tags);                       /* frame 3 */
    swf_end(&tags);

    t = load_tags(&tags, "three cues on three frames");

    ps_swf_track_tick(t, 16, 0);
    ps_swf_track_tick(t, 16, 1);
    {
        const int16_t *up;
        size_t         ulen;

        chk("the in/out cue spans out - in", (long)fake_voice[EV_SLOT0].frames,
            1000);
        chk("and does not loop", fake_voice[EV_SLOT0].loop, 0);
        up = (const int16_t *)(const void *)
                 fake_voice_ring(fake_voice[EV_SLOT0].smp, &ulen);
        if(up)
            chk("it begins at the in point, not at zero", up[0], 500);
    }

    /* A stereo cue needs both voices and there is one free, so the mono one
     * still sounding is taken - the policy ps_audio.c already uses. */
    ps_swf_track_tick(t, 16, 2);
    ps_swf_track_get_stats(t, &st);
    chk("the stereo cue started", (long)st.ev_started, 2);
    chk("by taking the sounding one", (long)st.ev_stolen, 1);
    chk("it holds both voices", fake_voice[EV_SLOT0].live +
        fake_voice[EV_SLOT1].live, 2);
    chk("hard left", fake_voice[EV_SLOT0].pan, 0);
    chk("hard right", fake_voice[EV_SLOT1].pan, 255);

    ps_swf_track_tick(t, 16, 3);
    ps_swf_track_get_stats(t, &st);
    chk("and a third cue takes one back", (long)st.ev_started, 3);
    chk_range("stealing, not dropping", (long)st.ev_dropped, 0, 0);

    ps_swf_track_destroy(t);
}

[[maybe_unused]] static void test_event_stop_and_multiple(void)
{
    bw            tags;
    uint8_t      *pcm;
    size_t        n;
    swf_startinfo         si;
    ps_swf_track *t;
    ps_swf_track_stats st;

    printf("--- SyncStop and SyncNoMultiple\n");
    fake_voice_reset();

    pcm = ramp_pcm(20000, 0, 1, &n);            /* ~0.9s, long enough to stop */
    bw_init(&tags, 1 << 17);
    put_define_sound(&tags, 5, SND_RATE_CODE, 1, 0, 20000, pcm, n);
    free(pcm);
    swf_showframe(&tags);                       /* 0 */

    memset(&si, 0, sizeof si);
    si.no_multiple = 1;
    swf_start_sound(&tags, 5, &si);
    swf_showframe(&tags);                       /* 1 */
    swf_start_sound(&tags, 5, &si);             /* the same cue again */
    swf_showframe(&tags);                       /* 2 */

    memset(&si, 0, sizeof si);
    si.stop = 1;
    swf_start_sound(&tags, 5, &si);
    swf_showframe(&tags);                       /* 3 */
    swf_end(&tags);

    t = load_tags(&tags, "start, start again, stop");

    ps_swf_track_tick(t, 16, 0);
    ps_swf_track_tick(t, 16, 1);
    ps_swf_track_get_stats(t, &st);
    chk("the first fires", (long)st.ev_started, 1);

    ps_swf_track_tick(t, 16, 2);
    ps_swf_track_get_stats(t, &st);
    chk("SyncNoMultiple suppresses the second", (long)st.ev_started, 1);
    chk("and it is not counted as a refusal", (long)st.ev_dropped, 0);

    ps_swf_track_tick(t, 16, 3);
    ps_swf_track_get_stats(t, &st);
    chk("SyncStop silences it", (long)st.ev_stopped, 1);
    chk("the voice is gone", fake_voice[EV_SLOT0].live, 0);

    ps_swf_track_destroy(t);
}

[[maybe_unused]] static void test_event_envelope(void)
{
    bw            tags;
    uint8_t      *pcm;
    size_t        n;
    swf_startinfo         si;
    ps_swf_track *t;
    int           first;

    printf("--- a volume envelope reaches the level register\n");
    fake_voice_reset();

    /* 22050 frames is a second, faded to nothing over it. The envelope's
     * positions are in 44100Hz units whatever the sound's rate is, which is
     * the trap ps_swf_sound.h names: 44100 here is the end of a one second
     * sound at 22050Hz, not the halfway point. */
    pcm = ramp_pcm(22050, 0, 1, &n);
    bw_init(&tags, 1 << 17);
    put_define_sound(&tags, 9, SND_RATE_CODE, 1, 0, 22050, pcm, n);
    free(pcm);
    swf_showframe(&tags);

    memset(&si, 0, sizeof si);
    si.nenv     = 2;
    si.pos44[0] = 0;
    si.left[0]  = 32768;
    si.right[0] = 32768;
    si.pos44[1] = 44100;
    si.left[1]  = 0;
    si.right[1] = 0;
    swf_start_sound(&tags, 9, &si);
    swf_showframe(&tags);
    swf_end(&tags);

    t = load_tags(&tags, "a cue with an envelope");

    ps_swf_track_tick(t, 16, 0);
    ps_swf_track_tick(t, 16, 1);
    first = fake_voice[EV_SLOT0].vol;
    chk_range("it starts at the track's own level", first, 200, 255);

    ps_swf_track_tick(t, 250, 1);
    chk_range("a quarter of the way through it is three quarters down",
              fake_voice[EV_SLOT0].vol, first * 6 / 10, first * 9 / 10);
    ps_swf_track_tick(t, 500, 1);
    chk_range("three quarters through it is nearly out",
              fake_voice[EV_SLOT0].vol, 0, first * 4 / 10);
    chk_true("and the level was written, not the sample rewritten",
             fake_voice[EV_SLOT0].levels >= 2);
    chk("nothing was re-uploaded", fake_uploads, 1);

    ps_swf_track_destroy(t);
}

/* --- test: a DefineSound too long for a voice goes on the ring ----------- */

[[maybe_unused]] static void test_long_event_on_ring(void)
{
    bw            tags;
    uint8_t      *pcm;
    size_t        n;
    swf_startinfo         si;
    ps_swf_track *t;
    sim           s;
    ps_swf_track_stats st;
    const int16_t *r;
    int           i;

    printf("--- a soundtrack shipped as one DefineSound\n");
    fake_voice_reset();
    memset(&s, 0, sizeof s);

    /* 90000 frames is past the 65535 the loop-end register can address, so
     * this cannot be keyed as one sample however much memory is free. */
    pcm = ramp_pcm(90000, 0, 1, &n);
    bw_init(&tags, 1 << 19);
    put_define_sound(&tags, 4, SND_RATE_CODE, 1, 0, 90000, pcm, n);
    free(pcm);
    memset(&si, 0, sizeof si);
    swf_start_sound(&tags, 4, &si);
    /* Long enough that the timeline does not come back round to frame 0 during
     * the run: a cue on frame 0 fires again when it does, which is what a
     * looping movie wants and would confuse what is being measured here. */
    for(i = 0; i < 250; i++)
        swf_showframe(&tags);
    swf_end(&tags);

    t = load_tags(&tags, "a movie whose soundtrack is an event sound");
    chk_true("it claims the page's music",
             ps_swf_track_has_soundtrack(t));
    chk_true("but nothing sounds until the cue's frame",
             !ps_swf_track_is_playing(t));

    s.t            = t;
    s.rate         = SND_RATE;
    s.us_per_frame = 1000000 / 12;
    s.nframe       = 250;

    ps_swf_track_tick(t, 16, 0);
    chk_true("the cue puts it on the ring", ps_swf_track_is_playing(t));
    chk("the ring voice loops", fake_voice[RING_SLOT].loop, 1);

    r = ring16();
    chk_true("the ring exists", r != NULL);
    if(r) {
        chk("and holds the start of the sound", r[0], 0);
        chk("running on", r[CHUNK_FRAMES], (int16_t)(uint16_t)CHUNK_FRAMES);
    }

    /* 90000 frames at 22050Hz is 4.08 seconds. Free-running, unpaced - an
     * event sound is not tied to the frame rate and the header says so. Six
     * seconds is past the end plus the lap of silence the drain owes. */
    for(i = 0; i < 380; i++)
        sim_step(&s, 16, 1);

    ps_swf_track_get_stats(t, &st);
    chk_true("nothing paced it", st.resyncs == 0);
    chk_true("it ran out and stopped", !ps_swf_track_is_playing(t));
    chk("no transfer the AICA would have dropped", fake_bad_write, 0);

    ps_swf_track_destroy(t);
}

/* --- test: what a movie with no sound costs ------------------------------ */

[[maybe_unused]] static void test_silent_movie(void)
{
    bw            tags;
    ps_swf_track *t;

    printf("--- a movie with no sound at all\n");
    fake_voice_reset();

    bw_init(&tags, 256);
    swf_showframe(&tags);
    swf_end(&tags);

    t = load_tags(&tags, "an empty movie walks");
    chk_true("it has no audio", !ps_swf_track_has_audio(t));
    chk_true("claims no music", !ps_swf_track_has_soundtrack(t));
    chk_true("and plays nothing", !ps_swf_track_is_playing(t));
    chk("no sound memory was taken", fake_uploads, 0);

    ps_swf_track_tick(t, 16, 0);
    ps_swf_track_tick(t, 16, 1);
    chk("still nothing keyed", fake_keyed, 0);

    ps_swf_track_destroy(t);
}

/* --- test: stopping releases everything ---------------------------------- */

[[maybe_unused]] static void test_stop_releases(void)
{
    uint8_t      *img;
    size_t        len;
    char          err[128];
    ps_swf_track *t;

    printf("--- stopping, which is what navigation does\n");
    fake_voice_reset();

    img = build_stream_movie(&len);
    t   = ps_swf_track_create();
    chk("the file walks", ps_swf_track_load(t, img, len, err, sizeof err), 0);
    free(img);

    chk_true("it is sounding", ps_swf_track_is_playing(t));
    ps_swf_track_stop(t);
    chk_true("and then it is not", !ps_swf_track_is_playing(t));
    chk_true("the voice was killed rather than released to fade",
             fake_killed >= 1);
    chk_true("nothing is claimed any more",
             !ps_swf_track_has_soundtrack(t) && !ps_swf_track_has_audio(t));

    /* Twice, because a navigation handler calls this blind and a page change
     * during a load can reach it with nothing loaded. */
    ps_swf_track_stop(t);
    ps_swf_track_tick(t, 16, 3);
    chk_true("stopping an idle track is safe", 1);

    ps_swf_track_destroy(t);
}

/* --- the retention cap --------------------------------------------------- */

/* Built as a second binary with PS_CFG_MAX_SWF_AUDIO_BYTES turned down. The
 * real ceiling is a megabyte on the Dreamcast and sixteen on the host, and
 * building a sixteen megabyte movie to prove a comparison is a poor trade -
 * but the comparison is the one thing standing between a hostile page and the
 * whole of main memory, so it is not one to leave untested either. */
[[maybe_unused]] static void test_cap(void)
{
    uint8_t      *img;
    size_t        len;
    char          err[128];
    ps_swf_track *t;

    printf("--- the retention cap (%u KB)\n",
           (unsigned)(PS_CFG_MAX_SWF_AUDIO_BYTES / 1024u));
    fake_voice_reset();

    img = build_stream_movie(&len);
    t   = ps_swf_track_create();
    err[0] = '\0';
    chk("a movie over the cap is refused",
        ps_swf_track_load(t, img, len, err, sizeof err), -1);
    chk_true("with a reason", err[0] != '\0');
    printf("      %s\n", err);
    free(img);

    chk_true("and nothing is left playing", !ps_swf_track_is_playing(t));
    chk_true("nor claimed", !ps_swf_track_has_soundtrack(t));
    chk("no sound memory was taken", fake_uploads, 0);

    ps_swf_track_destroy(t);
}

/* What one real file costs, which is the number the 16MB budget cares about
 * and the one a host suite can actually answer. Everything else about a
 * playing track is fixed: 16KB of staging, under a kilobyte of state, and a
 * ring that lives in sound memory and costs the page budget nothing. */
[[maybe_unused]] static void inventory(const char *path)
{
    ps_swf_track *t;
    FILE         *f = fopen(path, "rb");
    uint8_t      *buf;
    long          len;
    char          err[128];

    if(!f) {
        printf("cannot open %s\n", path);
        fails++;
        return;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (uint8_t *)malloc((size_t)len);
    if(fread(buf, 1, (size_t)len, f) != (size_t)len) {
        printf("short read on %s\n", path);
        fails++;
        fclose(f);
        free(buf);
        return;
    }
    fclose(f);

    fake_voice_reset();
    ps_swf_mem_reset_peak();

    t = ps_swf_track_create();
    err[0] = '\0';
    if(ps_swf_track_load(t, buf, (size_t)len, err, sizeof err) < 0) {
        printf("%s refused: %s\n", path, err);
        fails++;
    }
    else {
        printf("%s\n"
               "  file             %6ld KB\n"
               "  retained audio   %6u KB   held for as long as the page is\n"
               "  parser peak      %6u KB   transient, during the walk\n"
               "  staging          %6u KB   one chunk, both channels\n"
               "  sound memory     %6u KB   the ring, off the page budget\n"
               "  soundtrack       %s\n",
               path, len / 1024,
               (unsigned)(ps_swf_mem_live() / 1024u),
               (unsigned)(ps_swf_mem_peak() / 1024u),
               (unsigned)(ps_swf_track_has_soundtrack(t)
                          ? CHUNK_FRAMES * 2u * 2u / 1024u : 0u),
               (unsigned)(ps_swf_track_is_playing(t)
                          ? RING_FRAMES * 2u / 1024u : 0u),
               ps_swf_track_has_soundtrack(t) ? "yes" : "event sounds only");
    }

    ps_swf_track_destroy(t);
    free(buf);
}

int main(int argc, char **argv)
{
#ifdef SWFSND_CAP_ONLY
    (void)argc;
    (void)argv;
    test_cap();
#else
    if(argc > 1) {
        int i;

        for(i = 1; i < argc; i++)
            inventory(argv[i]);
        printf(fails ? "\n%d FAILED\n" : "\nok\n", fails);
        return fails ? 1 : 0;
    }

    printf("ps_swf_track: ring %u frames in %u chunks, voices at %d..%d\n",
           (unsigned)RING_FRAMES, (unsigned)RING_CHUNKS, BASE_SLOT,
           BASE_SLOT + PS_AUDIO_SWF_SLOTS - 1);

    test_prime();
    test_adpcm();
    test_steady();
    test_stall();
    test_wrap();
    test_resync_exact();
    test_event_basic();
    test_event_policy();
    test_event_stop_and_multiple();
    test_event_envelope();
    test_long_event_on_ring();
    test_silent_movie();
    test_stop_releases();
#endif

    printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
