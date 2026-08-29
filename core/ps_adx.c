/* CRI ADX decoder. See ps_adx.h for the licensing reason this is clean-room.
 *
 * The format, as used by every Dreamcast title that streamed music:
 *
 *   0x00 u16  0x8000
 *   0x02 u16  offset of the "(c)CRI" marker; sample data starts four bytes on
 *   0x04 u8   encoding type
 *   0x05 u8   block size in bytes, per channel
 *   0x06 u8   bits per sample
 *   0x07 u8   channels
 *   0x08 u32  sample rate
 *   0x0c u32  samples per channel
 *   0x10 u16  highpass frequency
 *   0x12 u8   version
 *   0x13 u8   flags
 *
 * Everything big-endian, which is the one thing about ADX that does not match
 * the rest of this project's assets - hence the private readers below rather
 * than ps_rd_u32le.
 *
 * A block is two bytes of signed scale followed by packed 4-bit residuals,
 * high nibble first. Each residual is added to a second-order prediction:
 *
 *   sample = clamp16(nibble * scale + ((c1*prev1 + c2*prev2) >> 12))
 *
 * with c1 and c2 fixed for the whole file. Stereo interleaves whole blocks,
 * left then right, so the two channels are decoded independently and land in
 * separate buffers - which is also exactly what the AICA wants, since its
 * voices are mono and a stereo stream is two of them.
 */
#include "ps_adx.h"

#include <math.h>
#include <string.h>

static uint16_t rd_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t rd_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

/* The predictor coefficients are not stored; they are computed from the
 * highpass frequency and the sample rate, which is why two files at different
 * rates decode with different filters.
 *
 * The filter is the one a first-order highpass at that cutoff becomes when
 * folded into the second-order predictor:
 *
 *   z  = cos(2*pi*hpf/rate)
 *   a  = sqrt(2) - z,  b = sqrt(2) - 1
 *   c  = (a - sqrt(a^2 - b^2)) / b
 *   c1 = 2c, c2 = -c^2,  both scaled by 4096
 *
 * Double precision and two transcendentals are a poor fit for an SH-4, but
 * this runs once per file, not once per sample, so it costs nothing that
 * matters. Everything in the inner loop is integer.
 */
static void adx_coefs(uint32_t rate, uint32_t hpf, int32_t *c1, int32_t *c2)
{
    static const double PI = 3.14159265358979323846;
    static const double R2 = 1.41421356237309504880;
    double a, b, c, z, disc;

    /* A cutoff at or above Nyquist is not a filter, it is a corrupt or absent
     * field. 500Hz is what CRI's own encoder writes and what practically every
     * file in the wild carries, so it is the right thing to fall back to. */
    if(hpf == 0 || hpf * 2 >= rate)
        hpf = 500;

    z = cos(2.0 * PI * (double)hpf / (double)rate);
    a = R2 - z;
    b = R2 - 1.0;

    disc = (a + b) * (a - b);
    if(disc < 0.0)
        disc = 0.0;

    c = (a - sqrt(disc)) / b;

    *c1 = (int32_t)(c * 2.0 * 4096.0);
    *c2 = (int32_t)(-(c * c) * 4096.0);
}

/* Byte offset of the block holding a given sample. Also the cross-check that
 * decides whether the header's loop fields have been read at the right
 * offsets - see below. */
static uint32_t block_byte(const ps_adx_info *in, uint32_t frame)
{
    return in->data_off +
           (frame / in->block_frames) * in->block_size * in->channels;
}

/* Reads one of the two loop-block layouts and accepts it only if it is
 * self-consistent.
 *
 * The loop record moved between header versions and the published accounts of
 * where it sits disagree, which is a bad thing to guess at: read four bytes
 * from the wrong offset and the stream loops to an arbitrary point with
 * nothing to say it went wrong. But the record stores each loop point twice,
 * once as a sample index and once as a byte offset, and those two are related
 * by the block size. Recomputing one from the other turns "which layout is
 * this" into a question the file itself answers - a wrong guess essentially
 * never passes.
 */
static int read_loop(ps_adx_info *in, const uint8_t *d, uint32_t at)
{
    uint32_t start, start_b, end, end_b;

    if(at + 24 > in->data_off)
        return 0;

    start   = rd_be32(d + at + 8);
    start_b = rd_be32(d + at + 12);
    end     = rd_be32(d + at + 16);
    end_b   = rd_be32(d + at + 20);

    if(end == 0 || start >= end || end > in->frames)
        return 0;
    if(start_b != block_byte(in, start) || end_b != block_byte(in, end))
        return 0;

    in->loop_start = start;
    in->loop_end   = end;
    return 1;
}

int ps_adx_parse(const void *data, size_t len, ps_adx_info *out)
{
    const uint8_t *d = (const uint8_t *)data;
    uint32_t       copyright_off, hpf, blocks, avail;
    uint8_t        version, flags;

    if(!d || !out || len < 0x20)
        return PS_ADX_ENOTADX;

    memset(out, 0, sizeof *out);

    if(rd_be16(d) != 0x8000)
        return PS_ADX_ENOTADX;

    copyright_off = rd_be16(d + 2);
    out->data_off = copyright_off + 4;

    /* The marker sits immediately before the sample data. Requiring it costs
     * nothing and is the difference between refusing a file that merely
     * happens to start with 0x8000 and decoding noise out of it. */
    if(out->data_off < 6 || (size_t)out->data_off > len)
        return PS_ADX_ENOTADX;
    if(memcmp(d + out->data_off - 6, "(c)CRI", 6) != 0)
        return PS_ADX_ENOTADX;

    version = d[0x12];
    flags   = d[0x13];

    /* Encrypted ADX exists (Sega used it on some later titles) and there is no
     * key here. Saying so beats decoding static. */
    if(flags == 0x08 || flags == 0x09)
        return PS_ADX_EENCRYPTED;

    if(d[4] != PS_ADX_ENC_STANDARD)
        return PS_ADX_EENCODING;
    if(d[6] != 4)
        return PS_ADX_EENCODING;

    out->channels = d[7];
    if(out->channels < 1 || out->channels > PS_ADX_MAX_CHANNELS)
        return PS_ADX_ECHANNELS;

    out->block_size = d[5];
    if(out->block_size < 4 || (out->block_size & 1))
        return PS_ADX_EHEADER;
    out->block_frames = PS_ADX_BLOCK_FRAMES(out->block_size);

    out->rate = rd_be32(d + 8);
    if(out->rate < 4000 || out->rate > 48000)
        return PS_ADX_EHEADER;

    out->frames = rd_be32(d + 0x0c);
    if(out->frames == 0)
        return PS_ADX_EHEADER;

    hpf = rd_be16(d + 0x10);
    adx_coefs(out->rate, hpf, &out->coef1, &out->coef2);

    /* A short file is far more likely to be a download that was cut off than a
     * lie in the header, and playing what did arrive beats playing nothing.
     * Clamping here also means the decoder never has to bounds-check the
     * caller's buffer against the file's claims. */
    avail  = (uint32_t)(len - out->data_off);
    blocks = avail / (out->block_size * out->channels);
    if(blocks == 0)
        return PS_ADX_ETRUNCATED;
    if(out->frames > blocks * out->block_frames)
        out->frames = blocks * out->block_frames;

    if(version == 3)
        read_loop(out, d, 0x14);
    else if(version == 4)
        read_loop(out, d, 0x18);

    return PS_ADX_OK;
}

const char *ps_adx_strerror(int rc)
{
    switch(rc) {
    case PS_ADX_OK:          return "ok";
    case PS_ADX_ENOTADX:     return "not an ADX file";
    case PS_ADX_EENCODING:   return "unsupported ADX encoding";
    case PS_ADX_ECHANNELS:   return "unsupported channel count";
    case PS_ADX_EENCRYPTED:  return "encrypted ADX";
    case PS_ADX_EHEADER:     return "bad ADX header";
    case PS_ADX_ETRUNCATED:  return "truncated ADX";
    default:                 return "ADX error";
    }
}

void ps_adx_dec_init(ps_adx_dec *d, const ps_adx_info *info,
                     const void *data, size_t len)
{
    if(!d || !info || !data)
        return;

    memset(d, 0, sizeof *d);
    d->info = *info;
    d->data = (const uint8_t *)data;
    d->len  = len;
}

void ps_adx_seek(ps_adx_dec *d, uint32_t frame)
{
    if(!d)
        return;

    if(frame > d->info.frames)
        frame = d->info.frames;

    d->frame    = (frame / d->info.block_frames) * d->info.block_frames;
    d->hist1[0] = d->hist1[1] = 0;
    d->hist2[0] = d->hist2[1] = 0;
}

/* Decodes one block, updating the channel's predictor history across the whole
 * block even when only part of it is wanted - the history is the file's, not
 * the caller's, and truncating it would corrupt everything after a short read.
 */
static void decode_block(ps_adx_dec *d, uint32_t ch, const uint8_t *blk,
                         int16_t *dst, uint32_t want)
{
    int32_t  scale = (int16_t)rd_be16(blk);
    int32_t  h1 = d->hist1[ch], h2 = d->hist2[ch];
    int32_t  c1 = d->info.coef1, c2 = d->info.coef2;
    uint32_t i, n = d->info.block_frames;

    for(i = 0; i < n; i++) {
        uint8_t byte = blk[2 + (i >> 1)];
        int32_t nib  = (i & 1) ? (byte & 0x0f) : (byte >> 4);
        int32_t v;

        if(nib > 7)
            nib -= 16;

        /* Arithmetic right shift on a negative value. C99 leaves that
         * implementation-defined; every compiler this project targets defines
         * it as a floor-division by 4096, which is what the format wants. */
        v = nib * scale + ((c1 * h1 + c2 * h2) >> 12);

        if(v > 32767)
            v = 32767;
        else if(v < -32768)
            v = -32768;

        h2 = h1;
        h1 = v;

        if(i < want)
            dst[i] = (int16_t)v;
    }

    d->hist1[ch] = h1;
    d->hist2[ch] = h2;
}

uint32_t ps_adx_decode(ps_adx_dec *d, int16_t *const *out, uint32_t want)
{
    uint32_t produced = 0;

    if(!d || !out || !d->data || !want)
        return 0;

    while(produced < want) {
        uint32_t left = d->info.frames - d->frame;
        uint32_t stride, off, n, ch;

        if(left == 0)
            break;

        n = d->info.block_frames;
        if(n > left)
            n = left;
        if(produced + n > want)
            break;

        stride = d->info.block_size * d->info.channels;
        off    = block_byte(&d->info, d->frame);
        if((size_t)off + stride > d->len)
            break;

        for(ch = 0; ch < d->info.channels; ch++)
            decode_block(d, ch, d->data + off + ch * d->info.block_size,
                         out[ch] + produced, n);

        d->frame += n;
        produced += n;
    }

    return produced;
}
