/* Decoding the five bitmap tags, and sampling what comes out. See
 * ps_swf_image.h for why the size bound comes before the inflate and why the
 * JPEG splice exists at all. */
#include "ps_swf_image.h"
#include "ps_swf_mem.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

/* stb_image's allocations are routed through the player's own allocator so
 * that "what did this file cost" still has an answer with a JPEG in it. That
 * matters more here than anywhere else in the parser: a bitmap is the largest
 * single thing a SWF can ask for, and it is the one allocation whose size is
 * not visible in the file's length. */
#define STBI_MALLOC(sz)       ps_swf_alloc(sz)
#define STBI_REALLOC(p, sz)   ps_swf_realloc(p, sz)
#define STBI_FREE(p)          ps_swf_dealloc(p)

/* A private copy of the decoder rather than a shared one, because the browser
 * links core/ps_image.c which is also a STB_IMAGE_IMPLEMENTATION, and the two
 * configurations are not interchangeable: that one takes GIF and BMP and
 * allocates through plain malloc, this one refuses both and routes every byte
 * through the allocator whose totals ps_swf_mem_peak reports. Sharing the
 * browser's instance would hand pixels from malloc to ps_swf_dealloc, which
 * reads a size prefix that was never written. */
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_BMP
#define STBI_NO_GIF
/* The SSE2 colour conversion rounds differently from the scalar one, by one
 * step on some inputs. Every assertion about a decoded bitmap in this tree is
 * an exact pixel value, so the build that produces those values has to be the
 * same on every machine that runs the tests. */
#define STBI_NO_SIMD
#define STBI_MAX_DIMENSIONS ((int)PS_SWF_BITMAP_MAX_DIM)
/* Relative to this file rather than by include path, so the same line works
 * from the host build in tests/swf-host and from the Dreamcast build at the
 * repository root.
 *
 * STB_IMAGE_STATIC makes stb's whole public surface static, and this file calls
 * two of it, so the compiler reports the other thirty. They cost nothing - a
 * static function nothing calls is not emitted - and the warning is about
 * vendored code this build has no business editing. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../vendor/stb_image.h"
#pragma GCC diagnostic pop

/* The decoded pixel array is handed straight out of stb_image rather than
 * copied into a fresh one, which is worth four megabytes off the peak on the
 * largest bitmap this will accept - but only if the two layouts are the same
 * four bytes in the same order. If ps_swf_rgba ever gains a field or a
 * different order this stops compiling, which is the point. */
static_assert(sizeof(ps_swf_rgba) == 4, "ps_swf_rgba must be four packed bytes");
static_assert(offsetof(ps_swf_rgba, r) == 0, "ps_swf_rgba must be R,G,B,A");
static_assert(offsetof(ps_swf_rgba, g) == 1, "ps_swf_rgba must be R,G,B,A");
static_assert(offsetof(ps_swf_rgba, b) == 2, "ps_swf_rgba must be R,G,B,A");
static_assert(offsetof(ps_swf_rgba, a) == 3, "ps_swf_rgba must be R,G,B,A");

/* Two lengths cross into stb_image as int - the inflate target and the encoded
 * stream - so the caps have to leave room inside one. Four megabytes against
 * two gigabytes is not close, and the assertion is here so that raising a cap
 * cannot quietly make it close. */
static_assert((unsigned long long)PS_SWF_BITMAP_MAX_PIXELS * 4u
              < (unsigned long long)INT_MAX,
              "a decoded bitmap must fit in stb_image's int lengths");

/* A row of 8-bit or 15-bit pixels is padded out to a 32-bit boundary, and the
 * padding is inside the compressed stream, so getting this wrong does not
 * shift a picture - it makes the expected decompressed size wrong and refuses
 * a legitimate file outright. */
#define PAD4(n) (((n) + 3u) & ~3u)

const char *ps_swf_img_reason(ps_swf_img_err e)
{
    switch(e) {
    case PS_SWF_IMG_OK:          return "ok";
    case PS_SWF_IMG_TRUNCATED:   return "tag ends inside its own header";
    case PS_SWF_IMG_FORMAT:      return "BitmapFormat this tag cannot carry";
    case PS_SWF_IMG_TOO_BIG:     return "dimensions refused";
    case PS_SWF_IMG_ZLIB:        return "zlib stream is not the size it must be";
    case PS_SWF_IMG_PALETTE:     return "colour index past the end of the table";
    case PS_SWF_IMG_NO_TABLES:   return "DefineBits with no JPEGTables ahead of it";
    case PS_SWF_IMG_CODEC:       return "decoder refused the stream";
    case PS_SWF_IMG_UNSUPPORTED: return "codec not implemented";
    case PS_SWF_IMG_MEMORY:      return "out of memory";
    }
    return "?";
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Every dimension check in one place, before anything is multiplied by four.
 * The order matters: each cap is what makes the next arithmetic safe, so a
 * caller that skips to the pixel count has already overflowed on the way. */
static ps_swf_img_err check_dims(unsigned w, unsigned h)
{
    if(w == 0 || h == 0)
        return PS_SWF_IMG_TOO_BIG;
    if(w > PS_SWF_BITMAP_MAX_DIM || h > PS_SWF_BITMAP_MAX_DIM)
        return PS_SWF_IMG_TOO_BIG;
    if((size_t)w * (size_t)h > PS_SWF_BITMAP_MAX_PIXELS)
        return PS_SWF_IMG_TOO_BIG;
    return PS_SWF_IMG_OK;
}

/* Inflate into exactly `need` bytes and refuse anything else.
 *
 * The buffer is not expandable, so a stream that wants to write past the end
 * fails at the byte it wants rather than after a reallocation - which is the
 * whole defence against a stream that claims to expand to a gigabyte, and the
 * reason the size is computed from the picture's dimensions rather than
 * discovered from the stream. A stream that stops early writes fewer bytes and
 * is caught by the same comparison, because a short picture is not a picture. */
static ps_swf_img_err inflate_exact(const uint8_t *z, size_t zlen, size_t need,
                                    uint8_t **out)
{
    uint8_t *buf;
    int      got;

    if(zlen == 0 || zlen > (size_t)INT_MAX || need > (size_t)INT_MAX)
        return PS_SWF_IMG_ZLIB;

    buf = ps_swf_alloc(need);
    if(!buf)
        return PS_SWF_IMG_MEMORY;

    got = stbi_zlib_decode_buffer((char *)buf, (int)need,
                                  (const char *)z, (int)zlen);
    if(got != (int)need) {
        ps_swf_dealloc(buf);
        return PS_SWF_IMG_ZLIB;
    }
    *out = buf;
    return PS_SWF_IMG_OK;
}

/* Five bits to eight, rounded rather than shifted.
 *
 * The shift-and-replicate trick that PNG readers use gets 31 to 255 as well as
 * this does and is cheaper, but it is not the same function in the middle -
 * they differ by one step over most of the range - and there is no reason to
 * choose the approximation when this runs once per pixel of a format almost
 * nothing uses. */
static uint8_t x5to8(unsigned v) { return (uint8_t)((v * 255u + 15u) / 31u); }

/* DefineBitsLossless2 stores colour premultiplied by alpha. Straight alpha is
 * what the sampler and every compositor downstream want, so the division
 * happens once here rather than per sample - and clamps, because a file may
 * carry a colour brighter than its own alpha allows, which is not an error in
 * any player but does divide out to more than 255. */
static ps_swf_rgba unpremultiply(unsigned a, unsigned r, unsigned g, unsigned b)
{
    ps_swf_rgba c;
    unsigned    v[3] = { r, g, b };
    int         i;

    c.a = (uint8_t)a;
    if(a == 0) {
        c.r = c.g = c.b = 0;
        return c;
    }
    for(i = 0; i < 3; i++) {
        unsigned t = (v[i] * 255u + a / 2u) / a;
        v[i] = t > 255u ? 255u : t;
    }
    c.r = (uint8_t)v[0];
    c.g = (uint8_t)v[1];
    c.b = (uint8_t)v[2];
    return c;
}

ps_swf_img_err ps_swf_bitmap_lossless(const uint8_t *body, size_t len, int ver,
                                      ps_swf_bitmap *out)
{
    unsigned       fmt, w, h, ncol = 0, bpe = 0;
    size_t         hdr, stride, need, npx;
    uint8_t       *raw = NULL;
    ps_swf_rgba   *px;
    ps_swf_img_err e;
    unsigned       x, y;

    if(len < 7)
        return PS_SWF_IMG_TRUNCATED;
    fmt = body[2];
    w   = rd16(body + 3);
    h   = rd16(body + 5);

    /* Format 4 is the 15-bit form and exists only in DefineBitsLossless;
     * format 5 means 24-bit there and 32-bit here. A file that puts a 15-bit
     * image in a DefineBitsLossless2 is stating a size per pixel that the tag
     * does not have, and the decompressed length would be wrong by a factor of
     * two - so it is refused rather than guessed at. */
    switch(fmt) {
    case 3:
        if(len < 8)
            return PS_SWF_IMG_TRUNCATED;
        ncol = (unsigned)body[7] + 1u;    /* stored one less than it means */
        bpe  = (ver >= 2) ? 4u : 3u;
        hdr  = 8;
        break;
    case 4:
        if(ver >= 2)
            return PS_SWF_IMG_FORMAT;
        hdr = 7;
        break;
    case 5:
        hdr = 7;
        break;
    default:
        return PS_SWF_IMG_FORMAT;
    }

    e = check_dims(w, h);
    if(e != PS_SWF_IMG_OK)
        return e;

    stride = (fmt == 3) ? PAD4(w) : (fmt == 4) ? PAD4(2u * w) : 4u * (size_t)w;
    need   = (size_t)h * stride + (size_t)ncol * bpe;
    npx    = (size_t)w * h;

    e = inflate_exact(body + hdr, len - hdr, need, &raw);
    if(e != PS_SWF_IMG_OK)
        return e;

    px = ps_swf_alloc(npx * sizeof *px);
    if(!px) {
        ps_swf_dealloc(raw);
        return PS_SWF_IMG_MEMORY;
    }

    for(y = 0; y < h; y++) {
        const uint8_t *row = raw + (size_t)ncol * bpe + (size_t)y * stride;
        ps_swf_rgba   *dst = px + (size_t)y * w;

        for(x = 0; x < w; x++) {
            switch(fmt) {
            case 3: {
                unsigned       idx = row[x];
                const uint8_t *c;

                /* Out of range is a malformed file and not a clamp. Clamping
                 * would draw a picture built from colours the file never
                 * named, which is worse than refusing it: it looks like art. */
                if(idx >= ncol) {
                    ps_swf_dealloc(px);
                    ps_swf_dealloc(raw);
                    return PS_SWF_IMG_PALETTE;
                }
                c = raw + (size_t)idx * bpe;
                if(bpe == 4)
                    dst[x] = unpremultiply(c[3], c[0], c[1], c[2]);
                else {
                    dst[x].r = c[0]; dst[x].g = c[1];
                    dst[x].b = c[2]; dst[x].a = 255;
                }
                break;
            }
            case 4: {
                /* PIX15 is a bitfield and so is big endian: one reserved bit
                 * then five each of red, green and blue, most significant
                 * first. Reading it little endian swaps red and blue and
                 * leaves green almost right, which is the kind of wrong that
                 * gets blamed on the display. */
                unsigned v = ((unsigned)row[2 * x] << 8) | row[2 * x + 1];

                dst[x].r = x5to8((v >> 10) & 31u);
                dst[x].g = x5to8((v >> 5) & 31u);
                dst[x].b = x5to8(v & 31u);
                dst[x].a = 255;
                break;
            }
            default: {
                const uint8_t *c = row + 4u * x;

                if(ver >= 2)
                    dst[x] = unpremultiply(c[0], c[1], c[2], c[3]);
                else {
                    /* PIX24's first byte is reserved, not red. Reading four
                     * channels here shifts every pixel's colour one place
                     * along and tints the whole image. */
                    dst[x].r = c[1]; dst[x].g = c[2];
                    dst[x].b = c[3]; dst[x].a = 255;
                }
                break;
            }
            }
        }
    }

    ps_swf_dealloc(raw);
    out->id = rd16(body);
    out->w  = (uint16_t)w;
    out->h  = (uint16_t)h;
    out->px = px;
    return PS_SWF_IMG_OK;
}

/* --- JPEG ---------------------------------------------------------------- */

/* The pair Flash left behind: an end-of-image immediately followed by a
 * start-of-image. It appears where a JPEGTables stream was joined to a
 * DefineBits stream, and also at the front of DefineBitsJPEG2 tags that were
 * never joined to anything - the writer emitted it either way. */
static int is_eoi_soi(const uint8_t *p)
{
    return p[0] == 0xff && p[1] == 0xd9 && p[2] == 0xff && p[3] == 0xd8;
}

/* Copies `data` into `dst`, dropping a leading marker pair and the first one
 * found anywhere else. Returns the length written. `dst` may be `data`, which
 * is why every move here is a memmove: the spliced case strips in place.
 *
 * Only the first embedded pair is removed, which is Ruffle's rule and is not
 * an oversight: FFD9 FFD8 is a legal byte sequence inside entropy-coded scan
 * data, where FF is followed by a stuffed 00 - so hunting for every occurrence
 * would corrupt images that merely contain those bytes. One pair is what the
 * writer produced and one pair is what gets removed. */
static size_t strip_marker_pair(const uint8_t *data, size_t len, uint8_t *dst)
{
    size_t i;

    if(len >= 4 && is_eoi_soi(data)) {
        data += 4;
        len  -= 4;
    }
    for(i = 0; i + 3 < len; i++) {
        if(is_eoi_soi(data + i)) {
            memmove(dst, data, i);
            memmove(dst + i, data + i + 4, len - i - 4);
            return len - 4;
        }
    }
    memmove(dst, data, len);
    return len;
}

/* SWF 8 allows a PNG or a GIF inside DefineBitsJPEG2 and JPEG3, where the
 * alpha rules are entirely different - a PNG carries its own alpha and the
 * separate alpha plane is absent. Detecting it by signature and saying so is
 * the honest answer at this target; decoding it as a JPEG produces a refusal
 * from the codec that reads as a corrupt file rather than an unread feature. */
static int looks_non_jpeg(const uint8_t *d, size_t n)
{
    if(n >= 8 && d[0] == 0x89 && !memcmp(d + 1, "PNG", 3))
        return 1;
    if(n >= 6 && !memcmp(d, "GIF8", 4))
        return 1;
    return 0;
}

ps_swf_img_err ps_swf_bitmap_jpeg(const uint8_t *body, size_t len, int ver,
                                  const uint8_t *tables, size_t tlen,
                                  ps_swf_bitmap *out)
{
    const uint8_t *jpg;
    size_t         jlen, alen = 0;
    const uint8_t *alpha = NULL;
    uint8_t       *joined = NULL;
    stbi_uc       *pixels;
    int            w = 0, h = 0, comp = 0;
    ps_swf_img_err e;

    if(len < 2)
        return PS_SWF_IMG_TRUNCATED;

    if(ver == 3) {
        uint32_t off;

        if(len < 6)
            return PS_SWF_IMG_TRUNCATED;
        off = rd32(body + 2);
        if((size_t)off > len - 6)
            return PS_SWF_IMG_TRUNCATED;
        jpg   = body + 6;
        jlen  = off;
        alpha = jpg + off;
        alen  = len - 6 - off;
    } else {
        jpg  = body + 2;
        jlen = len - 2;
    }

    if(jlen == 0 || jlen > (size_t)INT_MAX)
        return PS_SWF_IMG_TRUNCATED;
    if(looks_non_jpeg(jpg, jlen))
        return PS_SWF_IMG_UNSUPPORTED;

    /* DefineBits is the only one that needs the splice, and it is also the
     * only one that can be defeated by tag order: the tables have to have been
     * seen already. Nothing in the wild writes them the other way round, and
     * the alternative - deferring every DefineBits to a second pass - is a
     * whole extra parse state to carry for a file that has never existed. */
    if(ver == 1) {
        if(!tables || tlen == 0)
            return PS_SWF_IMG_NO_TABLES;
        if(tlen > PS_SWF_JPEGTABLES_MAX || tlen + jlen > (size_t)INT_MAX)
            return PS_SWF_IMG_TOO_BIG;
        joined = ps_swf_alloc(tlen + jlen);
        if(!joined)
            return PS_SWF_IMG_MEMORY;
        memcpy(joined, tables, tlen);
        memcpy(joined + tlen, jpg, jlen);
        jlen = strip_marker_pair(joined, tlen + jlen, joined);
    } else {
        joined = ps_swf_alloc(jlen);
        if(!joined)
            return PS_SWF_IMG_MEMORY;
        jlen = strip_marker_pair(jpg, jlen, joined);
    }

    pixels = stbi_load_from_memory(joined, (int)jlen, &w, &h, &comp, 4);
    ps_swf_dealloc(joined);
    if(!pixels)
        return PS_SWF_IMG_CODEC;

    e = check_dims((unsigned)w, (unsigned)h);
    if(e != PS_SWF_IMG_OK) {
        ps_swf_dealloc(pixels);
        return e;
    }

    /* The alpha plane is one byte per pixel with no row padding, which is the
     * one place a JPEG3 differs from a lossless bitmap's layout - and it is
     * bounded by the picture the JPEG actually decoded to, not by anything the
     * tag claims, so the same "inflate into exactly this many bytes" rule
     * applies with the size coming from a source the file does not control. */
    if(ver == 3) {
        uint8_t *plane = NULL;
        size_t   npx   = (size_t)w * (size_t)h;
        size_t   i;

        e = inflate_exact(alpha, alen, npx, &plane);
        if(e != PS_SWF_IMG_OK) {
            ps_swf_dealloc(pixels);
            return e;
        }
        for(i = 0; i < npx; i++)
            pixels[i * 4 + 3] = plane[i];
        ps_swf_dealloc(plane);
    }

    out->id = rd16(body);
    out->w  = (uint16_t)w;
    out->h  = (uint16_t)h;
    out->px = (ps_swf_rgba *)pixels;
    return PS_SWF_IMG_OK;
}

void ps_swf_bitmap_free(ps_swf_bitmap *b)
{
    ps_swf_dealloc(b->px);
    b->px = NULL;
    b->w = b->h = 0;
}

/* --- sampling ------------------------------------------------------------ */

static int addr(int i, int n, int tiled)
{
    if(!tiled)
        return i < 0 ? 0 : (i >= n ? n - 1 : i);
    i %= n;
    return i < 0 ? i + n : i;      /* C's % keeps the sign of the dividend */
}

ps_swf_rgba ps_swf_bitmap_sample(const ps_swf_bitmap *b, float u, float v,
                                 int tiled, int smoothed)
{
    int w = b->w, h = b->h;

    if(!smoothed) {
        /* floorf, not a cast: a cast truncates towards zero, so every texel
         * left of or above the origin would be one place wrong and a tiled
         * fill would show a seam exactly there. */
        int iu = addr((int)floorf(u), w, tiled);
        int iv = addr((int)floorf(v), h, tiled);

        return b->px[(size_t)iv * w + iu];
    }

    {
        /* Texel centres sit at half-integer coordinates, so the two taps
         * either side of a sample at u are at floor(u - 0.5) and one past it,
         * and the weight is the fraction between them. Getting the half wrong
         * shifts the whole image half a texel, which is invisible on a
         * photograph and obvious on anything with a straight edge. */
        float fu = u - 0.5f, fv = v - 0.5f;
        float bu = floorf(fu), bv = floorf(fv);
        float su = fu - bu, sv = fv - bv;
        int   u0 = addr((int)bu, w, tiled), u1 = addr((int)bu + 1, w, tiled);
        int   v0 = addr((int)bv, h, tiled), v1 = addr((int)bv + 1, h, tiled);
        const ps_swf_rgba *p00 = &b->px[(size_t)v0 * w + u0];
        const ps_swf_rgba *p10 = &b->px[(size_t)v0 * w + u1];
        const ps_swf_rgba *p01 = &b->px[(size_t)v1 * w + u0];
        const ps_swf_rgba *p11 = &b->px[(size_t)v1 * w + u1];
        float w00 = (1.0f - su) * (1.0f - sv), w10 = su * (1.0f - sv);
        float w01 = (1.0f - su) * sv,          w11 = su * sv;
        ps_swf_rgba      out;
        const uint8_t   *a = (const uint8_t *)p00, *bb = (const uint8_t *)p10;
        const uint8_t   *c = (const uint8_t *)p01, *d = (const uint8_t *)p11;
        uint8_t         *o = (uint8_t *)&out;
        int              k;

        /* Straight per-channel interpolation, alpha included and not weighting
         * the colour. Flash does not weight by alpha either, and doing so
         * would darken the fringe of every cut-out bitmap against what every
         * other player shows. */
        for(k = 0; k < 4; k++)
            o[k] = (uint8_t)(a[k] * w00 + bb[k] * w10 +
                             c[k] * w01 + d[k] * w11 + 0.5f);
        return out;
    }
}
