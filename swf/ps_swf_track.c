/* A movie's soundtrack on the hardware. See ps_swf_track.h for the shape and
 * the reasoning; this file is the mechanism.
 *
 * The ring is lifted straight from ps_adxstream.c, deliberately and almost
 * unchanged: one hardware voice per channel keyed as an infinite loop over a
 * fixed block of sound memory, divided into chunks, with exactly one rule
 * governing writes - refill the chunk the cursor has just left, never the one
 * it is in. That keeps a ring minus one chunk of audio always ahead of the
 * cursor, needs no locking against hardware that cannot be locked against, and
 * degrades into a repeat rather than into noise when the main loop is late.
 *
 * What is new here is the pacing. ADX free-runs, because a background tune has
 * nothing to stay in step with. A SWF stream block belongs to a frame, so this
 * one asks the timeline where it should be before every refill.
 */
#include "ps_swf_track.h"

#include "ps_swf_sound.h"

#include "ps_audio.h"    /* PS_AUDIO_VOICES, PS_AUDIO_STREAM_SLOTS, ..._SWF_SLOTS */
#include "ps_config.h"
#include "ps_voice.h"

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Ring geometry, in frames per channel. The numbers are ps_adxstream's and the
 * argument is its: the AICA's loop-end and current-address fields are sixteen
 * bits, so a ring the hardware can address at all cannot exceed 65535 frames;
 * a power of two keeps the chunk arithmetic to shifts and masks; and eight
 * chunks makes a chunk 8KB, a clean 256 store-queue transfers, at the cost of
 * an eighth of the buffer held back. */
#define RING_FRAMES  32768u
#define RING_CHUNKS  8u
#define CHUNK_FRAMES (RING_FRAMES / RING_CHUNKS)
#define CHUNK_BYTES  (CHUNK_FRAMES * 2u)

/* How long the reported play cursor may sit still before it is treated as not
 * reported at all - an emulator that does not model the position registers
 * being the case that matters. Half a second is far past any real stall and
 * far short of a full ring. */
#define POS_STALL_MS 500

/* How far the decode position may drift from the timeline before it is pulled
 * back. This cannot be tight: the timeline only names a sample per frame, so
 * its own resolution is one frame of audio, and a threshold near that would
 * resync on every chunk and turn ordinary jitter into a stutter. A quarter of
 * a second is past anything the AICA's pitch quantisation can accumulate in a
 * minute and comfortably short of a listener noticing lip sync. */
#define RESYNC_MS 250

/* Longest event sound that can be keyed as a single AICA sample.
 *
 * Not a memory budget - the loop-end register is sixteen bits, so a sample
 * past this cannot be addressed by the hardware at all however much sound
 * memory is free. At 22050Hz it is just under three seconds, which is what an
 * event sound in real content actually is. Anything longer is the movie using
 * DefineSound as its soundtrack, and goes on the ring instead. */
#define EVENT_MAX_FRAMES 65535u

/* Event voices, and so also the most event sounds that can overlap: a mono cue
 * takes one and a stereo cue takes two. The other two of the reservation are
 * the ring's, and the two figures have to add up or the sequencer is either
 * being robbed or stamped on. */
#define EV_SLOTS 2

static_assert(EV_SLOTS + 2 == PS_AUDIO_SWF_SLOTS,
              "the ring's voices and the event voices must be the whole "
              "reservation");

/* Block advances one chunk's fill may make before it gives up.
 *
 * A chunk is 4096 frames and a block is a frame of audio - 1837 samples at
 * 22050Hz and 12fps, but only 183 at 5512Hz and 30fps - so twenty-odd advances
 * is normal and a few hundred is not. The bound exists because a run of blocks
 * that decode to nothing would otherwise spin here forever, and a file is free
 * to contain one. */
#define BLOCK_GUARD 512

enum { SRC_NONE = 0, SRC_STREAM, SRC_EVENT };

/* One sounding event sound. */
typedef struct {
    int      active;
    uint16_t id;
    int      nvoice;
    int      slot[2];
    ps_smp   smp[2];
    uint32_t frames;        /* one pass, per channel, as uploaded */
    uint32_t rate;
    uint32_t in;            /* where the pass starts in the sound's own space */
    uint32_t total;         /* every pass together */
    uint64_t played_us;
    uint32_t pos;           /* frames elapsed, derived from played_us */
    const ps_swf_sndstart *ev;   /* for the envelope; owned by the audio */
} evinst;

struct ps_swf_track {
    ps_swf_audio a;
    int          loaded;

    int          nslots;
    int          slot[2];        /* the ring's two voices */
    int          ev_slot[EV_SLOTS];

    /* --- the ring --- */
    int          src;
    int          ring_on;
    ps_smp       ring[2];
    uint32_t     channels;
    uint32_t     rate;
    uint32_t     slack;          /* resync threshold, in samples */
    int16_t     *stage;          /* one chunk, both channels; the only decoded
                                  * audio in main RAM at any moment */

    /* Stream source. */
    const ps_swf_sndstream *st;
    ps_swf_snddec dec;
    int           dec_ok;
    uint32_t      cur_block;

    /* Event-on-the-ring source: a DefineSound too long to key directly. */
    const ps_swf_sndstart *ring_cue;
    const ps_swf_sound    *ring_snd;
    ps_swf_sndplay         play;

    /* Where the next decoded sample sits in the source's own sample space, and
     * where each chunk of the ring started - which is what turns a hardware
     * play cursor into "the movie is currently hearing sample N". */
    uint32_t at_sample;
    uint32_t chunk_first[RING_CHUNKS];
    uint32_t next_chunk;
    int      paced;
    int      reprime;            /* the timeline jumped back; the queue is void */
    int      drain;              /* silent chunks still owed before stopping */
    int      ended;

    /* Cursor tracking. clock_frames runs regardless so the fallback is warm
     * the moment it is needed. */
    int      last_pos;
    int      stall_ms;
    int      use_clock;
    uint32_t clock_frames;

    /* --- event sounds --- */
    evinst   inst[EV_SLOTS];
    uint8_t  slot_busy[EV_SLOTS];

    uint32_t frame;
    int      have_frame;
    int      vol;

    ps_swf_track_stats stats;
};

/* --- voices -------------------------------------------------------------- */

ps_swf_track *ps_swf_track_create(void)
{
    ps_swf_track *t = (ps_swf_track *)calloc(1, sizeof *t);
    int           base;
    int           i;

    if(!t)
        return NULL;

    t->vol   = 220;
    t->drain = -1;

    /* Below the two the ADX streamer holds and above nothing: the sequencer
     * stops where these begin. A movie cannot share the sequencer's pool
     * because the sequencer steals, and a stolen ring voice is not a clipped
     * note - it is silence for the rest of the page with nothing left to key
     * it again. */
    t->nslots = ps_voice_init(PS_AUDIO_VOICES);
    base      = t->nslots - PS_AUDIO_STREAM_SLOTS - PS_AUDIO_SWF_SLOTS;

    if(base < 0) {
        t->slot[0] = t->slot[1] = -1;
        for(i = 0; i < EV_SLOTS; i++)
            t->ev_slot[i] = -1;
        return t;
    }

    t->slot[0] = base;
    t->slot[1] = base + 1;
    for(i = 0; i < EV_SLOTS; i++)
        t->ev_slot[i] = base + 2 + i;

    return t;
}

static int have_voices(const ps_swf_track *t)
{
    return t->slot[0] >= 0;
}

/* --- event instances ----------------------------------------------------- */

static void ev_release(ps_swf_track *t, int i)
{
    evinst *e = &t->inst[i];
    int     v;

    if(!e->active)
        return;

    for(v = 0; v < e->nvoice; v++) {
        int idx = e->slot[v] - t->ev_slot[0];

        /* Kill, never stop. ps_voice_stop hands the note to the hardware
         * envelope, which goes on reading the sample for the length of the
         * fade - and the sample is about to be handed back to the sound
         * allocator, where the next cue will write over it. */
        ps_voice_kill(e->slot[v]);
        ps_voice_release(e->smp[v]);

        /* An instance being torn down half-built has a slot it never got, and
         * on a machine with no memory protection an index of minus one here is
         * not a wrong answer but a corrupted one somewhere else. */
        if(idx >= 0 && idx < EV_SLOTS)
            t->slot_busy[idx] = 0;
    }
    memset(e, 0, sizeof *e);
}

static int ev_find(const ps_swf_track *t, uint16_t id)
{
    int i;

    for(i = 0; i < EV_SLOTS; i++)
        if(t->inst[i].active && t->inst[i].id == id)
            return i;
    return -1;
}

static int ev_slots_free(const ps_swf_track *t)
{
    int i, n = 0;

    for(i = 0; i < EV_SLOTS; i++)
        if(!t->slot_busy[i])
            n++;
    return n;
}

/* Milliseconds an instance still has to run, or -1 if it is not running. */
static int ev_remain_ms(const evinst *e)
{
    if(!e->active || e->rate == 0)
        return -1;
    if(e->pos >= e->total)
        return 0;
    return (int)((uint64_t)(e->total - e->pos) * 1000u / e->rate);
}

/* Takes the instance closest to finishing.
 *
 * This is the policy ps_audio.c already uses when the sequencer runs out of
 * voices, and one policy is worth more than a better second one: the audible
 * cost is bounded to the tail of whichever sting was nearest its end, rather
 * than to whichever happened to be in slot zero. Refusing the new cue instead
 * was the alternative and is worse in the case that actually happens - a cue
 * fires because the timeline reached it, and dropping it loses an event the
 * movie is built around, while losing the last tenth of a second of an older
 * one loses decay. */
static void ev_steal(ps_swf_track *t)
{
    int best = -1, i;

    for(i = 0; i < EV_SLOTS; i++) {
        if(!t->inst[i].active)
            continue;
        if(best < 0 || ev_remain_ms(&t->inst[i]) < ev_remain_ms(&t->inst[best]))
            best = i;
    }
    if(best >= 0) {
        ev_release(t, best);
        t->stats.ev_stolen++;
    }
}

/* The level the hardware should apply to one voice of an instance, folding the
 * track volume into the cue's own envelope. */
static void ev_levels(const ps_swf_track *t, const evinst *e, int *vol,
                      int *pan)
{
    uint16_t l = 32768, r = 32768;
    uint32_t within = e->frames ? (e->pos % e->frames) : 0u;

    ps_swf_sndplay_envelope(e->ev, e->rate, e->in + within, &l, &r);

    if(e->nvoice == 2) {
        vol[0] = (int)((uint32_t)t->vol * l / 32768u);
        vol[1] = (int)((uint32_t)t->vol * r / 32768u);
        pan[0] = 0;
        pan[1] = 255;
    }
    else {
        /* A mono sound has no stereo image to steer, and an envelope on one is
         * a fade rather than a pan, so the two sides are averaged rather than
         * turned into a position the file never asked for. */
        vol[0] = (int)((uint32_t)t->vol * (((uint32_t)l + r) / 2u) / 32768u);
        pan[0] = 128;
    }
}

/* Puts a cue on the ring instead of on a voice: this is the movie using
 * DefineSound as its soundtrack, which is longer than the hardware can address
 * as one sample. Declared before ev_start, which is what routes to it. */
static int  ring_start_event(ps_swf_track *t);
static void ring_release(ps_swf_track *t);

static void ev_start(ps_swf_track *t, const ps_swf_sndstart *cue)
{
    const ps_swf_sound *s = ps_swf_find_sound(&t->a, cue->id);
    ps_swf_sndplay      p;
    int16_t            *stage;
    int16_t            *ptr[2];
    evinst             *e;
    uint32_t            frames, passes, ch, got;
    uint64_t            total;
    int                 idx = -1, v, i;
    int                 vol[2], pan[2];

    if(!s || !s->decodable || !have_voices(t)) {
        t->stats.ev_dropped++;
        return;
    }

    /* SyncNoMultiple: the file asking that a cue already sounding not be
     * restarted. Retriggering it would be audible as a flam rather than as a
     * second copy, since both would be at the same offset within a frame. */
    if(cue->no_multiple && ev_find(t, cue->id) >= 0)
        return;

    if(ps_swf_sndplay_init(&p, s, cue) < 0) {
        t->stats.ev_dropped++;
        return;
    }

    frames = p.out - p.in;
    passes = p.loops_left + 1u;

    if(frames > EVENT_MAX_FRAMES) {
        /* The movie's soundtrack, shipped as one character. The ring is the
         * only thing that can play it, and there is one ring - so a retrigger
         * restarts it rather than overlapping a second copy. Restarting is
         * what a looping movie needs: the cue sits on frame zero, the timeline
         * comes back round to it, and a player that declined would play the
         * music once and leave the rest of the session silent. */
        if(cue == t->ring_cue) {
            if(t->ring_on)
                ring_release(t);
            if(ring_start_event(t) < 0)
                t->stats.ev_dropped++;
            return;
        }
        printf("popsurf: swf sound %u is %u frames, past what a voice can "
               "address; dropped\n", (unsigned)cue->id, (unsigned)frames);
        t->stats.ev_dropped++;
        return;
    }

    if(s->channels < 1 || s->channels > 2) {
        t->stats.ev_dropped++;
        return;
    }

    while(ev_slots_free(t) < (int)s->channels) {
        int before = ev_slots_free(t);

        ev_steal(t);
        if(ev_slots_free(t) == before) {   /* nothing left to take */
            t->stats.ev_dropped++;
            return;
        }
    }

    for(i = 0; i < EV_SLOTS; i++) {
        if(!t->inst[i].active) {
            idx = i;
            break;
        }
    }
    if(idx < 0) {
        t->stats.ev_dropped++;
        return;
    }

    /* One pass, decoded whole. ps_swf_sndplay_read crosses loop seams without
     * telling the caller, so asking for exactly one pass' worth is what gets
     * one pass and not one and a fraction - and the hardware does the
     * repeating from here, seamlessly, because the uploaded region is exactly
     * the loop. */
    stage = (int16_t *)memalign(32, (size_t)frames * 2u * s->channels);
    if(!stage) {
        t->stats.ev_dropped++;
        return;
    }
    memset(stage, 0, (size_t)frames * 2u * s->channels);
    for(ch = 0; ch < s->channels; ch++)
        ptr[ch] = stage + (size_t)ch * frames;
    got = ps_swf_sndplay_read(&p, ptr, frames);
    if(got == 0) {
        free(stage);
        t->stats.ev_dropped++;
        return;
    }

    e = &t->inst[idx];
    memset(e, 0, sizeof *e);
    e->id     = cue->id;
    e->rate   = s->rate ? s->rate : 22050u;
    e->frames = frames;
    e->in     = p.in;
    e->ev     = cue;
    e->nvoice = (int)s->channels;

    total     = (uint64_t)frames * passes;
    e->total  = total > 0xffffffffu ? 0xffffffffu : (uint32_t)total;

    for(v = 0; v < e->nvoice; v++) {
        int slot = -1;

        for(i = 0; i < EV_SLOTS; i++) {
            if(!t->slot_busy[i]) {
                t->slot_busy[i] = 1;
                slot = t->ev_slot[i];
                break;
            }
        }
        e->slot[v] = slot;
        e->smp[v]  = ps_voice_upload(stage + (size_t)v * frames,
                                     (size_t)frames * 2u);
        if(slot < 0 || e->smp[v] == PS_SMP_NONE) {
            /* Half an upload is worse than none: the second voice would play
             * silence against the first and the sound would arrive off-centre
             * rather than absent. */
            e->nvoice = v + 1;
            e->active = 1;
            ev_release(t, idx);
            free(stage);
            t->stats.ev_dropped++;
            return;
        }
    }
    free(stage);

    e->active = 1;
    ev_levels(t, e, vol, pan);

    /* Armed, then keyed once. Two separate key events start a stereo pair a
     * few hundred microseconds apart and leave them that far out of phase for
     * the whole sound, which on material common to both channels is audible as
     * a hollow filtered tone rather than as a delay. */
    for(v = 0; v < e->nvoice; v++)
        ps_voice_arm(e->slot[v], e->smp[v], PS_FMT_PCM16, passes > 1u, 0,
                     frames, e->rate, vol[e->nvoice == 2 ? v : 0],
                     pan[e->nvoice == 2 ? v : 0]);
    ps_voice_key(e->slot[0]);

    t->stats.ev_started++;
}

static void ev_stop_id(ps_swf_track *t, uint16_t id)
{
    int i;

    for(i = 0; i < EV_SLOTS; i++) {
        if(t->inst[i].active && t->inst[i].id == id) {
            ev_release(t, i);
            t->stats.ev_stopped++;
        }
    }
}

static void ev_tick(ps_swf_track *t, int dt_ms)
{
    int i;

    for(i = 0; i < EV_SLOTS; i++) {
        evinst *e = &t->inst[i];
        int     vol[2], pan[2], v;

        if(!e->active)
            continue;

        /* Derived from a running microsecond total rather than accumulated per
         * tick, so a 16.6ms frame at 22050Hz does not lose the 0.3 of a sample
         * that integer division drops - which over a three-minute sting is
         * seconds of envelope position. */
        e->played_us += (uint64_t)(dt_ms > 0 ? dt_ms : 0) * 1000u;
        e->pos = (uint32_t)(e->played_us * e->rate / 1000000u);

        if(e->pos >= e->total) {
            ev_release(t, i);
            continue;
        }

        /* Only a cue with an envelope pays for one. Every one of these is a
         * pair of G2 register writes under the bus lock, and a movie with four
         * sounding stings and no envelopes would be paying that sixty times a
         * second for a level that never changes. */
        if(e->ev && e->ev->nenv) {
            ev_levels(t, e, vol, pan);
            for(v = 0; v < e->nvoice; v++)
                ps_voice_set_level(e->slot[v], vol[e->nvoice == 2 ? v : 0],
                                   pan[e->nvoice == 2 ? v : 0]);
        }
    }
}

/* --- the timeline -------------------------------------------------------- */

/* Where the stream should be at `frame`, in samples from the start of the
 * stream.
 *
 * ps_swf_stream_frame_sample is the answer whenever the frame carries a block,
 * and it is deliberately the block table's figure rather than frame * spf: the
 * head's samples-per-frame is an average that need not divide evenly, and
 * accumulating it drifts a sample every other frame. The fallback below is
 * only for a gap - a frame inside the stream's span carrying no block - where
 * there is no ground truth and the average is all there is. */
static uint32_t timeline_sample(const ps_swf_track *t)
{
    const ps_swf_sndstream *st = t->st;
    uint32_t                at, i, best = 0;
    int                     have = 0;

    if(!st || st->nblock == 0)
        return 0;

    if(ps_swf_stream_frame_sample(st, t->frame, &at) == 0)
        return at;

    /* Linear, and the blocks are in frame order so it stops early. A stream
     * has one block per frame, so the worst a Flash 4 file can produce is a
     * scan of a few thousand small records, five times a second. */
    for(i = 0; i < st->nblock; i++) {
        if(st->blocks[i].frame > t->frame)
            break;
        best = i;
        have = 1;
    }
    if(!have)
        return 0;

    /* In sixty-four bits and clamped, because both terms come from the file: a
     * movie may declare a hundred thousand frames and a head may declare
     * 65535 samples in each, and the product of those two is not a number a
     * sample position fits in. */
    {
        uint64_t at64 = (uint64_t)st->blocks[best].first
                      + (uint64_t)(t->frame - st->blocks[best].frame)
                        * st->spf;

        return at64 > 0xffffffffu ? 0xffffffffu : (uint32_t)at64;
    }
}

/* --- the stream source --------------------------------------------------- */

/* Positions the decoder so the next sample read is `want`.
 *
 * Every block is independently decodable, which is what makes a resync exact
 * rather than approximate: there is no predictor state carried across a block
 * boundary to reconstruct, so landing on an arbitrary sample costs one decoder
 * init and at most one packet of throwaway ADPCM inside that block. */
static void stream_pick(ps_swf_track *t, uint32_t want)
{
    const ps_swf_sndstream *st = t->st;
    uint32_t                i, b = 0, off;

    t->dec_ok    = 0;
    t->at_sample = want;

    if(!st || st->nblock == 0)
        return;

    for(i = 0; i < st->nblock; i++) {
        if(st->blocks[i].first > want)
            break;
        b = i;
    }
    t->cur_block = b;

    off = want - st->blocks[b].first;
    if(off > st->blocks[b].nsample)
        return;                      /* past the end of the track: silence */
    if(ps_swf_snddec_init_block(&t->dec, st, b) < 0)
        return;
    if(ps_swf_snddec_seek(&t->dec, off) < 0)
        return;
    t->dec_ok = 1;
}

static int stream_next_block(ps_swf_track *t)
{
    const ps_swf_sndstream *st = t->st;

    while(st && t->cur_block + 1u < st->nblock) {
        t->cur_block++;
        if(ps_swf_snddec_init_block(&t->dec, st, t->cur_block) == 0) {
            /* Blocks are contiguous in sample space by construction - `first`
             * is a running total of what the blocks actually decode to - so
             * this is the same number the running count already holds, and
             * restating it costs nothing and survives a damaged block. */
            t->at_sample = st->blocks[t->cur_block].first;
            t->dec_ok    = 1;
            return 1;
        }
    }
    return 0;
}

static uint32_t fill_stream(ps_swf_track *t, int16_t *const *ptr)
{
    uint32_t got = 0, guard = 0;

    while(got < CHUNK_FRAMES && guard < BLOCK_GUARD) {
        int16_t *p[2];
        uint32_t n, ch;

        if(!t->dec_ok) {
            if(!stream_next_block(t))
                break;
            guard++;
        }

        for(ch = 0; ch < 2u; ch++)
            p[ch] = ptr[ch] + got;

        n = ps_swf_snddec_read(&t->dec, p, CHUNK_FRAMES - got);
        if(n == 0) {
            t->dec_ok = 0;           /* this block is spent; try the next */
            guard++;
            continue;
        }
        got          += n;
        t->at_sample += n;
    }
    return got;
}

/* --- the ring ------------------------------------------------------------ */

static void fill_chunk(ps_swf_track *t, uint32_t chunk, uint32_t queued)
{
    int16_t *ptr[2];
    uint32_t got = 0, ch;

    /* Cleared first so a short fill lands as silence rather than as the
     * previous lap of the ring, which would otherwise be the last fifth of a
     * second repeating once wherever the source ran dry. */
    memset(t->stage, 0, (size_t)CHUNK_FRAMES * 2u * 2u);
    ptr[0] = t->stage;
    ptr[1] = t->stage + CHUNK_FRAMES;

    if(t->src == SRC_STREAM && t->paced) {
        /* Where this chunk will be heard, not where it is written: everything
         * between the cursor and here has still to play, so the audio that
         * belongs in it is the timeline's position plus that queue. In the
         * steady state the difference is zero and nothing happens. */
        uint32_t want = timeline_sample(t) + queued;
        int32_t  d    = (int32_t)(t->at_sample - want);

        if(d > (int32_t)t->slack || d < -(int32_t)t->slack) {
            stream_pick(t, want);
            t->stats.resyncs++;
        }
    }

    t->chunk_first[chunk] = t->at_sample;

    if(t->src == SRC_STREAM) {
        got = fill_stream(t, ptr);
    }
    else if(t->src == SRC_EVENT) {
        got = ps_swf_sndplay_read(&t->play, ptr, CHUNK_FRAMES);
        t->at_sample += got;
        if(got < CHUNK_FRAMES)
            t->ended = 1;
    }

    if(got < CHUNK_FRAMES)
        t->stats.underruns++;

    /* A one-shot on the ring still has most of a second of audio ahead of the
     * cursor when it runs out. Pushing silence for a whole lap lets that play
     * before the voices are killed, instead of chopping the last note off.
     *
     * A paced stream never gets here, and must not: a movie's playhead wraps,
     * so a stream that has run past its last block is not finished but waiting
     * for the timeline to come back round - at which point the drift is the
     * whole track and the next refill resyncs to the beginning. Stopping the
     * ring would mean a looping movie played its soundtrack exactly once. */
    if(t->ended && t->src == SRC_EVENT && t->drain < 0)
        t->drain = (int)RING_CHUNKS;

    for(ch = 0; ch < t->channels; ch++)
        ps_voice_write(t->ring[ch], chunk * CHUNK_BYTES,
                       t->stage + ch * CHUNK_FRAMES, CHUNK_BYTES);

    t->stats.refills++;
}

/* Fills the whole ring from where the timeline now says, and starts the voices
 * again from its first frame.
 *
 * This is what a backward jump of the playhead costs, and it is a deliberate
 * choice between two losses that cannot both be avoided. The ring holds most
 * of a second of audio that has been committed but not yet heard, and a
 * playhead that jumps has just made every sample of it wrong. Correcting in
 * place - the cheap resync - leaves that queue to play out, so the movie loops
 * back to its first frame while the soundtrack is still finishing the last
 * one, and then rejoins a second and a half into the new pass with the opening
 * skipped. Every loop of a short intro would lose its own downbeat.
 *
 * Throwing the queue away instead loses the tail of the pass that just ended,
 * which is a decay rather than an attack, and puts frame zero and sample zero
 * back on the same instant. The timeline is the master here; what was queued
 * against a position the master has abandoned is not audio any more.
 *
 * Only backwards. A forward jump is a stall the machine has already lost the
 * time to, so skipping ahead in place is both right and eight times cheaper
 * than rewriting the ring. */
static void ring_reprime(ps_swf_track *t)
{
    uint32_t i, ch;

    stream_pick(t, timeline_sample(t));

    t->paced = 0;
    for(i = 0; i < RING_CHUNKS; i++)
        fill_chunk(t, i, 0);
    t->paced = 1;

    t->next_chunk   = 0;
    t->last_pos     = -1;
    t->stall_ms     = 0;
    t->clock_frames = 0;

    /* Re-keyed, which is what puts the hardware's own play cursor back to the
     * start of the ring - and armed first, because two key events a few
     * hundred microseconds apart leave a stereo pair permanently out of
     * phase. */
    for(ch = 0; ch < t->channels; ch++)
        ps_voice_arm(t->slot[ch], t->ring[ch], PS_FMT_PCM16, 1, 0, RING_FRAMES,
                     t->rate, t->vol,
                     t->channels == 1u ? 128 : (ch == 0u ? 0 : 255));
    ps_voice_key(t->slot[0]);

    /* Restated here rather than left for the next tick, because the key event
     * just moved the cursor to the start of the ring and the reading from
     * before the jump now describes a position that no longer exists. */
    t->stats.cursor_sample = t->chunk_first[0];
    t->stats.drift         = (int32_t)(t->chunk_first[0] - timeline_sample(t));
    t->stats.resyncs++;
}

static void ring_release(ps_swf_track *t)
{
    uint32_t ch;

    if(t->ring_on)
        for(ch = 0; ch < t->channels; ch++)
            ps_voice_kill(t->slot[ch]);

    for(ch = 0; ch < 2u; ch++) {
        if(t->ring[ch] != PS_SMP_NONE) {
            ps_voice_release(t->ring[ch]);
            t->ring[ch] = PS_SMP_NONE;
        }
    }

    free(t->stage);
    t->stage = NULL;

    t->ring_on      = 0;
    t->src          = SRC_NONE;
    t->paced        = 0;
    t->reprime      = 0;
    t->ended        = 0;
    t->drain        = -1;
    t->next_chunk   = 0;
    t->at_sample    = 0;
    t->dec_ok       = 0;
    t->cur_block    = 0;
    t->clock_frames = 0;
    t->stall_ms     = 0;
    t->last_pos     = -1;
    t->use_clock    = 0;
}

static int ring_open(ps_swf_track *t, uint32_t rate, uint32_t channels)
{
    uint32_t ch, i;

    if(!have_voices(t) || channels < 1u || channels > 2u || rate == 0u)
        return -1;

    t->channels = channels;
    t->rate     = rate;

    /* A quarter of a second, but never less than two frames of audio: the
     * timeline's own resolution is one frame, so a threshold below that would
     * fire on quantisation alone. */
    t->slack = rate / 4u;
    if(t->st && t->slack < t->st->spf * 2u)
        t->slack = t->st->spf * 2u;

    /* Sized for two channels whatever the source has, so a mono decoder can
     * never be handed a pointer into a buffer that was allocated for one. */
    t->stage = (int16_t *)memalign(32, (size_t)CHUNK_FRAMES * 2u * 2u);
    if(!t->stage)
        return -1;

    for(ch = 0; ch < channels; ch++) {
        t->ring[ch] = ps_voice_alloc(RING_FRAMES * 2u);
        if(t->ring[ch] == PS_SMP_NONE) {
            printf("popsurf: no sound memory for the swf ring\n");
            ring_release(t);
            return -1;
        }
    }

    t->at_sample = 0;
    t->drain     = -1;
    t->last_pos  = -1;
    t->paced     = 0;

    /* The whole ring before anything is keyed, so the first thing heard is the
     * start of the track rather than the silence ps_voice_alloc left. Unpaced,
     * because there is no cursor yet to be behind. */
    for(i = 0; i < RING_CHUNKS; i++)
        fill_chunk(t, i, 0);

    t->next_chunk = 0;
    t->drain      = -1;
    t->paced      = 1;

    for(ch = 0; ch < channels; ch++)
        ps_voice_arm(t->slot[ch], t->ring[ch], PS_FMT_PCM16, 1, 0, RING_FRAMES,
                     rate, t->vol,
                     channels == 1u ? 128 : (ch == 0u ? 0 : 255));
    ps_voice_key(t->slot[0]);

    t->ring_on = 1;
    return 0;
}

static int ring_start_stream(ps_swf_track *t)
{
    t->src = SRC_STREAM;
    stream_pick(t, 0);

    /* The stream's own rate, not its playback rate. The pair exists so a file
     * can say "decode it like this, hand it to the mixer like that", and the
     * mixer's figure is what Flash would resample to - playing the decoded
     * samples at it instead of at the rate they were coded at would transpose
     * the whole track. */
    if(ring_open(t, t->st->rate, t->st->channels) < 0) {
        t->src = SRC_NONE;
        return -1;
    }

    printf("popsurf: swf stream %u Hz %s, %u blocks, %u samples, %u gaps\n",
           (unsigned)t->st->rate, t->st->channels == 2 ? "stereo" : "mono",
           (unsigned)t->st->nblock, (unsigned)t->st->nsample,
           (unsigned)t->st->gaps);
    return 0;
}

static int ring_start_event(ps_swf_track *t)
{
    if(!t->ring_cue || !t->ring_snd)
        return -1;

    if(ps_swf_sndplay_init(&t->play, t->ring_snd, t->ring_cue) < 0)
        return -1;

    t->src = SRC_EVENT;
    if(ring_open(t, t->ring_snd->rate, t->ring_snd->channels) < 0) {
        t->src = SRC_NONE;
        return -1;
    }

    printf("popsurf: swf event sound %u on the ring, %u Hz %s, %u samples\n",
           (unsigned)t->ring_cue->id, (unsigned)t->ring_snd->rate,
           t->ring_snd->channels == 2 ? "stereo" : "mono",
           (unsigned)ps_swf_sndplay_total(t->ring_snd, t->ring_cue));
    return 0;
}

/* Where the AICA has reached in the ring, in frames, or -1 if it will not say.
 *
 * The hardware counter is authoritative and the clock is not: the AICA's pitch
 * is a quantised field, so its true rate differs from the requested one by a
 * fraction of a percent and a write cursor driven by wall time walks into the
 * read cursor within a few minutes. The clock is only ever a fallback for a
 * host that does not implement the counter at all. */
static int ring_cursor(ps_swf_track *t, int dt_ms)
{
    int pos;

    t->clock_frames += (uint32_t)((uint64_t)(dt_ms > 0 ? dt_ms : 0)
                                  * t->rate / 1000u);

    if(t->use_clock)
        return (int)(t->clock_frames % RING_FRAMES);

    pos = ps_voice_pos(t->slot[0]);
    if(pos < 0) {
        t->use_clock = 1;
        printf("popsurf: swf cursor unavailable, running on the clock\n");
        return (int)(t->clock_frames % RING_FRAMES);
    }

    if(pos == t->last_pos) {
        t->stall_ms += dt_ms > 0 ? dt_ms : 0;
        if(t->stall_ms > POS_STALL_MS) {
            t->use_clock = 1;
            printf("popsurf: swf cursor stuck at %d, running on the clock\n",
                   pos);
        }
    }
    else {
        t->stall_ms = 0;
        t->last_pos = pos;
    }

    return pos;
}

static void ring_tick(ps_swf_track *t, int dt_ms)
{
    uint32_t cur_chunk;
    int      pos, guard;

    /* Before the cursor is read at all, because rewriting the ring moves it. */
    if(t->reprime) {
        t->reprime = 0;
        if(t->src == SRC_STREAM) {
            ring_reprime(t);
            return;
        }
    }

    pos = ring_cursor(t, dt_ms);
    if(pos < 0 || pos >= (int)RING_FRAMES)
        return;

    /* What the hardware is reading right now, in the source's sample space.
     * This is the measurement the whole pacing rule turns on, and it is worth
     * keeping even when nothing is asking: an off-by-one in the chunk map is
     * inaudible until it is a click, and this number says so on the host. */
    t->stats.cursor_sample = t->chunk_first[(uint32_t)pos / CHUNK_FRAMES]
                           + (uint32_t)pos % CHUNK_FRAMES;
    if(t->src == SRC_STREAM)
        t->stats.drift =
            (int32_t)(t->stats.cursor_sample - timeline_sample(t));

    cur_chunk = (uint32_t)pos / CHUNK_FRAMES;

    /* Chase the cursor from behind. Normally this writes nothing or one chunk;
     * more than that means the main loop was held up longer than a chunk lasts
     * - the loader holds every frame while a transfer runs - and the cursor
     * has already replayed stale audio. Catching up is still the right move,
     * it just cannot un-hear the repeat. What it can do is put the stream back
     * in step with the picture, which is what the resync above is for. */
    for(guard = 0; t->next_chunk != cur_chunk && guard < (int)RING_CHUNKS;
        guard++) {
        uint32_t queued;

        if(t->drain == 0)
            break;
        if(t->drain > 0)
            t->drain--;

        queued = (t->next_chunk * CHUNK_FRAMES + RING_FRAMES - (uint32_t)pos)
                 % RING_FRAMES;
        fill_chunk(t, t->next_chunk, queued);
        t->next_chunk = (t->next_chunk + 1u) % RING_CHUNKS;
    }

    /* A whole lap of silence has been written, so everything that was still
     * queued ahead of the cursor has played. */
    if(t->drain == 0)
        ring_release(t);
}

/* --- cues ---------------------------------------------------------------- */

static void fire(ps_swf_track *t, const ps_swf_sndstart *cue)
{
    if(cue->stop)
        ev_stop_id(t, cue->id);
    else
        ev_start(t, cue);
}

/* Cues on frames in (after, upto], in file order.
 *
 * File order matters and is why this is one pass over the whole list rather
 * than a lookup per frame: a frame may carry a stop for one sound and a start
 * for another, and a player that ran all the starts before all the stops would
 * silence a sound it had just been asked to begin. */
static void fire_span(ps_swf_track *t, uint32_t after, uint32_t upto)
{
    uint32_t i;

    for(i = 0; i < t->a.nstart; i++) {
        const ps_swf_sndstart *cue = &t->a.starts[i];

        /* Only the root's. A sprite's frame numbers are its own and the player
         * exposes no sprite playhead to resolve them against, so firing them
         * off the root's frame would put every nested sound at the wrong time
         * - which is worse than not playing it. */
        if(cue->timeline != 0)
            continue;
        if(cue->frame > after && cue->frame <= upto)
            fire(t, cue);
    }
}

static void fire_at(ps_swf_track *t, uint32_t frame)
{
    uint32_t i;

    for(i = 0; i < t->a.nstart; i++) {
        const ps_swf_sndstart *cue = &t->a.starts[i];

        if(cue->timeline == 0 && cue->frame == frame)
            fire(t, cue);
    }
}

/* --- public -------------------------------------------------------------- */

void ps_swf_track_stop(ps_swf_track *t)
{
    int i;

    if(!t)
        return;

    for(i = 0; i < EV_SLOTS; i++)
        ev_release(t, i);

    ring_release(t);

    if(t->loaded)
        ps_swf_audio_free(&t->a);
    memset(&t->a, 0, sizeof t->a);

    t->loaded     = 0;
    t->st         = NULL;
    t->ring_cue   = NULL;
    t->ring_snd   = NULL;
    t->frame      = 0;
    t->have_frame = 0;
    memset(&t->stats, 0, sizeof t->stats);
}

void ps_swf_track_destroy(ps_swf_track *t)
{
    if(!t)
        return;
    ps_swf_track_stop(t);
    free(t);
}

int ps_swf_track_load(ps_swf_track *t, const uint8_t *data, size_t len,
                      char *err, size_t errlen)
{
    uint64_t held = 0;
    uint32_t i;

    if(!t)
        return -1;

    ps_swf_track_stop(t);

    if(ps_swf_audio_load(data, len, &t->a, err, errlen) < 0)
        return -1;
    t->loaded = 1;

    /* Measured after the walk rather than refused during it, because the walk
     * is what discovers the size and the peak it reaches is transient. What is
     * being capped is the resident cost: this is encoded audio held for as
     * long as the page is open, on a machine where a page already costs two to
     * five megabytes of sixteen. */
    for(i = 0; i < t->a.nsound; i++)
        held += t->a.sounds[i].len;
    for(i = 0; i < t->a.nstream; i++)
        held += t->a.streams[i].len;

    if(held > (uint64_t)PS_CFG_MAX_SWF_AUDIO_BYTES) {
        if(err && errlen)
            snprintf(err, errlen, "audio is %u KB, over the %u KB cap",
                     (unsigned)(held / 1024u),
                     (unsigned)(PS_CFG_MAX_SWF_AUDIO_BYTES / 1024u));
        ps_swf_track_stop(t);
        return -1;
    }

    /* The root's stream, if it has one. The format allows one per timeline and
     * a sprite's would need the sprite's own playhead to pace it. */
    for(i = 0; i < t->a.nstream; i++) {
        const ps_swf_sndstream *st = &t->a.streams[i];

        if(st->timeline != 0 || !st->decodable || st->nblock == 0)
            continue;
        if(st->channels < 1 || st->channels > 2 || st->rate == 0)
            continue;
        t->st = st;
        break;
    }

    if(t->st) {
        if(ring_start_stream(t) < 0)
            printf("popsurf: swf stream could not be started; movie is silent\n");
    }
    else {
        /* No stream, so the movie's soundtrack - if it has one - is a
         * DefineSound too long to key on a voice. The first such cue on the
         * root claims the ring; it starts when the timeline reaches it, not
         * here, because it is still a cue and firing it early would put it
         * ahead of the picture. */
        for(i = 0; i < t->a.nstart; i++) {
            const ps_swf_sndstart *cue = &t->a.starts[i];
            const ps_swf_sound    *s;
            ps_swf_sndplay         p;

            if(cue->timeline != 0 || cue->stop)
                continue;
            s = ps_swf_find_sound(&t->a, cue->id);
            if(!s || !s->decodable || s->channels < 1 || s->channels > 2)
                continue;
            if(ps_swf_sndplay_init(&p, s, cue) < 0)
                continue;
            if(p.out - p.in <= EVENT_MAX_FRAMES)
                continue;
            t->ring_cue = cue;
            t->ring_snd = s;
            break;
        }
    }

    return 0;
}

void ps_swf_track_tick(ps_swf_track *t, int dt_ms, uint32_t frame)
{
    if(!t || !t->loaded)
        return;

    /* Cues before the refill: a cue that fires on this frame should already be
     * keyed by the time the ring is written for it, or it lands a chunk late. */
    if(!t->have_frame) {
        t->frame      = frame;
        t->have_frame = 1;
        fire_at(t, frame);
    }
    else if(frame != t->frame) {
        /* Forwards, every frame stepped over gets its cues - a 12fps movie on
         * a loop that dropped a tick has skipped a frame, and the sound on it
         * is not optional. Backwards is a wrap or a jump, and only the frame
         * landed on fires: replaying every cue from zero on a looping movie
         * would fire the whole intro's worth at once. */
        if(frame > t->frame) {
            fire_span(t, t->frame, frame);
        }
        else {
            fire_at(t, frame);
            t->reprime = 1;
        }
        t->frame = frame;
    }

    ev_tick(t, dt_ms);

    if(t->ring_on)
        ring_tick(t, dt_ms);
}

int ps_swf_track_has_audio(const ps_swf_track *t)
{
    return t && t->loaded && (t->a.nsound || t->a.nstream);
}

int ps_swf_track_has_soundtrack(const ps_swf_track *t)
{
    return t && t->loaded && (t->st != NULL || t->ring_cue != NULL);
}

int ps_swf_track_is_playing(const ps_swf_track *t)
{
    int i;

    if(!t)
        return 0;
    if(t->ring_on)
        return 1;
    for(i = 0; i < EV_SLOTS; i++)
        if(t->inst[i].active)
            return 1;
    return 0;
}

void ps_swf_track_set_volume(ps_swf_track *t, int vol)
{
    uint32_t ch;
    int      i;

    if(!t)
        return;

    t->vol = vol < 0 ? 0 : (vol > 255 ? 255 : vol);

    if(t->ring_on)
        for(ch = 0; ch < t->channels; ch++)
            ps_voice_set_level(t->slot[ch], t->vol,
                               t->channels == 1u ? 128 : (ch == 0u ? 0 : 255));

    for(i = 0; i < EV_SLOTS; i++) {
        evinst *e = &t->inst[i];
        int     lvl[2], pan[2], v;

        if(!e->active)
            continue;
        ev_levels(t, e, lvl, pan);
        for(v = 0; v < e->nvoice; v++)
            ps_voice_set_level(e->slot[v], lvl[e->nvoice == 2 ? v : 0],
                               pan[e->nvoice == 2 ? v : 0]);
    }
}

void ps_swf_track_get_stats(const ps_swf_track *t, ps_swf_track_stats *s)
{
    if(!s)
        return;
    if(!t) {
        memset(s, 0, sizeof *s);
        return;
    }
    *s = t->stats;
}
