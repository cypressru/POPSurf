/* Streamed ADX playback: a looping voice over a ring buffer in sound memory.
 *
 * This is a different shape from ps_audio and deliberately not part of it. The
 * sequencer's job is to decide which of a fixed set of resident samples to key
 * and when; a stream has one sample that is never resident and never stops
 * changing. Forcing one through the other would give the sequencer a voice it
 * must never allocate and the stream a tick it must never miss.
 *
 * The memory argument, which drives everything else here:
 *
 * Two minutes of 22kHz stereo is about ten megabytes of PCM16. The Dreamcast
 * has sixteen, no virtual memory and no swap, and a page already costs two to
 * six of them, so decoded audio can never exist as a whole - not in main RAM,
 * and not in the AICA's 2MB either. What exists instead is a ring in sound
 * memory holding around a second and a half, refilled from the main loop a
 * chunk at a time as the AICA's play cursor moves past.
 *
 * Peak main-RAM cost of a playing stream:
 *
 *   compressed file    up to PS_CFG_MAX_ADX_BYTES (1MB on Dreamcast)
 *   staging buffer     16KB - one chunk of PCM16, both channels
 *   stream state       under 256 bytes
 *
 * so everything except the compressed file is 17KB, flat, regardless of how
 * long the track is. The file itself is the one unbounded-looking term and is
 * the reason for the cap: the loader hands over whole responses, and there is
 * no progressive path through it today (see the note on refills below), so a
 * track larger than the cap is refused rather than attempted. 1MB is about
 * forty seconds of 22kHz stereo, which is what period web background music
 * actually is.
 *
 * Sound-memory cost is 64KB per channel, 128KB for stereo, out of 2MB that
 * costs the page budget nothing.
 */
#ifndef PS_ADXSTREAM_H
#define PS_ADXSTREAM_H

#include "ps_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ps_adxstream ps_adxstream;

ps_adxstream *ps_adx_stream_create(void);
void          ps_adx_stream_destroy(ps_adxstream *st);

/* Starts a track. Takes ownership of data, which must come from malloc, and
 * frees it on every path including refusal - so the caller hands it over and
 * forgets it, exactly like ps_audio_set_bank.
 *
 * Anything already playing stops first. Returns 0 when the stream is sounding
 * and non-zero when it is not, in which case the page is simply silent: a
 * refused or unplayable track is never a reason to fail a page. */
int  ps_adx_stream_play(ps_adxstream *st, void *data, size_t len, int loop);

/* Stops and releases everything, including the compressed file. Safe to call
 * on an idle stream, which is what makes it usable straight from a navigation
 * handler. */
void ps_adx_stream_stop(ps_adxstream *st);

/* Refills the ring behind the play cursor. Must be called from the main loop;
 * a stream that is not ticked for longer than the ring holds repeats the
 * second it already has, which is the failure mode this trades for never
 * touching the network or the heap from an interrupt. */
void ps_adx_stream_tick(ps_adxstream *st, int dt_ms);

int  ps_adx_stream_is_playing(const ps_adxstream *st);

/* 0..255, applied to both channels. Persists across tracks. */
void ps_adx_stream_set_volume(ps_adxstream *st, int vol);

#ifdef __cplusplus
}
#endif

#endif /* PS_ADXSTREAM_H */
