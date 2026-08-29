/* SWF sound: whole samples the timeline triggers, and audio cut up between
 * frames. Two mechanisms, not one, and conflating them is the first mistake
 * available here.
 *
 *   Event sounds.  DefineSound carries an entire sample as a character.
 *                  StartSound names one from a frame and says how to play it:
 *                  a loop count, an in and an out point, and a volume envelope.
 *                  Nothing about it is tied to the frame rate - once triggered
 *                  it runs to its own length, and the timeline may well have
 *                  moved on or stopped.
 *
 *   Stream sounds. SoundStreamHead declares a format once and states how many
 *                  samples a frame is worth. SoundStreamBlock then carries the
 *                  audio itself, one block sitting in the tag stream between
 *                  the frame it belongs to and the ShowFrame that ends it. The
 *                  per-frame pacing is the whole point: this is how Flash keeps
 *                  music aligned to a timeline that may be dropping frames.
 *
 * Which tags, and which version each arrived in: DefineSound (14), StartSound
 * (15), SoundStreamHead (18) and SoundStreamBlock (19) are all SWF 1;
 * SoundStreamHead2 (45) is SWF 3 and differs from 18 only in which combinations
 * of codec and sample size it is permitted to declare - the encoding is
 * identical, so both are read the same way. DefineButtonSound (17, SWF 2) is
 * not read: which of its four sounds plays is a function of where the pointer
 * is, and that is not a question this phase can answer. StartSound2 (89) is
 * SWF 9 and out of range entirely.
 *
 * Codecs. Uncompressed PCM at 8 and 16 bits is decoded, as is Flash ADPCM,
 * which is what real content of this era actually uses. MP3 (SWF 4),
 * Nellymoser (SWF 6) and Speex (SWF 10) are parsed - the tag is read, the
 * geometry is recorded, the frame-to-sample map still comes out right - and
 * then declined, with `decodable` clear and ps_swf_sound_fmtname saying which.
 * Declining is deliberate rather than pending: an MP3 decoder is a separate
 * body of code with a separate licensing question, and guessing at audio is
 * worse than admitting there is none.
 *
 * On the endianness rule, because it is the one PCM detail that is silent when
 * wrong: codec 0 is "uncompressed" with the byte order left as whatever the
 * authoring machine used, which is why SWF 4 added codec 3 to mean explicitly
 * little-endian. Both are read as little-endian here. There is no field that
 * says which, so the only alternative is a guess, and every file that has ever
 * been observed in the wild is little-endian because the authoring tool ran on
 * x86. Eight-bit PCM is unsigned in both, centred on 128, and is widened to
 * signed 16 on the way out.
 *
 * This walks the tag stream itself rather than adding cases to the walk in
 * ps_swf_parse.c. That is not tidiness: the display-list walk abandons the rest
 * of the file on the first malformed shape, which is right for geometry and
 * wrong for audio - a movie whose art is truncated still has a soundtrack, and
 * a movie with one bad sound tag still has its art. Two walks make the two
 * failures independent, and cost one extra pass over bytes already in memory.
 *
 * Output is deinterleaved int16, one buffer per channel, which is the same
 * shape ps_adx_decode uses and for the same reason: the AICA's voices are mono,
 * so a stereo sound is two of them and interleaving would only have to be
 * undone. Nothing here allocates during decode and nothing here is sized by a
 * field the file chose.
 */
#ifndef PS_SWF_SOUND_H
#define PS_SWF_SOUND_H

#include "ps_swf_bits.h"

#include <stddef.h>
#include <stdint.h>

/* SoundFormat / StreamSoundCompression, the file's own numbering. */
#define PS_SWF_SND_PCM       0    /* byte order unstated; read little-endian */
#define PS_SWF_SND_ADPCM     1
#define PS_SWF_SND_MP3       2
#define PS_SWF_SND_PCM_LE    3
#define PS_SWF_SND_NELLY16   4
#define PS_SWF_SND_NELLY8    5
#define PS_SWF_SND_NELLY     6
#define PS_SWF_SND_SPEEX    11

/* An ADPCM packet is a fixed 4096 samples: one stored verbatim in the packet
 * header and 4095 deltas. The count is not in the file anywhere, so a reader
 * that does not know it decodes the second packet's header as four deltas and
 * a step index, and the sound turns to noise 93 milliseconds in. */
#define PS_SWF_ADPCM_PACKET 4096

/* Envelope points kept per StartSound. Flash's own Edit Envelope dialog offers
 * eight and the field that counts them is a byte, so a file may claim more;
 * further points are read and dropped exactly as gradient stops past the
 * fifteenth are, because reading is what positions the stream and storing is
 * only what lets the curve be evaluated. */
#define PS_SWF_MAX_ENVPT 8

/* One node of a volume envelope.
 *
 * `pos44` is the trap. It is a sample position at 44100Hz whatever the sound's
 * own rate is - so on an 11025Hz sound every position has to be divided by four
 * before it means anything, and a reader that skips that conversion applies the
 * whole curve in the first quarter of the sound and holds the last level for
 * the rest. Levels are 0..32768, where 32768 is unattenuated. */
typedef struct {
    uint32_t pos44;
    uint16_t left, right;
} ps_swf_envpt;

/* A DefineSound character: the encoded bytes and the geometry to read them.
 *
 * `data` is a copy. The parse-in-place contract the rest of the player uses
 * says the file image need not outlive the load, and audio is the one thing
 * large enough to make that tempting to break - but a page may hand the loader
 * a file and free it, and holding a pointer into it would be a use-after-free
 * that only sounds wrong. The copy is of the *encoded* bytes: ADPCM at four
 * bits is a quarter of the PCM it decodes to, and decoding it up front would
 * throw that saving away on a machine with 16MB. */
typedef struct {
    uint16_t id;
    uint8_t  format;
    uint8_t  rate_code;    /* 0..3, as stored */
    uint8_t  bits;         /* 8 or 16, as stored */
    uint8_t  channels;     /* 1 or 2 */
    uint8_t  decodable;    /* 0 when the codec is declined */
    uint8_t  clamped;      /* declared sample count exceeded the bytes present */
    uint32_t rate;         /* Hz */
    /* Per channel. For a codec we decode this is clamped to what the bytes can
     * actually yield, with `clamped` set if the file claimed more; for one we
     * decline it is simply what the file claimed, since there is nothing to
     * check it against. `declared` is always the file's own figure. */
    uint32_t nsample;
    uint32_t declared;
    uint8_t *data;         /* encoded, owned */
    uint32_t len;
} ps_swf_sound;

/* One StartSound, resolved against the timeline it was found on.
 *
 * `timeline` is 0 for the root and otherwise the sprite's character ID, and
 * `frame` counts ShowFrame tags within that timeline, so the pair says exactly
 * when the cue fires. A sprite's frame numbers are its own and have nothing to
 * do with the root's.
 *
 * `stop` is SyncStop, which makes this the opposite of a start: the tag names a
 * sound and asks for every instance of it to be silenced. It is stored in the
 * same list because it arrives on a frame like any other cue and a player has
 * to act on the two in file order. */
typedef struct {
    uint16_t id;
    uint16_t timeline;
    uint32_t frame;
    uint8_t  stop;
    uint8_t  no_multiple;  /* do not retrigger if already playing */
    uint8_t  has_in, has_out;
    uint8_t  nenv;
    uint16_t loops;        /* LoopCount; 0 and 1 both mean play once */
    uint32_t in, out;      /* sample positions in the sound's own rate */
    ps_swf_envpt env[PS_SWF_MAX_ENVPT];
} ps_swf_sndstart;

/* One SoundStreamBlock, placed on the frame it arrived on.
 *
 * `first` is what the streaming half of this file exists to produce: the sample
 * offset, from the start of the stream, at which this block's audio begins. It
 * is a running total of what the blocks actually decode to rather than a
 * multiple of the head's declared rate, because the head's figure is an average
 * and an encoder is free to vary the real count from frame to frame. */
typedef struct {
    uint32_t frame;
    uint32_t first;
    uint32_t nsample;      /* per channel, in this block */
    uint32_t off, len;     /* bytes, into the stream's data buffer */
    uint8_t  damaged;      /* holds less audio than the frame is worth */
} ps_swf_sndblock;

/* A stream: one per timeline, which is all the format allows.
 *
 * The playback fields are separate from the stream fields on purpose. A file
 * may declare 8-bit mono audio played back as 16-bit stereo, and the pair is
 * how Flash says "decode it like this, then hand it to the mixer like that".
 * Only the stream fields describe the bytes.
 *
 * `gaps` and `dups` are what turn "the block count disagrees with the frame
 * count" into something a caller can test. A stream should carry exactly one
 * block per frame across its span; a missing one is a silent gap the player has
 * to fill, and two on one frame is a file that cannot be paced at all. */
typedef struct {
    uint16_t timeline;
    uint8_t  head_tag;     /* 18 or 45, kept because the constraints differ */
    uint8_t  format;
    uint8_t  rate_code;
    uint8_t  bits;
    uint8_t  channels;
    uint8_t  decodable;
    uint8_t  play_rate_code;
    uint8_t  play_bits;
    uint8_t  play_channels;
    uint32_t rate, play_rate;
    uint32_t spf;           /* StreamSoundSampleCount: samples per frame */
    int16_t  latency_seek;  /* MP3 only, and only recorded */

    uint32_t nframe;        /* ShowFrames in the owning timeline */
    uint32_t gaps;          /* frames inside the span carrying no block */
    uint32_t dups;          /* frames carrying more than one */

    /* How many blocks have `damaged` set: either their bytes do not divide into
     * whole samples, or they carry materially less audio than the head says a
     * frame is worth. The last block is exempt from the second test because a
     * track is allowed to end part way through a frame.
     *
     * There is no way to tell a cut-short ADPCM block from a legitimately short
     * one by its bytes alone - four-bit codes divide a byte evenly, so losing a
     * byte just loses two samples in silence - so the head's own declared rate
     * is the only thing left to check a block against. A sample of slack each
     * way, because that rate is an integer and the true one usually is not:
     * 22050Hz at 12fps is 1837.5 samples a frame. */
    uint32_t short_blocks;

    uint8_t         *data;  /* every block's payload, concatenated, owned */
    uint32_t         len;
    ps_swf_sndblock *blocks;
    uint32_t         nblock;
    uint32_t         nsample;   /* total, per channel */
} ps_swf_sndstream;

typedef struct {
    int      version;
    float    fps;
    uint32_t nframe;        /* ShowFrames actually walked in the root */

    ps_swf_sound     *sounds;   uint32_t nsound;
    ps_swf_sndstart  *starts;   uint32_t nstart;
    ps_swf_sndstream *streams;  uint32_t nstream;
} ps_swf_audio;

/* Walks the tag stream for sound alone. Returns 0 on success, -1 if the file
 * is not a SWF this can walk at all, with a reason in err.
 *
 * A sound tag that does not parse is dropped and the walk continues, which is
 * the opposite of the shape parser's policy and right for the same reason it is
 * right there: a shape that misparses leaves the tag cursor untrustworthy,
 * whereas every tag here is located by the tag header alone and one bad cue
 * costs only itself. */
[[nodiscard]] int ps_swf_audio_load(const uint8_t *data, size_t len,
                                    ps_swf_audio *a, char *err, size_t errlen);
void ps_swf_audio_free(ps_swf_audio *a);

const ps_swf_sound *ps_swf_find_sound(const ps_swf_audio *a, uint16_t id);

/* "MP3", "Flash ADPCM", and so on. Never NULL. */
const char *ps_swf_sound_fmtname(int format);

/* Sample offset at which the block placed on `frame` of this stream begins.
 * Returns -1 when no block was placed on that frame, which is a real answer and
 * not an error - it is what a gap looks like from the caller's side.
 *
 * The nominal answer, frame * st->spf, is deliberately not what this returns.
 * The head's samples-per-frame is an average that need not divide evenly - at
 * 22050Hz and 12fps it is 1837.5, and the field is an integer - so accumulating
 * it drifts a sample every other frame, which over a two-minute track is a
 * tenth of a second of lip sync. The block table is the ground truth. */
[[nodiscard]] int ps_swf_stream_frame_sample(const ps_swf_sndstream *st,
                                             uint32_t frame, uint32_t *sample);

/* --- decoding ------------------------------------------------------------ */

/* One decode in progress. Allocation-free and copyable: everything it needs is
 * either in it or in the sound it was initialised from. */
typedef struct {
    const uint8_t *data;
    uint32_t       len;
    uint8_t        format;
    uint8_t        bits;
    uint8_t        channels;
    uint8_t        nbits;      /* ADPCM code width, 2..5 */

    uint32_t       total;      /* samples per channel this run can produce */
    uint32_t       at;         /* samples already produced */
    uint32_t       pkt_at;     /* position within the current ADPCM packet */

    int32_t        pred[2];
    int32_t        index[2];

    ps_bits        b;
} ps_swf_snddec;

/* Both return -1 when the codec is one this declines, which is the only
 * failure that is not the caller's mistake and the one that must not be
 * ignored - an uninitialised decoder reads zero samples and sounds exactly
 * like a sound that has finished. */
[[nodiscard]] int ps_swf_snddec_init(ps_swf_snddec *d, const ps_swf_sound *s);
[[nodiscard]] int ps_swf_snddec_init_block(ps_swf_snddec *d,
                                           const ps_swf_sndstream *st,
                                           uint32_t block);

/* Positions so the next read produces `sample`. Returns -1 past the end.
 *
 * For ADPCM this is not a seek in the usual sense. The predictor state at any
 * sample is the sum of every delta since the packet header, so landing exactly
 * on a sample means starting at the packet that contains it and decoding
 * forward with the output thrown away - at most 4095 samples, and it has to
 * happen or a loop that restarts mid-packet restarts with the wrong state and
 * cracks. */
[[nodiscard]] int ps_swf_snddec_seek(ps_swf_snddec *d, uint32_t sample);

/* Fills up to `want` samples per channel into out[0] and, for a stereo sound,
 * out[1]. Returns how many it actually produced, which is short only at the end
 * of the run or where the bytes ran out. */
[[nodiscard]] uint32_t ps_swf_snddec_read(ps_swf_snddec *d, int16_t *const *out,
                                          uint32_t want);

/* --- playing an event sound ---------------------------------------------- */

/* A StartSound in progress: the in and out points applied, and the loop count
 * counted down.
 *
 * This exists because of a bug the ADX streamer had, which was not an ADX bug
 * at all. Almost no sound is a whole number of decoder blocks long, so a loop
 * that restarts on a block boundary leaves a remainder at the seam - and a
 * refill that asks for the remainder and is handed nothing reads that as the
 * end of the track, so a looping tune stops after one pass. The fix is that the
 * seam is a sample position and not a block position, which means the rewind
 * has to be an exact seek and the caller must never see a short read that is
 * not genuinely the end. Both of those live here rather than in every caller. */
typedef struct {
    ps_swf_snddec dec;
    uint32_t      in, out;     /* one pass is samples [in, out) */
    uint32_t      loops_left;  /* passes still to play after the current one */
} ps_swf_sndplay;

/* -1 if the codec is declined or the in/out points describe nothing. */
[[nodiscard]] int ps_swf_sndplay_init(ps_swf_sndplay *p, const ps_swf_sound *s,
                                      const ps_swf_sndstart *ev);

/* Total samples the whole run will produce - every pass, in the sound's own
 * rate. Zero means the cue produces nothing. */
uint32_t ps_swf_sndplay_total(const ps_swf_sound *s, const ps_swf_sndstart *ev);

/* Short only at the true end of the last pass. A loop seam inside the requested
 * span is crossed without the caller being told, which is the entire point. */
[[nodiscard]] uint32_t ps_swf_sndplay_read(ps_swf_sndplay *p,
                                           int16_t *const *out, uint32_t want);

/* Envelope level at `sample`, in the sound's own rate, as 0..32768 per channel.
 * Held flat before the first point and after the last; linear between. A cue
 * with no envelope reports 32768 on both, so a caller can apply this
 * unconditionally.
 *
 * Not folded into the decoder on purpose: on the target this becomes a
 * ps_voice_set_level on a sounding voice, which the hardware applies for free,
 * and multiplying it into the samples would both cost a pass over the PCM and
 * bake it into a buffer that may be replayed at a different level. */
void ps_swf_sndplay_envelope(const ps_swf_sndstart *ev, uint32_t rate,
                             uint32_t sample, uint16_t *left, uint16_t *right);

#endif /* PS_SWF_SOUND_H */
