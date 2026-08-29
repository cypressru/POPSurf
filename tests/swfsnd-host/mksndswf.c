/* Writes the movies that go on the disc so a person can check the soundtrack
 * on a Dreamcast, and the pages that embed them.
 *
 * Nothing a host test can assert answers the question these exist for. The
 * host suite proves the ring holds the sample it says it holds; it cannot
 * prove that the AICA plays it at the right pitch, that the store queue
 * transfer lands, or - the one that matters most and that no automated check
 * anywhere can settle - that the click lands on the frame it belongs to when a
 * person watches and listens at the same time.
 *
 * So every file here is built to be judged by ear against the picture:
 *
 *   a bar that steps across the stage every frame, so it is obvious the
 *   timeline is running and at what speed;
 *
 *   a large square that flashes for two frames on each of four beats, one a
 *   second, so the eye has an exact instant to compare against;
 *
 *   a click in the audio on the same frame as each flash, sharp enough that
 *   being a frame out is audible rather than arguable;
 *
 *   and a quiet 110Hz drone under all of it, at 440 whole cycles across the
 *   four second loop. The drone is there because silence hides everything a
 *   ring buffer can do wrong: a chunk written twice, a chunk missed, a
 *   transfer that did not land, all of which are inaudible between clicks and
 *   all of which put a step in a continuous tone.
 *
 * Everything is first party and generated. No Flash file from anywhere else is
 * on this disc and none should be - we have no right to redistribute somebody
 * else's content.
 */
#include "swfbuild.h"

#include "ps_swf_sound.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STAGE_W_PX 320
#define STAGE_H_PX 240
#define TWIP       20

#define FPS      12
#define NFRAME   48                    /* four seconds */
#define RATE     22050u
#define RATECODE 2

/* 22050 / 12 is 1837.5, which the head's integer field cannot say.
 *
 * The PCM file alternates either side of it, so the block table disagrees with
 * frame * spf and the true total is exactly four seconds - which is the case
 * ps_swf_stream_frame_sample exists to get right, and the case a shipped file
 * ought to contain.
 *
 * The ADPCM file cannot. A block is 2 bits of code width, 22 of packet header
 * and four per delta, so it only ends on a byte boundary when the sample count
 * is odd; an even one leaves four spare bits that a reader has no way to know
 * are padding and decodes as one more delta. That extra sample is a single
 * spike of an eighth of a step, once every block, which at twelve blocks a
 * second is an audible buzz - and one that would look exactly like a fault in
 * the player rather than in the file it was handed. */
#define SPF 1837u

static uint32_t block_len(int f, int adpcm)
{
    return adpcm ? SPF : SPF + ((f & 1) ? 0u : 1u);
}

static uint32_t block_first(int f, int adpcm)
{
    uint32_t at = 0;
    int      i;

    for(i = 0; i < f; i++)
        at += block_len(i, adpcm);
    return at;
}

/* The soundtrack, as one array: a drone all the way through, with a decaying
 * click on each beat. Generated once and then either shipped raw or encoded. */
static int16_t *soundtrack(int adpcm, uint32_t *n_out)
{
    uint32_t n = block_first(NFRAME, adpcm);
    int16_t *s = (int16_t *)malloc((size_t)n * sizeof *s);
    uint32_t i;
    int      b;

    for(i = 0; i < n; i++) {
        /* 440 cycles across the loop, so the drone joins itself at the seam
         * and any click there is the player's and not the file's. */
        double ph = 2.0 * 3.14159265358979 * 440.0 * (double)i / (double)n;

        s[i] = (int16_t)(2000.0 * sin(ph));
    }

    for(b = 0; b < 4; b++) {
        uint32_t at  = block_first(b * 12, adpcm);
        uint32_t len = RATE / 20u;                  /* 50ms */

        for(i = 0; i < len && at + i < n; i++) {
            double t   = (double)i / (double)RATE;
            double env = exp(-t * 45.0);
            double ph  = 2.0 * 3.14159265358979 * 1200.0 * t;
            int    v   = s[at + i] + (int)(21000.0 * env * sin(ph));

            s[at + i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
        }
    }

    *n_out = n;
    return s;
}

/* --- the picture, which is the same in every file ------------------------ */

#define ID_STEP  1
#define ID_FLASH 2
#define ID_BASE  3

static void put_characters(bw *tags)
{
    swf_bgcolor(tags, 0x101418);
    swf_rect_shape(tags, ID_STEP,  16 * TWIP,  16 * TWIP, 0x30c0ff);
    swf_rect_shape(tags, ID_FLASH, 96 * TWIP,  96 * TWIP, 0xffc040);
    swf_rect_shape(tags, ID_BASE, 300 * TWIP,   4 * TWIP, 0x2a3440);
}

/* Everything that happens on frame f except the sound, which the caller adds
 * before this so the block sits between the frame and its ShowFrame. */
static void put_frame_art(bw *tags, int f)
{
    if(f == 0)
        swf_place(tags, 1, ID_BASE, 10 * TWIP, 200 * TWIP, 0);

    /* One character moved, not forty-eight placed: a Move keeps the same
     * instance at the same depth, which is what the display list is for. */
    swf_place(tags, 2, f == 0 ? ID_STEP : -1,
              (10 + f * 6) * TWIP, 176 * TWIP, f != 0);

    if(f % 12 == 0)
        swf_place(tags, 3, ID_FLASH, 112 * TWIP, 40 * TWIP, 0);
    else if(f % 12 == 2)
        swf_remove(tags, 3);
}

/* --- the files ----------------------------------------------------------- */

static void write_file(const char *dir, const char *name, const uint8_t *d,
                       size_t n)
{
    char  path[512];
    FILE *f;

    snprintf(path, sizeof path, "%s/%s", dir, name);
    f = fopen(path, "wb");
    if(!f) {
        printf("mksndswf: cannot write %s\n", path);
        exit(1);
    }
    fwrite(d, 1, n, f);
    fclose(f);
    printf("  %-16s %6u bytes\n", name, (unsigned)n);
}

static void emit(const char *dir, const char *name, bw *tags)
{
    uint8_t *img;
    size_t   len;

    swf_end(tags);
    img = swf_finish(tags, STAGE_W_PX * TWIP, STAGE_H_PX * TWIP, FPS, NFRAME,
                     &len);
    write_file(dir, name, img, len);
    free(img);
    bw_free(tags);
}

/* A stream sound: one block per frame, which is the whole point of the
 * mechanism and the only thing that keeps music aligned to a timeline. */
static void build_stream(const char *dir, const char *name, int adpcm)
{
    bw        tags;
    int16_t  *pcm;
    uint32_t  n;
    int       f;

    pcm = soundtrack(adpcm, &n);
    (void)n;

    bw_init(&tags, 1 << 20);
    put_characters(&tags);
    swf_stream_head(&tags, adpcm ? PS_SWF_SND_ADPCM : PS_SWF_SND_PCM_LE,
                    RATECODE, 0, (int)SPF);

    for(f = 0; f < NFRAME; f++) {
        uint32_t len   = block_len(f, adpcm);
        uint32_t first = block_first(f, adpcm);

        if(adpcm) {
            bw blk;

            /* Each block carries its own code width and its own packet
             * header, because each has to be decodable without the one before
             * it - that is what makes a seek to a frame exact. */
            bw_init(&blk, len);
            swf_adpcm_stream(&blk, pcm + first, len, 4);
            swf_stream_block(&tags, blk.b, blk.n);
            bw_free(&blk);
        }
        else {
            uint8_t *raw = (uint8_t *)malloc((size_t)len * 2u);
            uint32_t i;

            for(i = 0; i < len; i++) {
                uint16_t v = (uint16_t)pcm[first + i];

                raw[i * 2]     = (uint8_t)(v & 0xff);
                raw[i * 2 + 1] = (uint8_t)(v >> 8);
            }
            swf_stream_block(&tags, raw, (size_t)len * 2u);
            free(raw);
        }

        put_frame_art(&tags, f);
        swf_showframe(&tags);
    }

    free(pcm);
    emit(dir, name, &tags);
}

/* Event sounds: a short blip fired from four frames, with the last one carrying
 * a fade so the envelope path is exercised too. Deliberately no stream, so the
 * page's own <bgsound> is left alone and the two can be heard together. */
static void build_events(const char *dir, const char *name)
{
    bw            tags;
    swf_startinfo si;
    uint8_t      *raw;
    uint32_t      len = RATE / 8u;              /* 125ms */
    uint32_t      i;
    int           f;

    raw = (uint8_t *)malloc((size_t)len * 2u);
    for(i = 0; i < len; i++) {
        double   t   = (double)i / (double)RATE;
        double   env = exp(-t * 22.0);
        double   ph  = 2.0 * 3.14159265358979 * 900.0 * t;
        uint16_t v   = (uint16_t)(int16_t)(20000.0 * env * sin(ph));

        raw[i * 2]     = (uint8_t)(v & 0xff);
        raw[i * 2 + 1] = (uint8_t)(v >> 8);
    }

    bw_init(&tags, 1 << 18);
    put_characters(&tags);
    swf_define_sound(&tags, 10, PS_SWF_SND_PCM_LE, RATECODE, 1, 0, len, raw,
                     (size_t)len * 2u);
    free(raw);

    for(f = 0; f < NFRAME; f++) {
        if(f == 0 || f == 12) {
            memset(&si, 0, sizeof si);
            swf_start_sound(&tags, 10, &si);
        }
        else if(f == 24) {
            /* Three times over, which the hardware does by looping the
             * uploaded region rather than by being keyed three times. */
            memset(&si, 0, sizeof si);
            si.loops = 3;
            swf_start_sound(&tags, 10, &si);
        }
        else if(f == 36) {
            memset(&si, 0, sizeof si);
            si.nenv     = 2;
            si.pos44[0] = 0;
            si.left[0]  = 32768;
            si.right[0] = 32768;
            /* Positions are in 44100Hz units whatever the sound's rate is, so
             * this is the end of a 125ms sound at 22050Hz and not the middle
             * of it. Getting that wrong is silent: the fade simply happens
             * twice as fast and sounds like a shorter sample. */
            si.pos44[1] = len * 2u;
            si.left[1]  = 0;
            si.right[1] = 0;
            swf_start_sound(&tags, 10, &si);
        }

        put_frame_art(&tags, f);
        swf_showframe(&tags);
    }

    emit(dir, name, &tags);
}

/* The third path: a movie whose soundtrack is one DefineSound, four seconds
 * long, which is past the 65535 frames the AICA's loop-end register can
 * address and so cannot be keyed on a voice at all. It goes on the ring. */
static void build_big_event(const char *dir, const char *name)
{
    bw            tags;
    swf_startinfo si;
    int16_t      *pcm;
    uint32_t      n, i;
    uint8_t      *raw;
    int           f;

    pcm = soundtrack(0, &n);
    raw = (uint8_t *)malloc((size_t)n * 2u);
    for(i = 0; i < n; i++) {
        uint16_t v = (uint16_t)pcm[i];

        raw[i * 2]     = (uint8_t)(v & 0xff);
        raw[i * 2 + 1] = (uint8_t)(v >> 8);
    }
    free(pcm);

    bw_init(&tags, 1 << 20);
    put_characters(&tags);
    swf_define_sound(&tags, 20, PS_SWF_SND_PCM_LE, RATECODE, 1, 0, n, raw,
                     (size_t)n * 2u);
    free(raw);

    memset(&si, 0, sizeof si);
    swf_start_sound(&tags, 20, &si);

    for(f = 0; f < NFRAME; f++) {
        put_frame_art(&tags, f);
        swf_showframe(&tags);
    }

    emit(dir, name, &tags);
}

/* --- pages --------------------------------------------------------------- */

static void page(const char *dir, const char *name, const char *movie,
                 const char *title, const char *what, const char *listen,
                 const char *bgsound)
{
    char  path[512];
    FILE *f;

    snprintf(path, sizeof path, "%s/%s", dir, name);
    f = fopen(path, "w");
    if(!f) {
        printf("mksndswf: cannot write %s\n", path);
        exit(1);
    }

    fprintf(f,
        "<!doctype html>\n"
        "<!-- Generated by tests/swfsnd-host/mksndswf. A movie with sound,\n"
        "     at its own stage size, for checking on hardware: what is being\n"
        "     judged is whether the click lands on the flash. -->\n"
        "<html>\n"
        "  <head><title>%s</title></head>\n"
        "  <body bgcolor=\"#101418\" text=\"#e8e8e8\" link=\"#ffc447\" "
        "vlink=\"#c89a2f\"%s>\n"
        "    <font size=\"2\"><a href=\"../test_swf.html\">"
        "&lt;&lt; testbeds</a></font>\n"
        "    <br><br>\n"
        "    <table cellspacing=\"0\" cellpadding=\"6\" border=\"0\"><tr>\n"
        "      <td bgcolor=\"#242424\">"
        "<embed src=\"%s\" width=\"%d\" height=\"%d\"></td>\n"
        "    </tr></table>\n"
        "    <br>\n"
        "    <font size=\"4\"><b>%s</b></font><br>\n"
        "    <font size=\"2\" color=\"#8a949e\">%s</font><br><br>\n"
        "    <font size=\"2\" color=\"#8a949e\">Listen for: %s</font><br><br>\n"
        "    <font size=\"2\" color=\"#8a949e\">Stage %d x %d, drawn at 1:1, "
        "12fps, a four second loop.</font>\n"
        "  </body>\n"
        "</html>\n",
        title, bgsound ? bgsound : "", movie, STAGE_W_PX, STAGE_H_PX, title,
        what, listen, STAGE_W_PX, STAGE_H_PX);

    fclose(f);
    printf("  %-16s page\n", name);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "../../cd/swf";

    printf("mksndswf -> %s\n", dir);

    build_stream(dir, "t_snd.swf", 0);
    build_stream(dir, "t_snd_a.swf", 1);
    build_events(dir, "t_snd_ev.swf");
    build_big_event(dir, "t_snd_big.swf");

    page(dir, "t_snd.html", "t_snd.swf", "t_snd",
         "A stream sound: uncompressed PCM, one block per frame, paced by the "
         "timeline.",
         "four clicks a second apart, each on the frame the yellow square "
         "flashes, over a quiet steady drone. A click that is early or late by "
         "a frame is the pacing; a step or a stutter in the drone is the ring.",
         NULL);

    page(dir, "t_snd_a.html", "t_snd_a.swf", "t_snd_a",
         "The same soundtrack as Flash ADPCM, which is what real content of "
         "this era uses. Four bits a sample, so a quarter of the bytes and all "
         "of the decode.",
         "exactly what t_snd sounds like, a little grainier. Anything else - "
         "noise, a rising hiss, the drone turning to buzz - is the decoder or "
         "the per-block seek, not the ring.",
         NULL);

    page(dir, "t_snd_ev.html", "t_snd_ev.swf", "t_snd_ev",
         "Event sounds only: a blip fired from four frames, the third playing "
         "three times over and the fourth fading out.",
         "a blip on the first flash and the second; a triple blip on the "
         "third; and a single blip that fades to nothing on the fourth.",
         NULL);

    page(dir, "t_snd_big.html", "t_snd_big.swf", "t_snd_big",
         "A soundtrack shipped as one DefineSound rather than as a stream. "
         "Four seconds is past what the AICA's loop-end register can address, "
         "so it cannot be keyed on a voice and goes on the ring instead.",
         "the same four clicks and the same drone as t_snd. This one is not "
         "paced by the timeline - an event sound runs to its own length - so "
         "the clicks may drift away from the flashes over a few loops, and "
         "that is the format saying so rather than a fault.",
         NULL);

    return 0;
}
