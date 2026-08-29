/* Streamed ADX playback over an AICA ring buffer. See ps_adxstream.h for the
 * memory reasoning; this file is the mechanism.
 *
 * The ring is one hardware voice per channel, keyed as an infinite loop over a
 * fixed block of sound memory. Nothing ever stops or restarts it - the audio
 * changes underneath a voice that just keeps going round, which is why the
 * refill has to stay strictly behind the play cursor and why knowing where
 * that cursor is matters more than anything else here.
 *
 * The ring is divided into fixed chunks and exactly one rule governs writing:
 * refill the chunk the cursor has just left, never the one it is in. That
 * keeps a full ring minus one chunk of audio always ahead of the cursor, needs
 * no locking against the hardware, and degrades into a repeat rather than into
 * noise if the main loop is late.
 */
#include "ps_adxstream.h"

#include "ps_adx.h"
#include "ps_audio.h"   /* PS_AUDIO_VOICES, PS_AUDIO_STREAM_SLOTS */
#include "ps_config.h"
#include "ps_voice.h"

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Ring geometry, in frames per channel.
 *
 * 32768 is not a taste decision: the AICA's loop-end and current-address
 * fields are sixteen bits, so a ring the hardware can address at all cannot
 * exceed 65535 frames, and a power of two keeps the chunk arithmetic to shifts
 * and masks. At 22050Hz it is 1.49 seconds, which is the whole lookahead
 * budget available on this machine.
 *
 * Eight chunks of 4096 frames. Bigger chunks mean fewer, larger transfers into
 * sound memory and more decode work in one frame; smaller chunks mean the
 * refill is called more often for less each time and the usable lookahead
 * grows, since one chunk of the ring is always held back. Eight is the point
 * where a chunk is 8KB - a clean 256 store-queue transfers - and the held-back
 * chunk costs an eighth of the buffer.
 */
#define RING_FRAMES  32768
#define RING_CHUNKS  8
#define CHUNK_FRAMES (RING_FRAMES / RING_CHUNKS)
#define CHUNK_BYTES  (CHUNK_FRAMES * 2)

/* How long the reported play cursor may sit still before it is treated as not
 * reported at all. A voice at 22kHz moves the cursor several hundred frames
 * per tick, so any repeat beyond a fraction of a second means the readback is
 * not working - which is exactly what happens under an emulator that does not
 * model the position registers, the same class of problem that put this
 * project's voice backend on the registers in the first place. Half a second
 * is far past any real stall and far short of a full ring. */
#define POS_STALL_MS 500

struct ps_adxstream {
    uint8_t    *file;       /* compressed, owned */
    size_t      file_len;

    ps_adx_info info;
    ps_adx_dec  dec;

    ps_smp      ring[PS_ADX_MAX_CHANNELS];
    int         slot[PS_ADX_MAX_CHANNELS];
    int         nslots;     /* voices this backend actually gave us */
    uint32_t    channels;

    /* One chunk of decoded PCM16 per channel, 32-byte aligned for the store
     * queue transfer. This is the only decoded audio that exists in main RAM
     * at any moment. */
    int16_t    *stage;

    int         playing;
    int         loop;
    uint32_t    loop_target;  /* frame to seek to when the decoder runs out */
    int         ended;        /* decoder exhausted, one-shot */
    int         drain;        /* silent chunks still to push before stopping */

    uint32_t    next_chunk;   /* the chunk the refill will write next */

    /* Cursor tracking. clock_frames runs regardless so the fallback is warm
     * the moment it is needed. */
    int         last_pos;
    int         stall_ms;
    int         use_clock;
    uint32_t    clock_frames;

    int         vol;
};

ps_adxstream *ps_adx_stream_create(void)
{
    ps_adxstream *st = (ps_adxstream *)calloc(1, sizeof *st);

    if(!st)
        return NULL;

    st->vol   = 220;
    st->drain = -1;

    /* The top slots of the voice pool, matching what ps_audio holds back. A
     * stream cannot survive being stolen the way a note can: losing a note is
     * a clipped chord, losing the stream is silence for the rest of the page. */
    st->nslots  = ps_voice_init(PS_AUDIO_VOICES);
    st->slot[0] = st->nslots - PS_AUDIO_STREAM_SLOTS;
    st->slot[1] = st->nslots - PS_AUDIO_STREAM_SLOTS + 1;

    return st;
}

/* Decodes one chunk and pushes it into the ring.
 *
 * The staging buffer is cleared first so that a short read - the last chunk of
 * a one-shot - lands as silence rather than as the previous pass through the
 * ring, which would otherwise be the last second of the track repeating once
 * as it ends. */
static void fill_chunk(ps_adxstream *st, uint32_t chunk)
{
    int16_t *ptr[PS_ADX_MAX_CHANNELS];
    uint32_t got = 0, ch;
    uint32_t seeked_at = 0xffffffffu;

    memset(st->stage, 0, (size_t)CHUNK_FRAMES * 2 * st->channels);

    while(got < CHUNK_FRAMES && !st->ended) {
        uint32_t n;

        /* Less than a block of room left. The decoder only emits whole blocks
         * except at the very end of the file, so asking again here would come
         * back empty and be mistaken for the end of the track. Leave the few
         * frames to the next chunk instead. */
        if(CHUNK_FRAMES - got < st->info.block_frames)
            break;

        for(ch = 0; ch < st->channels; ch++)
            ptr[ch] = st->stage + ch * CHUNK_FRAMES + got;

        n = ps_adx_decode(&st->dec, ptr, CHUNK_FRAMES - got);
        if(n) {
            got += n;
            continue;
        }

        /* Out of samples. A loop shorter than one chunk has to rewind several
         * times to fill it - a short jingle is a real thing to loop - so what
         * stops this spinning is not a rewind count but progress: a rewind
         * that produced nothing before running out again means the loop region
         * itself is empty, and there is nothing to be gained by trying it a
         * third time. */
        if(st->loop && got != seeked_at) {
            ps_adx_seek(&st->dec, st->loop_target);
            seeked_at = got;
            continue;
        }
        st->ended = 1;
    }

    /* Once the audio has run out, the ring still has most of a second of it
     * ahead of the cursor. Pushing silence for a whole lap lets that play out
     * before the voice is killed, instead of chopping the last note off. */
    if(st->ended && st->drain < 0)
        st->drain = RING_CHUNKS;

    for(ch = 0; ch < st->channels; ch++)
        ps_voice_write(st->ring[ch], chunk * CHUNK_BYTES,
                       st->stage + ch * CHUNK_FRAMES, CHUNK_BYTES);
}

void ps_adx_stream_stop(ps_adxstream *st)
{
    uint32_t ch;

    if(!st)
        return;

    if(st->playing) {
        /* Kill, never stop. ps_voice_stop hands the note to the hardware
         * envelope, which goes on reading the sample for the length of the
         * fade - and the sample is about to be handed back to the sound
         * allocator. See ps_voice.h. */
        for(ch = 0; ch < st->channels; ch++)
            ps_voice_kill(st->slot[ch]);
    }

    for(ch = 0; ch < PS_ADX_MAX_CHANNELS; ch++) {
        if(st->ring[ch] != PS_SMP_NONE) {
            ps_voice_release(st->ring[ch]);
            st->ring[ch] = PS_SMP_NONE;
        }
    }

    free(st->stage);
    st->stage = NULL;
    free(st->file);
    st->file     = NULL;
    st->file_len = 0;

    st->playing      = 0;
    st->ended        = 0;
    st->drain        = -1;
    st->next_chunk   = 0;
    st->clock_frames = 0;
    st->stall_ms     = 0;
    st->last_pos     = -1;
    st->use_clock    = 0;
    st->channels     = 0;
}

void ps_adx_stream_destroy(ps_adxstream *st)
{
    if(!st)
        return;

    ps_adx_stream_stop(st);
    free(st);
}

int ps_adx_stream_play(ps_adxstream *st, void *data, size_t len, int loop)
{
    uint32_t ch, i;
    int      rc;

    if(!st || !data) {
        free(data);
        return -1;
    }

    ps_adx_stream_stop(st);

    /* Refuse loudly and cheaply rather than trying and dying. There is no
     * progressive path from the loader, so playing this track means holding
     * all of it; on a machine with no memory protection, taking the browser
     * down for a background tune is not a trade worth making. */
    if(len > PS_CFG_MAX_ADX_BYTES) {
        printf("popsurf: adx %u KB exceeds the %u KB stream cap; page stays "
               "silent\n", (unsigned)(len / 1024),
               (unsigned)(PS_CFG_MAX_ADX_BYTES / 1024));
        free(data);
        return -1;
    }

    rc = ps_adx_parse(data, len, &st->info);
    if(rc != PS_ADX_OK) {
        printf("popsurf: adx rejected: %s\n", ps_adx_strerror(rc));
        free(data);
        return -1;
    }

    if(st->nslots < PS_AUDIO_STREAM_SLOTS) {
        printf("popsurf: no reserved voices for streaming\n");
        free(data);
        return -1;
    }

    st->file     = (uint8_t *)data;
    st->file_len = len;
    st->channels = st->info.channels;
    st->loop     = loop ? 1 : 0;
    st->drain    = -1;
    st->last_pos = -1;

    /* A file that declares its own loop region gets it, but only when the page
     * asked to loop at all: <bgsound loop=0> means play once, and a track that
     * then jumped back to its internal loop point would be ignoring the page. */
    if(st->loop && st->info.loop_end) {
        st->loop_target = st->info.loop_start;
        st->info.frames = st->info.loop_end;
    }
    else {
        st->loop_target = 0;
    }

    /* A looping track ends on a block boundary, dropping at most 31 frames -
     * under a millisecond and a half, and inaudible.
     *
     * That is worth more than the frames it costs. A block is the decoder's
     * unit and a loop point is where a block begins, so an end that is not
     * block aligned leaves the refill wanting a fraction of a block on every
     * lap; there is nowhere for that fraction to come from, and the honest
     * choices are a silent gap at the seam or losing it. Real files are almost
     * never a whole number of blocks long, so this is the normal case rather
     * than an edge one. */
    if(st->loop)
        st->info.frames -= st->info.frames % st->info.block_frames;

    if(st->info.frames == 0) {
        printf("popsurf: adx has no complete blocks to play\n");
        ps_adx_stream_stop(st);
        return -1;
    }

    ps_adx_dec_init(&st->dec, &st->info, st->file, st->file_len);

    st->stage = (int16_t *)memalign(32, (size_t)CHUNK_FRAMES * 2 * st->channels);
    if(!st->stage) {
        printf("popsurf: adx staging buffer failed\n");
        ps_adx_stream_stop(st);
        return -1;
    }

    for(ch = 0; ch < st->channels; ch++) {
        st->ring[ch] = ps_voice_alloc(RING_FRAMES * 2);
        if(st->ring[ch] == PS_SMP_NONE) {
            printf("popsurf: no sound memory for the adx ring\n");
            ps_adx_stream_stop(st);
            return -1;
        }
    }

    /* Fill the whole ring before keying anything, so the first thing heard is
     * the start of the track rather than the silence ps_voice_alloc left. */
    for(i = 0; i < RING_CHUNKS; i++)
        fill_chunk(st, i);
    st->next_chunk = 0;
    st->drain      = -1;     /* a short track must not start already draining */

    /* Arm both halves, then one key: a stereo pair started by two separate
     * key events sits permanently out of phase. Hard left and right, because
     * the file's two channels are a stereo image, not two instruments. */
    for(ch = 0; ch < st->channels; ch++)
        ps_voice_arm(st->slot[ch], st->ring[ch], PS_FMT_PCM16, 1, 0,
                     RING_FRAMES, st->info.rate, st->vol,
                     st->channels == 1 ? 128 : (ch == 0 ? 0 : 255));
    ps_voice_key(st->slot[0]);

    st->playing = 1;

    printf("popsurf: adx %u Hz %s, %u frames, %u KB compressed%s\n",
           (unsigned)st->info.rate, st->channels == 2 ? "stereo" : "mono",
           (unsigned)st->info.frames, (unsigned)(len / 1024),
           st->loop ? ", looping" : "");
    fflush(stdout);
    return 0;
}

/* Where the AICA has reached in the ring, in frames, or -1 if it will not say.
 *
 * The hardware counter is authoritative and the clock is not: the AICA's pitch
 * is a quantised field, so its true rate differs from the requested one by a
 * fraction of a percent, and a write cursor driven by wall time drifts into
 * the read cursor within a few minutes. The clock is only ever a fallback for
 * a host that does not implement the counter at all. */
static int ring_cursor(ps_adxstream *st, int dt_ms)
{
    int pos;

    st->clock_frames += (uint32_t)((uint64_t)dt_ms * st->info.rate / 1000);

    if(st->use_clock)
        return (int)(st->clock_frames % RING_FRAMES);

    pos = ps_voice_pos(st->slot[0]);
    if(pos < 0) {
        st->use_clock = 1;
        printf("popsurf: adx cursor unavailable, running on the clock\n");
        return (int)(st->clock_frames % RING_FRAMES);
    }

    if(pos == st->last_pos) {
        st->stall_ms += dt_ms;
        if(st->stall_ms > POS_STALL_MS) {
            st->use_clock = 1;
            printf("popsurf: adx cursor stuck at %d, running on the clock\n",
                   pos);
        }
    }
    else {
        st->stall_ms = 0;
        st->last_pos = pos;
    }

    return pos;
}

void ps_adx_stream_tick(ps_adxstream *st, int dt_ms)
{
    uint32_t cur_chunk;
    int      pos, guard;

    if(!st || !st->playing)
        return;

    pos = ring_cursor(st, dt_ms);
    if(pos < 0 || pos >= RING_FRAMES)
        return;

    cur_chunk = (uint32_t)pos / CHUNK_FRAMES;

    /* Chase the cursor from behind. Normally this writes nothing or one chunk;
     * more than that means the main loop was held up longer than a chunk lasts
     * - a page load holds every frame while a transfer runs - and the cursor
     * has already replayed stale audio. Catching up is still the right move,
     * it just cannot un-hear the repeat. */
    for(guard = 0; st->next_chunk != cur_chunk && guard < RING_CHUNKS; guard++) {
        if(st->drain == 0)
            break;
        if(st->drain > 0)
            st->drain--;

        fill_chunk(st, st->next_chunk);
        st->next_chunk = (st->next_chunk + 1) % RING_CHUNKS;
    }

    /* A whole lap of silence has been written, so everything that was still
     * queued ahead of the cursor has played. */
    if(st->drain == 0)
        ps_adx_stream_stop(st);
}

int ps_adx_stream_is_playing(const ps_adxstream *st)
{
    return st && st->playing;
}

void ps_adx_stream_set_volume(ps_adxstream *st, int vol)
{
    uint32_t ch;

    if(!st)
        return;

    st->vol = vol < 0 ? 0 : (vol > 255 ? 255 : vol);

    if(!st->playing)
        return;

    for(ch = 0; ch < st->channels; ch++)
        ps_voice_set_level(st->slot[ch], st->vol,
                           st->channels == 1 ? 128 : (ch == 0 ? 0 : 255));
}
