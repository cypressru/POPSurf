/* A movie's soundtrack, on the hardware.
 *
 * ps_swf_sound.c stops at "here are the samples and here is when they are
 * due". This is what turns that into sound on an AICA, and it is the piece
 * that was missing: every Flash file with audio played in silence because
 * nothing ever called the decoder.
 *
 * Two mechanisms arrive from the parser and they are kept apart here for the
 * same reason ps_swf_sound.h keeps them apart, but they land on two different
 * kinds of hardware resource:
 *
 *   The soundtrack goes on a ring.  Whatever the movie means as continuous
 *                  music - a SoundStreamHead/Block stream, or a DefineSound
 *                  too long to be a sting - is decoded a chunk at a time into
 *                  a looping buffer in SPU RAM, exactly the way ps_adxstream
 *                  does it. Decoded audio never exists in main RAM as a unit;
 *                  a two-minute stereo track would be ten megabytes on a
 *                  machine with sixteen and no virtual memory.
 *
 *   Stings are uploaded whole.  A short event sound is a fixed sample keyed
 *                  on a voice of its own, which is what the AICA is for and
 *                  costs no per-frame work at all. "Short" is not a taste
 *                  judgement: the loop-end register is sixteen bits, so a
 *                  sample longer than 65535 frames cannot be addressed as one
 *                  AICA sample however much memory is free.
 *
 * Pacing, which is the whole point of the streaming half. A stream block
 * belongs to a frame, and if the audio drifts from the timeline the animation
 * and its music come apart - which is worse than either being slightly late.
 * So the refill does not simply decode forward: on every chunk it asks where
 * the timeline says the audio should be, and if the decode position has
 * drifted past a quarter of a second it is pulled back to the block the
 * timeline names. Blocks are independently decodable, so that correction is
 * exact and costs one decoder init.
 *
 * That single rule also makes a looping movie work with no extra code. Flash
 * playheads wrap, so when the frame goes back to zero the drift is the whole
 * track and the next refill resyncs to the beginning. A stream that has run
 * past its last block goes quiet rather than stopping, waiting for the wrap.
 *
 * Voices. PS_AUDIO_SWF_SLOTS of them, taken off the top of the pool below the
 * two the ADX streamer holds, and never shared with the sequencer: a stolen
 * note clips, a stolen stream never comes back. Two go to the ring, so a
 * stereo soundtrack fits, and two to event sounds. When a third cue arrives
 * with both event voices busy the one closest to finishing is taken, which is
 * the policy ps_audio.c already uses for notes and is worth having only one of.
 *
 * What this does about a page that is already playing music: a movie that puts
 * anything on the ring is claiming to be the page's soundtrack, and the caller
 * is expected to stop <bgsound> when ps_swf_track_has_soundtrack says so. Two
 * tunes at once in different keys is not a feature. Event sounds do not make
 * that claim and do not silence anything - a MIDI loop under a Flash button
 * that clicks is exactly how these pages were built.
 *
 * Peak main-RAM cost of a playing movie soundtrack:
 *
 *   retained encoded audio   up to PS_CFG_MAX_SWF_AUDIO_BYTES
 *   ring staging buffer      16KB - one chunk of PCM16, both channels
 *   event staging            transient, freed before the upload returns
 *   track state              under 1KB
 *
 * Sound memory: 64KB per ring channel, plus up to 128KB per sounding event
 * sound, out of 2MB that costs the page budget nothing.
 */
#ifndef PS_SWF_TRACK_H
#define PS_SWF_TRACK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ps_swf_track ps_swf_track;

/* NULL only when the allocation failed, which means the page is silent and
 * not that anything else should stop. */
ps_swf_track *ps_swf_track_create(void);
void          ps_swf_track_destroy(ps_swf_track *t);

/* Walks `data` for sound alone and starts whatever is due immediately.
 *
 * `data` need not outlive the call: everything retained is copied, which is
 * the contract ps_swf_sound.h states and the reason a page may hand the loader
 * a file and free it.
 *
 * Returns 0 when the file was walked - including when it turned out to have no
 * audio at all, which is the common case and not a failure - and -1 with a
 * reason in err when it is not a SWF this can walk or its audio is over
 * budget. Must not be ignored: a track left uninitialised produces silence,
 * which is indistinguishable from a movie that had nothing to play. */
[[nodiscard]] int ps_swf_track_load(ps_swf_track *t, const uint8_t *data,
                                    size_t len, char *err, size_t errlen);

/* Silences everything and releases both the sound memory and the retained
 * audio. Safe on an idle track, so a navigation handler can call it blind. */
void ps_swf_track_stop(ps_swf_track *t);

/* Advances the soundtrack. `frame` is the root timeline's current frame, which
 * is what paces the stream and what fires event cues, and dt_ms is wall clock.
 *
 * Must be called from the main loop and never from a thread, and never while
 * the loader holds the frame: this writes sound memory, and doing that while
 * the network adapter has the G2 bus is the failure that takes the machine
 * down. ps_adxstream made the same call for the same reason. A track that goes
 * unticked longer than its ring holds repeats the second and a half it already
 * has, which is a glitch rather than a crash. */
void ps_swf_track_tick(ps_swf_track *t, int dt_ms, uint32_t frame);

/* Whether the movie carried anything at all this can play. */
int ps_swf_track_has_audio(const ps_swf_track *t);

/* Whether the movie claims the page's music. True from the moment the file is
 * walked, not from the moment the first sample sounds, so a caller can stop
 * <bgsound> before the two ever overlap. */
int ps_swf_track_has_soundtrack(const ps_swf_track *t);

int ps_swf_track_is_playing(const ps_swf_track *t);

/* 0..255, applied to the ring and to every sounding event voice. Persists
 * across loads. */
void ps_swf_track_set_volume(ps_swf_track *t, int vol);

/* What the ring arithmetic actually did. This exists because every failure
 * mode of a ring sounds like a periodic click that could equally be the
 * decoder, the pitch or the transfer - so the host test asserts these numbers
 * against a simulated cursor rather than asking someone to listen. */
typedef struct {
    uint32_t resyncs;     /* times the decode position was pulled to the timeline */
    uint32_t underruns;   /* chunks that could not be filled with real audio */
    uint32_t refills;     /* chunks written since the track started */
    uint32_t ev_started;  /* event cues that reached a voice */
    uint32_t ev_dropped;  /* cues refused: undecodable, too long, or no memory */
    uint32_t ev_stolen;   /* cues that took a sounding voice */
    uint32_t ev_stopped;  /* SyncStop cues that silenced something */
    uint32_t cursor_sample; /* stream sample the play cursor is on right now */
    int32_t  drift;       /* that, minus where the timeline says it should be */
} ps_swf_track_stats;

void ps_swf_track_get_stats(const ps_swf_track *t, ps_swf_track_stats *s);

#ifdef __cplusplus
}
#endif

#endif /* PS_SWF_TRACK_H */
