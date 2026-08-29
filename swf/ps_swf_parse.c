/* SWF header, tag stream and shape records.
 *
 * Everything here parses hostile input: a .swf arrives over HTTP from a page
 * we did not write, and the lengths, counts and bit widths that drive every
 * loop below are all fields inside that file. So the rules are:
 *
 *   - no read may leave the buffer. ps_bits enforces that centrally, and the
 *     sticky overrun flag is checked at each loop head so a corrupt length
 *     ends the loop instead of spinning on it.
 *   - no allocation may be sized directly from a file field. Every array is
 *     capped against the file's own length, using the fact that each element
 *     costs a known minimum number of bits to encode: a file of L bytes
 *     cannot honestly describe more than L edges or L styles. A file claiming
 *     65535 fill styles in 40 bytes gets rejected rather than allocating for
 *     them.
 *
 * That second rule matters more than it looks. The eventual target has 16MB
 * and no virtual memory, so an allocation that merely "fails gracefully" on a
 * desktop takes the whole browser down there.
 *
 * The record primitives both rules are built on - the bounded array, the RECT,
 * the MATRIX, the colour - now live in ps_swf_read.h, because the morph shape
 * and edit text parsers need the same ones and three copies of the MATRIX
 * stepping rule would be three chances for them to drift apart.
 */
#include "ps_swf.h"
#include "ps_swf_bits.h"
#include "ps_swf_edit.h"
#include "ps_swf_image.h"
#include "ps_swf_mem.h"
#include "ps_swf_morph.h"
#include "ps_swf_read.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ps_alloc   ps_swf_alloc
#define ps_realloc ps_swf_realloc
#define ps_free    ps_swf_dealloc

/* --- primitives --------------------------------------------------------- */

/* GRADIENT: two bits of spread mode, two of interpolation mode, four of stop
 * count, then that many records of a ratio byte and a colour. The two mode
 * fields are SWF 8 and are zero in everything this reads, but they occupy
 * bits in every version, so they are read and discarded rather than skipped.
 *
 * `mean` comes back as well as the stops because the fill style keeps it - see
 * ps_swf_fill. The mean, not the first stop: a Flash 3 background is usually a
 * two-stop ramp used as a wash, and a caller that has to pick one colour reads
 * much closer to the frame with the average than with either end. */
static void read_gradient(ps_bits *b, int with_alpha, ps_swf_gradient *g,
                          ps_swf_rgba *mean)
{
    unsigned n, i;
    unsigned r = 0, gr = 0, bl = 0, a = 0;

    (void)ps_bits_ub(b, 4);            /* spread + interpolation, SWF 8 only */
    n = ps_bits_ub(b, 4);

    g->nstop = 0;
    for(i = 0; i < n && !b->over; i++) {
        uint8_t     ratio = ps_bits_u8(b);
        ps_swf_rgba c     = read_color(b, with_alpha);

        /* Every record is read even past the storage limit. Reading is what
         * positions the stream for the next fill style; storing is only what
         * lets this one be shaded. */
        if(g->nstop < PS_SWF_MAX_STOPS) {
            g->ratio[g->nstop] = ratio;
            g->color[g->nstop] = c;
            g->nstop++;
        }
        r += c.r; gr += c.g; bl += c.b; a += c.a;
    }

    if(n == 0 || b->over) {
        /* A gradient with no stops is not drawable and not worth a special
         * case downstream, so it becomes opaque black - visible, and wrong in
         * a way that points at the file rather than at the shader. */
        mean->r = mean->g = mean->b = 0;
        mean->a = 255;
        return;
    }
    mean->r = (uint8_t)(r / n);
    mean->g = (uint8_t)(gr / n);
    mean->b = (uint8_t)(bl / n);
    mean->a = (uint8_t)(a / n);
}

/* CXFORM and CXFORMWITHALPHA.
 *
 * The trap here is that the two flag bits are in the opposite order to the
 * fields they announce: HasAddTerms is read first, but the multiply terms are
 * stored first. A reader that follows the flag order gets a colour transform
 * that is subtly wrong on every file that uses both, which looks like a
 * blending bug rather than a parsing one.
 *
 * Multiply terms are 8.8 fixed point, so 256 is unchanged; add terms are plain
 * integers in -255..255. Both are signed and the file may ask for values that
 * take a channel out of range - Flash clamps at the end rather than rejecting,
 * so the parser stores what it was told and the clamp lives in the apply. */
static void read_cxform(ps_bits *b, int with_alpha, ps_swf_cxform *cx)
{
    int has_add, has_mult, n, i;

    ps_swf_cxform_identity(cx);
    ps_bits_align(b);
    has_add  = (int)ps_bits_ub(b, 1);
    has_mult = (int)ps_bits_ub(b, 1);
    n        = (int)ps_bits_ub(b, 4);

    if(has_mult)
        for(i = 0; i < (with_alpha ? 4 : 3); i++)
            cx->mult[i] = (int16_t)ps_bits_sb(b, n);
    if(has_add)
        for(i = 0; i < (with_alpha ? 4 : 3); i++)
            cx->add[i] = (int16_t)ps_bits_sb(b, n);
    ps_bits_align(b);
}

/* A null-terminated string, copied out.
 *
 * Stepping over one exactly is the part that cannot be got wrong - the fields
 * after a PlaceObject2's Name are read from wherever this leaves the cursor -
 * so the walk is unconditional and the copy is what may fail. NULL comes back
 * for an empty string, for a string that ran off the end, and for one that
 * would not fit in `cap` bytes, which is the same per-input-byte ceiling every
 * other allocation here is bounded by. A lost name costs a script the ability
 * to name that instance; it does not cost the file. */
static char *read_string(ps_bits *b, uint32_t cap)
{
    size_t start;
    size_t len;
    char  *out;

    /* Aligned before `start` is taken rather than left to the first u8, which
     * aligns as a side effect: the two would disagree by a byte on a string
     * that follows a bitfield, and the copy would start one early. */
    ps_bits_align(b);
    start = b->pos;

    while(!b->over && ps_bits_u8(b) != 0)
        ;
    if(b->over || b->pos <= start + 1)
        return NULL;

    len = b->pos - start - 1;          /* the terminator is not kept */
    if(len >= cap)
        return NULL;
    out = ps_alloc(len + 1);
    if(!out)
        return NULL;
    memcpy(out, b->data + start, len);
    out[len] = '\0';
    return out;
}

/* --- style arrays ------------------------------------------------------- */

static int parse_fill_styles(ps_bits *b, vec *fills, vec *grads, vec *bfills,
                             int ver, uint32_t cap)
{
    uint32_t count = ps_bits_u8(b);
    uint32_t i;
    /* A gradient fill style cannot be encoded in fewer than seven bytes - one
     * for the type, one for the smallest MATRIX once it is byte aligned, one
     * for the mode and count field, and four for a single RGB stop - so a file
     * of L bytes cannot honestly describe more than L/7 of them. Same argument
     * as the per-byte cap on the style tables themselves, just with a
     * different minimum, and it has to be a different one: a gradient is a
     * hundred bytes of storage against a fill style's twelve. */
    uint32_t gcap = cap / 7u ? cap / 7u : 1u;
    /* A bitmap fill style is four bytes at its tightest - one of type, two of
     * character ID, and one for a MATRIX that is all default flags - so its
     * out-of-line record gets its own ceiling rather than borrowing the
     * gradient's. Sharing the gradient's would refuse a legitimate file of
     * closely packed bitmap fills. */
    uint32_t bcap = cap / 4u ? cap / 4u : 1u;

    /* 0xff is only an escape from DefineShape2 onwards; in a v1 shape it is
     * a literal count of 255. */
    if(count == 0xff && ver >= 2)
        count = ps_bits_u16(b);
    if(cap > MAX_STYLES)
        cap = MAX_STYLES;
    if(gcap > MAX_STYLES)
        gcap = MAX_STYLES;
    if(bcap > MAX_STYLES)
        bcap = MAX_STYLES;

    for(i = 0; i < count; i++) {
        ps_swf_fill f;

        if(b->over)
            return -1;
        memset(&f, 0, sizeof f);
        f.type = ps_bits_u8(b);

        switch(f.type) {
        case PS_SWF_FILL_SOLID:
            f.color = read_color(b, ver >= 3);
            break;
        case PS_SWF_FILL_LINEAR:
        case PS_SWF_FILL_RADIAL: {
            ps_swf_gradient g;

            memset(&g, 0, sizeof g);
            read_matrix(b, g.mat);
            read_gradient(b, ver >= 3, &g, &f.color);
            if(grads->n >= MAX_STYLES || vec_push(grads, &g, gcap) < 0)
                return -1;
            f.grad = (uint16_t)grads->n;      /* 1-based */
            break;
        }
        case 0x40: case 0x41: case 0x42: case 0x43: {
            ps_swf_bitmapfill bf;

            memset(&bf, 0, sizeof bf);
            f.bitmap_id = ps_bits_u16(b);
            read_matrix(b, bf.mat);
            if(bfills->n >= MAX_STYLES || vec_push(bfills, &bf, bcap) < 0)
                return -1;
            f.bfill = (uint16_t)bfills->n;    /* 1-based */
            /* The fallback for a bitmap that never resolves. Mid grey rather
             * than transparent because a fill that draws nothing is
             * indistinguishable from geometry that was never parsed, and the
             * two get debugged in completely different places. */
            f.color.r = f.color.g = f.color.b = 128;
            f.color.a = 255;
            break;
        }
        default:
            /* An unknown fill type means the rest of the array cannot be
             * located, so there is nothing to do but stop. Continuing would
             * read style records out of the middle of edge data. */
            return -1;
        }
        if(vec_push(fills, &f, cap) < 0)
            return -1;
    }
    return b->over ? -1 : 0;
}

static int parse_line_styles(ps_bits *b, vec *lines, int ver, uint32_t cap)
{
    uint32_t count = ps_bits_u8(b);
    uint32_t i;

    if(count == 0xff && ver >= 2)
        count = ps_bits_u16(b);
    if(cap > MAX_STYLES)
        cap = MAX_STYLES;

    for(i = 0; i < count; i++) {
        ps_swf_line l;

        if(b->over)
            return -1;
        l.width = ps_bits_u16(b);
        l.color = read_color(b, ver >= 3);
        if(vec_push(lines, &l, cap) < 0)
            return -1;
    }
    return b->over ? -1 : 0;
}

/* --- shape records ------------------------------------------------------ */

static int parse_shape_records(ps_bits *b, ps_swf_shape *sh, int ver,
                               vec *fills, vec *lines, vec *grads, vec *bfills,
                               vec *edges, uint32_t cap)
{
    uint32_t fill_base = 0, line_base = 0;
    uint32_t fill0 = 0, fill1 = 0, line = 0;
    int32_t  x = 0, y = 0;
    int      nfillbits, nlinebits;
    uint8_t  layer = 0;

    nfillbits = (int)ps_bits_ub(b, 4);
    nlinebits = (int)ps_bits_ub(b, 4);

    for(;;) {
        if(b->over)
            return -1;

        if(ps_bits_ub(b, 1) == 0) {
            uint32_t flags = ps_bits_ub(b, 5);

            if(flags == 0)
                return 0;                      /* EndShapeRecord */

            if(flags & 0x01) {                 /* StateMoveTo */
                int mb = (int)ps_bits_ub(b, 5);
                x = ps_bits_sb(b, mb);
                y = ps_bits_sb(b, mb);
            }
            if(flags & 0x02)
                fill0 = ps_bits_ub(b, nfillbits);
            if(flags & 0x04)
                fill1 = ps_bits_ub(b, nfillbits);
            if(flags & 0x08)
                line = ps_bits_ub(b, nlinebits);

            if(flags & 0x10) {                 /* StateNewStyles */
                if(ver < 2)
                    return -1;                 /* not encodable in a v1 shape */
                if(layer == 255)
                    return -1;
                fill_base = fills->n;
                line_base = lines->n;
                if(parse_fill_styles(b, fills, grads, bfills, ver, cap) < 0)
                    return -1;
                if(parse_line_styles(b, lines, ver, cap) < 0)
                    return -1;
                nfillbits = (int)ps_bits_ub(b, 4);
                nlinebits = (int)ps_bits_ub(b, 4);
                /* A new style table means the old indices are meaningless,
                 * and the spec requires the styles to be restated before any
                 * further edge, so clearing them is not just tidiness - it
                 * stops a stale index selecting a colour from the new table. */
                fill0 = fill1 = line = 0;
                layer++;
                if(layer + 1u > sh->nlayer)
                    sh->nlayer = layer + 1u;
            }
        } else {
            ps_swf_edge e;
            int         straight = (int)ps_bits_ub(b, 1);
            int         nb       = (int)ps_bits_ub(b, 4) + 2;
            int32_t     nx, ny;

            memset(&e, 0, sizeof e);
            e.x0 = x;
            e.y0 = y;

            if(straight) {
                int32_t dx = 0, dy = 0;

                if(ps_bits_ub(b, 1)) {         /* GeneralLineFlag */
                    dx = ps_bits_sb(b, nb);
                    dy = ps_bits_sb(b, nb);
                } else if(ps_bits_ub(b, 1)) {  /* VertLineFlag */
                    dy = ps_bits_sb(b, nb);
                } else {
                    dx = ps_bits_sb(b, nb);
                }
                nx = x + dx;
                ny = y + dy;
            } else {
                int32_t cdx = ps_bits_sb(b, nb);
                int32_t cdy = ps_bits_sb(b, nb);
                int32_t adx, ady;

                e.cx  = x + cdx;
                e.cy  = y + cdy;
                adx   = ps_bits_sb(b, nb);
                ady   = ps_bits_sb(b, nb);
                nx    = e.cx + adx;
                ny    = e.cy + ady;
                e.curve = 1;
            }

            e.x1    = nx;
            e.y1    = ny;
            e.fill0 = style_index(fill_base, fill0, fills->n);
            e.fill1 = style_index(fill_base, fill1, fills->n);
            e.line  = style_index(line_base, line,  lines->n);
            e.layer = layer;

            /* An edge with nothing on either side and no stroke is a pen move
             * expressed the long way round. It can never contribute a pixel,
             * so it is dropped rather than stored - Flash exporters emit
             * enough of them for it to be worth the line of code. */
            if((e.fill0 || e.fill1 || e.line) && vec_push(edges, &e, cap) < 0)
                return -1;

            x = nx;
            y = ny;
        }
    }
}

static int parse_define_shape(const uint8_t *body, size_t blen, int ver,
                              ps_swf_shape *sh, uint32_t cap)
{
    ps_bits b;
    vec     fills = { NULL, 0, 0, sizeof(ps_swf_fill) };
    vec     lines = { NULL, 0, 0, sizeof(ps_swf_line) };
    vec     grads = { NULL, 0, 0, sizeof(ps_swf_gradient) };
    vec     bfills = { NULL, 0, 0, sizeof(ps_swf_bitmapfill) };
    vec     edges = { NULL, 0, 0, sizeof(ps_swf_edge) };

    memset(sh, 0, sizeof *sh);
    sh->nlayer = 1;

    ps_bits_init(&b, body, blen);
    sh->id = ps_bits_u16(&b);
    read_rect(&b, &sh->xmin, &sh->xmax, &sh->ymin, &sh->ymax);

    if(parse_fill_styles(&b, &fills, &grads, &bfills, ver, cap) < 0 ||
       parse_line_styles(&b, &lines, ver, cap) < 0 ||
       parse_shape_records(&b, sh, ver, &fills, &lines, &grads, &bfills,
                           &edges, cap) < 0) {
        ps_free(fills.base);
        ps_free(lines.base);
        ps_free(grads.base);
        ps_free(bfills.base);
        ps_free(edges.base);
        return -1;
    }

    sh->fills  = fills.base;   sh->nfill  = fills.n;
    sh->lines  = lines.base;   sh->nline  = lines.n;
    sh->grads  = grads.base;   sh->ngrad  = grads.n;
    sh->bfills = bfills.base;  sh->nbfill = bfills.n;
    sh->edges  = edges.base;   sh->nedge  = edges.n;
    return 0;
}

/* --- fonts, text and buttons -------------------------------------------- */

/* A glyph outline is a SHAPE, not a SHAPEWITHSTYLE: no fill style array, no
 * line style array, just the fill/line bit widths and the records. The spec
 * requires the first style change of every glyph to select fill style 1, into
 * a table that does not exist in the file - so the renderer is expected to
 * invent one. Inventing it here, as a single opaque white entry, is what lets
 * a glyph be an ordinary ps_swf_shape from this point on: white multiplied by
 * the text colour is the text colour, and the colour arrives as a transform at
 * draw time exactly as Flash delivers it.
 *
 * parse_shape_records already begins by reading the two four-bit widths, which
 * is precisely the byte a glyph starts with, so it is reused unchanged. */
static int parse_glyph(ps_bits *b, ps_swf_shape *sh, uint32_t cap)
{
    vec         fills = { NULL, 0, 0, sizeof(ps_swf_fill) };
    vec         lines = { NULL, 0, 0, sizeof(ps_swf_line) };
    vec         grads = { NULL, 0, 0, sizeof(ps_swf_gradient) };
    vec         bfills = { NULL, 0, 0, sizeof(ps_swf_bitmapfill) };
    vec         edges = { NULL, 0, 0, sizeof(ps_swf_edge) };
    ps_swf_fill white;

    memset(sh, 0, sizeof *sh);
    sh->nlayer = 1;

    memset(&white, 0, sizeof white);
    white.type    = PS_SWF_FILL_SOLID;
    white.color.r = white.color.g = white.color.b = white.color.a = 255;
    if(vec_push(&fills, &white, cap) < 0)
        return -1;

    if(parse_shape_records(b, sh, 1, &fills, &lines, &grads, &bfills, &edges,
                           cap) < 0) {
        ps_free(fills.base);
        ps_free(lines.base);
        ps_free(grads.base);
        ps_free(bfills.base);
        ps_free(edges.base);
        return -1;
    }
    sh->fills  = fills.base;   sh->nfill  = fills.n;
    sh->lines  = lines.base;   sh->nline  = lines.n;
    sh->grads  = grads.base;   sh->ngrad  = grads.n;
    sh->bfills = bfills.base;  sh->nbfill = bfills.n;
    sh->edges  = edges.base;   sh->nedge  = edges.n;
    return 0;
}

static void free_font(ps_swf_font *f)
{
    uint32_t i;

    for(i = 0; i < f->nglyph; i++) {
        ps_free(f->glyphs[i].fills);
        ps_free(f->glyphs[i].lines);
        ps_free(f->glyphs[i].grads);
        ps_free(f->glyphs[i].bfills);
        ps_free(f->glyphs[i].edges);
    }
    ps_free(f->glyphs);
    ps_free(f->code);
    ps_free(f->advance);
    f->glyphs  = NULL;
    f->code    = NULL;
    f->advance = NULL;
    f->nglyph  = 0;
}

/* DefineFont, and the trick that makes it parseable: the glyph count is not
 * stored anywhere. What is stored is a table of byte offsets to the outlines,
 * measured from the start of that table - and since the outlines begin
 * immediately after it, the first offset is exactly twice the number of
 * entries. Dividing it by two is the only way to know how many there are, and
 * a reader that assumes anything else reads glyph data as offsets. */
static int parse_define_font(const uint8_t *body, size_t blen, ps_swf_font *f,
                             uint32_t cap)
{
    ps_bits  b;
    vec      glyphs = { NULL, 0, 0, sizeof(ps_swf_shape) };
    uint32_t n, i, first;
    /* Two bytes of offset per glyph, so a table of n needs 2n bytes and the
     * outlines need at least one more each. */
    uint32_t gcap = cap / 3u ? cap / 3u : 1u;

    memset(f, 0, sizeof *f);
    ps_bits_init(&b, body, blen);
    f->id = ps_bits_u16(&b);
    first = ps_bits_u16(&b);
    if(b.over || (first & 1) || first < 2)
        return -1;
    n = first / 2;

    for(i = 0; i < n; i++) {
        ps_swf_shape g;
        ps_bits      gb;
        uint32_t     off;

        if(i == 0) {
            off = first;
        } else {
            b.pos = 2 + (size_t)i * 2;
            b.bit = 0;
            off = ps_bits_u16(&b);
            if(b.over)
                break;
        }
        /* Offsets are from the first byte of the offset table, which is two
         * bytes into the tag - after the font ID. */
        if((size_t)off + 2 >= blen)
            break;
        ps_bits_init(&gb, body, blen);
        gb.pos = (size_t)off + 2;

        if(parse_glyph(&gb, &g, cap) < 0)
            break;
        if(vec_push(&glyphs, &g, gcap) < 0) {
            ps_free(g.fills);
            ps_free(g.lines);
            ps_free(g.grads);
            ps_free(g.bfills);
            ps_free(g.edges);
            break;
        }
    }
    f->glyphs = glyphs.base;
    f->nglyph = glyphs.n;
    return 0;
}

/* The code table and, if the file states one, the advances and the vertical
 * metrics. All of it exists for DefineEditText and for nothing else: a
 * DefineText names glyphs by index and carries its own advances, so a font
 * without any of this still draws every static text in the movie.
 *
 * It is read after the outlines rather than beside them because that is where
 * it is - the code table sits behind the whole glyph shape table, at an offset
 * stated in the entry immediately after the offset table, and the layout block
 * sits behind the code table with no offset to it at all. Its position is only
 * derivable from the glyph count, which is why nothing can be read here until
 * the glyphs have been. */
static void font2_tables(ps_bits *b, ps_swf_font *f, size_t table,
                         uint32_t nglyph, uint32_t wide_off, uint32_t wide_code,
                         int has_layout)
{
    uint32_t off, i;

    if(f->nglyph == 0)
        return;

    b->pos = table + (size_t)nglyph * (wide_off ? 4 : 2);
    b->bit = 0;
    off = wide_off ? ps_bits_u32(b) : ps_bits_u16(b);
    if(b->over || table + off >= b->len)
        return;
    b->pos = table + off;
    b->bit = 0;

    /* Two counts, and keeping them apart is the whole of this function. Both
     * tables are as long as the header says, which is what decides where the
     * one behind them starts; but only as many entries are kept as there are
     * outlines, because a truncated font yields fewer of those than it claims
     * and a code that names a glyph which is not there is worse than no code.
     * Reading only the kept ones would leave the cursor inside the code table
     * and read half of it as advances. */
    f->code = ps_alloc((size_t)f->nglyph * sizeof *f->code);
    if(!f->code)
        return;
    for(i = 0; i < nglyph; i++) {
        uint16_t c = (uint16_t)(wide_code ? ps_bits_u16(b) : ps_bits_u8(b));

        if(i < f->nglyph)
            f->code[i] = b->over ? 0 : c;
    }

    if(!has_layout)
        return;
    f->ascent  = (int16_t)ps_bits_u16(b);
    f->descent = (int16_t)ps_bits_u16(b);
    f->leading = (int16_t)ps_bits_u16(b);

    f->advance = ps_alloc((size_t)f->nglyph * sizeof *f->advance);
    if(!f->advance)
        return;
    for(i = 0; i < nglyph; i++) {
        int16_t a = (int16_t)ps_bits_u16(b);

        if(i < f->nglyph)
            f->advance[i] = a;
    }
    if(b->over) {
        /* A layout block that ran out mid-table would give some glyphs a zero
         * advance and stack them, which is worse than falling back on the
         * outline bounds for all of them. */
        ps_free(f->advance);
        f->advance = NULL;
        f->ascent = f->descent = f->leading = 0;
    }
}

/* DefineFont2. Same outlines, but the count is explicit and the header in
 * front of the offset table is variable - and the field that catches readers
 * out is LanguageCode, which is present in every version even though it is
 * required to be zero before SWF 6. Skip it and every offset afterwards is
 * read one byte early. */
static int parse_define_font2(const uint8_t *body, size_t blen, ps_swf_font *f,
                              uint32_t cap)
{
    ps_bits  b;
    vec      glyphs = { NULL, 0, 0, sizeof(ps_swf_shape) };
    uint32_t n, i, wide, wide_code, namelen;
    int      has_layout;
    size_t   table;
    uint32_t gcap = cap / 3u ? cap / 3u : 1u;

    memset(f, 0, sizeof *f);
    ps_bits_init(&b, body, blen);
    f->id = ps_bits_u16(&b);
    {
        /* The flag byte reads MSB first in the order the field list gives, so
         * HasLayout is the top bit and the two width flags are in the middle. */
        unsigned flags = ps_bits_u8(&b);
        has_layout = (flags & 0x80) != 0;
        wide       = (flags & 0x08) != 0;    /* wide offsets */
        wide_code  = (flags & 0x04) != 0;
    }
    (void)ps_bits_u8(&b);                    /* LanguageCode, always present */
    namelen = ps_bits_u8(&b);
    ps_bits_skip(&b, namelen);
    n = ps_bits_u16(&b);
    if(b.over)
        return -1;

    /* Offsets are measured from here, the first byte of the offset table, and
     * so include the table itself and the code table offset behind it. */
    table = b.pos;

    for(i = 0; i < n; i++) {
        ps_swf_shape g;
        ps_bits      gb;
        uint32_t     off;

        b.pos = table + (size_t)i * (wide ? 4 : 2);
        b.bit = 0;
        off = wide ? ps_bits_u32(&b) : ps_bits_u16(&b);
        if(b.over || table + off >= blen)
            break;

        ps_bits_init(&gb, body, blen);
        gb.pos = table + off;
        if(parse_glyph(&gb, &g, cap) < 0)
            break;
        if(vec_push(&glyphs, &g, gcap) < 0) {
            ps_free(g.fills);
            ps_free(g.lines);
            ps_free(g.grads);
            ps_free(g.bfills);
            ps_free(g.edges);
            break;
        }
    }
    f->glyphs = glyphs.base;
    f->nglyph = glyphs.n;
    font2_tables(&b, f, table, n, wide, wide_code, has_layout);
    return 0;
}

/* DefineText / DefineText2.
 *
 * Two orderings here are the reverse of what the field list suggests, and both
 * are silent when wrong. TextHeight is gated on the same HasFont flag as the
 * font ID but is read after the two offsets, not beside the ID. And a record
 * that omits XOffset does not restart at zero - it continues where the last
 * glyph left the pen, which is how an exporter writes a line whose colour
 * changes half way along. Resetting there splits every styled line into
 * overlapping pieces stacked at the left margin. */
static int parse_define_text(const uint8_t *body, size_t blen, int ver,
                             ps_swf_text *t, uint32_t cap)
{
    ps_bits     b;
    vec         glyphs = { NULL, 0, 0, sizeof(ps_swf_glyphref) };
    int32_t     x0, x1, y0, y1;
    int         gbits, abits;
    uint16_t    font_id = 0, height = 0;
    ps_swf_rgba color = { 0, 0, 0, 255 };
    int32_t     px = 0, py = 0;

    memset(t, 0, sizeof *t);
    ps_bits_init(&b, body, blen);
    t->id = ps_bits_u16(&b);
    read_rect(&b, &x0, &x1, &y0, &y1);
    read_matrix(&b, t->mat.m);
    gbits = (int)ps_bits_u8(&b);
    abits = (int)ps_bits_u8(&b);
    if(b.over || gbits > 32 || abits > 32)
        return -1;

    for(;;) {
        unsigned flags;
        unsigned count, i;

        if(b.over)
            break;
        flags = ps_bits_u8(&b);
        if(flags == 0)
            break;                            /* end of records */

        if(flags & 0x08)
            font_id = ps_bits_u16(&b);
        if(flags & 0x04)
            color = read_color(&b, ver >= 2);  /* RGBA only in DefineText2 */
        if(flags & 0x01)
            px = (int16_t)ps_bits_u16(&b);
        if(flags & 0x02)
            py = (int16_t)ps_bits_u16(&b);
        if(flags & 0x08)
            height = ps_bits_u16(&b);

        count = ps_bits_u8(&b);
        for(i = 0; i < count && !b.over; i++) {
            ps_swf_glyphref g;
            uint32_t        idx = ps_bits_ub(&b, gbits);
            int32_t         adv = ps_bits_sb(&b, abits);

            g.font_id = font_id;
            g.glyph   = (uint16_t)(idx > 0xffffu ? 0xffffu : idx);
            g.height  = height;
            g.color   = color;
            g.x       = px;
            g.y       = py;
            if(vec_push(&glyphs, &g, cap) < 0)
                break;
            /* The advance is already in twips at the rendered size - it is not
             * in em units and is not scaled by the text height. The font's own
             * advance table is the one that needs scaling, and this is not it. */
            px += adv;
        }
        /* The glyph entries are a continuous bit stream that stops on a byte
         * boundary before the next record's flags. */
        ps_bits_align(&b);
    }
    t->glyphs = glyphs.base;
    t->nglyph = glyphs.n;
    return 0;
}

/* DefineButton and DefineButton2: keep the up state, drop the rest.
 *
 * The record list has no count and is terminated by a zero flags byte, which
 * works because the two high bits of a real record are reserved and zero only
 * for the terminator. Records are drawn in depth order, so they are sorted
 * here rather than at every frame. */
static int parse_define_button(const uint8_t *body, size_t blen, int ver,
                               ps_swf_button *btn, uint32_t cap)
{
    ps_bits b;
    vec     recs = { NULL, 0, 0, sizeof(ps_swf_btnrec) };
    uint32_t bcap = cap / 6u ? cap / 6u : 1u;

    memset(btn, 0, sizeof *btn);
    ps_bits_init(&b, body, blen);
    btn->id = ps_bits_u16(&b);
    if(ver >= 2) {
        (void)ps_bits_u8(&b);                 /* TrackAsMenu */
        (void)ps_bits_u16(&b);                /* offset to the action list */
    }

    for(;;) {
        unsigned      flags;
        ps_swf_btnrec r;
        uint32_t      at, j;

        if(b.over)
            break;
        flags = ps_bits_u8(&b);
        if(flags == 0)
            break;

        memset(&r, 0, sizeof r);
        ps_swf_xform_identity(&r.mat);
        ps_swf_cxform_identity(&r.cx);
        r.id    = ps_bits_u16(&b);
        r.depth = ps_bits_u16(&b);
        read_matrix(&b, r.mat.m);
        if(ver >= 2)
            read_cxform(&b, 1, &r.cx);
        if(b.over)
            break;

        /* Bit 0 is the up state. Bit 3 is the hit area, which is never drawn
         * under any circumstances - it exists only to say where the pointer
         * counts as being over the button. */
        if(!(flags & 0x01))
            continue;

        for(at = 0; at < recs.n; at++)
            if(((ps_swf_btnrec *)recs.base)[at].depth > r.depth)
                break;
        if(vec_push(&recs, &r, bcap) < 0)
            break;
        for(j = recs.n - 1; j > at; j--) {
            ps_swf_btnrec *a = (ps_swf_btnrec *)recs.base;
            ps_swf_btnrec  tmp = a[j];
            a[j] = a[j - 1];
            a[j - 1] = tmp;
        }
    }
    btn->recs = recs.base;
    btn->nrec = recs.n;
    return 0;
}

/* --- file --------------------------------------------------------------- */

#define ERR(...) do { if(err && errlen) snprintf(err, errlen, __VA_ARGS__); } while(0)

/* A sprite inside a sprite is not legal SWF, so any nesting at all is already
 * a malformed file - but "malformed" here means "written by an attacker", and
 * unbounded recursion driven by file content is the one bug in a parser that
 * cannot be contained by bounds checks. Eight is far past anything a mistake
 * would produce and far short of a stack this can exhaust. */
#define MAX_SPRITE_DEPTH 8

typedef struct {
    const uint8_t *data;
    uint32_t       cap;
    ps_swf_movie  *m;
    vec            shapes;
    vec            chars;
    vec            sprites;
    vec            fonts;
    vec            texts;
    vec            buttons;
    vec            bitmaps;
    vec            morphs;
    vec            edits;
    /* DoInitAction only. Its sibling DoAction is a local of walk_tags instead,
     * because it belongs to the timeline being walked and this context is
     * shared with every nested sprite walk - see the note in ps_swf.h. */
    vec            inits;
    /* JPEGTables, held by reference into the caller's buffer rather than
     * copied. There is exactly one per file, it is only ever read while that
     * buffer is still alive, and copying it would be the only allocation in
     * the parser made for data that may never be used.
     *
     * Movie scope rather than timeline scope for the same reason `inits` is:
     * this context is shared with every nested sprite walk, and a DefineBits
     * inside a sprite is spliced against the file's one table just as a
     * sprite's initialiser belongs to the file rather than to where it was
     * written. */
    const uint8_t *jtab;
    size_t         jtablen;
    /* Set once the err channel has named a tag this build declines, so a file
     * made of forty of them says one thing rather than overwriting the same
     * sentence forty times. A declined tag also yields to any reason already
     * on the channel, and is overwritten by any that comes after: "we do not
     * read this tag" is the weakest thing that can be said about a file, and
     * anything else the walk found is worth more. */
    int            declined;
    char          *err;
    size_t         errlen;
} loadctx;

/* The tags this build recognises and does not read, by name.
 *
 * A list rather than a default case that prints a number, because the number
 * is not the answer: "tag 83" tells a caller nothing, and "DefineShape4" tells
 * them the file is SWF 8 artwork and that no retry will help. Everything not
 * here is a tag that carries nothing this player would draw or run - the sound
 * tags, which ps_swf_sound.h walks separately, and the metadata ones. */
static const char *declined_tag(uint32_t code)
{
    switch(code) {
    case 23: return "DefineButtonCxform";     /* SWF 2, and inside the target */
    case 70: return "PlaceObject3";           /* SWF 8: blend modes, filters */
    case 75: return "DefineFont3";
    case 83: return "DefineShape4";
    case 84: return "DefineMorphShape2";
    case 90: return "DefineBitsJPEG4";
    default: return NULL;
    }
}

static int add_char(loadctx *lc, uint16_t id, uint8_t kind, uint32_t index)
{
    ps_swf_char c;

    c.id    = id;
    c.kind  = kind;
    c.index = index;
    return vec_push(&lc->chars, &c, lc->cap);
}

const ps_swf_font *ps_swf_find_font(const ps_swf_movie *m, uint16_t id)
{
    uint32_t i;

    for(i = m->nfont; i-- > 0; )
        if(m->fonts[i].id == id)
            return &m->fonts[i];
    return NULL;
}

const ps_swf_bitmap *ps_swf_find_bitmap(const ps_swf_movie *m, uint16_t id)
{
    uint32_t i;

    for(i = m->nbitmap; i-- > 0; )
        if(m->bitmaps[i].id == id)
            return &m->bitmaps[i];
    return NULL;
}

const ps_swf_char *ps_swf_find_char(const ps_swf_movie *m, uint16_t id)
{
    uint32_t i;

    /* Last definition wins. A file may redefine an ID, and while that is
     * unusual it is legal, and the later tag is the one in force by the time
     * anything can refer to it. */
    for(i = m->nchar; i-- > 0; )
        if(m->chars[i].id == id)
            return &m->chars[i];
    return NULL;
}

/* Each block owns its bytes - see ps_swf_actions - so the array cannot simply
 * be released. Shared by the root, every sprite and the init list, which is
 * the whole reason it is a function. */
static void free_actions(ps_swf_actions *a, uint32_t n)
{
    uint32_t i;

    for(i = 0; i < n; i++)
        ps_free(a[i].code);
    ps_free(a);
}

/* The two arrays whose elements own a string, freed the same way and for the
 * same reason as the action blocks above: shared by the root and by every
 * sprite, so a loop written twice would be one place for a leak to hide. */
static void free_ops(ps_swf_op *ops, uint32_t n)
{
    uint32_t i;

    for(i = 0; i < n; i++)
        ps_free(ops[i].name);
    ps_free(ops);
}

static void free_labels(ps_swf_label *l, uint32_t n)
{
    uint32_t i;

    for(i = 0; i < n; i++)
        ps_free(l[i].name);
    ps_free(l);
}

/* Walks a tag stream into `tl`, stopping at an End tag or at `stop`.
 *
 * One function for the root and for every sprite, because a sprite's control
 * tags are the same tags in the same encoding - only the set that is legal
 * differs, and rejecting the illegal ones buys nothing when ignoring them
 * costs a default case. `stop` is what keeps a sprite from reading past its
 * own tag and into the movie behind it.
 *
 * Errors are recorded but do not unwind. A truncated file still has whole
 * frames at the front, and the tag that failed is where playback should stop,
 * not where the file should be thrown away. */
static void walk_tags(ps_bits *b, size_t stop, loadctx *lc,
                      ps_swf_timeline *tl, int depth)
{
    vec      ops    = { NULL, 0, 0, sizeof(ps_swf_op) };
    vec      frames = { NULL, 0, 0, sizeof(ps_swf_frame) };
    vec      acts   = { NULL, 0, 0, sizeof(ps_swf_actions) };
    vec      labels = { NULL, 0, 0, sizeof(ps_swf_label) };
    uint32_t frame_start = 0;
    char    *err    = lc->err;
    size_t   errlen = lc->errlen;

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
        if(tlen > stop - b->pos) {
            /* uint32_t is int on the host and long on the SH-4, so the width
             * the format names has to be reached explicitly. */
            ERR("tag %u claims %u bytes, %zu left", (unsigned)code,
                (unsigned)tlen, stop - b->pos);
            break;
        }
        body = b->pos;

        switch(code) {
        case 1: {                               /* ShowFrame */
            ps_swf_frame f;

            f.first_op = frame_start;
            f.nop      = ops.n - frame_start;
            if(vec_push(&frames, &f, lc->cap) < 0) {
                ERR("out of memory holding frames");
                goto done;
            }
            frame_start = ops.n;
            break;
        }
        case 12: case 59: {                     /* DoAction / DoInitAction */
            ps_swf_actions a;
            size_t         off = body;
            uint32_t       n   = tlen;
            vec           *into;

            memset(&a, 0, sizeof a);
            if(code == 12) {
                /* The frame a script runs with is the one it precedes, so it
                 * is the number of ShowFrame tags seen so far in *this*
                 * timeline. Reading it off frames.n rather than off a counter
                 * of its own is what keeps a sprite's scripts numbered against
                 * the sprite's frames. */
                a.frame = (int32_t)frames.n;
                into    = &acts;
            } else {
                if(tlen < 2)
                    break;                      /* no sprite to initialise */
                a.sprite = (uint16_t)(lc->data[body] | (lc->data[body + 1] << 8));
                a.frame  = -1;
                off      = body + 2;
                n        = tlen - 2;
                into     = &lc->inits;
            }
            if(n == 0)
                break;
            /* n is bounded by the bytes actually remaining in the file - the
             * tag length was checked against `stop` above - so this is not an
             * allocation sized by a file field in the sense the header of this
             * file warns about. The vector holding the blocks is capped per
             * input byte like every other. */
            a.len  = n;
            a.code = ps_alloc(n);
            if(!a.code) {
                ERR("out of memory holding an action block");
                goto done;
            }
            memcpy(a.code, lc->data + off, n);
            if(vec_push(into, &a, lc->cap) < 0) {
                ps_free(a.code);
                ERR("too many action blocks");
                goto done;
            }
            break;
        }
        case 43: {                              /* FrameLabel */
            ps_swf_label lab;
            ps_bits      sb;

            /* The label names the frame it precedes, so it is the number of
             * ShowFrame tags seen so far in this timeline - the same rule
             * DoAction is numbered by, and for the same reason.
             *
             * A reader of its own over the tag body: SWF 6 puts a NamedAnchor
             * byte behind the string, and reading through `b` would leave the
             * cursor a byte short of where the jump below puts it anyway. */
            ps_bits_init(&sb, lc->data, body + tlen);
            sb.pos     = body;
            lab.frame  = frames.n;
            lab.name   = read_string(&sb, lc->cap);
            if(!lab.name)
                break;                          /* empty, or would not fit */
            if(vec_push(&labels, &lab, lc->cap) < 0) {
                ps_free(lab.name);
                ERR("out of memory holding frame labels");
                goto done;
            }
            break;
        }
        case 9:                                 /* SetBackgroundColor */
            if(tlen >= 3) {
                lc->m->bg.r = lc->data[body];
                lc->m->bg.g = lc->data[body + 1];
                lc->m->bg.b = lc->data[body + 2];
                lc->m->bg.a = 255;
            }
            break;
        case 2: case 22: case 32: {              /* DefineShape 1 / 2 / 3 */
            ps_swf_shape sh;
            int ver = (code == 2) ? 1 : (code == 22) ? 2 : 3;

            if(parse_define_shape(lc->data + body, tlen, ver, &sh,
                                  lc->cap) < 0) {
                ERR("malformed DefineShape%d at offset %zu", ver, body);
                /* Keep what is already read: a truncated intro still has
                 * usable art in front of the damage. */
                goto done;
            }
            if(vec_push(&lc->shapes, &sh, lc->cap) < 0 ||
               add_char(lc, sh.id, PS_SWF_CHAR_SHAPE, lc->shapes.n - 1) < 0) {
                ps_free(sh.fills);
                ps_free(sh.lines);
                ps_free(sh.grads);
                ps_free(sh.bfills);
                ps_free(sh.edges);
                ERR("out of memory holding shapes");
                goto done;
            }
            break;
        }
        case 4: case 26: {                       /* PlaceObject / 2 */
            ps_swf_op op;

            memset(&op, 0, sizeof op);
            op.op = PS_SWF_OP_PLACE;
            ps_swf_xform_identity(&op.mat);
            ps_swf_cxform_identity(&op.cx);

            if(code == 4) {
                /* PlaceObject always names a character and always carries a
                 * matrix; its colour transform is optional in the only way
                 * SWF 1 had of saying so, which is that the tag simply ends.
                 * That is why this one needs the tag length and PlaceObject2
                 * does not. */
                op.id    = ps_bits_u16(b);
                op.depth = ps_bits_u16(b);
                read_matrix(b, op.mat.m);
                op.has_matrix = 1;
                if(b->pos < body + tlen) {
                    read_cxform(b, 0, &op.cx);
                    op.has_cxform = 1;
                }
            } else {
                /* The flag byte reads MSB first, so bit 0 is Move and bit 7 is
                 * HasClipActions - the reverse of the order the spec lists
                 * them in. The optional fields then follow in the order the
                 * flags are listed, not the order of the bits. */
                unsigned flags = ps_bits_u8(b);

                op.move  = (flags & 0x01) != 0;
                op.depth = ps_bits_u16(b);
                if(flags & 0x02)
                    op.id = ps_bits_u16(b);
                if(flags & 0x04) {
                    read_matrix(b, op.mat.m);
                    op.has_matrix = 1;
                }
                if(flags & 0x08) {
                    read_cxform(b, 1, &op.cx);
                    op.has_cxform = 1;
                }
                if(flags & 0x10)
                    op.ratio = ps_bits_u16(b);
                if(flags & 0x20)
                    op.name = read_string(b, lc->cap);
                if(flags & 0x40)
                    op.clip_depth = ps_bits_u16(b);
                /* Clip actions are ActionScript attached to a clip and are
                 * stepped over by the jump to the end of the tag below. */
            }
            /* The op owns its name from here, so both ways out of this arm
             * have to release it - a truncated tag is dropped and a full ops
             * vector is fatal, and neither reaches the free that walks the
             * pushed ops. */
            if(b->over) {
                ps_free(op.name);
                break;
            }
            if(vec_push(&ops, &op, lc->cap) < 0) {
                ps_free(op.name);
                ERR("out of memory holding display list ops");
                goto done;
            }
            break;
        }
        case 5: case 28: {                       /* RemoveObject / 2 */
            ps_swf_op op;

            memset(&op, 0, sizeof op);
            op.op = PS_SWF_OP_REMOVE;
            if(code == 5) {
                op.id    = ps_bits_u16(b);
                op.depth = ps_bits_u16(b);
            } else {
                op.depth = ps_bits_u16(b);
            }
            if(b->over)
                break;
            if(vec_push(&ops, &op, lc->cap) < 0) {
                ERR("out of memory holding display list ops");
                goto done;
            }
            break;
        }
        case 39: {                               /* DefineSprite */
            ps_swf_timeline sp;
            ps_bits         nb;
            uint16_t        sid;

            memset(&sp, 0, sizeof sp);
            /* A reader limited to this tag's own bytes, so a sprite whose
             * nested stream is missing its End tag stops at the end of the
             * sprite rather than eating the rest of the movie. */
            ps_bits_init(&nb, lc->data, body + tlen);
            nb.pos = body;
            sid = ps_bits_u16(&nb);
            (void)ps_bits_u16(&nb);   /* declared frame count */
            sp.id = sid;

            if(depth < MAX_SPRITE_DEPTH)
                walk_tags(&nb, body + tlen, lc, &sp, depth + 1);

            /* Split for the reason the bitmap arm below states: once the push
             * succeeds the vector owns everything hanging off `sp`, and
             * releasing it after a failed add_char would leave ps_swf_free to
             * release it a second time. */
            if(vec_push(&lc->sprites, &sp, lc->cap) < 0) {
                free_ops(sp.ops, sp.nop);
                ps_free(sp.frames);
                free_actions(sp.acts, sp.nact);
                free_labels(sp.labels, sp.nlabel);
                ERR("out of memory holding sprites");
                goto done;
            }
            if(add_char(lc, sid, PS_SWF_CHAR_SPRITE, lc->sprites.n - 1) < 0) {
                ERR("out of memory holding characters");
                goto done;
            }
            break;
        }
        case 10: case 48: {                      /* DefineFont / DefineFont2 */
            ps_swf_font fnt;
            int         ok;

            ok = (code == 10)
                 ? parse_define_font(lc->data + body, tlen, &fnt, lc->cap)
                 : parse_define_font2(lc->data + body, tlen, &fnt, lc->cap);
            if(ok < 0) {
                /* A bad font loses its text and not the rest of the movie -
                 * but it is said out loud, because the symptom is a frame with
                 * the artwork and no words on it, which reads as a layout
                 * fault rather than as a refused tag. */
                ERR("malformed DefineFont%d at offset %zu",
                    code == 10 ? 1 : 2, body);
                break;
            }
            if(vec_push(&lc->fonts, &fnt, lc->cap) < 0) {
                free_font(&fnt);
                ERR("out of memory holding fonts");
                goto done;
            }
            break;
        }
        case 11: case 33: {                      /* DefineText / DefineText2 */
            ps_swf_text t;

            if(parse_define_text(lc->data + body, tlen,
                                 code == 11 ? 1 : 2, &t, lc->cap) < 0) {
                ERR("malformed DefineText%d at offset %zu",
                    code == 11 ? 1 : 2, body);
                break;
            }
            if(vec_push(&lc->texts, &t, lc->cap) < 0 ||
               add_char(lc, t.id, PS_SWF_CHAR_TEXT, lc->texts.n - 1) < 0) {
                ps_free(t.glyphs);
                ERR("out of memory holding text");
                goto done;
            }
            break;
        }
        case 8:                                  /* JPEGTables */
            /* One per file and it must precede the DefineBits tags that need
             * it. A second one replaces the first, which is what Flash does
             * and is the only reading that cannot lose a picture. */
            if(tlen && tlen <= PS_SWF_JPEGTABLES_MAX) {
                lc->jtab    = lc->data + body;
                lc->jtablen = tlen;
            }
            break;
        case 6: case 20: case 21: case 35: case 36: {
            /* The five tags that carry pixels. A bitmap that will not decode
             * costs its own picture and nothing else - the fill styles that
             * name it fall back to grey and the rest of the movie is
             * unaffected - so the reason is recorded and the walk continues,
             * where a malformed shape stops it. The difference is that a shape
             * cannot be skipped: its bytes are the only way to find the next
             * tag's, and a bitmap's are not. */
            ps_swf_bitmap  bm;
            ps_swf_img_err ie;
            int            ver;

            if(code == 20 || code == 36) {
                ver = (code == 20) ? 1 : 2;
                ie  = ps_swf_bitmap_lossless(lc->data + body, tlen, ver, &bm);
            } else {
                ver = (code == 6) ? 1 : (code == 21) ? 2 : 3;
                ie  = ps_swf_bitmap_jpeg(lc->data + body, tlen, ver,
                                         lc->jtab, lc->jtablen, &bm);
            }
            if(ie != PS_SWF_IMG_OK) {
                ERR("tag %u at offset %zu: %s", (unsigned)code, body,
                    ps_swf_img_reason(ie));
                break;
            }
            /* Split rather than folded into one condition, because the vector
             * takes ownership of the pixels the moment the push succeeds:
             * freeing them after a failed add_char would leave the vector
             * holding a pointer ps_swf_free goes on to release again. */
            if(vec_push(&lc->bitmaps, &bm, lc->cap) < 0) {
                ps_swf_bitmap_free(&bm);
                ERR("out of memory holding bitmaps");
                goto done;
            }
            if(add_char(lc, bm.id, PS_SWF_CHAR_BITMAP,
                        lc->bitmaps.n - 1) < 0) {
                ERR("out of memory holding characters");
                goto done;
            }
            break;
        }
        case 13:                                 /* DefineFontInfo */
            /* Codes for a font that has none. Failure loses the mapping and so
             * loses any edit text using that font, which is the same outcome
             * as the tag not being there - no reason to stop the movie. */
            (void)ps_swf_font_info(lc->data + body, tlen,
                                   (ps_swf_font *)lc->fonts.base, lc->fonts.n);
            break;
        case 46: {                               /* DefineMorphShape */
            ps_swf_morph mo;

            /* A morph whose two halves disagree is refused, and refusing is
             * confined to the character: the tag length still says where the
             * next tag starts, so the walk resynchronises and the rest of the
             * movie plays without it. */
            if(ps_swf_morph_parse(lc->data + body, tlen, &mo, lc->cap) < 0) {
                ERR("malformed DefineMorphShape at offset %zu", body);
                break;
            }
            if(vec_push(&lc->morphs, &mo, lc->cap) < 0 ||
               add_char(lc, mo.id, PS_SWF_CHAR_MORPH, lc->morphs.n - 1) < 0) {
                ps_swf_morph_free(&mo);
                ERR("out of memory holding morph shapes");
                goto done;
            }
            break;
        }
        case 37: {                               /* DefineEditText */
            ps_swf_edittext et;

            /* Laid out against the fonts defined so far, which is every font a
             * well-formed file can have referred to: a character has to exist
             * before anything can name it. */
            if(ps_swf_edit_parse(lc->data + body, tlen,
                                 (const ps_swf_font *)lc->fonts.base,
                                 lc->fonts.n, &et, lc->cap) < 0) {
                ERR("malformed DefineEditText at offset %zu", body);
                break;
            }
            if(vec_push(&lc->edits, &et, lc->cap) < 0 ||
               add_char(lc, et.id, PS_SWF_CHAR_EDIT, lc->edits.n - 1) < 0) {
                ps_swf_edit_free(&et);
                ERR("out of memory holding edit text");
                goto done;
            }
            break;
        }
        case 7: case 34: {                       /* DefineButton / 2 */
            ps_swf_button btn;

            if(parse_define_button(lc->data + body, tlen,
                                   code == 7 ? 1 : 2, &btn, lc->cap) < 0) {
                ERR("malformed DefineButton%d at offset %zu",
                    code == 7 ? 1 : 2, body);
                break;
            }
            if(vec_push(&lc->buttons, &btn, lc->cap) < 0 ||
               add_char(lc, btn.id, PS_SWF_CHAR_BUTTON, lc->buttons.n - 1) < 0) {
                ps_free(btn.recs);
                ERR("out of memory holding buttons");
                goto done;
            }
            break;
        }
        default:
            /* A tag this build knows by name and declines. Saying so once is
             * the whole of the fix for a class of failure that has no other
             * symptom: five files in the corpus are a DefineShape4 and a
             * placement, and without this they render as an empty stage with
             * a clean exit status - indistinguishable from a blank movie, a
             * fetch that returned nothing, or a fault in the renderer.
             *
             * Once, and the first one, because the interesting fact is which
             * tag rather than how many: a file carrying one DefineShape4
             * carries nothing but. */
            if(!lc->declined && err && errlen && !err[0]) {
                const char *nm = declined_tag(code);

                if(nm) {
                    lc->declined = 1;
                    ERR("%s (tag %u) is not read by this build", nm,
                        (unsigned)code);
                }
            }
            break;
        }

        b->pos = body + tlen;
        b->bit = 0;
    }

done:
    /* Ops with no ShowFrame behind them are content the file never asked to
     * be shown. They are kept as a final frame anyway, because the usual
     * reason for them is truncation, and showing the last partial frame of a
     * cut-short intro is better than showing nothing. */
    if(ops.n > frame_start) {
        ps_swf_frame f;

        f.first_op = frame_start;
        f.nop      = ops.n - frame_start;
        (void)vec_push(&frames, &f, lc->cap);
    }
    tl->ops    = ops.base;
    tl->nop    = ops.n;
    tl->frames = frames.base;
    tl->nframe = frames.n;
    tl->acts   = acts.base;
    tl->nact   = acts.n;
    tl->labels = labels.base;
    tl->nlabel = labels.n;
}

int ps_swf_find_label(const ps_swf_timeline *tl, const char *label)
{
    uint32_t i;

    if(!label)
        return -1;
    /* First wins, unlike ps_swf_find_char's last-wins rule for character IDs.
     * A redefined ID is the later tag replacing the earlier one; a repeated
     * label is two frames both claiming the name, and Flash goes to the first
     * of them. */
    for(i = 0; i < tl->nlabel; i++)
        if(strcmp(tl->labels[i].name, label) == 0)
            return (int)tl->labels[i].frame;
    return -1;
}

int ps_swf_load(const uint8_t *data, size_t len, ps_swf_movie *m,
                char *err, size_t errlen)
{
    ps_bits  b;
    loadctx  lc;
    uint32_t declared;
    uint32_t cap, si;

    memset(m, 0, sizeof *m);
    m->bg.r = m->bg.g = m->bg.b = 255;
    m->bg.a = 255;

    if(len < 9 || (memcmp(data, "FWS", 3) && memcmp(data, "CWS", 3))) {
        ERR("not a SWF");
        return -1;
    }

    m->version    = data[3];
    m->compressed = (data[0] == 'C');
    declared      = (uint32_t)data[4] | ((uint32_t)data[5] << 8) |
                    ((uint32_t)data[6] << 16) | ((uint32_t)data[7] << 24);
    (void)declared;

    if(m->compressed) {
        /* An inflater is now linked here - stb_image's, which is what the
         * bitmap tags use and what this comment used to misname as miniz;
         * miniz is in the licensing table but is not vendored in this tree.
         * So this is a wiring job rather than a missing capability, and the
         * reason it is still not wired is the bound. A bitmap's decompressed
         * size is computable from its own dimensions before the inflate runs,
         * which is what makes ps_swf_image.c safe; a whole compressed file's
         * is not computable from anything, so the same technique does not
         * carry over and the policy is still open. Not urgent at the stated
         * target either way: compression arrived in SWF 6, so no SWF 4 file
         * is compressed. */
        ERR("zlib-compressed SWF (CWS); inflate not wired up");
        return -1;
    }

    /* Every array below is capped at one element per byte of input. The
     * tightest encoding of an edge record is ten bits, of a fill style is
     * thirty-two, and of a display list op is thirty-two, so this can never
     * reject a legitimate file, and it makes every allocation a function of
     * the bytes actually received. */
    cap = (uint32_t)(len > 0x40000000u ? 0x40000000u : len);
    if(cap < 16)
        cap = 16;

    ps_bits_init(&b, data, len);
    ps_bits_skip(&b, 8);
    read_rect(&b, &m->xmin, &m->xmax, &m->ymin, &m->ymax);
    {
        /* FIXED8, little endian: the fractional byte comes first. */
        uint8_t frac = ps_bits_u8(&b);
        uint8_t whole = ps_bits_u8(&b);
        m->fps = (float)whole + (float)frac / 256.0f;
    }
    m->frames = (int)ps_bits_u16(&b);

    if(b.over) {
        ERR("truncated header");
        return -1;
    }

    memset(&lc, 0, sizeof lc);
    lc.data         = data;
    lc.cap          = cap;
    lc.m            = m;
    lc.err          = err;
    lc.errlen       = errlen;
    lc.shapes.esz   = sizeof(ps_swf_shape);
    lc.chars.esz    = sizeof(ps_swf_char);
    lc.sprites.esz  = sizeof(ps_swf_timeline);
    lc.fonts.esz    = sizeof(ps_swf_font);
    lc.texts.esz    = sizeof(ps_swf_text);
    lc.buttons.esz  = sizeof(ps_swf_button);
    lc.bitmaps.esz  = sizeof(ps_swf_bitmap);
    lc.morphs.esz   = sizeof(ps_swf_morph);
    lc.edits.esz    = sizeof(ps_swf_edittext);
    lc.inits.esz    = sizeof(ps_swf_actions);

    walk_tags(&b, len, &lc, &m->root, 0);

    m->shapes  = lc.shapes.base;   m->nshape  = lc.shapes.n;
    m->chars   = lc.chars.base;    m->nchar   = lc.chars.n;
    m->sprites = lc.sprites.base;  m->nsprite = lc.sprites.n;
    m->fonts   = lc.fonts.base;    m->nfont   = lc.fonts.n;
    m->texts   = lc.texts.base;    m->ntext   = lc.texts.n;
    m->buttons = lc.buttons.base;  m->nbutton = lc.buttons.n;
    m->bitmaps = lc.bitmaps.base;  m->nbitmap = lc.bitmaps.n;
    m->morphs  = lc.morphs.base;   m->nmorph  = lc.morphs.n;
    m->edits   = lc.edits.base;    m->nedit   = lc.edits.n;
    m->inits   = lc.inits.base;    m->ninit   = lc.inits.n;

    /* Bitmap fill styles name a character by ID and are resolved to a pointer
     * here, once, rather than at every pass over every shape. It has to be
     * after the walk for two reasons and either one alone would be enough: a
     * DefineBitsLossless is allowed to follow the shape that uses it, and the
     * bitmap array is grown by doubling, so any pointer into it taken during
     * the walk is one reallocation away from being wrong. */
    for(si = 0; si < m->nshape; si++) {
        ps_swf_shape *sh = &m->shapes[si];
        uint32_t      k;

        for(k = 0; k < sh->nfill; k++) {
            ps_swf_fill *f = &sh->fills[k];

            if(f->bfill && f->bfill <= sh->nbfill)
                sh->bfills[f->bfill - 1].bmp =
                    ps_swf_find_bitmap(m, f->bitmap_id);
        }
    }
    return 0;
}

void ps_swf_free(ps_swf_movie *m)
{
    uint32_t i;

    for(i = 0; i < m->nshape; i++) {
        ps_free(m->shapes[i].fills);
        ps_free(m->shapes[i].lines);
        ps_free(m->shapes[i].grads);
        ps_free(m->shapes[i].bfills);
        ps_free(m->shapes[i].edges);
    }
    for(i = 0; i < m->nbitmap; i++)
        ps_swf_bitmap_free(&m->bitmaps[i]);
    for(i = 0; i < m->nsprite; i++) {
        free_ops(m->sprites[i].ops, m->sprites[i].nop);
        ps_free(m->sprites[i].frames);
        free_actions(m->sprites[i].acts, m->sprites[i].nact);
        free_labels(m->sprites[i].labels, m->sprites[i].nlabel);
    }
    for(i = 0; i < m->nfont; i++)
        free_font(&m->fonts[i]);
    for(i = 0; i < m->ntext; i++)
        ps_free(m->texts[i].glyphs);
    for(i = 0; i < m->nbutton; i++)
        ps_free(m->buttons[i].recs);
    for(i = 0; i < m->nmorph; i++)
        ps_swf_morph_free(&m->morphs[i]);
    for(i = 0; i < m->nedit; i++)
        ps_swf_edit_free(&m->edits[i]);
    free_ops(m->root.ops, m->root.nop);
    ps_free(m->root.frames);
    free_actions(m->root.acts, m->root.nact);
    free_labels(m->root.labels, m->root.nlabel);
    free_actions(m->inits, m->ninit);
    ps_free(m->shapes);
    ps_free(m->chars);
    ps_free(m->sprites);
    ps_free(m->fonts);
    ps_free(m->texts);
    ps_free(m->buttons);
    ps_free(m->bitmaps);
    ps_free(m->morphs);
    ps_free(m->edits);
    memset(m, 0, sizeof *m);
}
