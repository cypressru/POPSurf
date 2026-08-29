/* Generates small SWF files with known geometry.
 *
 * The real target file is a Sega intro: fifteen kilobytes, five shapes, and
 * no way to tell a subtly wrong result from a right one by eye. These are the
 * opposite. Each is a few hundred bytes describing a shape whose exact pixels
 * can be worked out on paper, so when the renderer is wrong the picture says
 * which part is wrong.
 *
 * They also cover the things about SWF shapes that are easy to get wrong and
 * impossible to notice on a file that happens not to use them: a hole made by
 * winding direction rather than by a subtract operation, one edge carrying a
 * different fill on each side, quadratic curves, gradient fills, and strokes.
 *
 * The gradient files carry a solid fill style declared *after* the gradient,
 * and assert its colour. That is the real point of them. A gradient fill style
 * is the only variable-length record in the fill style array, so if its
 * matrix or its stop list is stepped by one bit too few, every style after it
 * in the array decodes from the wrong offset - and the shape still draws,
 * still in the right places, just in colours read out of the middle of
 * something else. Asserting the colour of the style behind the gradient is
 * what turns that into a test failure instead of a puzzle.
 *
 * The later files are movies rather than shapes, and the same rule holds with
 * an extra step: a rectangle placed by the display list and clipped by another
 * rectangle leaves the area of an intersection, which is still a number worked
 * out on paper. The masks are placed on integer pixel boundaries so that answer
 * does not depend on how a mask edge is antialiased - which matters beyond
 * tidiness, since the hardware backend's mask edge is hard where this one is
 * soft, and a test whose answer moved between them would be no test at all.
 *
 *   ./mkswf outdir
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

/* -std=c99 does not expose M_PI. */
#define PS_PI 3.14159265358979323846

/* --- bit writer --------------------------------------------------------- */

typedef struct {
    uint8_t b[65536];
    size_t  n;
    uint8_t acc;
    int     nb;      /* bits held in acc */
} bw;

static void bw_bits(bw *w, uint32_t v, int n)
{
    while(n-- > 0) {
        w->acc = (uint8_t)((w->acc << 1) | ((v >> n) & 1));
        if(++w->nb == 8) {
            w->b[w->n++] = w->acc;
            w->acc = 0;
            w->nb  = 0;
        }
    }
}

static void bw_flush(bw *w)
{
    if(w->nb) {
        w->b[w->n++] = (uint8_t)(w->acc << (8 - w->nb));
        w->acc = 0;
        w->nb  = 0;
    }
}

static void bw_u8(bw *w, unsigned v)  { bw_flush(w); w->b[w->n++] = (uint8_t)v; }
static void bw_u16(bw *w, unsigned v) { bw_u8(w, v & 0xff); bw_u8(w, (v >> 8) & 0xff); }
static void bw_u32(bw *w, uint32_t v) { bw_u16(w, v & 0xffff); bw_u16(w, v >> 16); }

/* Narrowest signed field that can hold v, which is what the format asks for
 * everywhere and what makes a hand-written encoder disagree with a real one
 * if it is off by one. */
static int sbits(int32_t v)
{
    int n = 1;

    while(n < 32) {
        int64_t lo = -((int64_t)1 << (n - 1));
        int64_t hi = ((int64_t)1 << (n - 1)) - 1;
        if((int64_t)v >= lo && (int64_t)v <= hi)
            return n;
        n++;
    }
    return 32;
}

static int sbits4(int32_t a, int32_t b, int32_t c, int32_t d)
{
    int n = sbits(a), t;

    t = sbits(b); if(t > n) n = t;
    t = sbits(c); if(t > n) n = t;
    t = sbits(d); if(t > n) n = t;
    return n;
}

static void bw_rect(bw *w, int32_t x0, int32_t x1, int32_t y0, int32_t y1)
{
    int n = sbits4(x0, x1, y0, y1);

    bw_bits(w, (uint32_t)n, 5);
    bw_bits(w, (uint32_t)x0, n);
    bw_bits(w, (uint32_t)x1, n);
    bw_bits(w, (uint32_t)y0, n);
    bw_bits(w, (uint32_t)y1, n);
    bw_flush(w);
}

/* --- shape record helpers ----------------------------------------------- */

typedef struct {
    bw     *w;
    int     nfillbits;
    int     nlinebits;
    int32_t x, y;      /* pen, twips */
} shp;

/* line < 0 leaves the line style alone; 0 and up select one, with 0 meaning
 * no stroke. Fills keep the older convention where 0 means "do not mention
 * it", which is all the fill-only files here need. */
static void sc_move_ex(shp *s, int32_t x, int32_t y, int fill0, int fill1,
                       int line)
{
    int n = sbits(x) > sbits(y) ? sbits(x) : sbits(y);

    bw_bits(s->w, 0, 1);                          /* not an edge */
    bw_bits(s->w, (uint32_t)((line >= 0 ? 8 : 0) | (fill1 ? 4 : 0) |
                             (fill0 ? 2 : 0) | 1), 5);
    bw_bits(s->w, (uint32_t)n, 5);
    bw_bits(s->w, (uint32_t)x, n);
    bw_bits(s->w, (uint32_t)y, n);
    if(fill0)      bw_bits(s->w, (uint32_t)fill0, s->nfillbits);
    if(fill1)      bw_bits(s->w, (uint32_t)fill1, s->nfillbits);
    if(line >= 0)  bw_bits(s->w, (uint32_t)line,  s->nlinebits);
    s->x = x;
    s->y = y;
}

static void sc_move(shp *s, int32_t x, int32_t y, int fill0, int fill1)
{
    sc_move_ex(s, x, y, fill0, fill1, -1);
}

/* fill0/fill1 are "state changes", so -1 means leave alone and 0 means
 * explicitly clear - the distinction the format makes and the one a test
 * needs when an edge has to drop a fill it inherited. */
static void sc_style(shp *s, int fill0, int fill1)
{
    bw_bits(s->w, 0, 1);
    bw_bits(s->w, (uint32_t)((fill1 >= 0 ? 4 : 0) | (fill0 >= 0 ? 2 : 0)), 5);
    if(fill0 >= 0) bw_bits(s->w, (uint32_t)fill0, s->nfillbits);
    if(fill1 >= 0) bw_bits(s->w, (uint32_t)fill1, s->nfillbits);
}

static void sc_line_to(shp *s, int32_t x, int32_t y)
{
    int32_t dx = x - s->x, dy = y - s->y;
    int     n  = sbits(dx) > sbits(dy) ? sbits(dx) : sbits(dy);

    if(n < 2) n = 2;
    bw_bits(s->w, 1, 1);              /* edge */
    bw_bits(s->w, 1, 1);              /* straight */
    bw_bits(s->w, (uint32_t)(n - 2), 4);
    bw_bits(s->w, 1, 1);              /* general line, both deltas present */
    bw_bits(s->w, (uint32_t)dx, n);
    bw_bits(s->w, (uint32_t)dy, n);
    s->x = x;
    s->y = y;
}

static void sc_curve_to(shp *s, int32_t cx, int32_t cy, int32_t x, int32_t y)
{
    int32_t c0 = cx - s->x, c1 = cy - s->y;
    int32_t a0 = x - cx,    a1 = y - cy;
    int     n  = sbits4(c0, c1, a0, a1);

    if(n < 2) n = 2;
    bw_bits(s->w, 1, 1);
    bw_bits(s->w, 0, 1);              /* curved */
    bw_bits(s->w, (uint32_t)(n - 2), 4);
    bw_bits(s->w, (uint32_t)c0, n);
    bw_bits(s->w, (uint32_t)c1, n);
    bw_bits(s->w, (uint32_t)a0, n);
    bw_bits(s->w, (uint32_t)a1, n);
    s->x = x;
    s->y = y;
}

static void sc_end(shp *s)
{
    bw_bits(s->w, 0, 1);
    bw_bits(s->w, 0, 5);
    bw_flush(s->w);
}

/* --- file assembly ------------------------------------------------------ */

static void tag(bw *out, int code, const bw *body)
{
    if(body->n < 0x3f) {
        bw_u16(out, (unsigned)((code << 6) | (int)body->n));
    } else {
        bw_u16(out, (unsigned)((code << 6) | 0x3f));
        bw_u32(out, (uint32_t)body->n);
    }
    memcpy(out->b + out->n, body->b, body->n);
    out->n += body->n;
}

static void write_swf(const char *path, int stage_w, int stage_h,
                      const bw *tags)
{
    bw    hdr;
    FILE *f;
    uint32_t total;

    memset(&hdr, 0, sizeof hdr);
    bw_rect(&hdr, 0, stage_w * 20, 0, stage_h * 20);
    bw_u16(&hdr, 12 << 8);              /* FIXED8 12.0 fps, fraction first */
    bw_u16(&hdr, 1);                    /* frame count */

    total = (uint32_t)(8 + hdr.n + tags->n);

    f = fopen(path, "wb");
    if(!f) {
        fprintf(stderr, "cannot write %s\n", path);
        return;
    }
    fwrite("FWS", 1, 3, f);
    fputc(4, f);                        /* version */
    fputc((int)(total & 0xff), f);
    fputc((int)((total >> 8) & 0xff), f);
    fputc((int)((total >> 16) & 0xff), f);
    fputc((int)((total >> 24) & 0xff), f);
    fwrite(hdr.b, 1, hdr.n, f);
    fwrite(tags->b, 1, tags->n, f);
    fclose(f);
    printf("wrote %s (%u bytes)\n", path, total);
}

/* alpha < 0 writes RGB fills, which is what DefineShape and DefineShape2
 * carry; 0..255 writes RGBA, which only DefineShape3 does. The one byte of
 * difference shifts every field after it, so a reader that gets the shape
 * version wrong produces garbage rather than an error - hence a test that
 * exercises both. */
static void begin_shape(bw *body, shp *s, int id,
                        int32_t x0, int32_t x1, int32_t y0, int32_t y1,
                        const uint8_t *rgb, int nfill, int alpha)
{
    int i;

    memset(body, 0, sizeof *body);
    bw_u16(body, (unsigned)id);
    bw_rect(body, x0, x1, y0, y1);

    bw_u8(body, (unsigned)nfill);
    for(i = 0; i < nfill; i++) {
        bw_u8(body, 0x00);              /* solid */
        bw_u8(body, rgb[i * 3 + 0]);
        bw_u8(body, rgb[i * 3 + 1]);
        bw_u8(body, rgb[i * 3 + 2]);
        if(alpha >= 0)
            bw_u8(body, (unsigned)alpha);
    }
    bw_u8(body, 0);                     /* no line styles */

    /* Four bits is more than these need, but a fixed width keeps the
     * generator honest: a reader that ignores NumFillBits and assumes the
     * minimum still passes on a one-fill file, and would fail here. */
    s->w = body;
    s->nfillbits = 4;
    s->nlinebits = 0;
    s->x = s->y = 0;
    bw_bits(body, 4, 4);                /* NumFillBits */
    bw_bits(body, 0, 4);                /* NumLineBits */
}

/* --- gradient and stroke shapes ----------------------------------------- */

/* A fill style the generator can describe. The gradient matrix is given as a
 * scale and a translation only, because that is all a test needs: the ramp has
 * to land somewhere the arithmetic can predict, and a rotation would put the
 * boundary between two stops on a diagonal, where the pixel count stops being
 * derivable. Rotation is exercised by real files, not by this one. */
typedef struct {
    int     type;            /* 0x00 solid, 0x10/0x12 gradient, 0x40..0x43 bitmap */
    uint8_t rgba[4];         /* solid only */
    int     bitmap;          /* character ID, bitmap fills only */
    double  mscale;          /* source space -> shape twips */
    double  mscale2;         /* the d term, so a bitmap can be anisotropic */
    double  mrot0, mrot1;    /* b and c; a bitmap fill uses them, no gradient here does */
    int32_t mtx, mty;
    int     nstop;
    uint8_t ratio[8];
    uint8_t stop[8][4];
} fsty;

typedef struct {
    int     width;           /* twips */
    uint8_t rgba[4];
} lsty;

/* The general MATRIX, in the format's own a, b, c, d order.
 *
 * The scale block is always written and the rotate block only when it carries
 * something, which is the encoding real files use and the one whose optional
 * fields a reader most often mishandles: a missing scale block means 1, not 0,
 * and a reader that memsets the matrix and then fills in what it finds
 * produces a gradient collapsed to a point. The rotated case here is the
 * mirror of that mistake - it writes zero scales and non-zero skews, so a
 * reader that skips the rotate block is left with a singular matrix and draws
 * a flat fallback colour instead of a picture. */
static void bw_matrix_ex(bw *w, double a, double d, double b, double c,
                         int32_t tx, int32_t ty)
{
    int32_t fa = (int32_t)(a * 65536.0 + (a < 0 ? -0.5 : 0.5));
    int32_t fd = (int32_t)(d * 65536.0 + (d < 0 ? -0.5 : 0.5));
    int32_t fb = (int32_t)(b * 65536.0 + (b < 0 ? -0.5 : 0.5));
    int32_t fc = (int32_t)(c * 65536.0 + (c < 0 ? -0.5 : 0.5));
    int     n;

    bw_bits(w, 1, 1);                   /* HasScale */
    n = sbits(fa) > sbits(fd) ? sbits(fa) : sbits(fd);
    bw_bits(w, (uint32_t)n, 5);
    bw_bits(w, (uint32_t)fa, n);        /* ScaleX */
    bw_bits(w, (uint32_t)fd, n);        /* ScaleY */
    if(fb || fc) {
        bw_bits(w, 1, 1);               /* HasRotate */
        n = sbits(fb) > sbits(fc) ? sbits(fb) : sbits(fc);
        bw_bits(w, (uint32_t)n, 5);
        bw_bits(w, (uint32_t)fb, n);    /* RotateSkew0 */
        bw_bits(w, (uint32_t)fc, n);    /* RotateSkew1 */
    } else {
        bw_bits(w, 0, 1);
    }
    n = sbits(tx) > sbits(ty) ? sbits(tx) : sbits(ty);
    bw_bits(w, (uint32_t)n, 5);
    bw_bits(w, (uint32_t)tx, n);
    bw_bits(w, (uint32_t)ty, n);
    bw_flush(w);
}

static void bw_matrix(bw *w, double scale, int32_t tx, int32_t ty)
{
    bw_matrix_ex(w, scale, scale, 0.0, 0.0, tx, ty);
}

static void write_fills(bw *w, const fsty *f, int n, int rgba)
{
    int i, k, c;

    bw_u8(w, (unsigned)n);
    for(i = 0; i < n; i++) {
        bw_u8(w, (unsigned)f[i].type);
        if(f[i].type == 0x00) {
            for(c = 0; c < (rgba ? 4 : 3); c++)
                bw_u8(w, f[i].rgba[c]);
            continue;
        }
        if(f[i].type & 0x40) {
            /* The character ID comes before the matrix, and the matrix maps
             * bitmap space - twenty units to the texel - into shape space. */
            bw_u16(w, (unsigned)f[i].bitmap);
            bw_matrix_ex(w, f[i].mscale, f[i].mscale2,
                         f[i].mrot0, f[i].mrot1, f[i].mtx, f[i].mty);
            continue;
        }
        bw_matrix(w, f[i].mscale, f[i].mtx, f[i].mty);
        bw_bits(w, 0, 4);               /* spread and interpolation modes */
        bw_bits(w, (uint32_t)f[i].nstop, 4);
        for(k = 0; k < f[i].nstop; k++) {
            bw_u8(w, f[i].ratio[k]);
            for(c = 0; c < (rgba ? 4 : 3); c++)
                bw_u8(w, f[i].stop[k][c]);
        }
    }
}

static void write_lines(bw *w, const lsty *l, int n, int rgba)
{
    int i, c;

    bw_u8(w, (unsigned)n);
    for(i = 0; i < n; i++) {
        bw_u16(w, (unsigned)l[i].width);
        for(c = 0; c < (rgba ? 4 : 3); c++)
            bw_u8(w, l[i].rgba[c]);
    }
}

static void begin_shape_ex(bw *body, shp *s, int id,
                           int32_t x0, int32_t x1, int32_t y0, int32_t y1,
                           const fsty *f, int nfill,
                           const lsty *l, int nline, int rgba)
{
    memset(body, 0, sizeof *body);
    bw_u16(body, (unsigned)id);
    bw_rect(body, x0, x1, y0, y1);
    write_fills(body, f, nfill, rgba);
    write_lines(body, l, nline, rgba);

    s->w = body;
    s->nfillbits = 4;
    s->nlinebits = 4;
    s->x = s->y = 0;
    bw_bits(body, 4, 4);
    bw_bits(body, 4, 4);
}

/* StateNewStyles: a fresh style table, and with it a fresh drawing layer that
 * paints over everything before it. The real file leans on this heavily - its
 * logo shape has twenty-three of them - so it needs a case where the right
 * answer is obvious rather than only the case where it is buried in artwork. */
static void sc_new_styles(shp *s, const uint8_t *rgb, int nfill)
{
    int i;

    bw_bits(s->w, 0, 1);
    bw_bits(s->w, 0x10, 5);
    bw_u8(s->w, (unsigned)nfill);
    for(i = 0; i < nfill; i++) {
        bw_u8(s->w, 0x00);
        bw_u8(s->w, rgb[i * 3 + 0]);
        bw_u8(s->w, rgb[i * 3 + 1]);
        bw_u8(s->w, rgb[i * 3 + 2]);
    }
    bw_u8(s->w, 0);
    bw_bits(s->w, 4, 4);
    bw_bits(s->w, 0, 4);
    s->nfillbits = 4;
    s->nlinebits = 0;
}

static void box(shp *s, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                int fill0, int fill1)
{
    sc_move(s, x0, y0, fill0, fill1);
    sc_line_to(s, x1, y0);
    sc_line_to(s, x1, y1);
    sc_line_to(s, x0, y1);
    sc_line_to(s, x0, y0);
}

static void place(bw *out, int depth, int charid, int move,
                  double sx, double sy, int32_t tx, int32_t ty,
                  const int *cxmult, int clip_depth);

static void emit(const char *dir, const char *name, int sw, int sh_,
                 const bw *shape_body, int shape_tag,
                 int bgr, int bgg, int bgb)
{
    bw   tags, bg;
    char path[512];
    /* Every shape tag body begins with the character ID, so the file does not
     * have to be told twice which character it just defined. */
    int  id = shape_body->b[0] | (shape_body->b[1] << 8);

    memset(&tags, 0, sizeof tags);
    memset(&bg, 0, sizeof bg);
    bw_u8(&bg, (unsigned)bgr);
    bw_u8(&bg, (unsigned)bgg);
    bw_u8(&bg, (unsigned)bgb);
    tag(&tags, 9, &bg);
    tag(&tags, shape_tag, shape_body);

    /* Placed on the timeline as well as defined.
     *
     * The host tools reach a shape by its character index and never look at the
     * display list, so for their purposes this tag does nothing and every count
     * in the check is unchanged by it. What it changes is whether the file is a
     * movie: a player draws the root timeline, and a file that only defines a
     * character shows an empty stage, correctly and uselessly. Depth 1 at the
     * identity, because the shape's own coordinates are already where the file
     * says the picture is. */
    place(&tags, 1, id, 0, 1.0, 1.0, 0, 0, NULL, 0);

    {
        bw empty;
        memset(&empty, 0, sizeof empty);
        tag(&tags, 1, &empty);          /* ShowFrame */
        tag(&tags, 0, &empty);          /* End */
    }
    snprintf(path, sizeof path, "%s/%s", dir, name);
    write_swf(path, sw, sh_, &tags);
}

/* --- movies: display list, fonts and text -------------------------------- */

/* Same as emit(), but the caller supplies the whole tag stream between the
 * background colour and the End tag - so it can place things, show frames, and
 * define fonts, which a single-shape file cannot. */
static void emit_movie(const char *dir, const char *name, int sw, int sh_,
                       const bw *body, int bgr, int bgg, int bgb)
{
    bw   tags, bg, empty;
    char path[512];

    memset(&tags, 0, sizeof tags);
    memset(&bg, 0, sizeof bg);
    memset(&empty, 0, sizeof empty);
    bw_u8(&bg, (unsigned)bgr);
    bw_u8(&bg, (unsigned)bgg);
    bw_u8(&bg, (unsigned)bgb);
    tag(&tags, 9, &bg);
    memcpy(tags.b + tags.n, body->b, body->n);
    tags.n += body->n;
    tag(&tags, 0, &empty);              /* End */
    snprintf(path, sizeof path, "%s/%s", dir, name);
    write_swf(path, sw, sh_, &tags);
}

static void put_tag(bw *out, int code, const bw *body)
{
    tag(out, code, body);
}

static void show_frame(bw *out)
{
    bw empty;

    memset(&empty, 0, sizeof empty);
    tag(out, 1, &empty);
}

/* PlaceObject2. The flag byte is written LSB-first in meaning - bit 0 is Move
 * and bit 7 is HasClipActions - which is the reverse of the order the spec
 * lists the flags in, and the reverse of the order the optional fields then
 * appear. Writing it by hand here is deliberate: a generator that shared a
 * table with the reader could agree with it while both were wrong.
 *
 * ClipDepth is bit 6, and it is written after the colour transform and the
 * ratio, which is the order of the flag list and not the order of the bits.
 * Zero means no clip depth at all rather than a clip depth of zero, because
 * depth zero is a real depth and a mask that named it would clip nothing. */
/* Name is bit 5 and sits between the ratio and the clip depth, which is the
 * one ordering in this tag that a reader is likely to get right by accident
 * and then wrong under a flag combination it has never seen: a placement that
 * is named *and* a mask writes both, and a reader that reads the clip depth
 * first takes two bytes out of the middle of the string. */
static void place_named(bw *out, int depth, int charid, int move,
                        double sx, double sy, int32_t tx, int32_t ty,
                        const int *cxmult, int clip_depth, const char *name)
{
    bw b;
    unsigned flags = 0x04;              /* HasMatrix */

    memset(&b, 0, sizeof b);
    if(charid)     flags |= 0x02;
    if(move)       flags |= 0x01;
    if(cxmult)     flags |= 0x08;
    if(name)       flags |= 0x20;
    if(clip_depth) flags |= 0x40;
    bw_u8(&b, flags);
    bw_u16(&b, (unsigned)depth);
    if(charid)
        bw_u16(&b, (unsigned)charid);
    {
        int32_t fsx = (int32_t)(sx * 65536.0 + 0.5);
        int32_t fsy = (int32_t)(sy * 65536.0 + 0.5);
        int     n   = sbits(fsx) > sbits(fsy) ? sbits(fsx) : sbits(fsy);

        bw_bits(&b, 1, 1);
        bw_bits(&b, (uint32_t)n, 5);
        bw_bits(&b, (uint32_t)fsx, n);
        bw_bits(&b, (uint32_t)fsy, n);
        bw_bits(&b, 0, 1);              /* no rotate block */
        n = sbits(tx) > sbits(ty) ? sbits(tx) : sbits(ty);
        bw_bits(&b, (uint32_t)n, 5);
        bw_bits(&b, (uint32_t)tx, n);
        bw_bits(&b, (uint32_t)ty, n);
        bw_flush(&b);
    }
    if(cxmult) {
        /* CXFORMWITHALPHA, multiply terms only. Note PlaceObject2 always
         * carries the alpha terms even in a file with no alpha anywhere else. */
        int i, n = 2;

        for(i = 0; i < 4; i++)
            if(sbits(cxmult[i]) > n)
                n = sbits(cxmult[i]);
        bw_bits(&b, 0, 1);              /* HasAddTerms */
        bw_bits(&b, 1, 1);              /* HasMultTerms */
        bw_bits(&b, (uint32_t)n, 4);
        for(i = 0; i < 4; i++)
            bw_bits(&b, (uint32_t)cxmult[i], n);
        bw_flush(&b);
    }
    if(name) {
        const char *c;

        for(c = name; *c; c++)
            bw_u8(&b, (unsigned char)*c);
        bw_u8(&b, 0);
    }
    if(clip_depth)
        bw_u16(&b, (unsigned)clip_depth);
    put_tag(out, 26, &b);
}

static void place(bw *out, int depth, int charid, int move,
                  double sx, double sy, int32_t tx, int32_t ty,
                  const int *cxmult, int clip_depth)
{
    place_named(out, depth, charid, move, sx, sy, tx, ty, cxmult, clip_depth,
                NULL);
}

/* FrameLabel: a null-terminated string and nothing else at this target. SWF 6
 * puts a NamedAnchor byte behind it, which is why the reader gives this tag a
 * bit reader of its own rather than trusting the string to be the whole body. */
static void frame_label(bw *out, const char *name)
{
    bw          b;
    const char *c;

    memset(&b, 0, sizeof b);
    for(c = name; *c; c++)
        bw_u8(&b, (unsigned char)*c);
    bw_u8(&b, 0);
    put_tag(out, 43, &b);
}

/* DefineSprite: an id, a frame count, and a nested tag stream with its own End.
 * The frame count is declared and then not trusted - what a sprite actually has
 * is however many ShowFrames are in the stream - so the two are written
 * consistently here and a reader that believes the declared one is not caught
 * by this file. */
static void define_sprite(bw *out, int id, int frames, const bw *body)
{
    bw b, empty;

    memset(&b, 0, sizeof b);
    memset(&empty, 0, sizeof empty);
    bw_u16(&b, (unsigned)id);
    bw_u16(&b, (unsigned)frames);
    memcpy(b.b + b.n, body->b, body->n);
    b.n += body->n;
    tag(&b, 0, &empty);
    put_tag(out, 39, &b);
}

/* PlaceObject2 carrying a Ratio, which is the only thing that drives a morph:
 * one character at one depth, and a number saying how far through its own
 * shape it is. The field sits after the colour transform and before the name,
 * which is the order the flags are listed in and not the order of the bits -
 * and the clip depth sits behind it, for the same reason. Both on one
 * placement is how a morph becomes a mask that changes shape. */
static void place_ratio(bw *out, int depth, int charid, int move,
                        int32_t tx, int32_t ty, int ratio, int clip_depth)
{
    bw       b;
    unsigned flags = 0x04 | 0x10;       /* HasMatrix | HasRatio */

    memset(&b, 0, sizeof b);
    if(charid)     flags |= 0x02;
    if(move)       flags |= 0x01;
    if(clip_depth) flags |= 0x40;
    bw_u8(&b, flags);
    bw_u16(&b, (unsigned)depth);
    if(charid)
        bw_u16(&b, (unsigned)charid);
    {
        int n;

        bw_bits(&b, 0, 1);              /* no scale block: identity */
        bw_bits(&b, 0, 1);              /* no rotate block */
        n = sbits(tx) > sbits(ty) ? sbits(tx) : sbits(ty);
        bw_bits(&b, (uint32_t)n, 5);
        bw_bits(&b, (uint32_t)tx, n);
        bw_bits(&b, (uint32_t)ty, n);
        bw_flush(&b);
    }
    bw_u16(&b, (unsigned)ratio);
    if(clip_depth)
        bw_u16(&b, (unsigned)clip_depth);
    put_tag(out, 26, &b);
}

static void remove_depth(bw *out, int depth)
{
    bw b;

    memset(&b, 0, sizeof b);
    bw_u16(&b, (unsigned)depth);
    put_tag(out, 28, &b);               /* RemoveObject2 */
}

/* DefineButton: one BUTTONRECORD per state, terminated by a zero flag byte,
 * then the button's own action list - which is left empty here, and which is
 * why the tag length rather than a count is what ends the record loop.
 *
 * The four states get four different characters on purpose. A button's states
 * are the one place in the format where a character is defined and then
 * deliberately not drawn, so a renderer that picks the wrong bit, or that draws
 * every record it reads, produces a picture that is the right shape in the
 * wrong colour - which is invisible on real artwork, where the four states are
 * usually the same art recoloured.
 *
 * Only the up record carries the matrix. If the state selection were wrong the
 * area would move as well as the colour, so one assertion catches both. */
static void write_button(bw *out, int id, int up_id, int over_id, int down_id,
                         int hit_id, double scale, int32_t tx, int32_t ty)
{
    bw  b;
    int i;
    static const unsigned state[4] = { 0x01, 0x02, 0x04, 0x08 };
    int ids[4];

    ids[0] = up_id; ids[1] = over_id; ids[2] = down_id; ids[3] = hit_id;

    memset(&b, 0, sizeof b);
    bw_u16(&b, (unsigned)id);
    for(i = 0; i < 4; i++) {
        bw_u8(&b, state[i]);
        bw_u16(&b, (unsigned)ids[i]);
        bw_u16(&b, 1);                  /* depth, the same for all four */
        if(i == 0)
            bw_matrix(&b, scale, tx, ty);
        else
            bw_matrix(&b, 1.0, 0, 0);
    }
    bw_u8(&b, 0);                       /* end of records */
    bw_u8(&b, 0);                       /* an empty ACTIONRECORD list */
    put_tag(out, 7, &b);
}

/* A DefineFont holding one glyph: the full em square as a filled box.
 *
 * The glyph count is never stored. It is recovered by dividing the first entry
 * of the offset table by two, which works because the outlines start where the
 * table ends - so with one glyph the only offset is 2, and the outline begins
 * two bytes past the start of the table. Writing that by hand is the point of
 * this function: it is the one structure in the format whose size is implied
 * rather than stated.
 *
 * Glyph outlines are SHAPE, not SHAPEWITHSTYLE - there is no style array - and
 * the convention is that the first style change selects fill style 1 of a
 * table the reader has to invent. */
static void write_square_font(bw *out, int id)
{
    bw  b;
    shp s;

    memset(&b, 0, sizeof b);
    bw_u16(&b, (unsigned)id);
    bw_u16(&b, 2);                      /* offset[0]: one glyph, two bytes on */

    bw_bits(&b, 1, 4);                  /* NumFillBits */
    bw_bits(&b, 0, 4);                  /* NumLineBits */
    s.w = &b;
    s.nfillbits = 1;
    s.nlinebits = 0;
    s.x = s.y = 0;
    /* Wound so that fill style 0 - the negative side - is the inside, which is
     * what real fonts emit and what exercises the edge reversal. */
    bw_bits(&b, 0, 1);
    bw_bits(&b, 3, 5);                  /* StateMoveTo | StateFillStyle0 */
    bw_bits(&b, 5, 5);
    bw_bits(&b, 0, 5);
    bw_bits(&b, 0, 5);
    bw_bits(&b, 1, 1);                  /* FillStyle0 = 1 */
    sc_line_to(&s, 0, 1024);
    sc_line_to(&s, 1024, 1024);
    sc_line_to(&s, 1024, 0);
    sc_line_to(&s, 0, 0);
    sc_end(&s);
    put_tag(out, 10, &b);
}

/* DefineText placing `n` copies of glyph 0 at a fixed advance.
 *
 * TextHeight is read after the two offsets even though it is gated on the same
 * flag as the font ID, which comes before them. And the advance is in twips at
 * the final size already - it is not in em units and must not be scaled by the
 * height, unlike the font's own advance table. */
static void write_text(bw *out, int id, int fontid, int height,
                       const uint8_t *rgb, int n, int32_t advance)
{
    bw  b;
    int i;

    memset(&b, 0, sizeof b);
    bw_u16(&b, (unsigned)id);
    bw_rect(&b, 0, 32767, 0, 32767);
    bw_bits(&b, 0, 1);                  /* MATRIX: identity, no scale */
    bw_bits(&b, 0, 1);                  /* no rotate */
    bw_bits(&b, 0, 5);                  /* NTranslateBits = 0 */
    bw_flush(&b);
    bw_u8(&b, 4);                       /* GlyphBits - wider than needed */
    bw_u8(&b, 12);                      /* AdvanceBits */

    bw_u8(&b, 0x80 | 0x08 | 0x04 | 0x02 | 0x01);
    bw_u16(&b, (unsigned)fontid);
    bw_u8(&b, rgb[0]); bw_u8(&b, rgb[1]); bw_u8(&b, rgb[2]);
    bw_u16(&b, 0);                      /* XOffset */
    bw_u16(&b, 0);                      /* YOffset */
    bw_u16(&b, (unsigned)height);       /* TextHeight, after the offsets */
    bw_u8(&b, (unsigned)n);
    for(i = 0; i < n; i++) {
        bw_bits(&b, 0, 4);              /* glyph index 0 */
        bw_bits(&b, (uint32_t)advance, 12);
    }
    bw_flush(&b);
    bw_u8(&b, 0);                       /* end of records */
    put_tag(out, 11, &b);
}

/* --- bitmap tags --------------------------------------------------------- */

/* A zlib stream of stored deflate blocks.
 *
 * Stored blocks rather than a real compressor, because what is being tested is
 * the reader's bound on the *decompressed* size and its handling of the pixel
 * layout inside, neither of which cares how the bytes got there. A compressor
 * would add a second implementation of something with its own bugs between the
 * test and what it is testing. The header byte pair is the canonical 0x78 0x01
 * - the two together are a multiple of 31, which is the check a reader is
 * required to make and the first thing that rejects a stream built by hand. */
static void bw_zlib_stored(bw *w, const uint8_t *d, size_t n)
{
    uint32_t a = 1, b = 0;
    size_t   i, off = 0;

    bw_u8(w, 0x78);
    bw_u8(w, 0x01);
    do {
        size_t chunk = (n - off) > 65535u ? 65535u : (n - off);

        bw_u8(w, (off + chunk == n) ? 1u : 0u);   /* BFINAL, BTYPE = stored */
        bw_u16(w, (unsigned)chunk);
        bw_u16(w, (unsigned)(~chunk & 0xffffu)); /* LEN's own complement */
        memcpy(w->b + w->n, d + off, chunk);
        w->n += chunk;
        off  += chunk;
    } while(off < n);

    for(i = 0; i < n; i++) {
        a = (a + d[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    /* Adler-32, big endian, which is the one field in a zlib stream that is
     * not little endian. */
    bw_u8(w, (b >> 8) & 0xff);
    bw_u8(w, b & 0xff);
    bw_u8(w, (a >> 8) & 0xff);
    bw_u8(w, a & 0xff);
}

/* DefineBitsLossless (tag 20) or DefineBitsLossless2 (tag 36). `blob` is the
 * whole uncompressed body - colour table then padded rows - because the row
 * padding is what the reader's size bound is computed from, and a generator
 * that let the writer infer it could agree with a reader that inferred it the
 * same wrong way. */
static void write_lossless(bw *out, int tag_code, int id, int fmt, int w, int h,
                           int ncolors, const uint8_t *blob, size_t bloblen)
{
    bw b;

    memset(&b, 0, sizeof b);
    bw_u16(&b, (unsigned)id);
    bw_u8(&b, (unsigned)fmt);
    bw_u16(&b, (unsigned)w);
    bw_u16(&b, (unsigned)h);
    if(fmt == 3)
        bw_u8(&b, (unsigned)(ncolors - 1));   /* stored one less than it means */
    bw_zlib_stored(&b, blob, bloblen);
    put_tag(out, tag_code, &b);
}

/* --- a baseline JPEG, one flat DC coefficient per block ------------------ */

/* Why write an encoder at all: the splice between JPEGTables and DefineBits is
 * the part of the bitmap work most likely to be wrong and least likely to be
 * noticed, and there is no way to exercise it without a file that has been
 * split the way Flash split them. Nothing in the sample content has a bitmap
 * tag of any kind, so the only alternative to generating one is not testing it.
 *
 * The image is restricted to blocks of one flat colour, which is what makes
 * the expected output an exact number rather than a decoded approximation. A
 * constant 8x8 block has one non-zero DCT coefficient, F(0,0) = 8 times the
 * level-shifted sample, and with a quantiser of one that survives the round
 * trip exactly: the inverse transform of a DC-only block is that sample again,
 * with no rounding anywhere in it. Restricting further to greys makes the
 * colour conversion exact too, because Cb and Cr sit at their neutral 128 and
 * the red and blue terms drop out of the matrix entirely. So a grey block in
 * gives that grey out, and the pixel count is an area. */

static const uint8_t jpg_dc_bits[16] =
    { 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
static const uint8_t jpg_dc_vals[12] =
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };

static const uint8_t jpg_ac_bits[16] =
    { 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d };
static const uint8_t jpg_ac_vals[162] = {
    0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,
    0x61,0x07,0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,0x23,0x42,0xb1,0xc1,
    0x15,0x52,0xd1,0xf0,0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,0x17,0x18,
    0x19,0x1a,0x25,0x26,0x27,0x28,0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,
    0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4a,0x53,0x54,0x55,0x56,0x57,
    0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x73,0x74,0x75,
    0x76,0x77,0x78,0x79,0x7a,0x83,0x84,0x85,0x86,0x87,0x88,0x89,0x8a,0x92,
    0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,
    0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,
    0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,
    0xd9,0xda,0xe1,0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf1,0xf2,
    0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xfa
};

typedef struct {
    unsigned code[256];
    int      len[256];
} jhuff;

static void jh_build(jhuff *h, const uint8_t *bits, const uint8_t *vals)
{
    unsigned code = 0;
    int      s, i, k = 0;

    memset(h, 0, sizeof *h);
    for(s = 1; s <= 16; s++) {
        for(i = 0; i < bits[s - 1]; i++) {
            h->code[vals[k]] = code;
            h->len[vals[k]]  = s;
            k++;
            code++;
        }
        code <<= 1;
    }
}

/* Entropy-coded data has its own bit writer because it has its own escape:
 * an 0xff byte inside the scan is followed by a stuffed 0x00 so it cannot be
 * mistaken for a marker. Leaving that out produces a file that decodes right
 * until the first byte that happens to be 0xff. */
typedef struct {
    bw      *w;
    unsigned acc;
    int      nb;
} jbits;

static void jb_put(jbits *j, unsigned v, int n)
{
    while(n-- > 0) {
        j->acc = (j->acc << 1) | ((v >> n) & 1u);
        if(++j->nb == 8) {
            unsigned byte = j->acc & 0xffu;

            bw_u8(j->w, byte);
            if(byte == 0xff)
                bw_u8(j->w, 0x00);
            j->acc = 0;
            j->nb  = 0;
        }
    }
}

static void jb_flush(jbits *j)
{
    while(j->nb)
        jb_put(j, 1, 1);                /* the pad is ones, not zeroes */
}

static void jb_dc(jbits *j, const jhuff *h, int diff)
{
    int      s = 0, v = diff < 0 ? -diff : diff;
    unsigned m;

    while(v) {
        s++;
        v >>= 1;
    }
    jb_put(j, h->code[s], h->len[s]);
    if(s) {
        /* A negative value is stored as one less than itself, so the low bits
         * of the two's complement are exactly the encoding. */
        m = (unsigned)(diff < 0 ? diff - 1 : diff) & ((1u << s) - 1u);
        jb_put(j, m, s);
    }
}

static void jpg_marker(bw *w, unsigned m) { bw_u8(w, 0xff); bw_u8(w, m); }

/* Every multi-byte field in a JPEG is big endian, which is the opposite of
 * every multi-byte field in the SWF wrapped around it. */
static void bw_be16(bw *w, unsigned v)
{
    bw_u8(w, (v >> 8) & 0xff);
    bw_u8(w, v & 0xff);
}

static void jpg_dht(bw *w, unsigned tc_th, const uint8_t *bits,
                    const uint8_t *vals, int nvals)
{
    int i;

    jpg_marker(w, 0xc4);
    bw_be16(w, (unsigned)(2 + 1 + 16 + nvals));
    bw_u8(w, tc_th);
    for(i = 0; i < 16; i++)
        bw_u8(w, bits[i]);
    for(i = 0; i < nvals; i++)
        bw_u8(w, vals[i]);
}

/* The tables half: everything a DefineBits stream had stripped out of it,
 * wrapped as a complete JPEG stream of its own - which is exactly what makes
 * the splice awkward, because it ends with the end-of-image marker that has to
 * come back out. */
static void jpg_tables(bw *w)
{
    int i;

    jpg_marker(w, 0xd8);                /* SOI */
    jpg_marker(w, 0xdb);                /* DQT */
    bw_be16(w, 2 + 1 + 64);
    bw_u8(w, 0x00);                     /* 8-bit precision, table 0 */
    for(i = 0; i < 64; i++)
        bw_u8(w, 1);                    /* quantise by one: lossless for a DC */
    jpg_dht(w, 0x00, jpg_dc_bits, jpg_dc_vals, 12);
    jpg_dht(w, 0x10, jpg_ac_bits, jpg_ac_vals, 162);
    jpg_marker(w, 0xd9);                /* EOI */
}

/* The image half. `grey` is one flat value per 8x8 block, in raster order of
 * blocks; `bw_blk` and `bh_blk` are how many there are across and down. All
 * three components sample 1:1, so one MCU is one block of each and the MCU
 * order is the block order. */
static void jpg_image(bw *w, int bw_blk, int bh_blk, const uint8_t *grey,
                      int with_tables)
{
    jhuff dc, ac;
    jbits j;
    int   i, pred = 0;

    jh_build(&dc, jpg_dc_bits, jpg_dc_vals);
    jh_build(&ac, jpg_ac_bits, jpg_ac_vals);

    jpg_marker(w, 0xd8);                /* SOI */
    if(with_tables) {
        bw tb;
        memset(&tb, 0, sizeof tb);
        jpg_tables(&tb);
        /* Reuse the tables writer and drop its own SOI and EOI: a
         * self-contained stream carries the same tables in the middle. */
        memcpy(w->b + w->n, tb.b + 2, tb.n - 4);
        w->n += tb.n - 4;
    }

    jpg_marker(w, 0xc0);                /* SOF0, baseline */
    bw_be16(w, 2 + 1 + 2 + 2 + 1 + 3 * 3);
    bw_u8(w, 8);                        /* sample precision */
    bw_be16(w, (unsigned)(bh_blk * 8));  /* height comes before width */
    bw_be16(w, (unsigned)(bw_blk * 8));
    bw_u8(w, 3);                        /* three components */
    for(i = 1; i <= 3; i++) {
        bw_u8(w, (unsigned)i);
        bw_u8(w, 0x11);                 /* 1x1 sampling: no chroma subsampling */
        bw_u8(w, 0);                    /* quantisation table 0 */
    }

    jpg_marker(w, 0xda);                /* SOS */
    bw_be16(w, 2 + 1 + 2 * 3 + 3);
    bw_u8(w, 3);
    for(i = 1; i <= 3; i++) {
        bw_u8(w, (unsigned)i);
        bw_u8(w, 0x00);                 /* DC table 0, AC table 0 for all three */
    }
    bw_u8(w, 0); bw_u8(w, 63); bw_u8(w, 0);

    memset(&j, 0, sizeof j);
    j.w = w;
    for(i = 0; i < bw_blk * bh_blk; i++) {
        /* Level shift by 128, then the DCT of a constant block puts eight
         * times it in the DC and nothing anywhere else. */
        int val = 8 * ((int)grey[i] - 128);

        jb_dc(&j, &dc, val - pred);
        pred = val;
        jb_put(&j, ac.code[0], ac.len[0]);       /* end of block */

        /* Chroma is neutral, so both blocks are a zero DC difference and an
         * immediate end of block - which is also what proves the two chroma
         * predictors are tracked separately from luma's. */
        jb_put(&j, dc.code[0], dc.len[0]);
        jb_put(&j, ac.code[0], ac.len[0]);
        jb_put(&j, dc.code[0], dc.len[0]);
        jb_put(&j, ac.code[0], ac.len[0]);
    }
    jb_flush(&j);
    jpg_marker(w, 0xd9);                /* EOI */
}

/* --- rectangles, fonts with metrics, edit text, and morphs --------------- */

#define PX(n) ((int32_t)((n) * 20))

/* A solid rectangle with its top left corner on the origin, so a placement
 * translation is the rectangle's position in pixels and nothing has to be
 * subtracted to work out where it lands. Every masking answer below is the area
 * of an intersection of two of these. */
static void rect_shape(bw *body, int id, int32_t w, int32_t h,
                       const uint8_t *rgb)
{
    shp s;

    begin_shape(body, &s, id, 0, w, 0, h, rgb, 1, -1);
    box(&s, 0, 0, w, h, 0, 1);
    sc_end(&s);
}

/* The same, with a rectangular hole cut by winding the inner contour the other
 * way - the t_hole construction, because a mask is an ordinary shape and its
 * holes are ordinary holes. A mask whose fill rule was ignored would come out
 * solid, and the only visible difference is the part of the content that should
 * have shown through. */
static void holed_shape(bw *body, int id, int32_t w, int32_t h,
                        int32_t hx0, int32_t hy0, int32_t hx1, int32_t hy1,
                        const uint8_t *rgb)
{
    shp s;

    begin_shape(body, &s, id, 0, w, 0, h, rgb, 1, -1);
    box(&s, 0, 0, w, h, 0, 1);
    sc_move(&s, hx0, hy0, 0, 1);
    sc_line_to(&s, hx0, hy1);
    sc_line_to(&s, hx1, hy1);
    sc_line_to(&s, hx1, hy0);
    sc_line_to(&s, hx0, hy0);
    sc_end(&s);
}

/* DefineFont2 carrying everything DefineEditText needs and DefineText does
 * not: a code table, an advance table and a set of vertical metrics.
 *
 * Two glyphs, and both are chosen so that a layout is an area. Glyph 0 is the
 * whole em square sitting on the baseline - so at a text height of H it is an
 * H by H block of ink, and n of them are n*H*H pixels wherever they land.
 * Glyph 1 is the space: no outline at all, and an advance equal to the em, so
 * a gap is exactly as wide as a letter and the width of a line is the number
 * of characters in it. That is what makes a wrap point derivable rather than
 * measured.
 *
 * The ascent is the whole em and the descent is zero, so a baseline sits one
 * text height below the top of the line and the ink of a line fills the space
 * between two baselines exactly. A real font leaves a gap; this one does not,
 * on purpose, because a gap is a number the test would have to carry.
 *
 * Offsets are all relative to the first byte of the offset table, and the code
 * table's offset is the entry immediately behind the glyph offsets - which is
 * why the whole thing has to be assembled from the pieces backwards. */
static void write_font2_square(bw *out, int id)
{
    bw  b, g0, g1;
    shp s;

    /* Glyph 0: the em square, above the baseline. Wound so that fill style 0 -
     * the negative side - is the inside, which is what a real font emits. */
    memset(&g0, 0, sizeof g0);
    bw_bits(&g0, 1, 4);                 /* NumFillBits */
    bw_bits(&g0, 0, 4);                 /* NumLineBits */
    s.w = &g0;
    s.nfillbits = 1;
    s.nlinebits = 0;
    s.x = s.y = 0;
    bw_bits(&g0, 0, 1);
    bw_bits(&g0, 3, 5);                 /* StateMoveTo | StateFillStyle0 */
    bw_bits(&g0, 5, 5);
    bw_bits(&g0, 0, 5);
    bw_bits(&g0, 0, 5);
    bw_bits(&g0, 1, 1);                 /* FillStyle0 = 1 */
    sc_line_to(&s, 0, -1024);
    sc_line_to(&s, 1024, -1024);
    sc_line_to(&s, 1024, 0);
    sc_line_to(&s, 0, 0);
    sc_end(&s);

    /* Glyph 1: the space. A shape record with no edges at all, which is
     * exactly what a font stores for one and exactly why an advance table
     * matters - there is no outline to derive a width from. */
    memset(&g1, 0, sizeof g1);
    bw_bits(&g1, 1, 4);
    bw_bits(&g1, 0, 4);
    bw_bits(&g1, 0, 1);
    bw_bits(&g1, 0, 5);                 /* EndShapeRecord */
    bw_flush(&g1);

    memset(&b, 0, sizeof b);
    bw_u16(&b, (unsigned)id);
    bw_u8(&b, 0x80);                    /* HasLayout, narrow offsets and codes */
    bw_u8(&b, 0);                       /* LanguageCode */
    bw_u8(&b, 0);                       /* FontNameLen */
    bw_u16(&b, 2);                      /* NumGlyphs */

    /* Three UI16 of table: two glyph offsets and the code table's. */
    bw_u16(&b, 6);
    bw_u16(&b, (unsigned)(6 + g0.n));
    bw_u16(&b, (unsigned)(6 + g0.n + g1.n));
    memcpy(b.b + b.n, g0.b, g0.n); b.n += g0.n;
    memcpy(b.b + b.n, g1.b, g1.n); b.n += g1.n;

    bw_u8(&b, 'M');
    bw_u8(&b, ' ');

    bw_u16(&b, 1024);                   /* FontAscent, em units */
    bw_u16(&b, 0);                      /* FontDescent */
    bw_u16(&b, 0);                      /* FontLeading */
    bw_u16(&b, 1024);                   /* advance of 'M' */
    bw_u16(&b, 1024);                   /* advance of the space */
    bw_rect(&b, 0, 1024, -1024, 0);     /* FontBoundsTable, unread but stated */
    bw_rect(&b, 0, 0, 0, 0);
    bw_u16(&b, 0);                      /* KerningCount */
    put_tag(out, 48, &b);
}

/* DefineEditText. The sixteen flag bits are written one at a time in the order
 * the field list gives them, by hand and not from a shared table, for the same
 * reason PlaceObject2's byte is: a generator sharing a table with the reader
 * could agree with it while both were wrong. */
static void write_edit(bw *out, int id,
                       int32_t x0, int32_t x1, int32_t y0, int32_t y1,
                       int fontid, int height, const uint8_t *rgb,
                       int align, int multiline, int wordwrap, const char *text)
{
    bw b;

    memset(&b, 0, sizeof b);
    bw_u16(&b, (unsigned)id);
    bw_rect(&b, x0, x1, y0, y1);

    bw_bits(&b, 1, 1);                            /* HasText */
    bw_bits(&b, (uint32_t)(wordwrap != 0), 1);
    bw_bits(&b, (uint32_t)(multiline != 0), 1);
    bw_bits(&b, 0, 1);                            /* Password */
    bw_bits(&b, 1, 1);                            /* ReadOnly */
    bw_bits(&b, 1, 1);                            /* HasTextColor */
    bw_bits(&b, 0, 1);                            /* HasMaxLength */
    bw_bits(&b, 1, 1);                            /* HasFont */
    bw_bits(&b, 0, 1);                            /* HasFontClass, SWF 6 */
    bw_bits(&b, 0, 1);                            /* AutoSize */
    bw_bits(&b, 1, 1);                            /* HasLayout */
    bw_bits(&b, 0, 1);                            /* NoSelect */
    bw_bits(&b, 0, 1);                            /* Border */
    bw_bits(&b, 0, 1);                            /* WasStatic, SWF 6 */
    bw_bits(&b, 0, 1);                            /* HTML */
    bw_bits(&b, 0, 1);                            /* UseOutlines */
    bw_flush(&b);

    bw_u16(&b, (unsigned)fontid);
    bw_u16(&b, (unsigned)height);
    bw_u8(&b, rgb[0]); bw_u8(&b, rgb[1]); bw_u8(&b, rgb[2]); bw_u8(&b, 255);
    bw_u8(&b, (unsigned)align);
    bw_u16(&b, 0);                                /* LeftMargin */
    bw_u16(&b, 0);                                /* RightMargin */
    bw_u16(&b, 0);                                /* Indent */
    bw_u16(&b, 0);                                /* Leading */
    bw_u8(&b, 0);                                 /* VariableName: empty */
    {
        const char *p;
        for(p = text; *p; p++)
            bw_u8(&b, (unsigned char)*p);
        bw_u8(&b, 0);
    }
    put_tag(out, 37, &b);
}

/* A style change that carries a MoveTo and nothing else, which is all the end
 * half of a morph is permitted to hold. */
static void sc_move_only(shp *s, int32_t x, int32_t y)
{
    int n = sbits(x) > sbits(y) ? sbits(x) : sbits(y);

    bw_bits(s->w, 0, 1);
    bw_bits(s->w, 1, 5);                /* StateMoveTo alone */
    bw_bits(s->w, (uint32_t)n, 5);
    bw_bits(s->w, (uint32_t)x, n);
    bw_bits(s->w, (uint32_t)y, n);
    s->x = x;
    s->y = y;
}

/* DefineMorphShape: one rectangle becoming another, in one solid colour that
 * is also blending.
 *
 * `drop_last` writes an end shape one edge short of the start shape, which is
 * the refusal case: the two halves stop describing the same sequence of
 * records, so there is nothing to pair the last edge with.
 *
 * The Offset field is why this is assembled from three pieces. It counts from
 * the byte after itself to the first byte of the end shape, so it cannot be
 * written until the styles and the start shape have been - and a generator
 * that measured it from the start of the tag instead would produce a file that
 * reads plausible edge records out of the middle of the style array. */
static void write_morph_rect(bw *out, int id,
                             int32_t ax0, int32_t ay0, int32_t ax1, int32_t ay1,
                             int32_t bx0, int32_t by0, int32_t bx1, int32_t by1,
                             const uint8_t *rgba0, const uint8_t *rgba1,
                             int drop_last)
{
    bw  b, mid, tail;
    shp s;
    int i;

    memset(&b, 0, sizeof b);
    bw_u16(&b, (unsigned)id);
    bw_rect(&b, ax0, ax1, ay0, ay1);
    bw_rect(&b, bx0, bx1, by0, by1);

    memset(&mid, 0, sizeof mid);
    bw_u8(&mid, 1);                     /* one fill style */
    bw_u8(&mid, 0x00);                  /* solid, and always RGBA in a morph */
    for(i = 0; i < 4; i++) bw_u8(&mid, rgba0[i]);
    for(i = 0; i < 4; i++) bw_u8(&mid, rgba1[i]);
    bw_u8(&mid, 0);                     /* no line styles */

    s.w = &mid;
    s.nfillbits = 1;
    s.nlinebits = 0;
    s.x = s.y = 0;
    bw_bits(&mid, 1, 4);
    bw_bits(&mid, 0, 4);
    sc_move(&s, ax0, ay0, 0, 1);
    sc_line_to(&s, ax1, ay0);
    sc_line_to(&s, ax1, ay1);
    sc_line_to(&s, ax0, ay1);
    sc_line_to(&s, ax0, ay0);
    sc_end(&s);

    memset(&tail, 0, sizeof tail);
    s.w = &tail;
    s.nfillbits = 1;
    s.nlinebits = 0;
    s.x = s.y = 0;
    bw_bits(&tail, 1, 4);
    bw_bits(&tail, 0, 4);
    sc_move_only(&s, bx0, by0);
    sc_line_to(&s, bx1, by0);
    sc_line_to(&s, bx1, by1);
    sc_line_to(&s, bx0, by1);
    if(!drop_last)
        sc_line_to(&s, bx0, by0);
    sc_end(&s);

    bw_u32(&b, (uint32_t)mid.n);
    memcpy(b.b + b.n, mid.b, mid.n);   b.n += mid.n;
    memcpy(b.b + b.n, tail.b, tail.n); b.n += tail.n;
    put_tag(out, 46, &b);
}

/* A morph whose geometry never moves and whose gradient stop ratios do.
 *
 * Four stops making one hard edge, the same construction t_grad_l uses, but
 * the pair of stops carrying the edge is at ratio r0 in the start ramp and r1
 * in the end - and the colours at both ends of every stop are identical, so
 * the only thing a rendered difference can come from is the ratios. That is
 * the point: a reader that blended the colours and took the ratios from one
 * end would draw the right two colours with the boundary in the wrong place,
 * which is invisible in any total and obvious in a column count.
 *
 * MORPHGRADIENT is also where a reader that shared its gradient code with
 * DefineShape comes apart: the stop count here is a whole byte, where GRADIENT
 * spends four bits on modes and four on the count. */
static void write_morph_grad(bw *out, int id,
                             int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                             int r0, int r1,
                             const uint8_t *ca, const uint8_t *cb)
{
    bw     b, mid, tail;
    shp    s;
    int    i, k;
    double sc = (double)(x1 - x0) / 32768.0;

    memset(&b, 0, sizeof b);
    bw_u16(&b, (unsigned)id);
    bw_rect(&b, x0, x1, y0, y1);
    bw_rect(&b, x0, x1, y0, y1);

    memset(&mid, 0, sizeof mid);
    bw_u8(&mid, 1);
    bw_u8(&mid, 0x10);                  /* linear */
    bw_matrix(&mid, sc, (x0 + x1) / 2, (y0 + y1) / 2);
    bw_matrix(&mid, sc, (x0 + x1) / 2, (y0 + y1) / 2);
    bw_u8(&mid, 4);                     /* NumGradients: a whole byte */
    for(k = 0; k < 4; k++) {
        const uint8_t *c = k < 2 ? ca : cb;
        int start = (k == 0) ? 0 : (k == 3) ? 255 : r0;
        int end   = (k == 0) ? 0 : (k == 3) ? 255 : r1;

        bw_u8(&mid, (unsigned)start);
        for(i = 0; i < 4; i++) bw_u8(&mid, c[i]);
        bw_u8(&mid, (unsigned)end);
        for(i = 0; i < 4; i++) bw_u8(&mid, c[i]);
    }
    bw_u8(&mid, 0);                     /* no line styles */

    s.w = &mid;
    s.nfillbits = 1;
    s.nlinebits = 0;
    s.x = s.y = 0;
    bw_bits(&mid, 1, 4);
    bw_bits(&mid, 0, 4);
    sc_move(&s, x0, y0, 0, 1);
    sc_line_to(&s, x1, y0);
    sc_line_to(&s, x1, y1);
    sc_line_to(&s, x0, y1);
    sc_line_to(&s, x0, y0);
    sc_end(&s);

    memset(&tail, 0, sizeof tail);
    s.w = &tail;
    s.nfillbits = 1;
    s.nlinebits = 0;
    s.x = s.y = 0;
    bw_bits(&tail, 1, 4);
    bw_bits(&tail, 0, 4);
    sc_move_only(&s, x0, y0);
    sc_line_to(&s, x1, y0);
    sc_line_to(&s, x1, y1);
    sc_line_to(&s, x0, y1);
    sc_line_to(&s, x0, y0);
    sc_end(&s);

    bw_u32(&b, (uint32_t)mid.n);
    memcpy(b.b + b.n, mid.b, mid.n);   b.n += mid.n;
    memcpy(b.b + b.n, tail.b, tail.n); b.n += tail.n;
    put_tag(out, 46, &b);
}

/* A morph with no fill at all: one segment whose stroke width grows. The
 * MORPHLINESTYLE carries both widths in one record, which is the only place a
 * width is stated twice anywhere in the format. */
static void write_morph_stroke(bw *out, int id,
                               int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                               int w0, int w1, const uint8_t *rgba)
{
    bw  b, mid, tail;
    shp s;
    int i;

    memset(&b, 0, sizeof b);
    bw_u16(&b, (unsigned)id);
    bw_rect(&b, x0 - w0 / 2, x1 + w0 / 2, y0 - w0 / 2, y1 + w0 / 2);
    bw_rect(&b, x0 - w1 / 2, x1 + w1 / 2, y0 - w1 / 2, y1 + w1 / 2);

    memset(&mid, 0, sizeof mid);
    bw_u8(&mid, 0);                     /* no fill styles */
    bw_u8(&mid, 1);
    bw_u16(&mid, (unsigned)w0);
    bw_u16(&mid, (unsigned)w1);
    for(i = 0; i < 4; i++) bw_u8(&mid, rgba[i]);
    for(i = 0; i < 4; i++) bw_u8(&mid, rgba[i]);

    s.w = &mid;
    s.nfillbits = 0;
    s.nlinebits = 1;
    s.x = s.y = 0;
    bw_bits(&mid, 0, 4);
    bw_bits(&mid, 1, 4);
    sc_move_ex(&s, x0, y0, 0, 0, 1);
    sc_line_to(&s, x1, y1);
    sc_end(&s);

    memset(&tail, 0, sizeof tail);
    s.w = &tail;
    s.nfillbits = 0;
    s.nlinebits = 1;
    s.x = s.y = 0;
    bw_bits(&tail, 0, 4);
    bw_bits(&tail, 1, 4);
    sc_move_only(&s, x0, y0);
    sc_line_to(&s, x1, y1);
    sc_end(&s);

    bw_u32(&b, (uint32_t)mid.n);
    memcpy(b.b + b.n, mid.b, mid.n);   b.n += mid.n;
    memcpy(b.b + b.n, tail.b, tail.n); b.n += tail.n;
    put_tag(out, 46, &b);
}

/* A DefineEditText whose flag word claims four optional fields and whose body
 * ends at the flag word. Every one of them reads zeros off the end of the tag,
 * so nothing is diagnosable field by field - which is the point: the check has
 * to be that the record as a whole was refused. */
static void write_edit_truncated(bw *out, int id)
{
    bw b;

    memset(&b, 0, sizeof b);
    bw_u16(&b, (unsigned)id);
    bw_rect(&b, 0, PX(40), 0, PX(20));
    bw_bits(&b, 1, 1);                            /* HasText */
    bw_bits(&b, 0, 1);
    bw_bits(&b, 0, 1);
    bw_bits(&b, 0, 1);
    bw_bits(&b, 0, 1);
    bw_bits(&b, 1, 1);                            /* HasTextColor */
    bw_bits(&b, 0, 1);
    bw_bits(&b, 1, 1);                            /* HasFont */
    bw_bits(&b, 0, 1);
    bw_bits(&b, 0, 1);
    bw_bits(&b, 1, 1);                            /* HasLayout */
    bw_bits(&b, 0, 1);
    bw_bits(&b, 0, 1);
    bw_bits(&b, 0, 1);
    bw_bits(&b, 0, 1);
    bw_bits(&b, 0, 1);
    bw_flush(&b);
    put_tag(out, 37, &b);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";

    /* 1. One rectangle, 100x60 pixels at (20,20), solid red. Every number in
     *    the output can be checked against these four. */
    {
        static const uint8_t red[3] = { 0xe0, 0x20, 0x20 };
        bw  body;
        shp s;

        begin_shape(&body, &s, 1, PX(20), PX(120), PX(20), PX(80), red, 1, -1);
        sc_move(&s, PX(20), PX(20), 0, 1);
        sc_line_to(&s, PX(120), PX(20));
        sc_line_to(&s, PX(120), PX(80));
        sc_line_to(&s, PX(20),  PX(80));
        sc_line_to(&s, PX(20),  PX(20));
        sc_end(&s);
        emit(dir, "t_rect.swf", 160, 100, &body, 2, 255, 255, 255);
    }

    /* 2. A square with a square hole. Both contours use the same fill style;
     *    the only thing that makes the inner one a hole is that it runs the
     *    other way round. A renderer using an even-odd rule also passes this
     *    one - that is deliberate, it is the cheap test - but a renderer that
     *    ignores direction entirely fails it. */
    {
        static const uint8_t blue[3] = { 0x20, 0x50, 0xd0 };
        bw  body;
        shp s;

        begin_shape(&body, &s, 2, PX(10), PX(130), PX(10), PX(130), blue, 1, -1);
        sc_move(&s, PX(10), PX(10), 0, 1);
        sc_line_to(&s, PX(130), PX(10));
        sc_line_to(&s, PX(130), PX(130));
        sc_line_to(&s, PX(10),  PX(130));
        sc_line_to(&s, PX(10),  PX(10));
        sc_move(&s, PX(40), PX(40), 0, 1);
        sc_line_to(&s, PX(40),  PX(100));
        sc_line_to(&s, PX(100), PX(100));
        sc_line_to(&s, PX(100), PX(40));
        sc_line_to(&s, PX(40),  PX(40));
        sc_end(&s);
        emit(dir, "t_hole.swf", 150, 150, &body, 2, 255, 255, 255);
    }

    /* 3. Two touching squares sharing one edge, which carries a different
     *    fill on each side. This is the case a naive reader gets wrong: it
     *    has to reverse the fill-style-0 side before the second square's
     *    boundary closes at all. Get it wrong and the green square leaks. */
    {
        static const uint8_t two[6] = { 0xe0, 0x20, 0x20, 0x20, 0xc0, 0x40 };
        bw  body;
        shp s;

        begin_shape(&body, &s, 3, PX(10), PX(210), PX(10), PX(110), two, 2, -1);
        sc_move(&s, PX(10), PX(10), 0, 1);
        sc_line_to(&s, PX(110), PX(10));
        sc_style(&s, 2, -1);                    /* fill 2 on the far side */
        sc_line_to(&s, PX(110), PX(110));
        sc_style(&s, 0, -1);
        sc_line_to(&s, PX(10), PX(110));
        sc_line_to(&s, PX(10), PX(10));
        sc_move(&s, PX(110), PX(10), 0, 2);
        sc_line_to(&s, PX(210), PX(10));
        sc_line_to(&s, PX(210), PX(110));
        sc_line_to(&s, PX(110), PX(110));
        sc_end(&s);
        emit(dir, "t_share.swf", 240, 130, &body, 2, 255, 255, 255);
    }

    /* 4. A circle from eight quadratic arcs. The control point of a circular
     *    arc sits on the tangent intersection, at radius r/cos(half-angle) -
     *    so the flattened outline should touch the true circle at every arc
     *    end and bulge by well under a pixel between them. Anything visibly
     *    polygonal means the tolerance is not being honoured. */
    {
        static const uint8_t purple[3] = { 0x80, 0x30, 0xa0 };
        bw     body;
        shp    s;
        int    i;
        double r = 60.0, cxp = 70.0, cyp = 70.0;
        double half = PS_PI / 8.0;
        double rc = r / cos(half);

        begin_shape(&body, &s, 4, PX(10), PX(130), PX(10), PX(130), purple, 1, -1);
        sc_move(&s, PX(cxp + r), PX(cyp), 0, 1);
        for(i = 0; i < 8; i++) {
            double a0 = (double)i * 2.0 * half;
            double am = a0 + half;
            double a1 = a0 + 2.0 * half;

            sc_curve_to(&s,
                        (int32_t)(PX(cxp) + rc * cos(am) * 20.0),
                        (int32_t)(PX(cyp) + rc * sin(am) * 20.0),
                        (int32_t)(PX(cxp) + r * cos(a1) * 20.0),
                        (int32_t)(PX(cyp) + r * sin(a1) * 20.0));
        }
        sc_end(&s);
        emit(dir, "t_circle.swf", 140, 140, &body, 2, 255, 255, 255);
    }

    /* 5. Three layers, each one square offset from the last. Drawn in file
     *    order the result is a staircase with the last square fully on top;
     *    drawn in any other order the overlaps come out wrong, which is
     *    visible immediately and impossible to misread. This is also the only
     *    generated file that uses DefineShape2, and so the only one that
     *    exercises the extended style counts and the NewStyles record. */
    {
        static const uint8_t l1[3] = { 0xd0, 0x30, 0x30 };
        static const uint8_t l2[3] = { 0x30, 0xb0, 0x40 };
        static const uint8_t l3[3] = { 0x30, 0x40, 0xd0 };
        bw  body;
        shp s;

        begin_shape(&body, &s, 5, PX(10), PX(150), PX(10), PX(150), l1, 1, -1);
        box(&s, PX(10), PX(10), PX(90), PX(90), 0, 1);
        sc_new_styles(&s, l2, 1);
        box(&s, PX(40), PX(40), PX(120), PX(120), 0, 1);
        sc_new_styles(&s, l3, 1);
        box(&s, PX(70), PX(70), PX(150), PX(150), 0, 1);
        sc_end(&s);
        emit(dir, "t_layers.swf", 160, 160, &body, 22, 255, 255, 255);
    }

    /* 6. DefineShape3: two half-transparent squares that overlap. The only
     *    file here with RGBA fill styles, so the only one that would catch a
     *    reader using the RGB layout for a v3 shape - a mistake that shifts
     *    every subsequent field by a byte and shows up as noise. */
    {
        static const uint8_t two[6] = { 0xff, 0x00, 0x00, 0x00, 0x00, 0xff };
        bw  body;
        shp s;

        begin_shape(&body, &s, 6, PX(10), PX(150), PX(10), PX(150), two, 2, 128);
        box(&s, PX(10), PX(10), PX(100), PX(100), 0, 1);
        box(&s, PX(60), PX(60), PX(150), PX(150), 0, 2);
        sc_end(&s);
        emit(dir, "t_alpha.swf", 160, 160, &body, 32, 255, 255, 255);
    }

    /* 7. A linear gradient, and behind it the style that proves it was
     *    stepped correctly.
     *
     *    The ramp is four stops making one hard edge rather than a wash: two
     *    at ratio 0 and 128 carrying colour A, two at 128 and 255 carrying B.
     *    A wash cannot be counted, because every column is a different colour
     *    and where each one lands depends on rounding. A hard edge can: the
     *    quantiser rounds a sample to the nearest of the 256 ratios, so the
     *    switch to B happens at ratio 127.5, which is the exact centre of the
     *    ramp, and the matrix below maps the ramp exactly onto the rectangle.
     *    Each half is therefore exactly half the rectangle.
     *
     *    The gradient square is 32768 twips wide whatever it is mapped onto,
     *    so the scale that fits it to a 4000-twip rectangle is 4000/32768 and
     *    the translation is the rectangle's centre. */
    {
        fsty f[2];
        bw   body;
        shp  s;

        memset(f, 0, sizeof f);
        f[0].type   = 0x10;                       /* linear */
        f[0].mscale = (double)(PX(220) - PX(20)) / 32768.0;
        f[0].mtx    = (PX(20) + PX(220)) / 2;
        f[0].mty    = (PX(20) + PX(120)) / 2;
        f[0].nstop  = 4;
        f[0].ratio[0] = 0;   f[0].stop[0][0] = 0x00;
        f[0].ratio[1] = 128; f[0].stop[1][0] = 0x00;
        f[0].ratio[2] = 128; f[0].stop[2][0] = 0xc0;
        f[0].ratio[3] = 255; f[0].stop[3][0] = 0xc0;
        f[0].stop[0][1] = f[0].stop[1][1] = 0x80;
        f[0].stop[2][1] = f[0].stop[3][1] = 0x20;
        f[0].stop[0][2] = f[0].stop[1][2] = 0x40;
        f[0].stop[2][2] = f[0].stop[3][2] = 0x80;

        f[1].type = 0x00;
        f[1].rgba[0] = 0xff; f[1].rgba[1] = 0xff; f[1].rgba[2] = 0x00;

        begin_shape_ex(&body, &s, 7, PX(10), PX(250), PX(10), PX(170),
                       f, 2, NULL, 0, 0);
        box(&s, PX(20), PX(20), PX(220), PX(120), 0, 1);
        box(&s, PX(20), PX(130), PX(120), PX(160), 0, 2);
        sc_end(&s);
        emit(dir, "t_grad_l.swf", 260, 180, &body, 22, 255, 255, 255);
    }

    /* 8. A radial gradient with alpha, in a DefineShape3.
     *
     *    Same hard-edged construction, so the inner colour occupies a disc
     *    whose radius is exactly half the gradient square's - the ramp runs
     *    from the centre to the edge, and the switch is at its midpoint. The
     *    square is mapped to the rectangle's width, so that radius is a
     *    quarter of the rectangle's width, and the disc's area is pi r^2.
     *
     *    This is the only file where the answer is not an integer anyone can
     *    write down: a disc's pixel count depends on which side of the
     *    boundary each centre falls, the same reason the circle file needs a
     *    tolerance. There is no antialiasing across a gradient stop, though -
     *    the shader is evaluated once per pixel centre - so the error is the
     *    boundary ring's own thickness and nothing more.
     *
     *    The outer half is half-transparent, which is what makes this file
     *    check that a DefineShape3 gradient's stops are RGBA. Read them as RGB
     *    and every stop after the first is shifted by a byte. */
    {
        fsty f[2];
        bw   body;
        shp  s;

        memset(f, 0, sizeof f);
        f[0].type   = 0x12;                       /* radial */
        f[0].mscale = (double)(PX(220) - PX(20)) / 32768.0;
        f[0].mtx    = (PX(20) + PX(220)) / 2;
        f[0].mty    = (PX(20) + PX(220)) / 2;
        f[0].nstop  = 4;
        f[0].ratio[0] = 0;   f[0].ratio[1] = 128;
        f[0].ratio[2] = 128; f[0].ratio[3] = 255;
        {
            static const uint8_t inner[4] = { 0xff, 0xc0, 0x00, 0xff };
            static const uint8_t outer[4] = { 0x00, 0x40, 0xff, 0x80 };
            memcpy(f[0].stop[0], inner, 4);
            memcpy(f[0].stop[1], inner, 4);
            memcpy(f[0].stop[2], outer, 4);
            memcpy(f[0].stop[3], outer, 4);
        }

        f[1].type = 0x00;
        f[1].rgba[0] = 0x20; f[1].rgba[1] = 0xe0;
        f[1].rgba[2] = 0xe0; f[1].rgba[3] = 0xff;

        begin_shape_ex(&body, &s, 8, PX(10), PX(270), PX(10), PX(290),
                       f, 2, NULL, 0, 1);
        box(&s, PX(20), PX(20), PX(220), PX(220), 0, 1);
        box(&s, PX(20), PX(240), PX(120), PX(270), 0, 2);
        sc_end(&s);
        emit(dir, "t_grad_r.swf", 280, 300, &body, 32, 255, 255, 255);
    }

    /* 9. Strokes: one open segment and one closed square, no fills at all.
     *
     *    Split into two line styles because they test different things and
     *    their areas have to be counted separately. The segment has two free
     *    ends and so is entirely about caps; the square has four corners and
     *    no free ends and so is entirely about joins. DefineShape 1/2/3 line
     *    styles carry no cap or join field, and Flash's behaviour with none is
     *    round for both.
     *
     *    Both areas are derivable. A segment of length L stroked at width W is
     *    L*W plus the two half-discs at its ends, which together are one disc.
     *    A closed square of side L is 4LW at the straight parts, minus the W/2
     *    corner squares that the round joins replace with quarter discs -
     *    4 * (W/2)^2 * (1 - pi/4), which is W^2(1 - pi/4).
     *
     *    Both need a tolerance, for two reasons that pull the same way and so
     *    can share one number. A round cap is drawn as a polygon, which is
     *    inscribed and so slightly smaller than the circle the arithmetic
     *    assumes; and the curved parts are antialiased, so the pixels along
     *    them are a blend and are not counted as the stroke colour at all.
     *    Both losses scale with the arc, and there is exactly one circle's
     *    worth of arc in each figure. A hundred and fifty is wide enough for
     *    that and less than half the three hundred and forty-three that
     *    separates round joins from square ones, which is the distinction the
     *    file exists to make. */
    {
        lsty l[2];
        bw   body;
        shp  s;

        memset(l, 0, sizeof l);
        l[0].width = PX(20);
        l[0].rgba[0] = 0xe0; l[0].rgba[1] = 0x30;
        l[0].rgba[2] = 0x30; l[0].rgba[3] = 0xff;
        l[1].width = PX(20);
        l[1].rgba[0] = 0x30; l[1].rgba[1] = 0x80;
        l[1].rgba[2] = 0xe0; l[1].rgba[3] = 0xff;

        begin_shape_ex(&body, &s, 9, PX(0), PX(300), PX(0), PX(210),
                       NULL, 0, l, 2, 1);
        sc_move_ex(&s, PX(30), PX(30), 0, 0, 1);
        sc_line_to(&s, PX(130), PX(30));
        sc_move_ex(&s, PX(30), PX(90), 0, 0, 2);
        sc_line_to(&s, PX(130), PX(90));
        sc_line_to(&s, PX(130), PX(190));
        sc_line_to(&s, PX(30),  PX(190));
        sc_line_to(&s, PX(30),  PX(90));
        sc_end(&s);
        emit(dir, "t_stroke.swf", 300, 210, &body, 32, 255, 255, 255);
    }

    /* 10. A bowtie: one contour that crosses itself.
     *
     *     Every other file here has contours that meet only at their ends,
     *     which is what a Flash exporter normally emits and therefore what
     *     nothing in the sample file tests. A scanline renderer does not care
     *     - it re-sorts its crossings on every sample row - but the
     *     tessellator does, because a trapezoid whose bounding edges swap over
     *     partway up is bounded by the wrong edge for half its height. It has
     *     to find the crossing and cut the band there.
     *
     *     This file is the sharpest possible case for that. All four corners
     *     sit on just two scanlines, so the whole figure is a single band and
     *     the crossing is in the middle of it: without the cut there is no
     *     band boundary anywhere near it, all four edges meet at the midpoint
     *     sample, and the output is nonsense rather than slightly wrong.
     *
     *     The answer is exact. Nonzero winding fills the left and right
     *     triangles - not the top and bottom, which is worth knowing - each
     *     with a vertical base the full height of the figure and an apex at
     *     the centre, so together they are half the bounding box. The
     *     tolerance is for the four 45-degree edges: each crosses one pixel
     *     per row over a hundred rows, so several hundred pixels along them
     *     hold a coverage fraction and are not the fill colour. Seven hundred
     *     covers that and is still a small fraction of the ten thousand that
     *     losing either triangle would cost. */
    {
        static const uint8_t green[3] = { 0x40, 0xc0, 0x60 };
        bw  body;
        shp s;

        begin_shape(&body, &s, 10, PX(10), PX(130), PX(10), PX(130),
                    green, 1, -1);
        sc_move(&s, PX(20), PX(20), 0, 1);
        sc_line_to(&s, PX(120), PX(120));
        sc_line_to(&s, PX(120), PX(20));
        sc_line_to(&s, PX(20),  PX(120));
        sc_line_to(&s, PX(20),  PX(20));
        sc_end(&s);
        emit(dir, "t_bowtie.swf", 140, 140, &body, 2, 255, 255, 255);
    }

    /* 11. A movie rather than a shape: four frames of display list.
     *
     *     Everything before this file draws a shape against its own bounds,
     *     which is the one thing a real SWF never does - a shape definition
     *     has no position, and where it lands is entirely the display list's
     *     doing. So this checks the four things the display list can say, each
     *     with an answer that is still just an area:
     *
     *       frame 0  place at a translation           40x20 px = 800
     *       frame 1  move the same depth, scaled 2x   80x40 px = 3200
     *       frame 2  move again with a colour transform, half brightness,
     *                so the same 3200 pixels are a different, computable colour
     *       frame 3  remove it                        0
     *
     *     Frame 1 is a Move, not a place: it carries a matrix and no character
     *     ID, so the character has to survive from the frame before. Frame 3
     *     proves removal actually removes rather than merely stopping the
     *     updates. The colour under a half multiply is exact because the
     *     transform truncates a multiply by 128 and then divides by 256, which
     *     for e0/20/20 is 70/10/10 with no rounding anywhere. */
    {
        static const uint8_t red[3] = { 0xe0, 0x20, 0x20 };
        static const int     half[4] = { 128, 128, 128, 256 };
        bw  shape, tags;
        shp s;

        begin_shape(&shape, &s, 1, 0, PX(40), 0, PX(20), red, 1, -1);
        box(&s, 0, 0, PX(40), PX(20), 0, 1);
        sc_end(&s);

        memset(&tags, 0, sizeof tags);
        put_tag(&tags, 2, &shape);
        place(&tags, 1, 1, 0, 1.0, 1.0, PX(10), PX(10), NULL, 0);
        show_frame(&tags);
        place(&tags, 1, 0, 1, 2.0, 2.0, PX(60), PX(10), NULL, 0);
        show_frame(&tags);
        place(&tags, 1, 0, 1, 2.0, 2.0, PX(60), PX(10), half, 0);
        show_frame(&tags);
        remove_depth(&tags, 1);
        show_frame(&tags);
        emit_movie(dir, "t_place.swf", 200, 100, &tags, 255, 255, 255);
    }

    /* 12. Text: a font of one glyph, and the em square arithmetic.
     *
     *     The glyph is the whole 1024-unit em square as a filled box, which
     *     makes the placement arithmetic visible as an area. A glyph coordinate
     *     becomes twips by multiplying by TextHeight and dividing by 1024, so
     *     at a height of 400 twips the square is 400 twips - twenty pixels - on
     *     a side, and two of them are 800 pixels of ink whatever else is wrong.
     *
     *     The advance is 600 twips, which is the other half of the test: it is
     *     in twips at the final size, not in em units, so the second square
     *     starts thirty pixels along and the two do not touch. Scale the
     *     advance by the height as well - the mistake the font's own advance
     *     table invites - and they overlap into a single 800-pixel blob of the
     *     wrong shape, which the gap between them is there to catch.
     *
     *     The colour goes through a multiply rather than arriving directly,
     *     because the glyph outline is white and the text colour is a transform
     *     on it. Asserting an exact colour is asserting that multiply is
     *     lossless. */
    {
        static const uint8_t green[3] = { 0x40, 0xc0, 0x60 };
        bw tags;

        memset(&tags, 0, sizeof tags);
        write_square_font(&tags, 1);
        write_text(&tags, 2, 1, 400, green, 2, 600);
        place(&tags, 1, 2, 0, 1.0, 1.0, PX(10), PX(10), NULL, 0);
        show_frame(&tags);
        emit_movie(dir, "t_text.swf", 200, 100, &tags, 255, 255, 255);
    }

    /* 13. Bitmaps: the three lossless colour depths, one shape each.
     *
     *     All three carry the same 4x4 arrangement of four colours:
     *
     *         0 0 0 0        so colour 0 covers nine texels, colour 1 one,
     *         0 0 0 1        colour 2 two and colour 3 four. Sixteen in all.
     *         0 0 2 2
     *         3 3 3 3
     *
     *     The counts being all different is the point. A reader that read the
     *     rows in the wrong order, or read columns for rows, or dropped the
     *     row padding, still produces four blocks of colour in the right
     *     proportions if the pattern is symmetric - so the pattern is not.
     *     Only one arrangement of these pixels gives 9, 1, 2 and 4.
     *
     *     The fill matrix scales by twenty, and a bitmap texel is twenty units
     *     on a side, so a texel lands on exactly twenty pixels of shape space
     *     and the 4x4 image covers the shape's whole 80x80 box. At the scale
     *     of two that `make check` renders with, a texel is 40x40 output
     *     pixels - 1600 of them - and the four counts are 14400, 1600, 3200
     *     and 6400, which sum to the 25600 pixels of the box.
     *
     *     The 15-bit case is the one worth stating separately. PIX15 is a
     *     bitfield and so is big endian, one reserved bit then five each of
     *     red, green and blue; its four colours are chosen at the ends of the
     *     five-bit range, where every plausible expansion to eight bits agrees
     *     and the assertion is about the bit order rather than about rounding. */
    {
        static const uint8_t pat[16] = {
            0, 0, 0, 0,
            0, 0, 0, 1,
            0, 0, 2, 2,
            3, 3, 3, 3
        };
        static const uint8_t pal8[4][3] = {
            { 0xe0, 0x20, 0x20 }, { 0x20, 0xc0, 0x40 },
            { 0x30, 0x40, 0xd0 }, { 0xf0, 0xe0, 0x10 }
        };
        /* Red, green, blue and white at the extremes of five bits. */
        static const unsigned pix15[4] = { 31u << 10, 31u << 5, 31u, 0x7fffu };
        static const uint8_t pal24[4][3] = {
            { 0x11, 0x88, 0x44 }, { 0xcc, 0x22, 0x66 },
            { 0x00, 0xaa, 0xff }, { 0xff, 0xee, 0x11 }
        };
        uint8_t blob[128];
        bw      tags;
        int     i, y, x;
        size_t  n;

        memset(&tags, 0, sizeof tags);

        /* Format 3: the colour table, then one index per pixel with each row
         * padded out to a multiple of four bytes. Four wide needs no padding,
         * which is why the 15-bit case below is the one that proves the
         * padding is applied at all. */
        n = 0;
        for(i = 0; i < 4; i++) {
            blob[n++] = pal8[i][0];
            blob[n++] = pal8[i][1];
            blob[n++] = pal8[i][2];
        }
        for(i = 0; i < 16; i++)
            blob[n++] = pat[i];
        write_lossless(&tags, 20, 1, 3, 4, 4, 4, blob, n);

        /* Format 4: two bytes a pixel, so a row of four is eight bytes and is
         * already aligned - but the reader has to believe that rather than
         * assume it, and a reader that pads to eight instead of four computes
         * a decompressed size the stream will never match. */
        n = 0;
        for(y = 0; y < 4; y++)
            for(x = 0; x < 4; x++) {
                unsigned v = pix15[pat[y * 4 + x]];
                blob[n++] = (uint8_t)(v >> 8);
                blob[n++] = (uint8_t)(v & 0xff);
            }
        write_lossless(&tags, 20, 2, 4, 4, 4, 0, blob, n);

        /* Format 5 in a DefineBitsLossless is PIX24: four bytes a pixel whose
         * first is reserved and is not red. */
        n = 0;
        for(i = 0; i < 16; i++) {
            blob[n++] = 0;
            blob[n++] = pal24[pat[i]][0];
            blob[n++] = pal24[pat[i]][1];
            blob[n++] = pal24[pat[i]][2];
        }
        write_lossless(&tags, 20, 3, 5, 4, 4, 0, blob, n);

        for(i = 1; i <= 3; i++) {
            fsty f;
            bw   body;
            shp  s;

            memset(&f, 0, sizeof f);
            f.type    = 0x43;                  /* clipped, not smoothed */
            f.bitmap  = i;
            f.mscale  = f.mscale2 = 20.0;
            begin_shape_ex(&body, &s, 10 + i, 0, PX(80), 0, PX(80),
                           &f, 1, NULL, 0, 1);
            box(&s, 0, 0, PX(80), PX(80), 0, 1);
            sc_end(&s);
            put_tag(&tags, 32, &body);
        }
        show_frame(&tags);
        emit_movie(dir, "t_bmp.swf", 80, 80, &tags, 255, 255, 255);
    }

    /* 14. DefineBitsLossless2: the alpha variant, in both of the forms it has.
     *
     *     Same 4x4 pattern and so the same four areas. What is being asserted
     *     is the compositing arithmetic, which is exact for these values: a
     *     shape is drawn on swfrender's diagnostic canvas, which is a flat
     *     202020 and not the file's own background colour, so a covered pixel
     *     of colour c at alpha a lands at (32*(255-a) + c*a)/255 - and the four
     *     colours are chosen so that division has no remainder to argue about.
     *
     *     The wire format is premultiplied - a colour is stored already scaled
     *     by its own alpha - so a reader that skips the divide draws the half
     *     transparent colours at half brightness, which looks like a
     *     compositing bug and is a decoding one. The colours here are at the
     *     ends of the range, where the multiply and its inverse are both exact,
     *     so the assertion is about whether the divide happens rather than
     *     about how it rounds.
     *
     *     Colour 2 is fully transparent, and there the assertion is on the
     *     canvas: its two texels are 3200 output pixels which must come out
     *     unpainted, plus the 644 of margin around the 160x160 box in a 162x162
     *     canvas, for 3844. An alpha of zero that painted anything at all would
     *     take those pixels away from that count. */
    {
        static const uint8_t pat[16] = {
            0, 0, 0, 0,
            0, 0, 0, 1,
            0, 0, 2, 2,
            3, 3, 3, 3
        };
        /* Straight colour then alpha; the premultiply happens below. */
        static const uint8_t argb[4][4] = {
            { 0x40, 0xc0, 0x60, 0xff },
            { 0x00, 0x00, 0xff, 0x80 },
            { 0x00, 0x00, 0x00, 0x00 },
            { 0x00, 0x00, 0x00, 0x80 }
        };
        static const uint8_t pal[4][4] = {
            { 0xe0, 0x20, 0x20, 0xff },
            { 0xff, 0xff, 0xff, 0x80 },
            { 0x00, 0x00, 0x00, 0x00 },
            { 0x00, 0x00, 0x00, 0xff }
        };
        uint8_t blob[128];
        bw      tags;
        int     i, c;
        size_t  n;

        memset(&tags, 0, sizeof tags);

        /* Format 5 here is PIX32, and its bytes are alpha first. */
        n = 0;
        for(i = 0; i < 16; i++) {
            const uint8_t *p = argb[pat[i]];

            blob[n++] = p[3];
            for(c = 0; c < 3; c++)
                blob[n++] = (uint8_t)((p[c] * p[3] + 127) / 255);
        }
        write_lossless(&tags, 36, 1, 5, 4, 4, 0, blob, n);

        /* Format 3 in a DefineBitsLossless2 is the same paletted image with a
         * four-byte colour table, premultiplied in the same way. */
        n = 0;
        for(i = 0; i < 4; i++) {
            for(c = 0; c < 3; c++)
                blob[n++] = (uint8_t)((pal[i][c] * pal[i][3] + 127) / 255);
            blob[n++] = pal[i][3];
        }
        for(i = 0; i < 16; i++)
            blob[n++] = pat[i];
        write_lossless(&tags, 36, 2, 3, 4, 4, 4, blob, n);

        for(i = 1; i <= 2; i++) {
            fsty f;
            bw   body;
            shp  s;

            memset(&f, 0, sizeof f);
            f.type   = 0x43;
            f.bitmap = i;
            f.mscale = f.mscale2 = 20.0;
            begin_shape_ex(&body, &s, 20 + i, 0, PX(80), 0, PX(80),
                           &f, 1, NULL, 0, 1);
            box(&s, 0, 0, PX(80), PX(80), 0, 1);
            sc_end(&s);
            put_tag(&tags, 32, &body);
        }
        show_frame(&tags);
        emit_movie(dir, "t_bmp_a.swf", 80, 80, &tags, 0x20, 0x20, 0x20);
    }

    /* 15. The four bitmap fill styles: tiled against clipped, smoothed against
     *     not, and a matrix that is neither axis aligned nor uniform.
     *
     *     One 2x2 image of four colours, A B over C D, at twenty units a texel
     *     and a matrix scaling by twenty - so the image covers forty pixels of
     *     shape space and the 80x80 box is four times its area. Everything
     *     below is that box, and every count is in output pixels at the scale
     *     of two, which is four per pixel of shape space.
     *
     *     Shape 0, tiled: the image repeats twice each way, so each colour gets
     *     four texels of 20x20 shape pixels, 1600 each, 6400 in output.
     *
     *     Shape 1, clipped: clipped means clamp, not discard - the last row and
     *     column stretch across everything past the image, which is what the
     *     PVR's clamp mode does and what Flash shows. A is the only colour that
     *     stays 20x20; B stretches to 60 wide, C to 60 tall, and D takes the
     *     60x60 corner. That is 400, 1200, 1200 and 3600 shape pixels, so
     *     1600, 4800, 4800 and 14400 in output, and they still sum to 25600.
     *     A reader that discarded instead of clamping loses 6000 of them.
     *
     *     Shape 2, clipped through a quarter turn with unequal scales: the b
     *     and c terms carry 20 and -40 while both scale terms are zero, so a
     *     reader that skips the rotate block is left with a singular matrix and
     *     draws grey. The turn puts the image's u axis along the shape's y, and
     *     the -40 halves the image's reach along x before the clamp takes over:
     *     the first texel column becomes the top twenty pixels and the first
     *     row becomes the rightmost forty. A and C each take 40x20 shape
     *     pixels, B and D 40x60, so 3200, 9600, 3200 and 9600 in output.
     *
     *     Shape 3, tiled and smoothed, over an image whose texels are all one
     *     colour: bilinear between four identical taps is that colour exactly,
     *     so the whole 25600 comes out unchanged. It is the case that says the
     *     filtered path runs and does not tint what it filters; what the filter
     *     does between two different texels is asserted in imgtest, where the
     *     sampler can be called at a stated point instead of counted. */
    {
        static const uint8_t quad[4][3] = {
            { 0xe0, 0x20, 0x20 }, { 0x20, 0xc0, 0x40 },
            { 0x30, 0x40, 0xd0 }, { 0xf0, 0xe0, 0x10 }
        };
        static const uint8_t flat[3] = { 0x80, 0x30, 0xa0 };
        static const int     type[4] = { 0x42, 0x43, 0x43, 0x40 };
        uint8_t blob[64];
        bw      tags;
        int     i;
        size_t  n;

        memset(&tags, 0, sizeof tags);

        n = 0;
        for(i = 0; i < 4; i++) {
            blob[n++] = 0;
            blob[n++] = quad[i][0];
            blob[n++] = quad[i][1];
            blob[n++] = quad[i][2];
        }
        write_lossless(&tags, 20, 1, 5, 2, 2, 0, blob, n);

        n = 0;
        for(i = 0; i < 4; i++) {
            blob[n++] = 0;
            blob[n++] = flat[0];
            blob[n++] = flat[1];
            blob[n++] = flat[2];
        }
        write_lossless(&tags, 20, 2, 5, 2, 2, 0, blob, n);

        for(i = 0; i < 4; i++) {
            fsty f;
            bw   body;
            shp  s;

            memset(&f, 0, sizeof f);
            f.type   = type[i];
            f.bitmap = (i == 3) ? 2 : 1;
            if(i == 2) {
                f.mscale = f.mscale2 = 0.0;
                f.mrot0  = 20.0;               /* b */
                f.mrot1  = -40.0;              /* c */
                f.mtx    = PX(80);
            } else {
                f.mscale = f.mscale2 = 20.0;
            }
            begin_shape_ex(&body, &s, 30 + i, 0, PX(80), 0, PX(80),
                           &f, 1, NULL, 0, 1);
            box(&s, 0, 0, PX(80), PX(80), 0, 1);
            sc_end(&s);
            put_tag(&tags, 32, &body);
        }
        show_frame(&tags);
        emit_movie(dir, "t_bmp_fill.swf", 80, 80, &tags, 255, 255, 255);
    }

    /* 16. The three JPEG tags, and the splice that only one of them needs.
     *
     *     The image is 16x16 in four 8x8 blocks of flat grey - 30, 60, a0 and
     *     e0, none of them the 202020 of the canvas they are drawn on and none
     *     of them the 808080 a bitmap fill falls back to when its character
     *     will not decode - which is one block per MCU and one coefficient per
     *     block. That makes the decoded picture exact rather than approximate,
     *     for the reasons argued at the encoder above, so a block is an area
     *     and an area is a pixel count. At five shape pixels a texel the image
     *     covers the 80x80 box and each block is 40x40 shape pixels, 6400 in
     *     output at the scale of two.
     *
     *     Shape 0 is a DefineBits, whose stream has had its quantisation and
     *     Huffman tables taken out and put in the JPEGTables tag ahead of it.
     *     Rejoining them leaves an end-of-image marker followed by a
     *     start-of-image in the middle of the stream, because both halves are
     *     complete JPEG files in their own right. Flash wrote that pair and
     *     Flash's reader skipped it; a reader that does not gets nothing at
     *     all, so all four counts go to zero and the shape draws grey.
     *
     *     Shape 1 is a DefineBitsJPEG2, self-contained - and carrying the same
     *     marker pair at the very front, which is what Flash's writer emitted
     *     even where there was nothing to splice. It is a different code path
     *     from the one shape 0 exercises and is just as load bearing.
     *
     *     Shape 2 is a DefineBitsJPEG3, the only JPEG form with alpha: a
     *     second zlib stream of one byte per pixel, sitting behind the image
     *     and located by a byte offset rather than by a length. Its alpha is
     *     opaque over the left half and half over the right, so the two left
     *     blocks composite to themselves and the two right ones through the
     *     same arithmetic as the alpha bitmap file: grey g at alpha 128 over
     *     the canvas lands at (32*127 + g*128)/255, which is 40 for 60 and 80
     *     for e0. */
    {
        static const uint8_t grey[4] = { 0x30, 0x60, 0xa0, 0xe0 };
        bw      tags, tbl, img;
        uint8_t alpha[256];
        int     i, x, y;

        memset(&tags, 0, sizeof tags);

        memset(&tbl, 0, sizeof tbl);
        jpg_tables(&tbl);
        put_tag(&tags, 8, &tbl);            /* JPEGTables, before it is needed */

        {   /* DefineBits: the image with no tables of its own. */
            bw b;

            memset(&b, 0, sizeof b);
            bw_u16(&b, 20);
            memset(&img, 0, sizeof img);
            jpg_image(&img, 2, 2, grey, 0);
            memcpy(b.b + b.n, img.b, img.n);
            b.n += img.n;
            put_tag(&tags, 6, &b);
        }

        {   /* DefineBitsJPEG2, with the marker pair Flash left at the front. */
            bw b;

            memset(&b, 0, sizeof b);
            bw_u16(&b, 21);
            bw_u8(&b, 0xff); bw_u8(&b, 0xd9);
            bw_u8(&b, 0xff); bw_u8(&b, 0xd8);
            memset(&img, 0, sizeof img);
            jpg_image(&img, 2, 2, grey, 1);
            memcpy(b.b + b.n, img.b, img.n);
            b.n += img.n;
            put_tag(&tags, 21, &b);
        }

        {   /* DefineBitsJPEG3: image, then a zlib alpha plane behind it. */
            bw b;

            memset(&img, 0, sizeof img);
            jpg_image(&img, 2, 2, grey, 1);
            for(y = 0; y < 16; y++)
                for(x = 0; x < 16; x++)
                    alpha[y * 16 + x] = (x < 8) ? 0xff : 0x80;

            memset(&b, 0, sizeof b);
            bw_u16(&b, 22);
            bw_u32(&b, (uint32_t)img.n);    /* where the alpha stream starts */
            memcpy(b.b + b.n, img.b, img.n);
            b.n += img.n;
            bw_zlib_stored(&b, alpha, sizeof alpha);
            put_tag(&tags, 35, &b);
        }

        for(i = 0; i < 3; i++) {
            fsty f;
            bw   body;
            shp  s;

            memset(&f, 0, sizeof f);
            f.type   = 0x43;
            f.bitmap = 20 + i;
            f.mscale = f.mscale2 = 5.0;     /* five shape pixels a texel */
            begin_shape_ex(&body, &s, 40 + i, 0, PX(80), 0, PX(80),
                           &f, 1, NULL, 0, 1);
            box(&s, 0, 0, PX(80), PX(80), 0, 1);
            sc_end(&s);
            put_tag(&tags, 32, &body);
        }
        show_frame(&tags);
        emit_movie(dir, "t_jpeg.swf", 80, 80, &tags, 255, 255, 255);
    }

    /* 17. Clip depth: five things a mask can do to one rectangle.
     *
     *     A mask is a placement, not a character, so all five frames use the
     *     same two shapes and only the placements change. The content is a
     *     100x60 rectangle at (10,10), covering x 10..110 and y 10..70; the
     *     mask is a 60x40 rectangle, and every answer below is the area of the
     *     intersection of two axis-aligned rectangles, which is a product of
     *     two overlaps and has no rounding in it anywhere.
     *
     *       frame 0  mask at (60,30)   50 x 40 overlap        = 2000
     *       frame 1  mask scaled 3x at the origin, 180x120, which contains the
     *                content entirely                          = 6000
     *       frame 2  mask at (150,75), no overlap in either axis =   0
     *       frame 3  the mask removed, so the content is whole   = 6000
     *       frame 4  mask back at (60,30) over depth 2 only, and a second
     *                character at depth 3 which is above the clip range and so
     *                untouched: 2000 of red and all 1600 of green
     *       frame 5  the mask moved to depth 5 naming depth 6, where nothing
     *                is, so nothing is clipped: 6000 of red less the 1600 the
     *                green square is painted over the top of = 4400, and 1600
     *                of green
     *
     *     That subtraction in frame 5 is the reason the green square sits on
     *     top of the red rather than beside it: it is 1600 pixels of ordinary
     *     painter's order, and the fact that the same 1600 changes nothing in
     *     frame 4 says the green really is above the clip range and the red
     *     underneath it really is being cut away rather than covered up.
     *
     *     Frame 1 is a Move carrying only a matrix, so the clip depth has to
     *     survive from the frame before - a placement's clip depth is state on
     *     the slot like its colour transform, not something restated per frame.
     *
     *     Frames 4 and 5 are the two ends of "the range matters". In frame 4
     *     the green square is one depth above the range and must be untouched,
     *     which is what catches a renderer that masks everything above the mask
     *     rather than everything up to the depth named. In frame 5 the range
     *     contains nothing at all, and the thing to check is that the mask is
     *     still not drawn: the range being empty makes it a mask that clips
     *     nothing, not a character.
     *
     *     Which is why the mask's own colour appears nowhere else in the file
     *     and is asserted to cover zero pixels. A mask that gets painted is the
     *     most likely way to get this wrong and the least likely to be noticed,
     *     since it lands exactly where the content it was hiding used to be. */
    {
        static const uint8_t red[3]   = { 0xe0, 0x20, 0x20 };
        static const uint8_t green[3] = { 0x20, 0xc0, 0x40 };
        static const uint8_t maskc[3] = { 0x00, 0xc0, 0xff };
        bw content, mask, second, tags;

        rect_shape(&content, 1, PX(100), PX(60), red);
        rect_shape(&mask,    2, PX(60),  PX(40), maskc);
        rect_shape(&second,  3, PX(40),  PX(40), green);

        memset(&tags, 0, sizeof tags);
        put_tag(&tags, 2, &content);
        put_tag(&tags, 2, &mask);
        put_tag(&tags, 2, &second);

        place(&tags, 2, 1, 0, 1.0, 1.0, PX(10), PX(10), NULL, 0);
        place(&tags, 1, 2, 0, 1.0, 1.0, PX(60), PX(30), NULL, 2);
        show_frame(&tags);

        place(&tags, 1, 0, 1, 3.0, 3.0, 0, 0, NULL, 0);
        show_frame(&tags);

        place(&tags, 1, 0, 1, 1.0, 1.0, PX(150), PX(75), NULL, 0);
        show_frame(&tags);

        remove_depth(&tags, 1);
        show_frame(&tags);

        place(&tags, 1, 2, 0, 1.0, 1.0, PX(60), PX(30), NULL, 2);
        place(&tags, 3, 3, 0, 1.0, 1.0, PX(10), PX(10), NULL, 0);
        show_frame(&tags);

        remove_depth(&tags, 1);
        place(&tags, 5, 2, 0, 1.0, 1.0, PX(150), PX(75), NULL, 6);
        show_frame(&tags);

        emit_movie(dir, "t_clip.swf", 260, 120, &tags, 255, 255, 255);
    }

    /* 18. Two masks at once, and a mask with a hole.
     *
     *     Frame 0 is a mask inside a sprite that is itself masked, which is the
     *     case with more than one reasonable answer. Flash's is that the two
     *     intersect - a pixel is painted where every mask in force covers it -
     *     and the numbers here are chosen so that no other reading produces
     *     them. The sprite holds the 100x60 content at (10,10) and its own mask
     *     at (60,30), which alone would leave 50x40 = 2000. The root then masks
     *     the sprite with the same shape at (80,0), covering x 80..140 and
     *     y 0..40, and 30 x 10 = 300 of the 2000 survive. Either mask on its
     *     own leaves far more, and taking the union instead of the intersection
     *     leaves more still, so 300 can only come from intersecting.
     *
     *     Frame 1 is the mask with a hole: an 80x60 rectangle with a 40x20
     *     rectangle wound out of it, placed at (20,20). Against the same
     *     content that is 80x50 = 4000 minus the whole of the 40x20 hole, which
     *     falls inside that overlap, so 3200. The hole exists because a mask is
     *     an ordinary shape and its fill rule is the ordinary one; a renderer
     *     that filled the mask's outline rather than its region would answer
     *     4000 and look entirely plausible doing it. */
    {
        static const uint8_t red[3]   = { 0xe0, 0x20, 0x20 };
        static const uint8_t maskc[3] = { 0x00, 0xc0, 0xff };
        bw content, mask, holed, inner, tags;

        rect_shape(&content, 1, PX(100), PX(60), red);
        rect_shape(&mask,    2, PX(60),  PX(40), maskc);
        holed_shape(&holed,  3, PX(80),  PX(60),
                    PX(20), PX(20), PX(60), PX(40), maskc);

        memset(&inner, 0, sizeof inner);
        place(&inner, 1, 2, 0, 1.0, 1.0, PX(60), PX(30), NULL, 2);
        place(&inner, 2, 1, 0, 1.0, 1.0, PX(10), PX(10), NULL, 0);
        show_frame(&inner);

        memset(&tags, 0, sizeof tags);
        put_tag(&tags, 2, &content);
        put_tag(&tags, 2, &mask);
        put_tag(&tags, 2, &holed);
        define_sprite(&tags, 4, 1, &inner);

        place(&tags, 2, 4, 0, 1.0, 1.0, 0, 0, NULL, 0);
        place(&tags, 1, 2, 0, 1.0, 1.0, PX(80), 0, NULL, 2);
        show_frame(&tags);

        remove_depth(&tags, 1);
        remove_depth(&tags, 2);
        place(&tags, 4, 1, 0, 1.0, 1.0, PX(10), PX(10), NULL, 0);
        place(&tags, 3, 3, 0, 1.0, 1.0, PX(20), PX(20), NULL, 4);
        show_frame(&tags);

        emit_movie(dir, "t_clipn.swf", 260, 120, &tags, 255, 255, 255);
    }

    /* 19. A morph: one rectangle becoming another, sampled at both ends and in
     *     the middle.
     *
     *     40x20 pixels at (10,10) becomes 80x60 at the same corner, so only
     *     two numbers move and both move by 800 twips. The halfway frame is
     *     therefore 60x40 pixels, exactly: 32768/65535 overshoots one half by
     *     one part in 65535, which for a delta of 800 is a hundredth of a
     *     twip, and the truncation takes it back to 400. Every one of the
     *     three frames is an integer area:
     *
     *       ratio 0      40 x 20 =  800
     *       ratio 32768  60 x 40 = 2400
     *       ratio 65535  80 x 60 = 4800
     *
     *     The colour blends too, and it is the other half of the test. The
     *     fill runs from e02020 to 20e020, so each channel moves by 192 and
     *     the halfway colour is 808020 with no rounding anywhere - which is
     *     what turns "the geometry is right" into "the fill style array was
     *     stepped right as well", since a morph fill style carries both
     *     colours in one record and mis-stepping it would leave the shape
     *     correct and the colour read out of somewhere else.
     *
     *     The three frames are one place and two moves, so the ratio is the
     *     only thing that changes: a move with no character id means the
     *     morph has to survive the frame boundary the same way t_place's
     *     rectangle does.
     *
     *     Frame 3 is a morph being used as a mask, which neither the morph nor
     *     the clipping side of this player produces on its own - it exists
     *     only where the two meet, and so has no test on either side of that
     *     seam. The same morph at ratio 65535 is 80x60 at (10,10) and now
     *     names a clip depth; the blue rectangle under it is 100x40 at the
     *     origin, and the intersection is 80 x 30 = 2400. It is the one
     *     placement in the file that has to do two things at once - blend to
     *     its ratio and then not be drawn - and the assertion that the morph's
     *     own colour covers nothing is what says the second half happened. */
    {
        static const uint8_t from[4] = { 0xe0, 0x20, 0x20, 0xff };
        static const uint8_t to[4]   = { 0x20, 0xe0, 0x20, 0xff };
        static const uint8_t blue[3] = { 0x30, 0x60, 0xf0 };
        bw tags, under;

        memset(&tags, 0, sizeof tags);
        write_morph_rect(&tags, 1, PX(10), PX(10), PX(50), PX(30),
                         PX(10), PX(10), PX(90), PX(70), from, to, 0);
        rect_shape(&under, 2, PX(100), PX(40), blue);
        put_tag(&tags, 2, &under);

        place_ratio(&tags, 1, 1, 0, 0, 0, 0, 0);
        show_frame(&tags);
        place_ratio(&tags, 1, 0, 1, 0, 0, 32768, 0);
        show_frame(&tags);
        place_ratio(&tags, 1, 0, 1, 0, 0, 65535, 0);
        show_frame(&tags);

        remove_depth(&tags, 1);
        place_ratio(&tags, 1, 1, 0, 0, 0, 65535, 2);
        place(&tags, 2, 2, 0, 1.0, 1.0, 0, 0, NULL, 0);
        show_frame(&tags);
        emit_movie(dir, "t_morph.swf", 100, 80, &tags, 255, 255, 255);
    }

    /* 20. The two things a morph blends that are not coordinates: a gradient's
     *     stop ratios, and a stroke's width.
     *
     *     The gradient rectangle does not move at all. Its ramp is a hard edge
     *     made of a repeated stop, and only where that stop sits changes -
     *     ratio 64 at one end, 192 at the other, with identical colours - so
     *     the boundary between the two halves is the whole measurement, and
     *     nothing else in the file could move it.
     *
     *     Both frames are exact columns. The quantiser rounds a sample to the
     *     nearest of 256 ratios, so the switch happens half a step below the
     *     stated one. At ratio 0 that is 63.5/255 of the ramp, which across a
     *     hundred pixels puts the boundary at 34.90 and so gives 25 columns of
     *     A and 75 of B - a quarter and three quarters, times forty rows. At
     *     ratio 32768 the stops have blended to 128, the switch is at 127.5,
     *     which is dead centre, and each half is fifty columns.
     *
     *     A reader that took the ratios from either end alone would report
     *     1000/3000 in both frames, or 3000/1000 in both. There is no way to
     *     land on 2000/2000 without blending them.
     *
     *     The stroke is the width test, and it is the one number here that is
     *     not an integer on paper. Zero to twenty pixels, so ten at the
     *     halfway point, over a hundred-pixel segment: a rectangle of a
     *     thousand pixels plus the two half-discs at the ends, which together
     *     are one disc of radius five, seventy-nine more. The tolerance is for
     *     the same two effects t_stroke argues at length - the cap is drawn as
     *     an inscribed polygon and its arc is antialiased - and it is smaller
     *     here because there is half the radius and so half the arc. */
    {
        static const uint8_t a[4]   = { 0x00, 0x80, 0x40, 0xff };
        static const uint8_t b[4]   = { 0xc0, 0x20, 0x80, 0xff };
        static const uint8_t red[4] = { 0xe0, 0x30, 0x30, 0xff };
        bw tags;

        memset(&tags, 0, sizeof tags);
        write_morph_grad(&tags, 1, PX(10), PX(10), PX(110), PX(50),
                         64, 192, a, b);
        write_morph_stroke(&tags, 2, PX(10), PX(70), PX(110), PX(70),
                           0, PX(20), red);
        place_ratio(&tags, 1, 1, 0, 0, 0, 0, 0);
        place_ratio(&tags, 2, 2, 0, 0, 0, 0, 0);
        show_frame(&tags);
        place_ratio(&tags, 1, 0, 1, 0, 0, 32768, 0);
        place_ratio(&tags, 2, 0, 1, 0, 0, 32768, 0);
        show_frame(&tags);
        emit_movie(dir, "t_morphfx.swf", 130, 100, &tags, 255, 255, 255);
    }

    /* 21. Edit text alignment: the same one-glyph string, left, centred and
     *     right in three fields stacked down the stage.
     *
     *     A whole-image pixel count cannot tell these apart - three glyphs are
     *     twelve hundred pixels wherever they sit - so each is asserted inside
     *     a box that only the right answer lands in. That is what the region
     *     argument to ppmcheck is for.
     *
     *     Every position is arithmetic. The field is 200 pixels wide, the
     *     gutter is two on each side, so the line box is 196 pixels and the
     *     glyph is one text height wide, which at 400 twips is twenty. Left
     *     puts its pen at the gutter; right puts it 196 - 20 = 176 pixels
     *     further on; centre puts it half of that, 88, which is a whole number
     *     of pixels only because the width and the glyph are both even. The
     *     baseline sits one ascent below the top gutter and the ascent is the
     *     whole em, so the ink of each field is the twenty rows under its own
     *     top gutter.
     *
     *     The two zero counts matter as much as the three positive ones: they
     *     are what fails if alignment is parsed but ignored, since a renderer
     *     that draws everything flush left still puts the right number of
     *     pixels on the stage. */
    {
        static const uint8_t green[3] = { 0x40, 0xc0, 0x60 };
        bw tags;

        memset(&tags, 0, sizeof tags);
        write_font2_square(&tags, 1);
        write_edit(&tags, 2, 0, PX(200), PX(0),  PX(40), 1, 400, green,
                   0, 0, 0, "M");                      /* left */
        write_edit(&tags, 3, 0, PX(200), PX(40), PX(80), 1, 400, green,
                   2, 0, 0, "M");                      /* centre */
        write_edit(&tags, 4, 0, PX(200), PX(80), PX(120), 1, 400, green,
                   1, 0, 0, "M");                      /* right */
        place(&tags, 1, 2, 0, 1.0, 1.0, 0, 0, NULL, 0);
        place(&tags, 2, 3, 0, 1.0, 1.0, 0, 0, NULL, 0);
        place(&tags, 3, 4, 0, 1.0, 1.0, 0, 0, NULL, 0);
        show_frame(&tags);
        emit_movie(dir, "t_edit.swf", 200, 120, &tags, 255, 255, 255);
    }

    /* 22. A wrap that has to happen.
     *
     *     "MM MM MM" in a field 120 pixels wide. Every glyph and every space
     *     advances one text height, which is twenty pixels, so the line box of
     *     116 pixels holds five characters and not six. The first line is
     *     therefore "MM MM" - five advances, a hundred pixels - and the third
     *     word goes to the second line.
     *
     *     The space that would have been the sixth character is what makes
     *     this a real test rather than an arithmetic one. It overflows, and a
     *     break taken there would push "MM" down for the sake of a gap nobody
     *     can see; the line has to break at the space instead, which is behind
     *     the pen by then.
     *
     *     Total ink is six glyphs of four hundred pixels, and that total is
     *     the same whether the field wrapped or not - so it is asserted by
     *     row band rather than in total. Sixteen hundred pixels in the first
     *     twenty rows and eight hundred in the next twenty is a statement
     *     about where the second line went, and it is zero on both counts if
     *     the text ran off the side in one line. */
    {
        static const uint8_t green[3] = { 0x40, 0xc0, 0x60 };
        bw tags;

        memset(&tags, 0, sizeof tags);
        write_font2_square(&tags, 1);
        write_edit(&tags, 2, 0, PX(120), 0, PX(120), 1, 400, green,
                   0, 1, 1, "MM MM MM");
        place(&tags, 1, 2, 0, 1.0, 1.0, 0, 0, NULL, 0);
        show_frame(&tags);
        emit_movie(dir, "t_editwrap.swf", 120, 120, &tags, 255, 255, 255);
    }

    /* 23. Two records that have to be refused, and one that has to survive
     *     them.
     *
     *     The morph's end shape is one edge short of its start shape, so the
     *     two stop corresponding at the fourth record: one stream has an edge
     *     where the other has its terminator, and there is nothing to pair.
     *     The edit text's flag word claims a font, a colour, a layout block
     *     and text, and the tag ends at the flag word.
     *
     *     Refusing has to cost exactly the character it refuses. Both bad tags
     *     state their length honestly, so the walk resynchronises behind them,
     *     and the rectangle defined after both is what proves it did: the file
     *     must load with one character rather than three, and that one
     *     character must still draw its 800 pixels. A parser that let either
     *     refusal unwind the file would lose the rectangle; one that let a bad
     *     tag through would report three characters. */
    {
        static const uint8_t red[3]   = { 0xe0, 0x20, 0x20 };
        static const uint8_t from[4]  = { 0xe0, 0x20, 0x20, 0xff };
        static const uint8_t to[4]    = { 0x20, 0xe0, 0x20, 0xff };
        bw  shape, tags;
        shp s;

        memset(&tags, 0, sizeof tags);
        write_morph_rect(&tags, 2, PX(10), PX(10), PX(50), PX(30),
                         PX(10), PX(10), PX(90), PX(70), from, to, 1);
        write_edit_truncated(&tags, 3);

        begin_shape(&shape, &s, 1, 0, PX(40), 0, PX(20), red, 1, -1);
        box(&s, 0, 0, PX(40), PX(20), 0, 1);
        sc_end(&s);
        put_tag(&tags, 2, &shape);
        place(&tags, 1, 1, 0, 1.0, 1.0, PX(10), PX(10), NULL, 0);
        show_frame(&tags);
        emit_movie(dir, "t_refuse.swf", 100, 50, &tags, 255, 255, 255);
    }

    /* --- composites --------------------------------------------------------
     *
     * Everything above isolates one thing. Every file below deliberately puts
     * four or five together, because that is where the bugs that survive an
     * isolated test live: each subsystem here was written and proved on its
     * own, against files written by the same hand, and nothing until now has
     * asked what a mask does to a sprite's text under a colour transform.
     *
     * The rule does not change. Each answer is still an area worked out on
     * paper, and where a boundary is curved the curve is kept outside the
     * masked region rather than given a tolerance - which is why the stroke
     * file below asserts the stroke twice, once where a corner is in shot and
     * once where only straight sides are.
     */

    /* 24. A sprite holding text, masked by a clip depth, under a colour
     *     transform. Four subsystems, and the interesting part is the colour.
     *
     *     Text is the one place a colour arrives as a transform rather than as
     *     itself: the glyph outline is opaque white and the text colour is a
     *     multiply on it. Put a second multiply above that - the sprite's
     *     placement, at half - and the two compose, in 8.8 fixed point, with a
     *     truncation at each step. The answer is not the text colour halved:
     *
     *       text 40/c0/60 becomes multipliers ceil(c*256/255) = 65/193/97
     *       under half:    128*65/256 = 32, 128*193/256 = 96, 128*97/256 = 48
     *       applied to white: 255*32/256 = 31, 255*96/256 = 95, 255*48/256 = 47
     *
     *     so 1f/5f/2f, and every one of those three truncations has to happen
     *     in that order to land on it. Rounding anywhere, or composing the two
     *     multiplies the other way round, gives a colour one or two off - which
     *     is exactly the kind of wrong that nobody sees.
     *
     *     Asserting 40/c0/60 covers nothing is the other half of it: a
     *     renderer that dropped the sprite's transform on the way into the
     *     glyph would paint the text colour and look perfectly correct.
     *
     *     The geometry is four em squares at height 400 twips - 20 px - every
     *     600 twips, so 30 px apart, at (10,10) inside the sprite. The mask is
     *     60x40 at (35,15), so it takes the middle two squares whole in x and
     *     the bottom 15 rows of them in y: 2 * 20 * 15 = 600. */
    {
        static const uint8_t green[3] = { 0x40, 0xc0, 0x60 };
        static const uint8_t maskc[3] = { 0x00, 0xc0, 0xff };
        static const int     half[4]  = { 128, 128, 128, 256 };
        bw mask, inner, tags;

        rect_shape(&mask, 3, PX(60), PX(40), maskc);

        memset(&inner, 0, sizeof inner);
        place(&inner, 1, 2, 0, 1.0, 1.0, PX(10), PX(10), NULL, 0);
        show_frame(&inner);

        memset(&tags, 0, sizeof tags);
        write_square_font(&tags, 1);
        write_text(&tags, 2, 1, 400, green, 4, 600);
        put_tag(&tags, 2, &mask);
        define_sprite(&tags, 4, 1, &inner);

        place(&tags, 1, 3, 0, 1.0, 1.0, PX(35), PX(15), NULL, 2);
        place(&tags, 2, 4, 0, 1.0, 1.0, 0, 0, half, 0);
        show_frame(&tags);
        emit_movie(dir, "t_xtext.swf", 140, 80, &tags, 255, 255, 255);
    }

    /* 25. A button whose four states are four different characters, inside a
     *     sprite, with a matrix at every level.
     *
     *     A button is the only construction in the format that defines art and
     *     then requires it not to be drawn. Three of the four characters here
     *     must never appear, and each is a different colour, so a renderer that
     *     draws every record it reads - or that tests the wrong bit of the flag
     *     byte - fails on colour rather than on position.
     *
     *     Only the up record carries a matrix, so the state selection and the
     *     transform stack are one assertion. The 20x10 rectangle passes through
     *     three of them:
     *
     *       button record   +5,+5 px, unscaled
     *       in the sprite   x2 at +10,+10
     *       the sprite      x1.5 at +20,+20
     *
     *     20 x 1 x 2 x 1.5 = 60 wide and 10 x 3 = 30 tall, so 1800 pixels, and
     *     they land at (50,50) - ((0+5)*2+10)*1.5+20. Compose the three in any
     *     other order and neither number survives. */
    {
        static const uint8_t red[3]    = { 0xe0, 0x20, 0x20 };
        static const uint8_t green[3]  = { 0x20, 0xc0, 0x40 };
        static const uint8_t blue[3]   = { 0x30, 0x40, 0xd0 };
        static const uint8_t yellow[3] = { 0xf0, 0xe0, 0x20 };
        bw up, over, down, hit, inner, tags;

        rect_shape(&up,   1, PX(20), PX(10), red);
        rect_shape(&over, 2, PX(20), PX(10), green);
        rect_shape(&down, 3, PX(20), PX(10), blue);
        rect_shape(&hit,  4, PX(20), PX(10), yellow);

        memset(&inner, 0, sizeof inner);
        place(&inner, 1, 5, 0, 2.0, 2.0, PX(10), PX(10), NULL, 0);
        show_frame(&inner);

        memset(&tags, 0, sizeof tags);
        put_tag(&tags, 2, &up);
        put_tag(&tags, 2, &over);
        put_tag(&tags, 2, &down);
        put_tag(&tags, 2, &hit);
        write_button(&tags, 5, 1, 2, 3, 4, 1.0, PX(5), PX(5));
        define_sprite(&tags, 6, 1, &inner);

        place(&tags, 1, 6, 0, 1.5, 1.5, PX(20), PX(20), NULL, 0);
        show_frame(&tags);
        emit_movie(dir, "t_xbtn.swf", 140, 100, &tags, 255, 255, 255);
    }

    /* 26. Two sprites nested, with different frame counts, inside a root with
     *     a third - the file that pins the relationship between three
     *     playheads, which the specification does not describe at all.
     *
     *     The outer sprite has two frames and the inner has three. Colour says
     *     which frame the inner is showing (red, green, blue at its frames 0, 1
     *     and 2) and area says which frame the outer is showing, since the
     *     outer's second frame scales the inner by two: 20x10 = 200 pixels
     *     unscaled, 40x20 = 800 scaled.
     *
     *     Every clip has a playhead of its own that ticks once per parent tick
     *     and goes on ticking through the parent's loop - so the inner is on
     *     root tick N modulo three, whatever the outer wrapped to:
     *
     *       root 0   outer 0, inner 0   red   200
     *       root 1   outer 1, inner 1   green 800
     *       root 2   outer 0, inner 2   blue  200
     *       root 3   outer 1, inner 0   red   800
     *
     *     which is Flash, and which is what ps_swf_stage.c now does. Until it
     *     did, a child's frame came off the parent's *wrapped* frame, so the
     *     outer dragged the inner back to its frame 0 on every loop and this
     *     file traced red/green/red/green with blue never drawn at all. The
     *     error grew with each parent loop, and a short parent driving a long
     *     child is an ordinary construction rather than a corner.
     *
     *     Three frame counts that share no factor above one is the point of
     *     4, 2 and 3: any confusion between the three playheads lands on a
     *     different frame of at least one of them. Every frame asserts all
     *     three colours - one at its area, two at zero - so a playhead that
     *     drifts shows up as a colour in the wrong place and not only as a
     *     missing one. */
    {
        static const uint8_t red[3]   = { 0xe0, 0x20, 0x20 };
        static const uint8_t green[3] = { 0x20, 0xc0, 0x40 };
        static const uint8_t blue[3]  = { 0x30, 0x40, 0xd0 };
        bw f1, f2, f3, in, out, tags;

        rect_shape(&f1, 1, PX(20), PX(10), red);
        rect_shape(&f2, 2, PX(20), PX(10), green);
        rect_shape(&f3, 3, PX(20), PX(10), blue);

        /* The inner sprite swaps the character at one depth rather than moving
         * between depths, so the three frames cannot overlap even if the
         * playhead is wrong - a stale frame shows as a missing colour and not
         * as two colours at once. */
        memset(&in, 0, sizeof in);
        place(&in, 1, 1, 0, 1.0, 1.0, PX(10), PX(10), NULL, 0);
        show_frame(&in);
        place(&in, 1, 2, 1, 1.0, 1.0, PX(10), PX(10), NULL, 0);
        show_frame(&in);
        place(&in, 1, 3, 1, 1.0, 1.0, PX(10), PX(10), NULL, 0);
        show_frame(&in);

        memset(&out, 0, sizeof out);
        place(&out, 1, 10, 0, 1.0, 1.0, 0, 0, NULL, 0);
        show_frame(&out);
        place(&out, 1, 0, 1, 2.0, 2.0, 0, 0, NULL, 0);
        show_frame(&out);

        memset(&tags, 0, sizeof tags);
        put_tag(&tags, 2, &f1);
        put_tag(&tags, 2, &f2);
        put_tag(&tags, 2, &f3);
        define_sprite(&tags, 10, 3, &in);
        define_sprite(&tags, 11, 2, &out);

        place(&tags, 1, 11, 0, 1.0, 1.0, 0, 0, NULL, 0);
        show_frame(&tags);
        show_frame(&tags);
        show_frame(&tags);
        show_frame(&tags);
        emit_movie(dir, "t_xnest.swf", 100, 60, &tags, 255, 255, 255);
    }

    /* 27. One shape carrying a gradient fill and a stroke, masked, and dimmed:
     *     the four paths that turn geometry into a colour, all at once.
     *
     *     They are four genuinely different paths. A gradient's colour is
     *     interpolated first and recoloured after; a stroke's colour is flat
     *     and recoloured up front; a mask is neither, it is geometry; and the
     *     colour transform is the only thing all three share. Under a half
     *     multiply the three colours are exact, because every channel is even:
     *
     *       gradient A 00/80/40 -> 00/40/20
     *       gradient B c0/20/80 -> 60/10/40
     *       stroke     e0/30/30 -> 70/18/18
     *
     *     The rectangle runs (20,20) to (220,120) with a 20 px stroke centred
     *     on it, so the stroke band is 10 px either side and the fill that is
     *     left visible is (30,30) to (210,110). The ramp is the hard-edged
     *     four-stop construction of t_grad_l, mapped so its midpoint is the
     *     rectangle's, which puts the switch at x = 120.
     *
     *     Frame 0 masks with 100x60 at (60,40), a rectangle chosen to sit
     *     entirely inside the fill and clear of the stroke on all four sides:
     *
     *       A   x 60..120 by y 40..100   = 3600
     *       B   x 120..160 by y 40..100  = 2400
     *       stroke                       =    0
     *
     *     That zero is the assertion worth having. A mask that confined the
     *     fills and not the strokes would put a red frame around the picture
     *     and every other number here would still be right.
     *
     *     Frame 1 masks with 160x30 at (40,5), which takes the top stroke band
     *     and five rows of fill under it, and takes them where the band is
     *     straight - the round joins are at x < 30 and x > 210 and the mask
     *     starts at 40. So the stroke count is exact rather than tolerant:
     *
     *       stroke  x 40..200 by y 10..30  = 3200
     *       A       x 40..120 by y 30..35  =  400
     *       B       x 120..200 by y 30..35 =  400
     *
     *     A tolerance would have been the easy way to get the whole stroke
     *     into one number, and it would have hidden a corner that is a few
     *     pixels wrong. Cutting the corners out of shot instead costs a second
     *     frame and asserts to the pixel. */
    {
        static const uint8_t maskc[3] = { 0x00, 0xc0, 0xff };
        static const int     half[4]  = { 128, 128, 128, 256 };
        fsty f[1];
        lsty l[1];
        bw   shape, m0, m1, tags;
        shp  s;

        memset(f, 0, sizeof f);
        f[0].type   = 0x10;                       /* linear */
        f[0].mscale = (double)(PX(220) - PX(20)) / 32768.0;
        f[0].mtx    = (PX(20) + PX(220)) / 2;
        f[0].mty    = (PX(20) + PX(120)) / 2;
        f[0].nstop  = 4;
        f[0].ratio[0] = 0;   f[0].ratio[1] = 128;
        f[0].ratio[2] = 128; f[0].ratio[3] = 255;
        {
            static const uint8_t a[4] = { 0x00, 0x80, 0x40, 0xff };
            static const uint8_t b[4] = { 0xc0, 0x20, 0x80, 0xff };
            memcpy(f[0].stop[0], a, 4);
            memcpy(f[0].stop[1], a, 4);
            memcpy(f[0].stop[2], b, 4);
            memcpy(f[0].stop[3], b, 4);
        }

        memset(l, 0, sizeof l);
        l[0].width = PX(20);
        l[0].rgba[0] = 0xe0; l[0].rgba[1] = 0x30;
        l[0].rgba[2] = 0x30; l[0].rgba[3] = 0xff;

        /* Bounds have to hold the stroke as well as the fill, which is the one
         * thing about a stroked shape that a fill-only generator never says. */
        begin_shape_ex(&shape, &s, 1, PX(10), PX(230), PX(10), PX(130),
                       f, 1, l, 1, 1);
        sc_move_ex(&s, PX(20), PX(20), 0, 1, 1);
        sc_line_to(&s, PX(220), PX(20));
        sc_line_to(&s, PX(220), PX(120));
        sc_line_to(&s, PX(20),  PX(120));
        sc_line_to(&s, PX(20),  PX(20));
        sc_end(&s);

        rect_shape(&m0, 2, PX(100), PX(60), maskc);
        rect_shape(&m1, 3, PX(160), PX(30), maskc);

        memset(&tags, 0, sizeof tags);
        put_tag(&tags, 32, &shape);
        put_tag(&tags, 2, &m0);
        put_tag(&tags, 2, &m1);

        place(&tags, 2, 1, 0, 1.0, 1.0, 0, 0, half, 0);
        place(&tags, 1, 2, 0, 1.0, 1.0, PX(60), PX(40), NULL, 2);
        show_frame(&tags);

        remove_depth(&tags, 1);
        place(&tags, 1, 3, 0, 1.0, 1.0, PX(40), PX(5), NULL, 2);
        show_frame(&tags);

        emit_movie(dir, "t_xgrad.swf", 240, 140, &tags, 255, 255, 255);
    }

    /* 28. A morph inside a sprite, masked by a clip depth, under a colour
     *     transform, at three ratios.
     *
     *     File 19 blends a morph and file 17 masks a shape; this is the seam,
     *     and it is a seam with a playhead in it - the ratio changes because
     *     the sprite's own timeline advances, not because the root restated
     *     it, so the morph is a different shape on each of three frames while
     *     the root does nothing but count.
     *
     *     The rectangle is 40x20 pixels at (10,10) and its right edge alone
     *     moves, to 90, so the width is the only thing the ratio drives and
     *     the height is fixed at twenty. The mask is 60x40 at (20,15), which
     *     cuts the left of the rectangle at every ratio and its right at the
     *     last one only - so the first two frames measure the morph's own edge
     *     and the third measures the mask's, which is what makes the pair of
     *     them a test of both rather than of whichever is smaller.
     *
     *     The middle ratio is 16383 and not 16384, and the one apart is the
     *     whole point of the file. The blend is a truncating integer divide by
     *     65535, and at 16384 a delta of 800 twips gives exactly 200 either
     *     way a reader rounds - so that ratio asserts nothing about the
     *     rounding at all. At 16383 it gives 199 truncated and 200 rounded,
     *     which puts the right edge at 59.95 pixels rather than at exactly 60
     *     and costs a whole column, because the column a boundary falls inside
     *     is blended and is not the fill colour.
     *
     *     Which is also why none of the three needs a tolerance, and that is
     *     worth saying because the note this file replaces predicted one. A
     *     partial column is not counted rather than counted uncertainly, so
     *     the answer is the number of whole columns and is an integer at every
     *     ratio:
     *
     *       ratio 0      x 20..50    30 columns x 15 rows =  450
     *       ratio 16383  x 20..59.95 39 columns x 15 rows =  585
     *       ratio 65535  x 20..80    60 columns x 15 rows =  900
     *
     *     The first draft of this file asserted 585 at ratio 16384 and got
     *     600, which was the arithmetic here being wrong and not the blend.
     *     Recording that is the point of writing the derivation down: the
     *     expected number came from a division done by hand, the division was
     *     wrong by one part in 65535, and the only thing that caught it was
     *     that the number had to be defended rather than recorded.
     *
     *     The colour is constant at both ends of the morph - file 19 already
     *     asserts a blending one - so the only thing that can change it is the
     *     sprite's transform, and 6030c0 under a half multiply is 301860 with
     *     every channel even and no rounding. Asserting 6030c0 covers nothing
     *     is what catches a morph drawn with the ratio right and the transform
     *     lost, which is the failure this combination invites: the blend
     *     rebuilds a shape per frame, and a rebuilt shape is the easiest place
     *     to drop the colour transform that came down the display list. */
    {
        static const uint8_t hue[4]   = { 0x60, 0x30, 0xc0, 0xff };
        static const uint8_t maskc[3] = { 0x00, 0xc0, 0xff };
        static const int     half[4]  = { 128, 128, 128, 256 };
        bw mask, inner, tags;

        rect_shape(&mask, 2, PX(60), PX(40), maskc);

        memset(&inner, 0, sizeof inner);
        place_ratio(&inner, 1, 1, 0, 0, 0, 0, 0);
        show_frame(&inner);
        place_ratio(&inner, 1, 0, 1, 0, 0, 16383, 0);
        show_frame(&inner);
        place_ratio(&inner, 1, 0, 1, 0, 0, 65535, 0);
        show_frame(&inner);

        memset(&tags, 0, sizeof tags);
        write_morph_rect(&tags, 1, PX(10), PX(10), PX(50), PX(30),
                         PX(10), PX(10), PX(90), PX(30), hue, hue, 0);
        put_tag(&tags, 2, &mask);
        define_sprite(&tags, 3, 3, &inner);

        place(&tags, 2, 3, 0, 1.0, 1.0, 0, 0, half, 0);
        place(&tags, 1, 2, 0, 1.0, 1.0, PX(20), PX(15), NULL, 2);
        show_frame(&tags);
        show_frame(&tags);
        show_frame(&tags);
        emit_movie(dir, "t_xmorph.swf", 100, 60, &tags, 255, 255, 255);
    }

    /* 29. A bitmap fill inside a sprite, masked, and under a colour transform.
     *
     *     Files 13 to 16 draw a bitmap against its own shape's bounds and
     *     nothing else. Everything a display list can do to one is untested,
     *     and a bitmap is the case where that matters most: it is the only
     *     fill whose colour comes out of a decoded buffer rather than out of
     *     the shape's own style record, so it is the only one where a colour
     *     transform has to reach a place the parser wrote and not a place the
     *     renderer computed.
     *
     *     The image is 4x2 and deliberately not symmetric:
     *
     *         A A B B      five texels of A, three of B, and no reflection or
     *         A A A B      transposition of it gives that pair.
     *
     *     The fill matrix scales by twenty and a texel is twenty bitmap units,
     *     so a texel is twenty pixels square and the image covers the shape's
     *     80x40 box. The sprite puts it at (10,10), so texel column i occupies
     *     x 10+20i to 30+20i and row j occupies y 10+20j to 30+20j.
     *
     *     The mask is 60x30 at (30,20), which is chosen to fall on texel
     *     boundaries in x and to cut the first texel row in half in y - so the
     *     answer is not the texel counts scaled, it is a different pair, and a
     *     mask applied to the shape but not to the fill's sampling would give
     *     the first pair back:
     *
     *       row 0 keeps its bottom 10 rows, row 1 keeps all 20
     *       column 0 is outside the mask entirely; columns 1, 2 and 3 are in
     *
     *       A  (1,0) 20x10 + (1,1) 20x20 + (2,1) 20x20 = 1000
     *       B  (2,0) 20x10 + (3,0) 20x10 + (3,1) 20x20 =  800
     *
     *     ff60c0 halves to 7f3060 - 255 is the one channel where the multiply
     *     is not exact halving, and 255*128/256 truncates to 127 rather than
     *     rounding to 128, which is the assertion worth having in a file whose
     *     other three channels would pass under either rule. */
    {
        static const uint8_t pat[8] = {
            0, 0, 1, 1,
            0, 0, 0, 1
        };
        static const uint8_t pal[2][3] = {
            { 0xff, 0x60, 0xc0 }, { 0x40, 0xc0, 0x80 }
        };
        static const uint8_t maskc[3] = { 0x00, 0xc0, 0xff };
        static const int     half[4]  = { 128, 128, 128, 256 };
        uint8_t blob[64];
        fsty    f;
        bw      shape, mask, inner, tags;
        shp     s;
        size_t  n = 0;
        int     i;

        /* PIX24: four bytes a texel, the first reserved and not red. Four
         * texels a row is sixteen bytes, so the row padding is already
         * satisfied - file 13 is where that is tested and this is not the
         * place to test it twice. */
        for(i = 0; i < 8; i++) {
            blob[n++] = 0;
            blob[n++] = pal[pat[i]][0];
            blob[n++] = pal[pat[i]][1];
            blob[n++] = pal[pat[i]][2];
        }

        memset(&f, 0, sizeof f);
        f.type   = 0x43;                        /* clipped, not smoothed */
        f.bitmap = 1;
        f.mscale = f.mscale2 = 20.0;
        begin_shape_ex(&shape, &s, 2, 0, PX(80), 0, PX(40), &f, 1, NULL, 0, 1);
        box(&s, 0, 0, PX(80), PX(40), 0, 1);
        sc_end(&s);

        rect_shape(&mask, 3, PX(60), PX(30), maskc);

        memset(&inner, 0, sizeof inner);
        place(&inner, 1, 2, 0, 1.0, 1.0, PX(10), PX(10), NULL, 0);
        show_frame(&inner);

        memset(&tags, 0, sizeof tags);
        write_lossless(&tags, 20, 1, 5, 4, 2, 0, blob, n);
        put_tag(&tags, 32, &shape);
        put_tag(&tags, 2, &mask);
        define_sprite(&tags, 4, 1, &inner);

        place(&tags, 2, 4, 0, 1.0, 1.0, 0, 0, half, 0);
        place(&tags, 1, 3, 0, 1.0, 1.0, PX(30), PX(20), NULL, 2);
        show_frame(&tags);
        emit_movie(dir, "t_xbmp.swf", 110, 70, &tags, 255, 255, 255);
    }

    /* 30. An edit text inside a sprite, masked, and under a colour transform.
     *
     *     File 21 lays an edit text out and file 24 does this to *static*
     *     text, and the two are not the same path: a DefineText carries a pen
     *     position per glyph and an edit text computes one, so the thing this
     *     asks is whether a computed pen survives a transform stack and a
     *     mask. It also asks it of three glyphs where only two survive, which
     *     is the part a total cannot see.
     *
     *     "MMM" at height 400 twips in a field 200 pixels wide, left aligned.
     *     By file 21's arithmetic the gutter is two pixels, the glyph is one
     *     text height wide, and the baseline is one ascent - a whole em, so
     *     twenty pixels - below the top gutter. The three glyphs are therefore
     *     x 2..22, 22..42 and 42..62, all in rows y 2..22.
     *
     *     The mask is 42x28 at (0,12). Its right edge is on the boundary
     *     between the second glyph and the third, so the third must vanish
     *     entirely and the first two must survive whole; its top cuts the ink
     *     ten rows down. Forty columns by ten rows is 400, and the region
     *     assertions split that 200 and 200 - which is what says the two that
     *     survived are the two nearest the pen and not the same ink slid
     *     along. The count above x 43 is zero, and that is the one that fails
     *     if a mask reaches shapes and not glyphs.
     *
     *     The colour goes through two multiplies, as file 24's does, and lands
     *     somewhere neither of them is: 20c080 becomes multipliers 33, 193 and
     *     129, halves to 16, 96 and 64, and paints white as 0f5f3f. Every one
     *     of the three truncations has to happen in order. */
    {
        static const uint8_t green[3] = { 0x20, 0xc0, 0x80 };
        static const uint8_t maskc[3] = { 0x00, 0xc0, 0xff };
        static const int     half[4]  = { 128, 128, 128, 256 };
        bw mask, inner, tags;

        rect_shape(&mask, 3, PX(42), PX(28), maskc);

        memset(&inner, 0, sizeof inner);
        place(&inner, 1, 2, 0, 1.0, 1.0, 0, 0, NULL, 0);
        show_frame(&inner);

        memset(&tags, 0, sizeof tags);
        write_font2_square(&tags, 1);
        write_edit(&tags, 2, 0, PX(200), 0, PX(40), 1, 400, green,
                   0, 0, 0, "MMM");
        put_tag(&tags, 2, &mask);
        define_sprite(&tags, 4, 1, &inner);

        place(&tags, 2, 4, 0, 1.0, 1.0, 0, 0, half, 0);
        place(&tags, 1, 3, 0, 1.0, 1.0, 0, PX(12), NULL, 2);
        show_frame(&tags);
        emit_movie(dir, "t_xedit.swf", 200, 60, &tags, 255, 255, 255);
    }

    /* 32. The two readings of a mask that paints nothing, in one file.
     *
     *     Frame 0 places a clip depth over character 99, which nothing
     *     defines. That is not the file saying "confine this to nothing" - it
     *     is a mask this build could not construct, and the only honest thing
     *     to do with one is to drop it, because an empty mask hides every
     *     depth up to the clip depth and takes the whole picture with it. The
     *     red rectangle is 60x40 at scale 1, so 2400 pixels, all of them.
     *
     *     Frame 1 swaps the same depth to character 2, which is defined and is
     *     placed at (200,200) on a 100x60 stage - entirely off it. Now the
     *     file has said something: the mask exists and covers nothing, so it
     *     hides everything in its range and the count is 0. Green is asserted
     *     at zero on both frames, since a mask is never drawn either way and a
     *     mask that leaked onto the stage would show up here and nowhere else.
     *
     *     One file rather than two because the difference between the two
     *     frames is one character ID, which is exactly the distinction. */
    {
        static const uint8_t red[3]   = { 0xe0, 0x20, 0x20 };
        static const uint8_t green[3] = { 0x20, 0xc0, 0x40 };
        bw f1, f2, tags;

        rect_shape(&f1, 1, PX(60), PX(40), red);
        rect_shape(&f2, 2, PX(60), PX(40), green);

        memset(&tags, 0, sizeof tags);
        put_tag(&tags, 2, &f1);
        put_tag(&tags, 2, &f2);

        place(&tags, 5, 1, 0, 1.0, 1.0, 0, 0, NULL, 0);
        place(&tags, 1, 99, 0, 1.0, 1.0, 0, 0, NULL, 10);
        show_frame(&tags);
        place(&tags, 1, 2, 1, 1.0, 1.0, PX(200), PX(200), NULL, 10);
        show_frame(&tags);
        emit_movie(dir, "t_maskgap.swf", 100, 60, &tags, 255, 255, 255);
    }

    /* 33. Frame labels and an instance name: the two fields the parser claimed
     *     to read and did not.
     *
     *     Neither changes a pixel, so there is no count to derive - what is
     *     asserted is the mapping itself, printed by swfrender and checked
     *     line for line. The numbers in it are still arithmetic: a label names
     *     the frame it precedes, so "start" is frame 0 and "middle" is frame
     *     2, counted in ShowFrames and zero-based.
     *
     *     The named placement also carries a clip depth, which puts the Name
     *     and the ClipDepth in the same tag - the flag pair a reader gets away
     *     with mis-ordering until it meets both at once, since the Name sits
     *     between them and a two-byte overread lands in the middle of it. Its
     *     clip depth is below its own depth, so it names an empty range and is
     *     dropped without confining anything; the second, plain placement is
     *     what draws, at 40x20 = 800 pixels on every frame. */
    {
        static const uint8_t blue[3] = { 0x30, 0x40, 0xd0 };
        bw f1, tags;

        rect_shape(&f1, 1, PX(40), PX(20), blue);

        memset(&tags, 0, sizeof tags);
        put_tag(&tags, 2, &f1);

        frame_label(&tags, "start");
        place_named(&tags, 3, 1, 0, 1.0, 1.0, 0, 0, NULL, 2, "box");
        place(&tags, 5, 1, 0, 1.0, 1.0, PX(10), PX(10), NULL, 0);
        show_frame(&tags);
        show_frame(&tags);
        frame_label(&tags, "middle");
        show_frame(&tags);
        show_frame(&tags);
        emit_movie(dir, "t_label.swf", 100, 60, &tags, 255, 255, 255);
    }

    return 0;
}
