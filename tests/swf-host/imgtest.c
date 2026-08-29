/* What the bitmap decoder refuses, and what the sampler returns.
 *
 * The generated .swf files assert that a well-formed bitmap draws the right
 * pixels. This asserts the other half, which the picture cannot show: that a
 * malformed one is refused rather than tolerated, and refused for the stated
 * reason. Both halves matter equally here and the second one arguably more -
 * these tags decode a compressed stream that arrived over HTTP from a page we
 * did not write, onto a machine with no memory protection, and the difference
 * between "refused" and "drew something odd" is the difference between a
 * broken picture and a wild write.
 *
 * Two properties are checked beyond the return value. Refusals that happen
 * before any allocation are asserted to have allocated nothing, by comparing
 * the allocator's live total across the call - because "it returns an error
 * eventually" is not the claim being made about a stream that says it expands
 * to a gigabyte. And the sampler is called at stated points rather than
 * counted over an area, which is the only way to pin down what filtering does
 * between two texels: a pixel count can say a filter ran, but not that it
 * averaged the right two things in the right proportion.
 *
 *   ./imgtest
 */
#include "ps_swf.h"
#include "ps_swf_geom.h"
#include "ps_swf_image.h"

#include <stdio.h>
#include <string.h>

static int fails;

static void eq(const char *what, long got, long want)
{
    if(got == want) {
        printf("  ok    %-46s %ld\n", what, got);
        return;
    }
    printf("  FAIL  %-46s %ld, want %ld\n", what, got, want);
    fails++;
}

static void eq_err(const char *what, ps_swf_img_err got, ps_swf_img_err want)
{
    if(got == want) {
        printf("  ok    %-46s %s\n", what, ps_swf_img_reason(got));
        return;
    }
    printf("  FAIL  %-46s %s, want %s\n", what,
           ps_swf_img_reason(got), ps_swf_img_reason(want));
    fails++;
}

/* A zlib stream of stored deflate blocks, the same construction mkswf uses.
 * Duplicated rather than shared because the two want different things from it:
 * mkswf builds streams that are correct, and this file builds streams that are
 * wrong in one stated way each, which is easier to do to a local writer than
 * to a shared one that would grow a flag per malformation. */
static size_t zstore(uint8_t *dst, const uint8_t *src, size_t n)
{
    uint32_t a = 1, b = 0;
    size_t   i, w = 0;

    dst[w++] = 0x78;
    dst[w++] = 0x01;
    dst[w++] = 1;                       /* BFINAL, BTYPE = stored */
    dst[w++] = (uint8_t)(n & 0xff);
    dst[w++] = (uint8_t)(n >> 8);
    dst[w++] = (uint8_t)(~n & 0xff);
    dst[w++] = (uint8_t)((~n >> 8) & 0xff);
    memcpy(dst + w, src, n);
    w += n;
    for(i = 0; i < n; i++) {
        a = (a + src[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    dst[w++] = (uint8_t)(b >> 8);
    dst[w++] = (uint8_t)b;
    dst[w++] = (uint8_t)(a >> 8);
    dst[w++] = (uint8_t)a;
    return w;
}

/* A DefineBitsLossless body, with the compressed payload supplied by the
 * caller so it can be as wrong as the test needs. */
static size_t lossless_body(uint8_t *dst, int id, int fmt, int w, int h,
                            int ncolors, const uint8_t *z, size_t zlen)
{
    size_t n = 0;

    dst[n++] = (uint8_t)(id & 0xff);
    dst[n++] = (uint8_t)(id >> 8);
    dst[n++] = (uint8_t)fmt;
    dst[n++] = (uint8_t)(w & 0xff);
    dst[n++] = (uint8_t)(w >> 8);
    dst[n++] = (uint8_t)(h & 0xff);
    dst[n++] = (uint8_t)(h >> 8);
    if(fmt == 3)
        dst[n++] = (uint8_t)(ncolors - 1);
    memcpy(dst + n, z, zlen);
    return n + zlen;
}

/* Runs a decode that is expected to fail and asserts that it allocated nothing
 * on the way, which is the property the size bound exists to provide. */
static void refuse(const char *what, const uint8_t *body, size_t len, int ver,
                   ps_swf_img_err want, int no_alloc)
{
    ps_swf_bitmap  bm;
    size_t         before = ps_swf_mem_live();
    ps_swf_img_err e      = ps_swf_bitmap_lossless(body, len, ver, &bm);

    eq_err(what, e, want);
    if(e == PS_SWF_IMG_OK)
        ps_swf_bitmap_free(&bm);
    if(no_alloc)
        eq("  ... and held nothing afterwards",
           (long)(ps_swf_mem_live() - before), 0);
}

static void test_lossless_refusals(void)
{
    uint8_t body[512], z[512], raw[256];
    size_t  zlen, n;

    printf("lossless refusals\n");

    memset(raw, 0, sizeof raw);

    /* A 2x2 image in 24-bit form needs sixteen bytes and no more. This stream
     * carries sixty-four, which is the small end of the same lie a stream
     * claiming to expand to a gigabyte tells - and it fails the same way,
     * inside the inflate, against a buffer that cannot grow. */
    zlen = zstore(z, raw, 64);
    n    = lossless_body(body, 1, 5, 2, 2, 0, z, zlen);
    refuse("zlib expands past the size the picture implies", body, n, 1,
           PS_SWF_IMG_ZLIB, 0);

    /* Valid, terminates, and stops eight bytes short of a picture. Half an
     * image is not an image, so the count is checked as well as the status. */
    zlen = zstore(z, raw, 8);
    n    = lossless_body(body, 1, 5, 2, 2, 0, z, zlen);
    refuse("zlib stops before the picture is complete", body, n, 1,
           PS_SWF_IMG_ZLIB, 0);

    /* The stream itself cut off mid-block, which is what a truncated download
     * looks like rather than what a hostile file looks like. */
    zlen = zstore(z, raw, 16);
    n    = lossless_body(body, 1, 5, 2, 2, 0, z, zlen - 8);
    refuse("zlib stream truncated mid-block", body, n, 1,
           PS_SWF_IMG_ZLIB, 0);

    /* Two colours in the table and a pixel naming the sixth. Refused rather
     * than clamped: a clamp paints a picture out of colours the file never
     * named, and a picture is exactly what nobody looks at twice. */
    raw[0] = 0x11; raw[1] = 0x22; raw[2] = 0x33;    /* colour 0 */
    raw[3] = 0x44; raw[4] = 0x55; raw[5] = 0x66;    /* colour 1 */
    /* Two rows of two indices, each row padded out to four bytes - so the bad
     * index is at the start of the second row and the two bytes behind it are
     * padding that must never be looked at. */
    raw[6] = 0; raw[7] = 1;
    raw[10] = 5; raw[11] = 0;
    zlen = zstore(z, raw, 6 + 8);
    n    = lossless_body(body, 2, 3, 2, 2, 2, z, zlen);
    refuse("palette index past the end of the table", body, n, 1,
           PS_SWF_IMG_PALETTE, 1);

    /* 65535 by 65535 at four bytes a pixel is sixteen gigabytes, and the
     * product alone overflows a 32-bit size_t on the way to finding that out.
     * Nothing is allocated, which is the assertion that matters. */
    n = lossless_body(body, 3, 5, 65535, 65535, 0, z, zlen);
    refuse("dimensions whose product overflows", body, n, 1,
           PS_SWF_IMG_TOO_BIG, 1);

    /* Inside the per-dimension cap and four times past the pixel cap, so this
     * is the second bound doing the work rather than the first. */
    n = lossless_body(body, 3, 5, 2048, 2048, 0, z, zlen);
    refuse("dimensions past the pixel cap", body, n, 1,
           PS_SWF_IMG_TOO_BIG, 1);

    n = lossless_body(body, 3, 5, 0, 16, 0, z, zlen);
    refuse("a bitmap with no pixels", body, n, 1, PS_SWF_IMG_TOO_BIG, 1);

    /* 15-bit pixels do not exist in DefineBitsLossless2. Guessing at one costs
     * a factor of two in the expected size, so the stream would be refused
     * anyway - but for the wrong reason, and the reason is what gets read. */
    n = lossless_body(body, 4, 4, 2, 2, 0, z, zlen);
    refuse("15-bit pixels in a DefineBitsLossless2", body, n, 2,
           PS_SWF_IMG_FORMAT, 1);

    n = lossless_body(body, 4, 9, 2, 2, 0, z, zlen);
    refuse("a BitmapFormat that does not exist", body, n, 1,
           PS_SWF_IMG_FORMAT, 1);

    refuse("a tag that ends inside its own header", body, 4, 1,
           PS_SWF_IMG_TRUNCATED, 1);
}

/* One decode that must succeed, checked at a value the .swf tests deliberately
 * avoid: the middle of the five-bit range, where every plausible expansion to
 * eight bits gives a different answer and the one this uses is stated. */
static void test_lossless_decode(void)
{
    uint8_t        body[128], z[128], raw[16];
    ps_swf_bitmap  bm;
    ps_swf_img_err e;
    size_t         zlen, n;

    printf("lossless decode\n");

    /* One 15-bit pixel of (16, 31, 0) - a channel in the middle, one at the
     * top and one at the bottom. A row of one pixel is two bytes and pads out
     * to four, so the stream is four bytes for a one-pixel image, which is the
     * padding rule stated as an arithmetic fact rather than an area. */
    memset(raw, 0, sizeof raw);
    raw[0] = (uint8_t)(((16u << 10) | (31u << 5)) >> 8);
    raw[1] = (uint8_t)(((16u << 10) | (31u << 5)) & 0xff);
    zlen = zstore(z, raw, 4);
    n    = lossless_body(body, 7, 4, 1, 1, 0, z, zlen);

    e = ps_swf_bitmap_lossless(body, n, 1, &bm);
    eq_err("a 1x1 15-bit image with a padded row", e, PS_SWF_IMG_OK);
    if(e != PS_SWF_IMG_OK)
        return;
    eq("  id", bm.id, 7);
    eq("  width", bm.w, 1);
    /* (16*255 + 15) / 31 = 132, and 31 goes to 255 exactly. */
    eq("  red, five bits of 16 expanded to eight", bm.px[0].r, 132);
    eq("  green, five bits of 31 expanded to eight", bm.px[0].g, 255);
    eq("  blue", bm.px[0].b, 0);
    eq("  alpha, which this format does not carry", bm.px[0].a, 255);
    ps_swf_bitmap_free(&bm);
}

static void test_jpeg_refusals(void)
{
    uint8_t        body[64];
    ps_swf_bitmap  bm;
    ps_swf_img_err e;

    printf("JPEG refusals\n");

    /* A DefineBits is half a JPEG by construction. Without the other half
     * there is nothing to hand a decoder, and saying so is not the same as
     * saying the stream is corrupt. */
    memset(body, 0, sizeof body);
    body[2] = 0xff; body[3] = 0xd8;
    e = ps_swf_bitmap_jpeg(body, 16, 1, NULL, 0, &bm);
    eq_err("DefineBits with no JPEGTables", e, PS_SWF_IMG_NO_TABLES);

    /* SWF 8 lets a PNG live inside a tag whose name says JPEG, with entirely
     * different alpha rules. Named rather than decoded, because a picture that
     * silently does not appear is indistinguishable from a player that cannot
     * draw bitmaps at all. */
    memset(body, 0, sizeof body);
    body[2] = 0x89;
    memcpy(body + 3, "PNG\r\n\032\n", 7);
    e = ps_swf_bitmap_jpeg(body, 16, 2, NULL, 0, &bm);
    eq_err("a PNG inside a DefineBitsJPEG2", e, PS_SWF_IMG_UNSUPPORTED);

    memset(body, 0, sizeof body);
    memcpy(body + 2, "GIF89a", 6);
    e = ps_swf_bitmap_jpeg(body, 16, 2, NULL, 0, &bm);
    eq_err("a GIF inside a DefineBitsJPEG2", e, PS_SWF_IMG_UNSUPPORTED);

    memset(body, 0x5a, sizeof body);
    e = ps_swf_bitmap_jpeg(body, 32, 2, NULL, 0, &bm);
    eq_err("bytes that are not a JPEG at all", e, PS_SWF_IMG_CODEC);

    /* The alpha plane of a DefineBitsJPEG3 is located by an offset rather than
     * a length, so the offset is a number from the file pointing into the file
     * - the shape of bug that reads whatever follows the tag. */
    memset(body, 0, sizeof body);
    body[2] = 0xff; body[3] = 0xff; body[4] = 0xff; body[5] = 0x7f;
    e = ps_swf_bitmap_jpeg(body, 32, 3, NULL, 0, &bm);
    eq_err("a JPEG3 alpha offset past the end of the tag", e,
           PS_SWF_IMG_TRUNCATED);

    e = ps_swf_bitmap_jpeg(body, 4, 3, NULL, 0, &bm);
    eq_err("a JPEG3 that ends inside its own offset", e, PS_SWF_IMG_TRUNCATED);
}

/* A 2x2 bitmap held on the stack, so the sampler can be asked about a stated
 * point instead of being counted over an area. */
static ps_swf_rgba g_px[4];

static ps_swf_bitmap two_by_two(void)
{
    ps_swf_bitmap b;

    g_px[0] = (ps_swf_rgba){ 0x00, 0x00, 0x00, 0xff };   /* A, top left */
    g_px[1] = (ps_swf_rgba){ 0x40, 0x80, 0xc0, 0xff };   /* B, top right */
    g_px[2] = (ps_swf_rgba){ 0xff, 0x00, 0x00, 0xff };   /* C */
    g_px[3] = (ps_swf_rgba){ 0x00, 0xff, 0x00, 0xff };   /* D */
    b.id = 1;
    b.w  = 2;
    b.h  = 2;
    b.px = g_px;
    return b;
}

static void sample_is(const char *what, ps_swf_rgba got, ps_swf_rgba want)
{
    if(!memcmp(&got, &want, sizeof got)) {
        printf("  ok    %-46s %02x%02x%02x%02x\n", what,
               got.r, got.g, got.b, got.a);
        return;
    }
    printf("  FAIL  %-46s %02x%02x%02x%02x, want %02x%02x%02x%02x\n", what,
           got.r, got.g, got.b, got.a, want.r, want.g, want.b, want.a);
    fails++;
}

static void test_sampler(void)
{
    ps_swf_bitmap b = two_by_two();
    ps_swf_rgba   mid = { 0x20, 0x40, 0x60, 0xff };

    printf("sampler\n");

    /* Clipped clamps: everything left of the image is the left column and
     * everything past the right edge is the right column, however far past. */
    sample_is("clipped, far left of the image",
              ps_swf_bitmap_sample(&b, -9.5f, 0.5f, 0, 0), g_px[0]);
    sample_is("clipped, far right and below",
              ps_swf_bitmap_sample(&b, 40.5f, 40.5f, 0, 0), g_px[3]);

    /* Tiled wraps, and wraps correctly for negative coordinates - C's % keeps
     * the sign of the dividend, so the naive form puts a seam at the origin
     * and only there, which is exactly where nobody looks. */
    sample_is("tiled, two images to the right",
              ps_swf_bitmap_sample(&b, 4.5f, 0.5f, 1, 0), g_px[0]);
    sample_is("tiled, one texel to the left of the origin",
              ps_swf_bitmap_sample(&b, -0.5f, 0.5f, 1, 0), g_px[1]);
    sample_is("tiled, one texel above the origin",
              ps_swf_bitmap_sample(&b, 0.5f, -0.5f, 1, 0), g_px[2]);
    sample_is("tiled, a whole image above the origin",
              ps_swf_bitmap_sample(&b, 0.5f, -1.5f, 1, 0), g_px[0]);

    /* At a texel centre the filter has one tap at full weight, so a smoothed
     * sample there must equal the unsmoothed one exactly. If it does not, the
     * half-texel offset is missing and every filtered bitmap in the player is
     * half a texel out. */
    sample_is("smoothed, exactly on a texel centre",
              ps_swf_bitmap_sample(&b, 0.5f, 0.5f, 0, 1), g_px[0]);

    /* Half way between two texel centres, where the two colours were chosen so
     * that their mean is exact and the assertion is about the weight rather
     * than about rounding. The same point unsmoothed lands wholly in B, which
     * is what makes the two fill style IDs distinguishable at all. */
    sample_is("smoothed, half way between two texels",
              ps_swf_bitmap_sample(&b, 1.0f, 0.5f, 0, 1), mid);
    sample_is("the same point, unsmoothed",
              ps_swf_bitmap_sample(&b, 1.0f, 0.5f, 0, 0), g_px[1]);
}

/* The four bitmap fill style IDs, through the paint stage that a renderer
 * actually consults. Asserting the sampler honours tiling and filtering is
 * only half of it; the other half is that the style ID a file wrote reaches
 * the sampler as the pair of flags it means. */
static void test_fill_styles(void)
{
    ps_swf_bitmap     bm = two_by_two();
    ps_swf_bitmapfill bf;
    ps_swf_fill       fills[4];
    ps_swf_shape      sh;
    ps_swf_xform      xf;
    int               i;
    static const int  tiled[4]    = { 1, 0, 1, 0 };
    static const int  smoothed[4] = { 1, 1, 0, 0 };

    printf("fill styles\n");

    memset(&bf, 0, sizeof bf);
    bf.mat[0] = bf.mat[3] = 20.0f;      /* one texel to one unit of shape space */
    bf.bmp    = &bm;

    memset(fills, 0, sizeof fills);
    for(i = 0; i < 4; i++) {
        fills[i].type  = (uint8_t)(0x40 + i);
        fills[i].bfill = 1;
    }

    memset(&sh, 0, sizeof sh);
    sh.fills  = fills;
    sh.nfill  = 4;
    sh.bfills = &bf;
    sh.nbfill = 1;
    ps_swf_xform_identity(&xf);

    for(i = 0; i < 4; i++) {
        ps_swf_paint p;
        char         what[64];

        ps_geom_fill_paint(&sh, &xf, NULL, (uint32_t)(i + 1), &p);
        snprintf(what, sizeof what, "fill style 0x%02x resolves to a bitmap",
                 0x40 + i);
        eq(what, p.bitmap == &bm, 1);
        snprintf(what, sizeof what, "fill style 0x%02x tiled", 0x40 + i);
        eq(what, p.tiled != 0, tiled[i]);
        snprintf(what, sizeof what, "fill style 0x%02x smoothed", 0x40 + i);
        eq(what, p.smoothed != 0, smoothed[i]);
    }

    /* A fill whose character never resolved has no bitmap and no inverse, and
     * falls back to the flat colour the parser put there. Drawing the fallback
     * is a deliberate choice over drawing nothing: a missing rectangle is
     * indistinguishable from geometry that never parsed. */
    {
        ps_swf_paint p;

        bf.bmp = NULL;
        ps_geom_fill_paint(&sh, &xf, NULL, 1, &p);
        eq("an unresolved bitmap draws flat", p.bitmap == NULL, 1);
    }
}

int main(void)
{
    size_t peak;

    ps_swf_mem_reset_peak();

    test_lossless_refusals();
    test_lossless_decode();
    test_jpeg_refusals();
    test_sampler();
    test_fill_styles();

    peak = ps_swf_mem_peak();
    printf("held %zu bytes at the end, peaked at %zu\n",
           ps_swf_mem_live(), peak);
    if(fails)
        printf("%d FAILED\n", fails);
    return fails ? 1 : 0;
}
