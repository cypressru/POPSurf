/* CRI ADX decoder.
 *
 * ADX is Sega and CRI's ADPCM container and the format Dreamcast games used
 * for streamed music, so it is what period pages of the sonic1bba sort embed
 * for background audio. It is 4-bit ADPCM with a second-order predictor whose
 * two coefficients are derived once from the file header, which makes decoding
 * a multiply-add per sample - cheap enough that the SH-4 can keep a stream fed
 * out of the main loop without a thread.
 *
 * Written from the format description, not from any existing implementation.
 * Docs/licensing.md forbids GPL and LGPL code anywhere in libpopsurf, and every
 * widely-read ADX decoder is one or the other; a clean-room decoder is the only
 * kind that can ship inside a closed-source game.
 *
 * The shape of this API is dictated by memory, not by taste. A two-minute
 * stereo 22kHz track is about 10MB of PCM16, which is most of a Dreamcast, so
 * there is deliberately no "decode this file" entry point: the only way to get
 * samples out is to ask for a bounded number of them at a time. See
 * ps_adxstream.h for what consumes that.
 */
#ifndef PS_ADX_H
#define PS_ADX_H

#include "ps_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The only encoding this decodes. Type 2 is the fixed-coefficient variant and
 * type 4 uses an exponential scale; both are vanishingly rare and neither can
 * be verified here against a real file, so they are refused rather than
 * guessed at. AHX (0x10, 0x11) is MPEG2 audio wearing an ADX header and is a
 * different decoder entirely. */
#define PS_ADX_ENC_STANDARD 3

/* Samples one block yields per channel, for the standard 18-byte block: two
 * bytes of scale followed by sixteen bytes of nibble pairs. Held as a constant
 * only for readability - the value actually used comes from the header, since
 * the block size is a header field. */
#define PS_ADX_BLOCK_FRAMES(block_size) (((block_size) - 2) * 2)

#define PS_ADX_MAX_CHANNELS 2

typedef struct {
    uint32_t data_off;      /* first byte of block data */
    uint32_t rate;          /* Hz */
    uint32_t frames;        /* samples per channel in the whole file */
    uint32_t block_size;    /* bytes per block, per channel */
    uint32_t block_frames;  /* samples one block decodes to */
    uint32_t channels;

    /* Second-order predictor, Q12. Derived from the header's highpass
     * frequency and sample rate; see ps_adx.c. */
    int32_t coef1, coef2;

    /* Intra-file loop, in frames, taken from the header only when it
     * cross-checks against the byte offsets the header also carries.
     * loop_end == 0 means the file declares no usable loop. */
    uint32_t loop_start, loop_end;
} ps_adx_info;

#define PS_ADX_OK          0
#define PS_ADX_ENOTADX    (-1)   /* magic or the (c)CRI marker is wrong */
#define PS_ADX_EENCODING  (-2)   /* not standard ADPCM */
#define PS_ADX_ECHANNELS  (-3)   /* not mono or stereo */
#define PS_ADX_EENCRYPTED (-4)
#define PS_ADX_EHEADER    (-5)   /* self-inconsistent header */
#define PS_ADX_ETRUNCATED (-6)   /* header promises more data than exists */

/* Reads and validates the header. Touches no sample data and allocates
 * nothing, so it is safe to run on anything that arrived off the network
 * before deciding whether to commit memory to it. */
int ps_adx_parse(const void *data, size_t len, ps_adx_info *out);

const char *ps_adx_strerror(int rc);

/* Decode cursor. Holds the predictor history and nothing else - the compressed
 * bytes stay where the caller put them, and no decoded audio is retained. */
typedef struct {
    ps_adx_info    info;
    const uint8_t *data;
    size_t         len;
    uint32_t       frame;   /* next sample to produce, per channel */
    int32_t        hist1[PS_ADX_MAX_CHANNELS];
    int32_t        hist2[PS_ADX_MAX_CHANNELS];
} ps_adx_dec;

void ps_adx_dec_init(ps_adx_dec *d, const ps_adx_info *info,
                     const void *data, size_t len);

/* Repositions to the block containing frame, rounding down, and clears the
 * predictor history.
 *
 * Clearing is not optional: the history at a given point depends on every
 * sample before it, so seeking cannot reconstruct it without decoding the
 * whole file. ADX loop points are block aligned in practice, and a block
 * begins with an absolute scale, so the error from a cold predictor decays
 * within a few dozen samples. */
void ps_adx_seek(ps_adx_dec *d, uint32_t frame);

/* Decodes up to want samples per channel into out[0..channels-1], which must
 * each have room for want samples. Returns the number produced, which is zero
 * at the end of the file.
 *
 * Whole blocks only: want is rounded down to a multiple of info.block_frames,
 * except at the end of the file where a final partial block is emitted. A
 * caller that asks for less than one block gets nothing, which is why the
 * stream refills in block-multiple chunks. */
uint32_t ps_adx_decode(ps_adx_dec *d, int16_t *const *out, uint32_t want);

#ifdef __cplusplus
}
#endif

#endif /* PS_ADX_H */
