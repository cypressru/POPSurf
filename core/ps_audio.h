/* Background audio: Standard MIDI playback on the AICA.
 *
 * The Dreamcast has 2MB of SPU RAM separate from its 16MB of main memory, and
 * the AICA provides 64 hardware PCM voices with per-voice pitch, volume, pan
 * and looping, decoding 4-bit Yamaha ADPCM in hardware. So a General MIDI
 * sample bank lives entirely in sound RAM at no cost to the page budget, and
 * the SH-4 only has to parse the file and issue note on/off.
 *
 * That is how the machine was meant to do this, and it is why the original
 * browsers could play a .mid on the same 16MB we have. A software synthesiser
 * would spend main RAM on samples and CPU on mixing while a dedicated
 * 64-voice sampler sat idle.
 *
 * The sequencer is pure integer and allocation-light: a parsed MIDI file is a
 * few KB of events, and playback state is a fixed table of voices.
 */
#ifndef PS_AUDIO_H
#define PS_AUDIO_H

#include "ps_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "ps_config.h"

/* Per-console, because the ceiling is hardware. See ps_config.h. */
#define PS_AUDIO_VOICES PS_CFG_AUDIO_VOICES

/* The top voices are not the sequencer's to allocate; they belong to streamed
 * audio (ps_adxstream.h), which needs one per channel held for the whole track.
 *
 * The sequencer steals voices when it runs out, and that is the right answer
 * for notes - the worst case is one clipped tail. It is the wrong answer for a
 * stream: a stolen stream voice does not recover, because nothing keys it
 * again. Two, so a stereo track fits. */
#define PS_AUDIO_STREAM_SLOTS 2

/* And the next ones down belong to a Flash movie's soundtrack
 * (swf/ps_swf_track.h), for the same reason and with the same arithmetic: two
 * for a stereo ring, and two more for event sounds, which overlap each other
 * but must never be taken by a note.
 *
 * Four off the sequencer's pool leaves 34 on the Dreamcast profile, which is
 * still clear of the mid-twenties peak polyphony real General MIDI files
 * reach - so this costs nothing that can be heard, whereas a movie whose
 * soundtrack is stolen mid-page is silence with nothing left to restart it.
 *
 * It is a fixed reservation rather than a shared pool because sharing would
 * mean the sequencer had to know which of its voices it may not steal, and
 * that knowledge belongs in one place rather than two. */
#define PS_AUDIO_SWF_SLOTS 4

/* GM percussion lives on channel 10 (index 9) and indexes samples by note
 * rather than by program. */
#define PS_MIDI_DRUM_CHANNEL 9

#define PS_MIDI_CHANNELS 16

typedef struct ps_audio ps_audio;

ps_audio *ps_audio_create(void);
void      ps_audio_destroy(ps_audio *a);

/* Installs a sample bank fetched at runtime. The browser ships no soundfont:
 * one is downloaded on the first page that wants music and kept for the
 * session. Takes ownership of data, which must come from malloc.
 *
 * The bank stays in main RAM and individual samples are uploaded to SPU RAM
 * only while they are needed. That avoids the startup cost of pushing an
 * entire bank across, keeps the bank free to be larger than the 2MB of sound
 * RAM, and means one sequential read instead of a seek per instrument - which
 * on a GD-ROM is the difference between instant and tens of seconds. */
int ps_audio_set_bank(ps_audio *a, void *data, size_t len);
int ps_audio_has_bank(const ps_audio *a);

/* Starts a Standard MIDI File. The buffer is parsed and not retained. Any
 * previous tune stops. Returns 0 on success. */
int  ps_audio_play_midi(ps_audio *a, const void *data, size_t len, int loop);
void ps_audio_stop(ps_audio *a);

/* Drives the sequencer. Wall-clock, so tempo is independent of frame rate. */
void ps_audio_tick(ps_audio *a, int dt_ms);

/* 0..255. Applies to everything, and persists across tunes. */
void ps_audio_set_volume(ps_audio *a, int vol);
int  ps_audio_get_volume(const ps_audio *a);

void ps_audio_set_muted(ps_audio *a, int muted);
int  ps_audio_is_muted(const ps_audio *a);

int  ps_audio_is_playing(const ps_audio *a);

#ifdef __cplusplus
}
#endif

#endif /* PS_AUDIO_H */
