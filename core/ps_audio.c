#include "ps_audio.h"
#include "ps_config.h"

#include "ps_voice.h"

#include <stdlib.h>
#include <malloc.h>   /* memalign: the bank is relocated 32-byte aligned */
#include <string.h>
#include <stdio.h>

/* --- baked bank --------------------------------------------------------- */

/* Format written by tools/gmbake and documented in tools/README.md. */

#define PSGM_HDR_SIZE   20
#define PSGM_ENTRY_SIZE 24

/* Sample encodings a bank can carry. ADPCM is half the size, but the AICA
 * cannot pitch-shift it much beyond an octave without breaking up, and a bank
 * with one root note per instrument pitches every note it plays. PCM8 costs
 * twice the sound RAM and is worth it. */
#define PSGM_FMT_ADPCM 0
#define PSGM_FMT_PCM8  1

#define PSGM_KIND_MELODIC 0
#define PSGM_KIND_DRUM    1

/* One instrument. The ADPCM stays in the bank blob in main RAM; sfx is the
 * SPU-resident copy, present only while the sample is in use. */
typedef struct ps_sample_tag {
    uint8_t  present;
    uint8_t  root_note;
    uint8_t  loop;
    uint32_t rate;
    uint32_t frames;
    uint32_t offset;    /* into the sample blob */
    uint32_t length;    /* encoded bytes */
    uint32_t loop_start;/* frame the loop repeats from; 0 for one-shots */

    ps_smp   smp;       /* PS_SMP_NONE when not resident */
    uint32_t last_use;  /* for eviction */
} ps_sample;

/* --- sequencer ---------------------------------------------------------- */

/* A parsed event stream, already merged across tracks and sorted by time.
 * Merging at parse time means playback is a single cursor walk rather than a
 * per-track heap on every tick. */
/* Eight bytes. A tempo value is 24 bits and so are chan/a/b together, so the
 * two share those three bytes rather than the record carrying a fourth field
 * that only tempo events ever use. At tens of thousands of events per song
 * that halving is the difference between a quarter of a megabyte and half. */
typedef struct {
    uint32_t tick;      /* absolute, in MIDI ticks */
    uint8_t  status;    /* 0x80 note off, 0x90 note on, 0xC0 program, 0xFF tempo */
    uint8_t  chan;      /* tempo: bits 23..16 */
    uint8_t  a;         /* tempo: bits 15..8 */
    uint8_t  b;         /* tempo: bits 7..0 */
} ps_mev;

static uint32_t ev_tempo(const ps_mev *e)
{
    return ((uint32_t)e->chan << 16) | ((uint32_t)e->a << 8) | e->b;
}

typedef struct {
    uint8_t  midi_chan;
    uint8_t  note;
    uint8_t  vel;        /* to recompute level when a controller moves */
    uint8_t  released;   /* keyed off, still fading */
    int      active;
    int      remain_ms;  /* one-shots: time left before the sample ends */
    struct ps_sample_tag *sample;   /* what is sounding, for eviction */
} ps_voice;

struct ps_audio {
    /* Bank, resident in main RAM for the session. Melodic is indexed by GM
     * program, drums by MIDI note. */
    uint8_t  *bank;
    size_t    bank_len;
    uint32_t  data_off;
    int       fmt;      /* PS_FMT_* for the voice backend */
    ps_sample melodic[128];
    ps_sample drums[128];
    int       loaded;

    /* Monotonic counter standing in for a clock, so eviction can pick the
     * least recently played without needing real time. */
    uint32_t use_clock;

    /* Sequence. */
    ps_mev  *events;
    int      nevents;
    int      cursor;
    uint32_t division;      /* ticks per quarter note */
    uint32_t tempo_us;      /* microseconds per quarter */
    int      playing;
    int      loop;

    /* Accumulated playback position in MIDI ticks, kept in a fixed-point
     * accumulator so slow tempos do not quantise away at 60fps. */
    uint32_t tick;
    uint32_t tick_frac;     /* 1/1000 of a tick */

    uint8_t  program[PS_MIDI_CHANNELS];

    /* Per-channel mix, in raw MIDI units. GM defaults: volume 100,
     * expression full, pan centred. */
    uint8_t  cc_volume[PS_MIDI_CHANNELS];
    uint8_t  cc_expr[PS_MIDI_CHANNELS];
    uint8_t  cc_pan[PS_MIDI_CHANNELS];
    ps_voice voices[PS_AUDIO_VOICES];

    int nvoices;
    int volume;
    int muted;

    /* Diagnostic counters: a note that finds no resident sample is silently
     * dropped, and enough of those would look like a level problem. */
    uint32_t stat_played, stat_skipped;
};

#define PS_CC_VOLUME     7
#define PS_CC_PAN       10
#define PS_CC_EXPRESSION 11

/* --- big-endian readers, for MIDI only ---------------------------------- */

/* Standard MIDI Files are big-endian by specification. This is the one format
 * in the project that is, which is exactly why it gets its own readers rather
 * than being confused with the little-endian baked assets. */
static uint32_t rd_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t rd_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/* MIDI variable-length quantity: seven bits per byte, high bit continues. */
static uint32_t rd_vlq(const uint8_t *p, size_t len, size_t *pos)
{
    uint32_t v = 0;
    int      i;

    for(i = 0; i < 4 && *pos < len; i++) {
        uint8_t b = p[(*pos)++];

        v = (v << 7) | (b & 0x7f);
        if(!(b & 0x80))
            break;
    }
    return v;
}

/* GM startup values. A file that never sends CC7 expects 100, not silence
 * and not full scale. */
static void reset_channels(ps_audio *a)
{
    int i;

    for(i = 0; i < PS_MIDI_CHANNELS; i++) {
        a->cc_volume[i] = 100;
        a->cc_expr[i]   = 127;
        a->cc_pan[i]    = 64;
    }
}

/* --- bank loading ------------------------------------------------------- */

/* Uploads one sample into SPU RAM, evicting least-recently-used instruments
 * until it fits. The bank itself never leaves main RAM; only the working set
 * is resident, which is what lets the bank be larger than the 2MB of sound
 * memory and keeps startup to a single sequential read. */
static int make_resident(ps_audio *a, ps_sample *s)
{
    int guard;

    if(!s->present)
        return 0;
    if(s->smp != PS_SMP_NONE) {
        s->last_use = ++a->use_clock;
        return 1;
    }
    if((size_t)s->offset + s->length > a->bank_len - a->data_off)
        return 0;

    /* Bounded so a pathological bank cannot spin here. */
    for(guard = 0; guard < 256; guard++) {
        /* The bank is held exactly as downloaded, so this pointer inherits
         * whatever alignment the header happens to give it. The backend
         * bounces it if the hardware needs better. */
        char *src = (char *)(a->bank + a->data_off + s->offset);

        s->smp = ps_voice_upload(src, s->length);

        if(s->smp != PS_SMP_NONE) {
            s->last_use = ++a->use_clock;
            return 1;
        }

        /* Out of sound RAM: drop the coldest resident instrument and retry.
         * A voice currently sounding is never chosen, because unloading it
         * would cut the note. */
        {
            ps_sample *victim = NULL;
            int i, v;

            for(i = 0; i < 256; i++) {
                ps_sample *c = (i < 128) ? &a->melodic[i] : &a->drums[i - 128];

                if(!c->present || c->smp == PS_SMP_NONE || c == s)
                    continue;

                int in_use = 0;
                for(v = 0; v < a->nvoices; v++) {
                    if(a->voices[v].active && a->voices[v].sample == c) {
                        in_use = 1;
                        break;
                    }
                }
                if(in_use)
                    continue;

                if(!victim || c->last_use < victim->last_use)
                    victim = c;
            }

            if(!victim)
                return 0;

            ps_voice_release(victim->smp);
            victim->smp = PS_SMP_NONE;
        }
    }
    return 0;
}

ps_audio *ps_audio_create(void)
{
    ps_audio *a = (ps_audio *)calloc(1, sizeof *a);
    int       i;

    if(!a)
        return NULL;

    a->volume   = 220;
    reset_channels(a);
    a->tempo_us = 500000;   /* 120bpm, the MIDI default */

    for(i = 0; i < 128; i++) {
        a->melodic[i].smp = PS_SMP_NONE;
        a->drums[i].smp   = PS_SMP_NONE;
    }

    /* Fewer voices than asked for is workable - the allocator steals the
     * one closest to finishing - but none at all means silence, and the
     * caller should know that now rather than at the first note. */
    a->nvoices = ps_voice_init(PS_AUDIO_VOICES);

    /* Hand the top slots to the streamer and to a movie's soundtrack, and stop
     * at the boundary. See PS_AUDIO_STREAM_SLOTS: a note that loses its voice
     * clips, a stream that loses one never comes back. */
    a->nvoices -= PS_AUDIO_STREAM_SLOTS + PS_AUDIO_SWF_SLOTS;
    if(a->nvoices <= 0) {
        a->nvoices = 0;
        printf("popsurf: no hardware voices available, audio disabled\n");
    }

    return a;
}

int ps_audio_set_bank(ps_audio *a, void *data, size_t len)
{
    const uint8_t *b = (const uint8_t *)data;
    uint32_t n, i, data_off, fmt;

    if(!a || !b || len < PSGM_HDR_SIZE) {
        free(data);
        return -1;
    }

    if(b[0] != 'P' || b[1] != 'S' || b[2] != 'G' || b[3] != 'M' ||
       ps_rd_u32le(b + 4) != 4) {
        free(data);
        return -1;
    }

    n        = ps_rd_u32le(b + 8);
    data_off = ps_rd_u32le(b + 12);
    fmt      = ps_rd_u32le(b + 16);

    if(fmt != PSGM_FMT_ADPCM && fmt != PSGM_FMT_PCM8) {
        free(data);
        return -1;
    }

    if(n > 512 || (size_t)PSGM_HDR_SIZE + (size_t)n * PSGM_ENTRY_SIZE > len ||
       data_off > len) {
        free(data);
        return -1;
    }

    /* Replacing a bank means every resident sample is stale. */
    ps_audio_stop(a);
    for(i = 0; i < 128; i++) {
        if(a->melodic[i].smp != PS_SMP_NONE)
            ps_voice_release(a->melodic[i].smp);
        if(a->drums[i].smp != PS_SMP_NONE)
            ps_voice_release(a->drums[i].smp);
    }
    memset(a->melodic, 0, sizeof a->melodic);
    memset(a->drums, 0, sizeof a->drums);
    for(i = 0; i < 128; i++) {
        a->melodic[i].smp = PS_SMP_NONE;
        a->drums[i].smp   = PS_SMP_NONE;
    }

    /* Kept as downloaded. Samples are copied into SPU RAM one at a time and
     * the voice backend bounces any that are not 32-byte aligned, which costs
     * a few KB per instrument. Relocating the whole blob to an aligned
     * allocation would avoid those small copies, but it doubles peak memory
     * at exactly the wrong moment - the download buffer is still live and a
     * page with images is already resident - and on a 16MB machine that is a
     * far worse trade than a 6KB memcpy per instrument. */
    free(a->bank);
    a->bank     = (uint8_t *)data;
    a->bank_len = len;
    a->data_off = data_off;
    a->fmt      = (fmt == PSGM_FMT_PCM8) ? PS_FMT_PCM8 : PS_FMT_ADPCM;

    for(i = 0; i < n; i++) {
        const uint8_t *e = b + PSGM_HDR_SIZE + (size_t)i * PSGM_ENTRY_SIZE;
        int        kind  = e[0];
        int        index = e[1];
        ps_sample *s;

        if(index > 127)
            continue;

        s = (kind == PSGM_KIND_DRUM) ? &a->drums[index] : &a->melodic[index];

        s->present   = 1;
        s->root_note = e[2];
        s->loop      = e[3];
        s->rate      = ps_rd_u32le(e + 4);
        s->frames    = ps_rd_u32le(e + 8);
        s->offset    = ps_rd_u32le(e + 12);
        s->length    = ps_rd_u32le(e + 16);
        s->loop_start = ps_rd_u32le(e + 20);
        s->smp       = PS_SMP_NONE;
    }

    a->loaded = 1;
    return 0;
}

int ps_audio_has_bank(const ps_audio *a)
{
    return a && a->loaded;
}

void ps_audio_destroy(ps_audio *a)
{
    int i;

    if(!a)
        return;

    ps_audio_stop(a);

    for(i = 0; i < 128; i++) {
        if(a->melodic[i].smp != PS_SMP_NONE)
            ps_voice_release(a->melodic[i].smp);
        if(a->drums[i].smp != PS_SMP_NONE)
            ps_voice_release(a->drums[i].smp);
    }

    free(a->bank);
    free(a->events);
    free(a);
}

/* --- MIDI parsing ------------------------------------------------------- */

static int ev_cmp(const void *pa, const void *pb)
{
    const ps_mev *x = (const ps_mev *)pa, *y = (const ps_mev *)pb;

    if(x->tick != y->tick)
        return x->tick < y->tick ? -1 : 1;

    /* At the same tick, note-offs must precede note-ons or a repeated note
     * silences itself. */
    return (int)x->status - (int)y->status;
}

/* Per-console, from ps_config.h. The first cut capped this at 8192 and
 * The music on the Mania front page hit it exactly - 17 tracks in 44KB -
 * so the song simply stopped partway. The array now grows to fit and only
 * refuses genuinely absurd input. */
#define PS_MIDI_MAX_EVENTS PS_CFG_MIDI_EVENTS
#define PS_MIDI_MAX_TRACKS 64

/* Uploads every instrument the tune will use, before it starts.
 *
 * Doing it lazily at the first note of each instrument spreads a few hundred
 * kilobytes of sound-RAM transfer across the whole piece - and each of those
 * transfers suspends G2 DMA and disables interrupts for its duration, which
 * is the same bus the network adapter uses. A page loading while the music
 * plays would have its transfer repeatedly stalled.
 *
 * A sixteen-channel arrangement touches on the order of thirty samples, a few
 * hundred KB against 1.8MB of sound RAM, so the working set fits and the
 * residency cache has nothing to evict. Anything that does not fit still
 * falls back to loading on demand. */
static void preload_instruments(ps_audio *a)
{
    uint8_t prog[PS_MIDI_CHANNELS];
    uint8_t want_mel[128], want_drum[128];
    int     i, n = 0;

    memset(prog, 0, sizeof prog);
    memset(want_mel, 0, sizeof want_mel);
    memset(want_drum, 0, sizeof want_drum);

    for(i = 0; i < a->nevents; i++) {
        const ps_mev *e = &a->events[i];

        if(e->status == 0xc0)
            prog[e->chan & 15] = e->a;
        else if(e->status == 0x90) {
            if(e->chan == PS_MIDI_DRUM_CHANNEL)
                want_drum[e->a & 127] = 1;
            else
                want_mel[prog[e->chan & 15] & 127] = 1;
        }
    }

    for(i = 0; i < 128; i++) {
        if(want_mel[i] && make_resident(a, &a->melodic[i]))
            n++;
        if(want_drum[i] && make_resident(a, &a->drums[i]))
            n++;
    }

    printf("popsurf: audio preloaded %d instruments\n", n);
    fflush(stdout);
}

int ps_audio_play_midi(ps_audio *a, const void *data, size_t len, int loop)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t   pos = 0;
    uint16_t ntracks, division;
    ps_mev  *evs;
    int      count = 0;
    int      t;

    if(!a || !p || len < 14)
        return -1;

    ps_audio_stop(a);

    if(memcmp(p, "MThd", 4))
        return -1;

    ntracks  = rd_be16(p + 10);
    division = rd_be16(p + 12);

    /* SMPTE timing has the high bit set; only metrical timing is supported,
     * which is what every web-era .mid uses. */
    if(division & 0x8000)
        return -1;
    if(division == 0)
        division = 96;
    if(ntracks > PS_MIDI_MAX_TRACKS)
        ntracks = PS_MIDI_MAX_TRACKS;

    int cap = 2048;

    evs = (ps_mev *)calloc(cap, sizeof *evs);
    if(!evs)
        return -1;

    pos = 8 + rd_be32(p + 4);   /* skip the header chunk by its declared size */

    for(t = 0; t < ntracks && pos + 8 <= len; t++) {
        uint32_t tlen;
        size_t   tend;
        uint32_t tick = 0;
        uint8_t  running = 0;

        if(memcmp(p + pos, "MTrk", 4)) {
            /* Unknown chunk: skip by its length, as the spec requires. */
            tlen = rd_be32(p + pos + 4);
            pos += 8 + tlen;
            continue;
        }

        tlen = rd_be32(p + pos + 4);
        pos += 8;
        tend = pos + tlen;
        if(tend > len)
            tend = len;

        while(pos < tend && count < PS_MIDI_MAX_EVENTS) {
            /* Two slots, because a single iteration can emit at most one. */
            if(count + 2 > cap) {
                int      ncap = cap * 2;
                ps_mev  *ne;

                if(ncap > PS_MIDI_MAX_EVENTS)
                    ncap = PS_MIDI_MAX_EVENTS;
                ne = (ps_mev *)realloc(evs, (size_t)ncap * sizeof *evs);
                if(!ne)
                    break;
                evs = ne;
                cap = ncap;
            }

            uint32_t delta = rd_vlq(p, tend, &pos);
            uint8_t  st;

            if(pos >= tend)
                break;

            tick += delta;
            st = p[pos];

            if(st & 0x80) {
                pos++;
                running = st;
            }
            else {
                /* Running status: the previous status byte is reused. */
                st = running;
                if(!st)
                    break;
            }

            if(st == 0xff) {
                uint8_t  type;
                uint32_t mlen;

                if(pos >= tend)
                    break;
                type = p[pos++];
                mlen = rd_vlq(p, tend, &pos);

                if(type == 0x51 && mlen == 3 && pos + 3 <= tend) {
                    ps_mev *e = &evs[count++];

                    e->tick   = tick;
                    e->status = 0xff;
                    e->chan   = p[pos];
                    e->a      = p[pos + 1];
                    e->b      = p[pos + 2];
                }
                pos += mlen;
                continue;
            }

            if(st == 0xf0 || st == 0xf7) {
                uint32_t slen = rd_vlq(p, tend, &pos);
                pos += slen;
                continue;
            }

            {
                uint8_t hi   = st & 0xf0;
                uint8_t chan = st & 0x0f;
                uint8_t d1 = 0, d2 = 0;

                if(pos < tend)
                    d1 = p[pos++];

                /* Program change and channel pressure carry one byte; the
                 * rest carry two. */
                if(hi != 0xc0 && hi != 0xd0) {
                    if(pos < tend)
                        d2 = p[pos++];
                }

                /* Controllers 7, 10 and 11 are how a multi-track file
                 * carries its mix: without them every part plays at full
                 * level dead centre, and a sixteen-channel arrangement
                 * collapses into a wall of sound. The rest (modulation,
                 * reverb depth, bank select) have no effect on a sampler
                 * this simple and are dropped at parse time so they cost
                 * neither memory nor a branch per tick. */
                if(hi == 0xb0 && d1 != PS_CC_VOLUME &&
                   d1 != PS_CC_PAN && d1 != PS_CC_EXPRESSION)
                    continue;

                if(hi == 0x90 || hi == 0x80 || hi == 0xc0 || hi == 0xb0) {
                    ps_mev *e = &evs[count++];

                    e->tick = tick;
                    e->chan = chan;
                    e->a    = d1;
                    e->b    = d2;

                    /* A note-on with zero velocity is a note-off, and most
                     * files written for the web use exactly that. */
                    e->status = (hi == 0x90 && d2 == 0) ? 0x80 : hi;
                }
            }
        }

        pos = tend;
    }

    if(count == 0) {
        free(evs);
        return -1;
    }

    qsort(evs, count, sizeof *evs, ev_cmp);

    a->events   = evs;
    a->nevents  = count;
    a->cursor   = 0;
    a->division = division;
    a->tempo_us = 500000;
    a->tick     = 0;
    a->tick_frac = 0;
    a->playing  = 1;
    a->loop     = loop;

    memset(a->program, 0, sizeof a->program);
    reset_channels(a);
    preload_instruments(a);

    printf("popsurf: midi %d events, division %u, %d tracks\n", count,
           (unsigned)division, (int)ntracks);
    return 0;
}

/* --- playback ----------------------------------------------------------- */

static void voice_stop(ps_audio *a, int i)
{
    ps_voice_stop(i);
    a->voices[i].active = 0;
}

void ps_audio_stop(ps_audio *a)
{
    int i;

    if(!a)
        return;

    /* Cut rather than release. A tune stops because the page it belonged to
     * is gone, and the samples it was using are now free to be evicted for
     * whatever the next page wants - so nothing may still be reading them. A
     * musical release here is what made a page change sound like a glitch
     * rather than like one song ending and another starting. */
    for(i = 0; i < a->nvoices; i++) {
        ps_voice_kill(i);
        a->voices[i].active    = 0;
        a->voices[i].released  = 0;
        a->voices[i].remain_ms = 0;
        a->voices[i].sample    = NULL;
    }

    a->playing = 0;
    free(a->events);
    a->events  = NULL;
    a->nevents = 0;
}

/* Equal temperament: each semitone is the twelfth root of two. A table beats
 * powf here - twelve entries, no libm, and identical on every target. */
static const uint32_t semitone_num[12] = {
    65536, 69433, 73562, 77936, 82570, 87480,
    92682, 98195, 104039, 110232, 116798, 123759
};

static uint32_t pitch_rate(uint32_t base_rate, int note, int root)
{
    int      diff = note - root;
    int      oct  = 0;
    uint64_t r;

    while(diff < 0)  { diff += 12; oct--; }
    while(diff >= 12) { diff -= 12; oct++; }

    r = (uint64_t)base_rate * semitone_num[diff] >> 16;

    if(oct > 0)
        r <<= oct;
    else if(oct < 0)
        r >>= -oct;

    /* Bound to what the hardware's pitch encoding can express, and no
     * tighter. The AICA codes playback rate as 44100 * 2^OCT * (1 + FNS/1024)
     * with OCT a 4-bit signed field, so the representable span is 44100/256
     * up to 44100*128 - a range no MIDI note comes close to leaving.
     *
     * This used to clamp at 96kHz, which sounds generous until you notice a
     * bank recorded at middle C has to play everything above it by reading
     * faster: note 103 wants 264kHz. A third of the notes in a real tune were
     * landing on the same ceiling, so the whole top of the keyboard played
     * flat and at one pitch. */
    if(r < 172)
        r = 172;              /* 44100 >> 8, the lowest OCT can reach */
    if(r > 5644800)
        r = 5644800;          /* 44100 << 7, the highest */

    return (uint32_t)r;
}

/* Velocity, channel volume and expression are independent scalings and all
 * three apply. Kept in one place because a controller change has to arrive at
 * exactly the same number a note-on would have produced. */
static int chan_level(const ps_audio *a, int chan, int vel)
{
    int v = (int)(((uint32_t)vel * a->cc_volume[chan] * a->cc_expr[chan] *
                   a->volume) / (127u * 127u * 127u));

    return v > 255 ? 255 : v;
}

/* CC10 is 0..127 with 64 centre; the backend wants 0..255 with 128 centre,
 * and 64 must land exactly on centre rather than one step off it. */
static int chan_pan(const ps_audio *a, int chan)
{
    int p = a->cc_pan[chan];

    return (p <= 64) ? p * 2 : 128 + (p - 64) * 127 / 63;
}

/* Re-levels every voice already sounding on a channel. */
static void refresh_channel(ps_audio *a, int chan)
{
    int i, pan = chan_pan(a, chan);

    for(i = 0; i < a->nvoices; i++) {
        if(a->voices[i].active && a->voices[i].midi_chan == chan)
            ps_voice_set_level(i, chan_level(a, chan, a->voices[i].vel), pan);
    }
}

/* How long a keyed-off note is left alone to finish its hardware release.
 * Reusing the channel before then re-keys it mid-fade and clips the tail,
 * which is audible as the mix going hollow between notes. */
#define PS_RELEASE_MS 350

static void note_on(ps_audio *a, int chan, int note, int vel)
{
    ps_sample *s;
    int i, slot = -1;

    if(a->muted || note < 0 || note > 127)
        return;

    if(chan == PS_MIDI_DRUM_CHANNEL)
        s = &a->drums[note];
    else
        s = &a->melodic[a->program[chan]];

    if(!make_resident(a, s)) {
        a->stat_skipped++;
        return;
    }
    a->stat_played++;

    if(a->nvoices <= 0)
        return;

    for(i = 0; i < a->nvoices; i++) {
        if(!a->voices[i].active) {
            slot = i;
            break;
        }
    }

    /* Nothing idle: a voice still fading out is the cheapest thing to take,
     * since its note is already over. */
    if(slot < 0) {
        int best = -1, i2;

        for(i2 = 0; i2 < a->nvoices; i2++) {
            if(!a->voices[i2].released)
                continue;
            if(best < 0 || a->voices[i2].remain_ms < a->voices[best].remain_ms)
                best = i2;
        }
        slot = best;
    }

    /* All busy: steal the one closest to finishing rather than always slot
     * zero, which would cut the same voice over and over. */
    if(slot < 0) {
        int best = 0, i2;

        for(i2 = 1; i2 < a->nvoices; i2++) {
            int r  = a->voices[i2].remain_ms;
            int rb = a->voices[best].remain_ms;

            if(rb < 0 || (r >= 0 && r < rb))
                best = i2;
        }
        slot = best;
        voice_stop(a, slot);
    }

    {
        int vol = chan_level(a, chan, vel);
        int pan = chan_pan(a, chan);
        int freq;

        freq = (int)pitch_rate(s->rate, note, s->root_note);

        ps_voice_play(slot, s->smp, a->fmt, s->loop, s->loop_start,
                      s->frames, (uint32_t)freq, vol, pan);

        /* A one-shot finishes on its own; without knowing when, the slot
         * would stay marked busy forever and every later note would steal
         * slot zero, cutting itself off almost immediately. */
        a->voices[slot].remain_ms = s->loop ? -1
                                            : (int)((uint64_t)s->frames * 1000
                                                    / (freq > 0 ? freq
                                                                : (int)s->rate));
        a->voices[slot].sample    = s;
        a->voices[slot].midi_chan = (uint8_t)chan;
        a->voices[slot].vel       = (uint8_t)vel;
        a->voices[slot].released  = 0;
        a->voices[slot].note      = (uint8_t)note;
        a->voices[slot].active    = 1;
    }
}

static void note_off(ps_audio *a, int chan, int note)
{
    int i;

    for(i = 0; i < a->nvoices; i++) {
        if(!a->voices[i].active || a->voices[i].released ||
           a->voices[i].midi_chan != chan || a->voices[i].note != note)
            continue;

        /* GM percussion ignores note-off: a drum hit is a whole event and
         * its own decay is the ending. Everything else releases, which the
         * hardware envelope turns into a short fade rather than the hard cut
         * that used to truncate every note to a stub.
         *
         * Melodic one-shots must release too. The bank holds one sample per
         * instrument recorded at middle C, so a bass note is that sample
         * played four times slower - 2.4 seconds of it. Letting that ring
         * past the key release buries the mix in low end. */
        if(chan == PS_MIDI_DRUM_CHANNEL)
            return;

        /* Key off so the hardware envelope takes over, but hold the slot for
         * the length of that fade. Freeing it here lets the next note re-key
         * the same channel and cut the tail off. */
        ps_voice_stop(i);
        a->voices[i].released  = 1;
        a->voices[i].remain_ms = PS_RELEASE_MS;
        return;
    }
}

void ps_audio_tick(ps_audio *a, int dt_ms)
{
    uint64_t ticks_x1000;
    int      i;

    if(!a)
        return;

    /* Retire finished one-shots so their slots come back. */
    for(i = 0; i < a->nvoices; i++) {
        if(!a->voices[i].active || a->voices[i].remain_ms < 0)
            continue;

        a->voices[i].remain_ms -= dt_ms;
        if(a->voices[i].remain_ms <= 0) {
            voice_stop(a, i);
            a->voices[i].released = 0;
        }
    }

    if(!a->playing || !a->events || a->tempo_us == 0)
        return;

    /* ticks = ms * 1000 * division / tempo_us, kept in thousandths so a slow
     * tempo at 60fps does not round down to zero every frame. */
    ticks_x1000 = (uint64_t)dt_ms * 1000 * 1000 * a->division / a->tempo_us;

    a->tick_frac += (uint32_t)ticks_x1000;
    a->tick      += a->tick_frac / 1000;
    a->tick_frac %= 1000;

    {
        static int report = 0;

        report += dt_ms;
        if(report >= 5000) {
            report = 0;
            printf("popsurf: audio %u played, %u skipped\n",
                   (unsigned)a->stat_played, (unsigned)a->stat_skipped);
            fflush(stdout);
        }
    }

    while(a->cursor < a->nevents && a->events[a->cursor].tick <= a->tick) {
        const ps_mev *e = &a->events[a->cursor++];

        switch(e->status) {
        case 0x90: note_on(a, e->chan, e->a, e->b); break;
        case 0x80: note_off(a, e->chan, e->a);      break;
        case 0xc0: a->program[e->chan] = e->a;      break;
        case 0xb0:
            switch(e->a) {
            case PS_CC_VOLUME:     a->cc_volume[e->chan] = e->b; break;
            case PS_CC_EXPRESSION: a->cc_expr[e->chan]   = e->b; break;
            case PS_CC_PAN:        a->cc_pan[e->chan]    = e->b; break;
            default:               goto done_cc;
            }
            /* Expression in particular is continuous - this file sends 1296
             * of them - and a swell only exists if it reaches the notes that
             * are already sounding. */
            refresh_channel(a, e->chan);
done_cc:
            break;
        case 0xff: {
            uint32_t t = ev_tempo(e);

            if(t)
                a->tempo_us = t;
            break;
        }
        default: break;
        }
    }

    if(a->cursor >= a->nevents) {
        int i;

        printf("popsurf: audio %u notes played, %u skipped\n",
               (unsigned)a->stat_played, (unsigned)a->stat_skipped);
        fflush(stdout);

        for(i = 0; i < a->nvoices; i++)
            voice_stop(a, i);

        if(a->loop) {
            a->cursor    = 0;
            a->tick      = 0;
            a->tick_frac = 0;
            a->tempo_us  = 500000;
            reset_channels(a);
        }
        else {
            a->playing = 0;
        }
    }
}


void ps_audio_set_volume(ps_audio *a, int vol)
{
    if(!a)
        return;
    a->volume = vol < 0 ? 0 : (vol > 255 ? 255 : vol);
}

int ps_audio_get_volume(const ps_audio *a)
{
    return a ? a->volume : 0;
}

void ps_audio_set_muted(ps_audio *a, int muted)
{
    int i;

    if(!a)
        return;

    a->muted = muted;
    if(muted) {
        for(i = 0; i < a->nvoices; i++)
            voice_stop(a, i);
    }
}

int ps_audio_is_muted(const ps_audio *a)
{
    return a ? a->muted : 0;
}

int ps_audio_is_playing(const ps_audio *a)
{
    return a && a->playing;
}
