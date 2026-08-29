/* Bitmap characters: the five tags that carry pixels, and the sampler.
 *
 * This is the first thing in the player that decodes a compressed stream whose
 * expansion ratio is chosen by whoever wrote the file. Every other allocation
 * so far is bounded by the tag's own length, because a shape record cannot
 * describe more edges than it has bytes to describe them with. A zlib stream
 * has no such property: sixty bytes can honestly claim to expand to a
 * gigabyte, and the claim is not detectable from the bytes without inflating
 * them. So the rule here is stronger than "check the result" - the expansion
 * has to be refused before it is attempted.
 *
 * It is refused by construction. Width, height and format are read first, the
 * exact number of bytes those three imply is computed, and the inflate is run
 * into a buffer of exactly that size with expansion disabled. A stream that
 * wants one byte more fails at the byte, having allocated nothing, and a
 * stream that ends early produces fewer bytes than the picture needs and is
 * refused on the count. Neither case reaches a decoder and neither case sizes
 * an allocation from anything the file said about its own contents.
 *
 * On the inflater and the JPEG decoder: both are stb_image, which is vendored
 * at vendor/stb_image.h, is public domain or MIT at the caller's choice, and
 * is already what core/ps_image.c decodes the browser's page images with.
 * Docs/licensing.md is emphatic that nothing GPL may link into this library
 * and stb is the reason there was never a question. Two notes on wiring it up.
 * It is a single-header library, and the browser links two instances of it -
 * core/ps_image.c for page images and this file for bitmap tags - so this one
 * is STB_IMAGE_STATIC. The duplication is the price of the two being
 * configured differently, and the configuration is not cosmetic: see the note
 * at the define. And SIMD is switched off in this build - not for portability,
 * but because the SSE2 colour conversion path rounds differently from the
 * scalar one, and a test that asserts an exact pixel value has to get the same
 * answer on every machine that runs it.
 *
 * The JPEG tags need work stb cannot do, and it is the interesting half.
 * DefineBits carries a JPEG stream with its quantisation and Huffman tables
 * stripped out; the tables live once per file in a JPEGTables tag and have to
 * be spliced back in before any decoder will look at it. The splice is not a
 * concatenation, because both halves are complete JPEG streams in their own
 * right: the tables end with EOI and the image starts with SOI, so joining
 * them puts an end-of-image marker in the middle. Flash's own writer left that
 * pair in place and Flash's own reader skipped it, so files in the wild depend
 * on a reader that removes it - including files where the pair appears at the
 * very start of a DefineBitsJPEG2 that needs no splice at all. Both are
 * handled, and the rule is Ruffle's (render/src/utils.rs, MIT OR Apache-2.0):
 * drop a leading FFD9 FFD8, and drop the first such pair found anywhere else.
 */
#ifndef PS_SWF_IMAGE_H
#define PS_SWF_IMAGE_H

#include "ps_swf.h"

/* What a bitmap may cost before it is refused.
 *
 * Both numbers exist because the alternative is an allocation whose size is a
 * sixteen-bit field times another sixteen-bit field times four, which is
 * sixteen gigabytes at the top end and overflows a 32-bit size_t on the way
 * there. The dimension cap is the PVR's largest texture doubled, on the
 * grounds that a bitmap wider than that cannot be drawn on the target however
 * successfully it decodes; the pixel cap is what bounds the product, and at
 * four bytes a pixel it is four megabytes of a sixteen megabyte machine, which
 * is already more than one picture should ever be allowed to take. */
#define PS_SWF_BITMAP_MAX_DIM    2048u
#define PS_SWF_BITMAP_MAX_PIXELS (1024u * 1024u)

/* One JPEGTables tag per file, and it is a handful of Huffman and quantisation
 * tables - four hundred bytes in everything Flash wrote. The cap is three
 * orders of magnitude above that because the tag is held by reference rather
 * than copied, so its only cost is the splice buffer it is later half of. */
#define PS_SWF_JPEGTABLES_MAX (64u * 1024u)

/* Why a bitmap was refused. Every one of these is a decision to draw the
 * fill's fallback colour instead of its picture, and the caller is expected to
 * say which - a bitmap that silently does not appear is indistinguishable from
 * a renderer that cannot draw bitmaps, which is exactly the confusion that
 * costs a day. */
typedef enum {
    PS_SWF_IMG_OK = 0,
    PS_SWF_IMG_TRUNCATED,     /* the tag ends inside its own header */
    PS_SWF_IMG_FORMAT,        /* a BitmapFormat this tag cannot carry */
    PS_SWF_IMG_TOO_BIG,       /* past a cap above, or past what size_t holds */
    PS_SWF_IMG_ZLIB,          /* the stream lied about its size, or ended early */
    PS_SWF_IMG_PALETTE,       /* an index past the end of the colour table */
    PS_SWF_IMG_NO_TABLES,     /* DefineBits with no JPEGTables ahead of it */
    PS_SWF_IMG_CODEC,         /* the decoder refused the stream */
    PS_SWF_IMG_UNSUPPORTED,   /* a codec this build does not implement */
    PS_SWF_IMG_MEMORY
} ps_swf_img_err;

const char *ps_swf_img_reason(ps_swf_img_err e);

/* `ver` is 1 for DefineBitsLossless and 2 for DefineBitsLossless2. The two
 * differ by more than an alpha channel: 15-bit pixels are legal only in the
 * first and 32-bit pixels only in the second, so the version decides which
 * BitmapFormat values are accepted rather than only how wide a colour is.
 *
 * Nodiscard on both of these is not stylistic. They write `out` only on
 * success, so ignoring the return means drawing whatever the caller's
 * uninitialised ps_swf_bitmap happened to contain - which on this target is a
 * pointer into unmapped memory being read once per pixel. */
[[nodiscard]] ps_swf_img_err ps_swf_bitmap_lossless(const uint8_t *body,
                                                    size_t len, int ver,
                                                    ps_swf_bitmap *out);

/* `ver` is 1 for DefineBits, 2 for DefineBitsJPEG2 and 3 for DefineBitsJPEG3.
 * `tables` is the JPEGTables body and is required only by version 1, which is
 * the whole difference between it and version 2. */
[[nodiscard]] ps_swf_img_err ps_swf_bitmap_jpeg(const uint8_t *body, size_t len,
                                                int ver, const uint8_t *tables,
                                                size_t tlen,
                                                ps_swf_bitmap *out);

void ps_swf_bitmap_free(ps_swf_bitmap *b);

/* One texel, in texel coordinates, with the two address modes and the two
 * filters the four bitmap fill style IDs select between.
 *
 * Clipped clamps rather than dropping out. That is what the PVR's clamp mode
 * does and what Flash does, and it is visible: a clipped fill over a region
 * larger than its bitmap smears the edge row and column across the remainder
 * instead of leaving it blank, which looks wrong but is what the file asked
 * for and what every other player shows. */
ps_swf_rgba ps_swf_bitmap_sample(const ps_swf_bitmap *b, float u, float v,
                                 int tiled, int smoothed);

#endif /* PS_SWF_IMAGE_H */
