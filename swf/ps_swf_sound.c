/* SWF sound tags, Flash ADPCM, and the frame-to-sample map. See ps_swf_sound.h
 * for what is read and what is declined.
 *
 * Flash ADPCM, written from the format description in the SWF file format
 * specification's ADPCMSOUNDDATA. It is worth being precise about what it is
 * and is not, because it is routinely described as "IMA ADPCM" and a decoder
 * written on that assumption produces something that is recognisably the right
 * audio and audibly wrong:
 *
 *   - Samples are 2, 3, 4 or 5 bits wide, chosen once per stream by a two-bit
 *     code at the front. IMA is four bits and only four.
 *   - The bits are packed most significant first, continuously, with no regard
 *     for byte boundaries. IMA packs two nibbles per byte, low nibble first.
 *   - The stream is cut into packets of exactly 4096 samples. Each one begins
 *     with the predictor stated outright - a signed 16-bit sample and a 6-bit
 *     step index - and that stated sample is itself the packet's first output.
 *     The count is nowhere in the file. IMA's block header is a different
 *     shape and its length is carried in the container.
 *   - The step index adjustment depends on the sample width, and the tables for
 *     2, 3 and 5 bits have no IMA counterpart at all.
 *
 * What it does share with IMA is the 89-entry step size table and, at four
 * bits, the way a magnitude is turned back into an amplitude. That is why a
 * decoder built on the wrong assumptions still sounds nearly right at the one
 * width everybody tests.
 *
 * The reconstruction, for a code of `w` bits with the top bit as sign and the
 * rest as magnitude, is a plain binary weighting of the current step size:
 *
 *   diff = step/2^(w-1)                     the always-present half-step
 *   for each magnitude bit b, weight 2^b:   if set, diff += step/2^(w-1-b)
 *   predictor += sign ? -diff : +diff       clamped to 16 bits
 *   index     += adjust[magnitude]          clamped to 0..88
 *
 * All of it integer, all of it shifts. Nothing here allocates.
 */
#include "ps_swf_sound.h"

#include "ps_swf_bits.h"
#include "ps_swf_mem.h"

#include <stdio.h>
#include <string.h>

#define ps_alloc   ps_swf_alloc
#define ps_realloc ps_swf_realloc
#define ps_free    ps_swf_dealloc

/* --- tables -------------------------------------------------------------- */

/* The IMA/DVI step size table: 89 entries rising by about 11% each, from 7 to
 * the largest value that cannot overflow a signed 16-bit sample. */
static const int32_t step_tab[] = {
        7,     8,     9,    10,    11,    12,    13,    14,    16,    17,
       19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
       50,    55,    60,    66,    73,    80,    88,    97,   107,   118,
      130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
      337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
      876,   963,  1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
     2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
     5894,  6484,  7132,  7845,  8630,  9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

/* The index field is six bits wide, so the file can name an index of 63, and
 * every adjustment below is clamped into this table rather than trusted. */
static_assert(sizeof step_tab / sizeof step_tab[0] == 89,
              "the ADPCM step table has 89 entries and indices clamp to 88");

/* Step index adjustment, by sample width and then by magnitude. Row w-2 has
 * 2^(w-1) live entries; the rest are never indexed because the magnitude is the
 * code with its sign bit removed. */
static const int8_t idx_tab[4][16] = {
    { -1,  2 },
    { -1, -1,  2,  4 },
    { -1, -1, -1, -1,  2,  4,  6,  8 },
    { -1, -1, -1, -1, -1, -1, -1, -1,  1,  2,  4,  6,  8, 10, 13, 16 }
};

static_assert(sizeof idx_tab[0] / sizeof idx_tab[0][0] == 16,
              "the widest ADPCM code is five bits, so sixteen magnitudes");
static_assert(PS_SWF_ADPCM_PACKET == 4096,
              "an ADPCM packet is 4096 samples and the file never says so");

/* SoundRate, two bits. 5512 is really 5512.5 - it is 44100/8 - and the half
 * hertz is dropped because it is a tenth of a cent of pitch and because every
 * sink downstream, the AICA included, takes an integer. Nothing in the
 * frame-to-sample map depends on it: that comes from block sample counts. */
static const uint32_t rate_tab[4] = { 5512, 11025, 22050, 44100 };

static_assert(sizeof rate_tab / sizeof rate_tab[0] == 4,
              "SoundRate is a two-bit field");
static_assert(PS_SWF_MAX_ENVPT <= 255,
              "EnvPoints is a byte, so the stored subset must fit one too");

const char *ps_swf_sound_fmtname(int format)
{
    switch(format) {
    case PS_SWF_SND_PCM:      return "PCM (byte order unstated)";
    case PS_SWF_SND_ADPCM:    return "Flash ADPCM";
    case PS_SWF_SND_MP3:      return "MP3 (declined: separate decoder)";
    case PS_SWF_SND_PCM_LE:   return "PCM little-endian";
    case PS_SWF_SND_NELLY16:  return "Nellymoser 16kHz (declined)";
    case PS_SWF_SND_NELLY8:   return "Nellymoser 8kHz (declined)";
    case PS_SWF_SND_NELLY:    return "Nellymoser (declined)";
    case PS_SWF_SND_SPEEX:    return "Speex (declined)";
    default:                  return "unknown codec (declined)";
    }
}

static int fmt_decodable(int format)
{
    return format == PS_SWF_SND_PCM || format == PS_SWF_SND_PCM_LE ||
           format == PS_SWF_SND_ADPCM;
}

/* --- growable array ------------------------------------------------------ */

/* The same doubling vector ps_swf_parse.c uses, restated rather than shared.
 * Twenty lines duplicated is the price of this unit not needing a header out of
 * that one, which is what keeps the two walks independent. `cap_limit` is
 * always derived from the length of the input, never from a field in it. */
typedef struct {
    void    *base;
    uint32_t n, cap;
    size_t   esz;
} vec;

static int vec_push(vec *v, const void *elem, uint32_t cap_limit)
{
    if(v->n == v->cap) {
        uint32_t want = v->cap ? v->cap * 2 : 16;
        void    *nb;

        if(want > cap_limit)
            want = cap_limit;
        if(want <= v->n)
            return -1;
        nb = ps_realloc(v->base, (size_t)want * v->esz);
        if(!nb)
            return -1;
        v->base = nb;
        v->cap  = want;
    }
    memcpy((unsigned char *)v->base + (size_t)v->n * v->esz, elem, v->esz);
    v->n++;
    return 0;
}

/* --- ADPCM geometry ------------------------------------------------------ */

/* Bits one whole packet occupies: a stated sample and step index per channel,
 * then 4095 codes per channel. Every packet but the last is exactly this long,
 * which is what makes the start of packet k computable without a table. */
static uint64_t packet_bits(uint32_t nbits, uint32_t channels)
{
    return (22u + (uint64_t)(PS_SWF_ADPCM_PACKET - 1u) * nbits) * channels;
}

/* Samples the bytes present can actually yield, and how many bits are left over
 * afterwards.
 *
 * This is the same argument ps_adx_parse makes about a short file: a declared
 * count that the data cannot support is far more likely to be a download cut
 * off than a lie, so play what arrived. It is also the only bound the decoder
 * needs - having clamped here, nothing downstream has to re-check the buffer
 * against the file's claims.
 *
 * A well-formed block ends with under a byte of padding, so more than seven
 * bits left over means real data was cut mid-sample or mid-packet-header. */
static uint32_t adpcm_cap(uint32_t len, uint32_t nbits, uint32_t channels,
                          uint32_t *leftover)
{
    uint64_t bits = (uint64_t)len * 8u;
    uint64_t hdr  = 22u * channels;
    uint64_t per  = packet_bits(nbits, channels);
    uint64_t full, rem, n;

    *leftover = 0;
    if(bits < 2u)
        return 0;
    bits -= 2u;                      /* the code size field, read once */

    full = bits / per;
    rem  = bits % per;
    n    = full * PS_SWF_ADPCM_PACKET;

    if(rem >= hdr) {
        uint64_t step  = (uint64_t)nbits * channels;
        uint64_t extra = 1u + (rem - hdr) / step;

        if(extra > PS_SWF_ADPCM_PACKET)
            extra = PS_SWF_ADPCM_PACKET;
        n   += extra;
        rem -= hdr + (extra - 1u) * step;
    }

    *leftover = (uint32_t)rem;
    return n > 0xffffffffu ? 0xffffffffu : (uint32_t)n;
}

/* --- decoder ------------------------------------------------------------- */

static int dec_setup(ps_swf_snddec *d, const uint8_t *data, uint32_t len,
                     uint8_t format, uint8_t bits, uint8_t channels,
                     uint32_t total)
{
    uint32_t cap;

    memset(d, 0, sizeof *d);
    if(!fmt_decodable(format) || channels < 1 || channels > 2)
        return -1;

    d->data     = data;
    d->len      = len;
    d->format   = format;
    d->bits     = bits;
    d->channels = channels;
    d->total    = total;

    ps_bits_init(&d->b, data, len);

    if(format == PS_SWF_SND_ADPCM) {
        uint32_t leftover;

        /* Two bits naming a width of 2..5. Read once, before packet zero, and
         * never again - which is why every seek below starts from bit 2 and not
         * from bit 0. */
        d->nbits = (uint8_t)(ps_bits_ub(&d->b, 2) + 2u);
        if(d->b.over)
            return -1;
        cap = adpcm_cap(len, d->nbits, channels, &leftover);
    }
    else {
        uint32_t bpf = (uint32_t)(bits / 8u) * channels;

        cap = bpf ? len / bpf : 0;
    }

    /* Clamped again here even though the loader already did it, because a
     * caller may build one of these by hand and because everything downstream -
     * the seek arithmetic in particular - is written on the assumption that
     * `total` cannot exceed what the bytes hold. One comparison at setup buys
     * the right to not re-check inside the sample loop. */
    if(d->total > cap)
        d->total = cap;
    return 0;
}

int ps_swf_snddec_init(ps_swf_snddec *d, const ps_swf_sound *s)
{
    if(!d || !s || !s->data)
        return -1;
    return dec_setup(d, s->data, s->len, s->format, s->bits, s->channels,
                     s->nsample);
}

int ps_swf_snddec_init_block(ps_swf_snddec *d, const ps_swf_sndstream *st,
                             uint32_t block)
{
    const ps_swf_sndblock *bl;

    if(!d || !st || block >= st->nblock)
        return -1;
    bl = &st->blocks[block];
    return dec_setup(d, st->data + bl->off, bl->len, st->format, st->bits,
                     st->channels, bl->nsample);
}

static void adpcm_update(ps_swf_snddec *d, uint32_t ch, uint32_t code)
{
    uint32_t sign = 1u << (d->nbits - 1u);
    uint32_t mag  = code & (sign - 1u);
    int32_t  step = step_tab[d->index[ch]];
    int32_t  diff = 0;
    uint32_t bit;

    /* Weighted sum of halvings of the step, plus the half-step that is always
     * there. The trailing term is what stops a code of zero meaning "no change"
     * - a run of zeros still walks the predictor, which is how the format
     * represents a slow slope cheaply. */
    for(bit = sign >> 1; bit; bit >>= 1) {
        if(mag & bit)
            diff += step;
        step >>= 1;
    }
    diff += step;

    d->pred[ch] += (code & sign) ? -diff : diff;
    if(d->pred[ch] > 32767)
        d->pred[ch] = 32767;
    else if(d->pred[ch] < -32768)
        d->pred[ch] = -32768;

    d->index[ch] += idx_tab[d->nbits - 2u][mag];
    if(d->index[ch] < 0)
        d->index[ch] = 0;
    else if(d->index[ch] > 88)
        d->index[ch] = 88;
}

static uint32_t read_adpcm(ps_swf_snddec *d, int16_t *const *out, uint32_t want)
{
    uint32_t made = 0, ch;

    while(made < want && d->at < d->total) {
        if(d->pkt_at == 0) {
            int32_t  p[2];
            uint32_t ix[2];

            for(ch = 0; ch < d->channels; ch++) {
                p[ch]  = ps_bits_sb(&d->b, 16);
                ix[ch] = ps_bits_ub(&d->b, 6);
            }
            /* Nothing is emitted until the whole group has been read: a short
             * read yields zeros from the bit reader, and half a packet header
             * turned into a sample is a click at the seam rather than a
             * shortfall the caller can see. */
            if(d->b.over)
                break;
            for(ch = 0; ch < d->channels; ch++) {
                d->pred[ch]  = p[ch];
                d->index[ch] = (int32_t)(ix[ch] > 88u ? 88u : ix[ch]);
                out[ch][made] = (int16_t)p[ch];
            }
        }
        else {
            uint32_t code[2];

            for(ch = 0; ch < d->channels; ch++)
                code[ch] = ps_bits_ub(&d->b, d->nbits);
            if(d->b.over)
                break;
            for(ch = 0; ch < d->channels; ch++) {
                adpcm_update(d, ch, code[ch]);
                out[ch][made] = (int16_t)d->pred[ch];
            }
        }

        made++;
        d->at++;
        if(++d->pkt_at == PS_SWF_ADPCM_PACKET)
            d->pkt_at = 0;
    }
    return made;
}

static uint32_t read_pcm(ps_swf_snddec *d, int16_t *const *out, uint32_t want)
{
    uint32_t bpf = (uint32_t)(d->bits / 8u) * d->channels;
    uint32_t made = 0, ch;

    if(bpf == 0)
        return 0;

    while(made < want && d->at < d->total) {
        uint32_t off = d->at * bpf;

        if(off + bpf > d->len)
            break;
        for(ch = 0; ch < d->channels; ch++) {
            const uint8_t *p = d->data + off + ch * (d->bits / 8u);

            /* Eight-bit PCM is unsigned and centred on 128, sixteen-bit is
             * signed and little-endian. Reading either as the other is silent:
             * the first comes out as a loud square wave, the second as noise
             * that is still the right length. */
            out[ch][made] = d->bits == 16
                ? (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8))
                : (int16_t)(((int32_t)p[0] - 128) * 256);
        }
        made++;
        d->at++;
    }
    return made;
}

uint32_t ps_swf_snddec_read(ps_swf_snddec *d, int16_t *const *out,
                            uint32_t want)
{
    if(!d || !out || !d->data || !want)
        return 0;
    return d->format == PS_SWF_SND_ADPCM ? read_adpcm(d, out, want)
                                         : read_pcm(d, out, want);
}

int ps_swf_snddec_seek(ps_swf_snddec *d, uint32_t sample)
{
    uint64_t bit;
    uint32_t pkt;

    if(!d || !d->data || sample > d->total)
        return -1;

    if(d->format != PS_SWF_SND_ADPCM) {
        d->at = sample;
        return 0;
    }

    pkt = sample / PS_SWF_ADPCM_PACKET;
    bit = 2u + (uint64_t)pkt * packet_bits(d->nbits, d->channels);

    if((bit >> 3) > d->len)
        return -1;

    d->b.pos  = (size_t)(bit >> 3);
    d->b.bit  = (int)(bit & 7u);
    d->b.over = 0;
    d->at     = pkt * PS_SWF_ADPCM_PACKET;
    d->pkt_at = 0;

    /* The rest is decoded and thrown away, because the predictor at sample N is
     * every delta since the packet header applied in order and there is no
     * shortcut through it. Worst case 4095 samples, which is 93ms of audio and
     * about forty thousand integer operations - cheap enough to do on every
     * loop seam, which is exactly where it is needed. */
    while(d->at < sample) {
        int16_t  scratch[2][256];
        int16_t *ptr[2] = { scratch[0], scratch[1] };
        uint32_t n = sample - d->at;

        if(n > 256u)
            n = 256u;
        if(read_adpcm(d, ptr, n) != n)
            return -1;
    }
    return 0;
}

/* --- playing an event sound ---------------------------------------------- */

static void play_range(const ps_swf_sound *s, const ps_swf_sndstart *ev,
                       uint32_t *in, uint32_t *out, uint32_t *passes)
{
    uint32_t a = ev && ev->has_in  ? ev->in  : 0;
    uint32_t b = ev && ev->has_out ? ev->out : s->nsample;

    if(b > s->nsample)
        b = s->nsample;
    if(a > b)
        a = b;

    *in     = a;
    *out    = b;
    /* LoopCount is a count of plays, not of repeats, and zero means the field
     * said nothing - so both 0 and 1 are one pass. Reading it as repeats gives
     * every looped sound one extra play, which on a two-second sting is
     * obvious and on a one-frame click is not. */
    *passes = (ev && ev->loops > 1u) ? ev->loops : 1u;
}

uint32_t ps_swf_sndplay_total(const ps_swf_sound *s, const ps_swf_sndstart *ev)
{
    uint32_t in, out, passes;
    uint64_t total;

    if(!s)
        return 0;
    play_range(s, ev, &in, &out, &passes);
    total = (uint64_t)(out - in) * passes;
    return total > 0xffffffffu ? 0xffffffffu : (uint32_t)total;
}

int ps_swf_sndplay_init(ps_swf_sndplay *p, const ps_swf_sound *s,
                        const ps_swf_sndstart *ev)
{
    uint32_t passes;

    if(!p || !s)
        return -1;

    memset(p, 0, sizeof *p);
    play_range(s, ev, &p->in, &p->out, &passes);
    if(p->out == p->in)
        return -1;
    p->loops_left = passes - 1u;

    if(ps_swf_snddec_init(&p->dec, s) < 0)
        return -1;
    return ps_swf_snddec_seek(&p->dec, p->in);
}

uint32_t ps_swf_sndplay_read(ps_swf_sndplay *p, int16_t *const *out,
                             uint32_t want)
{
    uint32_t made = 0;

    if(!p || !out || !want)
        return 0;

    while(made < want) {
        int16_t *ptr[2] = { NULL, NULL };
        uint32_t n, got, ch;

        if(p->dec.at >= p->out) {
            if(p->loops_left == 0)
                break;
            p->loops_left--;
            /* The seam is a sample position, never a block position. That is
             * the whole lesson from the ADX streamer: a sound is almost never a
             * whole number of decoder blocks long, and rewinding to a block
             * boundary leaves a remainder that the next read reports as a short
             * one - which every caller reads as the end of the sound. */
            if(ps_swf_snddec_seek(&p->dec, p->in) < 0)
                break;
        }

        n = want - made;
        if(n > p->out - p->dec.at)
            n = p->out - p->dec.at;

        for(ch = 0; ch < p->dec.channels; ch++)
            ptr[ch] = out[ch] + made;

        got = ps_swf_snddec_read(&p->dec, ptr, n);
        if(got == 0)
            break;                     /* the bytes genuinely ran out */
        made += got;
    }
    return made;
}

void ps_swf_sndplay_envelope(const ps_swf_sndstart *ev, uint32_t rate,
                             uint32_t sample, uint16_t *left, uint16_t *right)
{
    uint64_t p44;
    uint32_t i;

    if(left)  *left  = 32768;
    if(right) *right = 32768;
    if(!ev || ev->nenv == 0 || rate == 0)
        return;

    /* Compared in 44100Hz space rather than converting the points down into the
     * sound's rate, because the points are what the file states exactly and the
     * conversion the other way loses up to eight samples of position on a
     * 5512Hz sound. */
    p44 = (uint64_t)sample * 44100u / rate;

    for(i = 0; i + 1u < ev->nenv; i++)
        if(p44 < ev->env[i + 1u].pos44)
            break;

    if(i + 1u >= ev->nenv || p44 <= ev->env[0].pos44) {
        uint32_t k = p44 <= ev->env[0].pos44 ? 0u : (uint32_t)ev->nenv - 1u;

        if(left)  *left  = ev->env[k].left;
        if(right) *right = ev->env[k].right;
        return;
    }

    {
        const ps_swf_envpt *a = &ev->env[i];
        const ps_swf_envpt *b = &ev->env[i + 1u];
        uint64_t span = (uint64_t)b->pos44 - a->pos44;
        uint64_t at   = p44 - a->pos44;

        if(span == 0) {
            if(left)  *left  = b->left;
            if(right) *right = b->right;
            return;
        }
        if(left)
            *left = (uint16_t)(a->left + (int64_t)(b->left - a->left) *
                                             (int64_t)at / (int64_t)span);
        if(right)
            *right = (uint16_t)(a->right + (int64_t)(b->right - a->right) *
                                               (int64_t)at / (int64_t)span);
    }
}

/* --- tag parsing --------------------------------------------------------- */

#define ERR(...) do { if(err && errlen) snprintf(err, errlen, __VA_ARGS__); } while(0)

/* Same ceiling as the shape parser's, and the same reasoning: a sprite inside a
 * sprite is already a malformed file, and recursion driven by file content is
 * the one parser bug bounds checks cannot contain. */
#define MAX_SPRITE_DEPTH 8

typedef struct {
    const uint8_t *data;
    uint32_t       cap;
    ps_swf_audio  *a;
    vec            sounds;
    vec            starts;
    vec            streams;
} audioctx;

/* A stream being assembled. It lives on the stack of the walk that owns the
 * timeline, so a sprite's stream and the root's never share state and neither
 * needs an index into an array that reallocates underneath it. */
typedef struct {
    ps_swf_sndstream st;
    vec              blocks;
    uint8_t         *blob;
    uint32_t         blob_len, blob_cap;
    int              open;
} strbuild;

const ps_swf_sound *ps_swf_find_sound(const ps_swf_audio *a, uint16_t id)
{
    uint32_t i;

    if(!a)
        return NULL;
    /* Last definition wins, as with characters: redefining an ID is unusual and
     * legal, and the later tag is the one in force. */
    for(i = a->nsound; i-- > 0; )
        if(a->sounds[i].id == id)
            return &a->sounds[i];
    return NULL;
}

int ps_swf_stream_frame_sample(const ps_swf_sndstream *st, uint32_t frame,
                               uint32_t *sample)
{
    uint32_t i;

    if(!st)
        return -1;
    /* Linear, and blocks are in frame order so it stops early. A stream has one
     * block per frame, so the worst case a Flash 4 file can produce is a scan
     * of a few thousand small records - a binary search would save microseconds
     * and cost the property that this is obviously correct. */
    for(i = 0; i < st->nblock; i++) {
        if(st->blocks[i].frame > frame)
            break;
        if(st->blocks[i].frame == frame) {
            if(sample)
                *sample = st->blocks[i].first;
            return 0;
        }
    }
    return -1;
}

/* DefineSound. */
static int parse_define_sound(const uint8_t *body, uint32_t blen,
                              ps_swf_sound *s)
{
    ps_bits  b;
    uint32_t hdr;

    memset(s, 0, sizeof *s);
    ps_bits_init(&b, body, blen);

    s->id        = ps_bits_u16(&b);
    s->format    = (uint8_t)ps_bits_ub(&b, 4);
    s->rate_code = (uint8_t)ps_bits_ub(&b, 2);
    s->bits      = ps_bits_ub(&b, 1) ? 16u : 8u;
    s->channels  = (uint8_t)(ps_bits_ub(&b, 1) + 1u);
    s->declared  = ps_bits_u32(&b);
    if(b.over)
        return -1;

    s->rate      = rate_tab[s->rate_code];
    s->decodable = (uint8_t)fmt_decodable(s->format);
    s->nsample   = s->declared;

    hdr = 7u;                             /* two, one packed byte, four */
    if(blen < hdr)
        return -1;
    s->len = blen - hdr;

    /* Everything compressed decodes to sixteen bits whatever SoundSize claims,
     * and the spec requires it to claim sixteen. Believing an ADPCM tag that
     * says eight would halve every byte offset the PCM path computes - which is
     * a path ADPCM never takes, but the field is also what a caller reads to
     * size its buffers. */
    if(s->format == PS_SWF_SND_ADPCM)
        s->bits = 16;

    if(s->decodable) {
        uint32_t cap, leftover = 0;

        if(s->format == PS_SWF_SND_ADPCM) {
            uint32_t nbits;

            if(s->len < 1u)
                return -1;
            nbits = (uint32_t)(body[hdr] >> 6) + 2u;
            cap   = adpcm_cap(s->len, nbits, s->channels, &leftover);
        } else {
            uint32_t bpf = (uint32_t)(s->bits / 8u) * s->channels;
            cap = bpf ? s->len / bpf : 0;
        }
        if(s->nsample > cap) {
            s->nsample = cap;
            s->clamped = 1;
        }
    }

    if(s->len) {
        /* Sized by the tag length, which the walk has already checked against
         * the bytes actually present - so this is a copy of memory we hold, not
         * an allocation a field in the file chose. */
        s->data = ps_alloc(s->len);
        if(!s->data)
            return -1;
        memcpy(s->data, body + hdr, s->len);
    }
    return 0;
}

/* StartSound's SOUNDINFO. The flag byte reads most significant bit first, so
 * the two reserved bits come off the top and HasInPoint is the last of them -
 * but the optional fields that follow are in the order the spec lists the
 * flags, which is the reverse. In and out first, then the loop count, then the
 * envelope. */
static int parse_start_sound(const uint8_t *body, uint32_t blen,
                             uint16_t timeline, uint32_t frame,
                             ps_swf_sndstart *ev)
{
    ps_bits  b;
    unsigned has_env, has_loops, n, i;

    memset(ev, 0, sizeof *ev);
    ps_bits_init(&b, body, blen);

    ev->timeline = timeline;
    ev->frame    = frame;
    ev->id       = ps_bits_u16(&b);

    (void)ps_bits_ub(&b, 2);                       /* reserved */
    ev->stop        = (uint8_t)ps_bits_ub(&b, 1);
    ev->no_multiple = (uint8_t)ps_bits_ub(&b, 1);
    has_env         = ps_bits_ub(&b, 1);
    has_loops       = ps_bits_ub(&b, 1);
    ev->has_out     = (uint8_t)ps_bits_ub(&b, 1);
    ev->has_in      = (uint8_t)ps_bits_ub(&b, 1);

    if(ev->has_in)
        ev->in = ps_bits_u32(&b);
    if(ev->has_out)
        ev->out = ps_bits_u32(&b);
    if(has_loops)
        ev->loops = ps_bits_u16(&b);

    if(has_env) {
        n = ps_bits_u8(&b);
        for(i = 0; i < n && !b.over; i++) {
            ps_swf_envpt e;

            e.pos44 = ps_bits_u32(&b);
            e.left  = ps_bits_u16(&b);
            e.right = ps_bits_u16(&b);
            /* Read past the storage limit, store up to it - the same policy as
             * gradient stops, and for the same reason: reading is what proves
             * the tag was the length it said it was. */
            if(ev->nenv < PS_SWF_MAX_ENVPT)
                ev->env[ev->nenv++] = e;
        }
    }
    return b.over ? -1 : 0;
}

/* SoundStreamHead and SoundStreamHead2. Identical encodings; the tag code is
 * kept because only tag 18's constraints forbid an uncompressed 8-bit stream,
 * and a file that declares one under tag 18 is malformed in a way worth being
 * able to see. */
static int parse_stream_head(const uint8_t *body, uint32_t blen, int tag,
                             uint16_t timeline, ps_swf_sndstream *st)
{
    ps_bits b;

    memset(st, 0, sizeof *st);
    ps_bits_init(&b, body, blen);

    (void)ps_bits_ub(&b, 4);                       /* reserved */
    st->play_rate_code = (uint8_t)ps_bits_ub(&b, 2);
    st->play_bits      = ps_bits_ub(&b, 1) ? 16u : 8u;
    st->play_channels  = (uint8_t)(ps_bits_ub(&b, 1) + 1u);

    st->format    = (uint8_t)ps_bits_ub(&b, 4);
    st->rate_code = (uint8_t)ps_bits_ub(&b, 2);
    st->bits      = ps_bits_ub(&b, 1) ? 16u : 8u;
    st->channels  = (uint8_t)(ps_bits_ub(&b, 1) + 1u);
    st->spf       = ps_bits_u16(&b);

    /* Present only for MP3, and skipping it on a file that has it would leave
     * two bytes of a tag unread - harmless here, because the tag ends, and
     * fatal in any reader that treats a short read as a parse failure. */
    if(st->format == PS_SWF_SND_MP3)
        st->latency_seek = (int16_t)ps_bits_u16(&b);

    if(b.over)
        return -1;

    st->head_tag  = (uint8_t)tag;
    st->timeline  = timeline;
    st->rate      = rate_tab[st->rate_code];
    st->play_rate = rate_tab[st->play_rate_code];
    st->decodable = (uint8_t)fmt_decodable(st->format);
    if(st->format == PS_SWF_SND_ADPCM)
        st->bits = 16;
    return 0;
}

/* Where the blob stops doubling and starts stepping.
 *
 * The concatenated stream is by a wide margin the largest thing this unit
 * holds - three minutes of ADPCM is two megabytes - and doubling a two-megabyte
 * buffer asks the allocator for four while the two are still live. On a machine
 * with 16MB, no virtual memory and no swap, that transient is the failure, not
 * the steady state. Past this point the growth becomes a fixed step, which
 * bounds the overshoot at 256KB and costs a handful of extra copies on a track
 * long enough for anyone to notice. */
#define BLOB_STEP (256u * 1024u)

static int blob_push(strbuild *sb, const uint8_t *src, uint32_t n,
                     uint32_t ceiling)
{
    if(sb->blob_len + n > sb->blob_cap) {
        uint32_t want = sb->blob_cap;
        uint8_t *nb;

        do {
            want = want < 1024u      ? 1024u
                 : want < BLOB_STEP  ? want * 2u
                                     : want + BLOB_STEP;
        } while(want < sb->blob_len + n && want < ceiling);
        if(want > ceiling)
            want = ceiling;
        if(want < sb->blob_len + n)
            return -1;
        nb = ps_realloc(sb->blob, want);
        if(!nb)
            return -1;
        sb->blob     = nb;
        sb->blob_cap = want;
    }
    memcpy(sb->blob + sb->blob_len, src, n);
    sb->blob_len += n;
    return 0;
}

/* One SoundStreamBlock. Returns -1 only when the block cannot be recorded at
 * all; a block whose bytes ran out short still contributes what it has, because
 * a stream with a hole in it is still a stream and stopping the whole soundtrack
 * over one frame is not a trade worth making. */
static int add_stream_block(strbuild *sb, const uint8_t *body, uint32_t blen,
                            uint32_t frame, uint32_t cap)
{
    ps_swf_sndstream *st = &sb->st;
    ps_swf_sndblock   bl;
    const uint8_t    *payload = body;
    uint32_t          plen = blen, n = 0, leftover = 0;

    memset(&bl, 0, sizeof bl);

    if(st->format == PS_SWF_SND_MP3) {
        /* An MP3 block states its own sample count and a seek offset before the
         * frames themselves. Reading it is what keeps the frame-to-sample map
         * right on a file this cannot decode a note of - and that map is what
         * the rest of the player uses to place cue points and captions. */
        if(blen < 4u)
            return -1;
        n        = (uint32_t)body[0] | ((uint32_t)body[1] << 8);
        payload  = body + 4;
        plen     = blen - 4u;
    }
    else if(st->format == PS_SWF_SND_ADPCM) {
        uint32_t nbits;

        if(blen < 1u)
            return -1;
        nbits = (uint32_t)(body[0] >> 6) + 2u;
        n     = adpcm_cap(blen, nbits, st->channels, &leftover);
        /* A well-formed block ends with under a byte of padding, so a whole
         * byte left over is data that was cut mid-sample or mid-header. */
        bl.damaged = leftover >= 8u;
    }
    else if(fmt_decodable(st->format)) {
        uint32_t bpf = (uint32_t)(st->bits / 8u) * st->channels;

        if(bpf == 0)
            return -1;
        n = blen / bpf;
        bl.damaged = (blen % bpf) != 0;
    }
    else {
        n = st->spf;              /* a codec we decline: pace it by the head */
    }

    bl.frame   = frame;
    bl.first   = st->nsample;
    bl.nsample = n;
    bl.off     = sb->blob_len;
    bl.len     = plen;

    if(plen && blob_push(sb, payload, plen, cap) < 0)
        return -1;
    if(vec_push(&sb->blocks, &bl, cap) < 0)
        return -1;

    st->nsample += n;
    return 0;
}

static int commit_stream(audioctx *ac, strbuild *sb, uint32_t nframe)
{
    /* By value, not by pointer into the builder: the builder is reset before
     * this returns so the caller can start a second stream on the same
     * timeline, and a pointer into it would be reading the reset. */
    ps_swf_sndstream  copy = sb->st;
    ps_swf_sndstream *st   = &copy;
    uint32_t          i;

    st->blocks = sb->blocks.base;
    st->nblock = sb->blocks.n;
    st->data   = sb->blob;
    st->len    = sb->blob_len;
    st->nframe = nframe;

    /* One block per frame across the span is what a stream is supposed to be.
     * Anything else has to be visible: a missing block is a gap the player must
     * fill with silence to stay in sync, and two on one frame cannot be paced
     * at all. Derived from the block frames rather than counted during the walk
     * so it stays right whatever order the tags arrived in. */
    for(i = 1; i < st->nblock; i++)
        if(st->blocks[i].frame == st->blocks[i - 1].frame)
            st->dups++;

    /* Every block but the last is also checked against the head's declared
     * rate. See ps_swf_sndstream.short_blocks for why that is the only check
     * available for a compressed stream, and why it carries a sample of slack. */
    for(i = 0; i + 1u < st->nblock; i++)
        if(st->blocks[i].nsample + 1u < st->spf)
            st->blocks[i].damaged = 1;
    for(i = 0; i < st->nblock; i++)
        if(st->blocks[i].damaged)
            st->short_blocks++;

    if(st->nblock) {
        uint32_t span = st->blocks[st->nblock - 1].frame -
                        st->blocks[0].frame + 1u;
        uint32_t placed = st->nblock - st->dups;

        st->gaps = span > placed ? span - placed : 0;
    }

    memset(sb, 0, sizeof *sb);
    sb->blocks.esz = sizeof(ps_swf_sndblock);

    if(vec_push(&ac->streams, st, ac->cap) < 0) {
        ps_free(st->blocks);
        ps_free(st->data);
        return -1;
    }
    return 0;
}

/* Walks one timeline's tags, collecting sound and nothing else.
 *
 * `timeline` is 0 for the root and a sprite's character ID otherwise, and the
 * frame counter is local to this call - which is the whole reason a sprite is a
 * recursive call rather than a flattened continuation. A sprite's frame 3 and
 * the root's frame 3 are different instants and a cue on one says nothing about
 * the other. */
static void walk_sound(ps_bits *b, size_t stop, audioctx *ac,
                       uint16_t timeline, int depth)
{
    strbuild sb;
    uint32_t frame = 0;

    memset(&sb, 0, sizeof sb);
    sb.blocks.esz = sizeof(ps_swf_sndblock);

    for(;;) {
        uint32_t rec, code, tlen;
        size_t   body;

        if(b->over || b->pos + 2 > stop)
            break;
        rec  = ps_bits_u16(b);
        code = rec >> 6;
        tlen = rec & 0x3f;
        if(tlen == 0x3f) {
            tlen = ps_bits_u32(b);
            if(b->over)
                break;
        }
        if(code == 0)
            break;
        if(tlen > stop - b->pos)
            break;
        body = b->pos;

        switch(code) {
        case 1:                                   /* ShowFrame */
            frame++;
            break;

        case 14: {                                /* DefineSound */
            ps_swf_sound s;

            if(parse_define_sound(ac->data + body, tlen, &s) < 0)
                break;                            /* one bad sound, not the lot */
            if(vec_push(&ac->sounds, &s, ac->cap) < 0)
                ps_free(s.data);
            break;
        }

        case 15: {                                /* StartSound */
            ps_swf_sndstart ev;

            if(parse_start_sound(ac->data + body, tlen, timeline, frame,
                                 &ev) < 0)
                break;
            (void)vec_push(&ac->starts, &ev, ac->cap);
            break;
        }

        case 18: case 45: {                       /* SoundStreamHead / 2 */
            ps_swf_sndstream st;

            if(parse_stream_head(ac->data + body, tlen, (int)code, timeline,
                                 &st) < 0)
                break;
            /* A second head on one timeline is not legal, but the only sane
             * reading of it is that the stream restarts - so close the one in
             * progress rather than dropping either. */
            if(sb.open && commit_stream(ac, &sb, frame) < 0)
                goto done;
            sb.st   = st;
            sb.open = 1;
            break;
        }

        case 19:                                  /* SoundStreamBlock */
            if(sb.open)
                (void)add_stream_block(&sb, ac->data + body, tlen, frame,
                                       ac->cap);
            break;

        case 39: {                                /* DefineSprite */
            ps_bits  nb;
            uint16_t sid;

            ps_bits_init(&nb, ac->data, body + tlen);
            nb.pos = body;
            sid = ps_bits_u16(&nb);
            (void)ps_bits_u16(&nb);               /* declared frame count */
            if(depth < MAX_SPRITE_DEPTH)
                walk_sound(&nb, body + tlen, ac, sid, depth + 1);
            break;
        }

        default:
            break;
        }

        b->pos = body + tlen;
        b->bit = 0;
    }

done:
    if(sb.open) {
        if(commit_stream(ac, &sb, frame) < 0) {
            ps_free(sb.blocks.base);
            ps_free(sb.blob);
        }
    } else {
        ps_free(sb.blocks.base);
        ps_free(sb.blob);
    }

    if(timeline == 0)
        ac->a->nframe = frame;
}

int ps_swf_audio_load(const uint8_t *data, size_t len, ps_swf_audio *a,
                      char *err, size_t errlen)
{
    ps_bits  b;
    audioctx ac;
    uint32_t cap;
    int      nbits, i;

    if(!a)
        return -1;
    memset(a, 0, sizeof *a);

    if(!data || len < 9 || (memcmp(data, "FWS", 3) && memcmp(data, "CWS", 3))) {
        ERR("not a SWF");
        return -1;
    }
    a->version = data[3];
    if(data[0] == 'C') {
        ERR("zlib-compressed SWF (CWS); inflate not wired up");
        return -1;
    }

    /* One element per byte of input, as everywhere else in this player. The
     * tightest sound tag is a StartSound at five bytes including its header, so
     * this cannot reject a real file and every allocation stays a function of
     * the bytes actually received. */
    cap = (uint32_t)(len > 0x40000000u ? 0x40000000u : len);
    if(cap < 16u)
        cap = 16u;

    /* The stage rectangle is read and dropped, but it has to be read: it is a
     * bit-packed RECT of file-chosen width, so the frame rate behind it cannot
     * be located by any fixed offset. */
    ps_bits_init(&b, data, len);
    ps_bits_skip(&b, 8);
    nbits = (int)ps_bits_ub(&b, 5);
    for(i = 0; i < 4; i++)
        (void)ps_bits_sb(&b, nbits);
    ps_bits_align(&b);
    {
        uint8_t frac  = ps_bits_u8(&b);
        uint8_t whole = ps_bits_u8(&b);

        a->fps = (float)whole + (float)frac / 256.0f;
    }
    (void)ps_bits_u16(&b);                         /* declared frame count */

    if(b.over) {
        ERR("truncated header");
        return -1;
    }

    memset(&ac, 0, sizeof ac);
    ac.data        = data;
    ac.cap         = cap;
    ac.a           = a;
    ac.sounds.esz  = sizeof(ps_swf_sound);
    ac.starts.esz  = sizeof(ps_swf_sndstart);
    ac.streams.esz = sizeof(ps_swf_sndstream);

    walk_sound(&b, len, &ac, 0, 0);

    a->sounds  = ac.sounds.base;   a->nsound  = ac.sounds.n;
    a->starts  = ac.starts.base;   a->nstart  = ac.starts.n;
    a->streams = ac.streams.base;  a->nstream = ac.streams.n;
    return 0;
}

void ps_swf_audio_free(ps_swf_audio *a)
{
    uint32_t i;

    if(!a)
        return;
    for(i = 0; i < a->nsound; i++)
        ps_free(a->sounds[i].data);
    for(i = 0; i < a->nstream; i++) {
        ps_free(a->streams[i].data);
        ps_free(a->streams[i].blocks);
    }
    ps_free(a->sounds);
    ps_free(a->starts);
    ps_free(a->streams);
    memset(a, 0, sizeof *a);
}
