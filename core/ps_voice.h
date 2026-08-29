/* Hardware voice backend for sample residency and playback. */
#ifndef PS_VOICE_H
#define PS_VOICE_H

#include "ps_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* An uploaded sample. Zero is the null handle, so a zeroed struct is
 * naturally "not resident" without a sentinel constant to remember. */
typedef uint32_t ps_smp;
#define PS_SMP_NONE ((ps_smp)0)

/* Values match the AICA PCMS field. */
#define PS_FMT_PCM16 0
#define PS_FMT_PCM8  1
#define PS_FMT_ADPCM 2

/* Reserves hardware voices. Returns the number actually available, which may
 * be fewer than requested if the host application is already using some.
 * Zero means no audio. */
int ps_voice_init(int want);

void ps_voice_shutdown(void);

/* Uploads and keys a fixed test tone, reporting whether the sample survived
 * the trip into sound memory. Diagnostic only: it answers "does this backend
 * reach the hardware", which is otherwise indistinguishable from "the
 * sequencer is choosing bad parameters" when the result is silence. */
void ps_voice_selftest(void);

/* Copies a sample into whatever memory the hardware plays from - on
 * Dreamcast, the 2MB of SPU RAM that is separate from main memory. Returns
 * PS_SMP_NONE when there is no room, which is the caller's signal to evict
 * something and retry rather than an error. */
ps_smp ps_voice_upload(const void *data, size_t len);
void   ps_voice_release(ps_smp s);

/* Reserves sound memory without filling it, and writes into a piece of it
 * later. This is what a stream needs and ps_voice_upload cannot give it: a
 * ring buffer is written to over and over while a voice reads from it, and
 * the whole point is that the decoded audio never exists in main RAM as a
 * unit. Returns silence-filled memory, so a voice keyed on it before the
 * first refill plays nothing rather than whatever was there before.
 *
 * off and len are bytes and must both be multiples of 32: the transfer into
 * sound memory goes through the SH-4's store queues, which move 32 bytes at a
 * time. ps_voice_write returns non-zero if they are not. */
ps_smp ps_voice_alloc(size_t len);
int    ps_voice_write(ps_smp s, uint32_t off, const void *data, size_t len);

/* Starts slot playing. freq is the playback rate in Hz after pitch shifting,
 * vol is 0..255 linear, pan is 0..255 with 128 centred.
 *
 * loop_start is where a looping sample restarts from. It is not normally
 * zero: an instrument's attack plays once and only the sustain repeats, so the
 * loop begins after the transient.
 *
 * frames is the sample length and is always required, not just when looping:
 * on hardware that plays from a start address to an end address, the end
 * address is where a one-shot stops. Passing zero for it does not mean
 * "no loop", it means "play nothing". */
void ps_voice_play(int slot, ps_smp s, int fmt, int loop, uint32_t loop_start,
                   uint32_t frames, uint32_t freq, int vol, int pan);

/* Same programming, but the voice is left ready rather than sounding; it
 * starts on the next ps_voice_key.
 *
 * A stereo stream is two mono voices, and if they are keyed one after another
 * they start a few hundred microseconds apart and stay that far out of phase
 * for the whole track - which on anything with content common to both channels
 * is audible as a hollow, filtered sound rather than as a delay. Arming both
 * and keying once starts them on the same sample. */
void ps_voice_arm(int slot, ps_smp s, int fmt, int loop, uint32_t loop_start,
                  uint32_t frames, uint32_t freq, int vol, int pan);

/* Starts every armed voice at once. Voices already sounding are untouched:
 * the hardware acts on a change of key state, not on the trigger itself. */
void ps_voice_key(int slot);

/* How far into its sample a voice has reached, in frames, or -1 if the
 * hardware cannot say.
 *
 * A stream has to know where the play cursor is before it can decide which
 * part of its ring is safe to overwrite. Estimating it from elapsed time does
 * not work well enough: the AICA's pitch is a quantised floating-point field,
 * so its real playback rate is a fraction of a percent off whatever was asked
 * for, and over a three-minute track that error grows to whole seconds - long
 * enough for the write cursor to lap the read cursor and shred the audio. */
int ps_voice_pos(int slot);

/* Changes the level and pan of a sounding voice without re-keying it. MIDI
 * expression is continuous - a file may send hundreds of CC11 events to shape
 * one phrase - and re-triggering the sample on each would turn a smooth swell
 * into a machine gun. */
void ps_voice_set_level(int slot, int vol, int pan);

void ps_voice_stop(int slot);

/* Silences a voice immediately rather than releasing it.
 *
 * ps_voice_stop lets the hardware envelope fade the note out, which is what a
 * key release should sound like - but the voice goes on reading its sample for
 * the length of that fade. When the sample is about to be freed, that memory
 * may be handed to a different instrument while the fade is still running, and
 * the note finishes as whatever landed there. Use this wherever samples are
 * about to be released: a page change, a new tune, teardown. */
void ps_voice_kill(int slot);

#ifdef __cplusplus
}
#endif

#endif /* PS_VOICE_H */
