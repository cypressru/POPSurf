/* DefineMorphShape: parsing a pair of shapes, and blending them.
 *
 * The parse is two cursors over one tag. The header states an offset to where
 * the end shape's edges begin, so the styles - which carry both ends in one
 * record - are read once with the first cursor, and the two edge streams are
 * then read in lockstep with a cursor each. Reading them one after the other
 * instead would mean holding the whole start shape to compare against, and
 * would lose the property that matters most: a disagreement is detected at
 * the record that disagrees, before anything has been built on top of it.
 *
 * Two things about the encoding are not what the static shape parser taught.
 * MORPHGRADIENT counts its stops in a whole byte where GRADIENT uses four bits
 * behind two mode fields, so a reader that shares the gradient code with
 * DefineShape mis-steps the first stop and every colour after it. And morph
 * colours are always RGBA, with no version to consult - there is no
 * DefineMorphShape without alpha.
 */
#include "ps_swf_morph.h"

#include "ps_swf_bits.h"
#include "ps_swf_mem.h"
#include "ps_swf_read.h"

#include <string.h>

/* A blend multiplies a signed 32-bit twip delta by the 16-bit Ratio before
 * dividing it back down, so the intermediate needs forty-eight bits. No
 * 32-bit type has them, which is why every blend below is written in int64_t
 * rather than left to the coordinate type it starts and ends in. */
static_assert((int64_t)INT32_MAX * 65535 > INT32_MAX,
              "the ratio blend overflows 32 bits and must widen");

/* MORPHGRADIENT states one to eight stops, in a field that could say 255. The
 * storage limit is the static gradient's, and it has to be at least the
 * format's stated maximum or a conforming file loses colour. */
static_assert(PS_SWF_MAX_STOPS >= 8,
              "a MORPHGRADIENT may declare eight stops");

/* --- style arrays -------------------------------------------------------- */

/* Both ends of one MORPHGRADIENT. Every record carries a ratio and a colour
 * for each end, so the two ramps come out of one pass and are the same length
 * by construction - which is what lets ps_swf_morph_at blend stop against
 * matching stop without searching. */
static void morph_gradient(ps_bits *b, ps_swf_gradient *ga, ps_swf_gradient *gb,
                           ps_swf_rgba *mean_a, ps_swf_rgba *mean_b)
{
    unsigned n = ps_bits_u8(b), i;
    unsigned sa[4] = { 0, 0, 0, 0 }, sb[4] = { 0, 0, 0, 0 };

    ga->nstop = gb->nstop = 0;
    for(i = 0; i < n && !b->over; i++) {
        uint8_t     r0 = ps_bits_u8(b);
        ps_swf_rgba c0 = read_color(b, 1);
        uint8_t     r1 = ps_bits_u8(b);
        ps_swf_rgba c1 = read_color(b, 1);

        /* Read past the storage limit as well: reading is what positions the
         * stream for the next fill style, storing is only what lets this one
         * be shaded. */
        if(ga->nstop < PS_SWF_MAX_STOPS) {
            ga->ratio[ga->nstop] = r0;
            ga->color[ga->nstop] = c0;
            gb->ratio[gb->nstop] = r1;
            gb->color[gb->nstop] = c1;
            ga->nstop++;
            gb->nstop++;
        }
        sa[0] += c0.r; sa[1] += c0.g; sa[2] += c0.b; sa[3] += c0.a;
        sb[0] += c1.r; sb[1] += c1.g; sb[2] += c1.b; sb[3] += c1.a;
    }

    if(n == 0 || b->over) {
        mean_a->r = mean_a->g = mean_a->b = 0;
        mean_a->a = 255;
        *mean_b = *mean_a;
        return;
    }
    mean_a->r = (uint8_t)(sa[0] / n); mean_a->g = (uint8_t)(sa[1] / n);
    mean_a->b = (uint8_t)(sa[2] / n); mean_a->a = (uint8_t)(sa[3] / n);
    mean_b->r = (uint8_t)(sb[0] / n); mean_b->g = (uint8_t)(sb[1] / n);
    mean_b->b = (uint8_t)(sb[2] / n); mean_b->a = (uint8_t)(sb[3] / n);
}

static int parse_morph_fills(ps_bits *b, vec *fa, vec *fb, vec *ga, vec *gb,
                             uint32_t cap)
{
    uint32_t count = ps_bits_u8(b);
    uint32_t i;
    /* A morph gradient fill cannot be encoded in fewer than fourteen bytes:
     * a type, two byte-aligned MATRIXes, a stop count, and one stop that
     * carries a ratio and an RGBA at each end. Same argument as everywhere
     * else - a file of L bytes cannot honestly describe more than L/14. */
    uint32_t gcap = cap / 14u ? cap / 14u : 1u;

    if(count == 0xff)
        count = ps_bits_u16(b);
    if(cap > MAX_STYLES)  cap  = MAX_STYLES;
    if(gcap > MAX_STYLES) gcap = MAX_STYLES;

    for(i = 0; i < count; i++) {
        ps_swf_fill a, c;

        if(b->over)
            return -1;
        memset(&a, 0, sizeof a);
        memset(&c, 0, sizeof c);
        a.type = c.type = ps_bits_u8(b);

        switch(a.type) {
        case PS_SWF_FILL_SOLID:
            a.color = read_color(b, 1);
            c.color = read_color(b, 1);
            break;
        case PS_SWF_FILL_LINEAR:
        case PS_SWF_FILL_RADIAL: {
            ps_swf_gradient g0, g1;

            memset(&g0, 0, sizeof g0);
            memset(&g1, 0, sizeof g1);
            read_matrix(b, g0.mat);
            read_matrix(b, g1.mat);
            morph_gradient(b, &g0, &g1, &a.color, &c.color);
            if(ga->n >= MAX_STYLES || vec_push(ga, &g0, gcap) < 0 ||
               vec_push(gb, &g1, gcap) < 0)
                return -1;
            a.grad = c.grad = (uint16_t)ga->n;      /* 1-based */
            break;
        }
        case 0x40: case 0x41: case 0x42: case 0x43: {
            float m[6];

            /* Two matrices here rather than the static shape's one, and both
             * are dropped - which is a gap now rather than an absence. A
             * static shape's bitmap fill draws its texture; a morph's draws
             * the grey below, because interpolating one means holding a
             * ps_swf_bitmapfill for each end, blending the six matrix terms,
             * and resolving the character ID after the walk the way
             * ps_swf_load does for shapes. None of that is hard and none of it
             * is written, so this says so rather than looking finished.
             *
             * Stepping over both exactly still matters either way - the next
             * fill style is read from where these leave the cursor, so a morph
             * that merely contains a bitmap fill would otherwise decode every
             * style after it out of the wrong bytes. That part is load-bearing
             * and is tested by nothing, since no generated file has one. */
            a.bitmap_id = c.bitmap_id = ps_bits_u16(b);
            read_matrix(b, m);
            read_matrix(b, m);
            a.color.r = a.color.g = a.color.b = 128;
            a.color.a = 255;
            c.color = a.color;
            break;
        }
        default:
            /* 0x13, the focal radial, belongs to DefineMorphShape2 and SWF 8.
             * Anything unknown means the rest of the array cannot be located,
             * so there is nothing to do but stop. */
            return -1;
        }
        if(vec_push(fa, &a, cap) < 0 || vec_push(fb, &c, cap) < 0)
            return -1;
    }
    return b->over ? -1 : 0;
}

/* MORPHLINESTYLE is two widths and two colours, and nothing else. Caps, joins
 * and the rest arrive with MORPHLINESTYLE2 in DefineMorphShape2, which is SWF
 * 8 - so a morph stroke here is round-capped and round-joined like every other
 * stroke the player draws. */
static int parse_morph_lines(ps_bits *b, vec *la, vec *lb, uint32_t cap)
{
    uint32_t count = ps_bits_u8(b);
    uint32_t i;

    if(count == 0xff)
        count = ps_bits_u16(b);
    if(cap > MAX_STYLES)
        cap = MAX_STYLES;

    for(i = 0; i < count; i++) {
        ps_swf_line a, c;

        if(b->over)
            return -1;
        a.width = ps_bits_u16(b);
        c.width = ps_bits_u16(b);
        a.color = read_color(b, 1);
        c.color = read_color(b, 1);
        if(vec_push(la, &a, cap) < 0 || vec_push(lb, &c, cap) < 0)
            return -1;
    }
    return b->over ? -1 : 0;
}

/* --- the paired edge streams --------------------------------------------- */

/* One edge record's deltas, resolved against the pen.
 *
 * A straight edge also yields a control point, at the midpoint, because the
 * two streams are free to disagree about whether a given edge is a line or a
 * curve - an exporter that turns a straight side into a bulge writes exactly
 * that - and a quadratic whose control point is the midpoint of its chord is
 * the chord. Promoting the straight one therefore changes nothing about the
 * shape it came from and makes the pair blendable. */
static void read_morph_edge(ps_bits *b, int straight, int nb,
                            int32_t x, int32_t y,
                            int32_t *cx, int32_t *cy, int32_t *nx, int32_t *ny)
{
    if(straight) {
        int32_t dx = 0, dy = 0;

        if(ps_bits_ub(b, 1)) {                /* GeneralLineFlag */
            dx = ps_bits_sb(b, nb);
            dy = ps_bits_sb(b, nb);
        } else if(ps_bits_ub(b, 1)) {         /* VertLineFlag */
            dy = ps_bits_sb(b, nb);
        } else {
            dx = ps_bits_sb(b, nb);
        }
        *nx = x + dx;
        *ny = y + dy;
        *cx = x + dx / 2;
        *cy = y + dy / 2;
    } else {
        int32_t c0 = ps_bits_sb(b, nb);
        int32_t c1 = ps_bits_sb(b, nb);
        int32_t a0, a1;

        *cx = x + c0;
        *cy = y + c1;
        a0  = ps_bits_sb(b, nb);
        a1  = ps_bits_sb(b, nb);
        *nx = *cx + a0;
        *ny = *cy + a1;
    }
}

/* Walks both shapes at once, one record at a time, and refuses the moment
 * they stop describing the same sequence.
 *
 * What has to correspond is the record structure: an edge in one stream must
 * face an edge in the other, a style change must face a style change, and both
 * must end together. What does not have to correspond is the detail inside a
 * style change - the spec says the end shape carries only a MoveTo, but
 * exporters restate the fill and line selectors it is not allowed to alter, so
 * those are read with the end stream's own bit widths and dropped. That is
 * redundancy rather than disagreement, and refusing it would reject files
 * Flash plays. */
static int parse_morph_edges(ps_bits *sb, ps_bits *eb, vec *edges, vec *bpts,
                             uint32_t nfill, uint32_t nline, uint32_t cap)
{
    uint32_t fill0 = 0, fill1 = 0, line = 0;
    int32_t  ax = 0, ay = 0, bx = 0, by = 0;
    int      sfbits, slbits, efbits, elbits;

    sfbits = (int)ps_bits_ub(sb, 4);
    slbits = (int)ps_bits_ub(sb, 4);
    efbits = (int)ps_bits_ub(eb, 4);
    elbits = (int)ps_bits_ub(eb, 4);

    for(;;) {
        uint32_t sedge, eedge;

        if(sb->over || eb->over)
            return -1;
        sedge = ps_bits_ub(sb, 1);
        eedge = ps_bits_ub(eb, 1);
        if(sedge != eedge)
            return -1;

        if(!sedge) {
            uint32_t sf = ps_bits_ub(sb, 5);
            uint32_t ef = ps_bits_ub(eb, 5);

            if((sf == 0) != (ef == 0))
                return -1;
            if(sf == 0)
                return 0;                    /* both ended, in step */

            /* StateNewStyles restarts the style table part way through a
             * shape. A morph declares one table for both ends before either
             * geometry, so a restart has no end counterpart and no meaning. */
            if((sf | ef) & 0x10)
                return -1;

            if(sf & 0x01) {
                int n = (int)ps_bits_ub(sb, 5);
                ax = ps_bits_sb(sb, n);
                ay = ps_bits_sb(sb, n);
            }
            if(sf & 0x02) fill0 = ps_bits_ub(sb, sfbits);
            if(sf & 0x04) fill1 = ps_bits_ub(sb, sfbits);
            if(sf & 0x08) line  = ps_bits_ub(sb, slbits);

            if(ef & 0x01) {
                int n = (int)ps_bits_ub(eb, 5);
                bx = ps_bits_sb(eb, n);
                by = ps_bits_sb(eb, n);
            }
            if(ef & 0x02) (void)ps_bits_ub(eb, efbits);
            if(ef & 0x04) (void)ps_bits_ub(eb, efbits);
            if(ef & 0x08) (void)ps_bits_ub(eb, elbits);
        } else {
            ps_swf_edge   e;
            ps_swf_mpoint p;
            int sstr = (int)ps_bits_ub(sb, 1);
            int estr = (int)ps_bits_ub(eb, 1);
            int snb  = (int)ps_bits_ub(sb, 4) + 2;
            int enb  = (int)ps_bits_ub(eb, 4) + 2;

            memset(&e, 0, sizeof e);
            e.x0 = ax; e.y0 = ay;
            p.x0 = bx; p.y0 = by;
            read_morph_edge(sb, sstr, snb, ax, ay, &e.cx, &e.cy, &e.x1, &e.y1);
            read_morph_edge(eb, estr, enb, bx, by, &p.cx, &p.cy, &p.x1, &p.y1);

            e.curve = (uint8_t)(!sstr || !estr);
            e.fill0 = style_index(0, fill0, nfill);
            e.fill1 = style_index(0, fill1, nfill);
            e.line  = style_index(0, line,  nline);
            e.layer = 0;

            /* Dropped in both arrays or neither: an edge with nothing on
             * either side and no stroke can never contribute a pixel at any
             * ratio, since the styles are the same at both ends. */
            if((e.fill0 || e.fill1 || e.line) &&
               (vec_push(edges, &e, cap) < 0 || vec_push(bpts, &p, cap) < 0))
                return -1;

            ax = e.x1; ay = e.y1;
            bx = p.x1; by = p.y1;
        }
    }
}

/* --- the tag ------------------------------------------------------------- */

int ps_swf_morph_parse(const uint8_t *body, size_t blen, ps_swf_morph *mo,
                       uint32_t cap)
{
    ps_bits sb, eb;
    vec     fa = { NULL, 0, 0, sizeof(ps_swf_fill) };
    vec     fb = { NULL, 0, 0, sizeof(ps_swf_fill) };
    vec     la = { NULL, 0, 0, sizeof(ps_swf_line) };
    vec     lb = { NULL, 0, 0, sizeof(ps_swf_line) };
    vec     ga = { NULL, 0, 0, sizeof(ps_swf_gradient) };
    vec     gb = { NULL, 0, 0, sizeof(ps_swf_gradient) };
    vec     ed = { NULL, 0, 0, sizeof(ps_swf_edge) };
    vec     bp = { NULL, 0, 0, sizeof(ps_swf_mpoint) };
    size_t  endpos;
    uint32_t off;

    memset(mo, 0, sizeof *mo);
    ps_bits_init(&sb, body, blen);
    mo->id = ps_bits_u16(&sb);
    read_rect(&sb, &mo->xmin,  &mo->xmax,  &mo->ymin,  &mo->ymax);
    read_rect(&sb, &mo->exmin, &mo->exmax, &mo->eymin, &mo->eymax);
    off = ps_bits_u32(&sb);
    if(sb.over)
        goto fail;

    /* The offset is counted from the byte after itself, not from the start of
     * the tag - the one field in this record that is relative to where it ends
     * rather than to where the tag begins. Getting that wrong lands the second
     * cursor four bytes into the style array, where it reads plausible edge
     * records out of colour data. */
    endpos = sb.pos + off;
    if(off == 0 || endpos >= blen)
        goto fail;

    ps_bits_init(&eb, body, blen);
    eb.pos = endpos;

    if(parse_morph_fills(&sb, &fa, &fb, &ga, &gb, cap) < 0 ||
       parse_morph_lines(&sb, &la, &lb, cap) < 0)
        goto fail;
    /* The start shape's edges have to fit in front of the end shape's. If the
     * styles alone have already run past the stated offset then the offset
     * does not describe this tag. */
    if(sb.pos > endpos)
        goto fail;
    if(parse_morph_edges(&sb, &eb, &ed, &bp, fa.n, la.n, cap) < 0 ||
       sb.pos > endpos)
        goto fail;

    mo->a.fills = fa.base; mo->a.lines = la.base; mo->a.grads = ga.base;
    mo->b.fills = fb.base; mo->b.lines = lb.base; mo->b.grads = gb.base;
    mo->nfill = fa.n;
    mo->nline = la.n;
    mo->ngrad = ga.n;
    mo->edges = ed.base;
    mo->bpts  = bp.base;
    mo->nedge = ed.n;
    return 0;

fail:
    ps_swf_dealloc(fa.base); ps_swf_dealloc(fb.base);
    ps_swf_dealloc(la.base); ps_swf_dealloc(lb.base);
    ps_swf_dealloc(ga.base); ps_swf_dealloc(gb.base);
    ps_swf_dealloc(ed.base); ps_swf_dealloc(bp.base);
    memset(mo, 0, sizeof *mo);
    return -1;
}

void ps_swf_morph_free(ps_swf_morph *mo)
{
    ps_swf_dealloc(mo->a.fills); ps_swf_dealloc(mo->a.lines);
    ps_swf_dealloc(mo->a.grads);
    ps_swf_dealloc(mo->b.fills); ps_swf_dealloc(mo->b.lines);
    ps_swf_dealloc(mo->b.grads);
    ps_swf_dealloc(mo->edges);
    ps_swf_dealloc(mo->bpts);
    memset(mo, 0, sizeof *mo);
}

/* --- the blend ----------------------------------------------------------- */

/* Linear, in integers, and exact at both ends: ratio 0 and 65535 give back the
 * two stated values bit for bit rather than something a float would land near.
 *
 * The halfway point is worth stating because a test rests on it. 32768/65535
 * exceeds one half by one part in 65535, so for an even delta the truncated
 * result is exactly half of it until the delta passes 131070 twips - six and a
 * half thousand pixels, which is twelve stages wide. Below that the halfway
 * frame of a morph is arithmetic anyone can do on paper. */
static int32_t blend32(int32_t a, int32_t b, uint16_t r)
{
    return a + (int32_t)((((int64_t)b - (int64_t)a) * (int64_t)r) / 65535);
}

static uint8_t blend8(uint8_t a, uint8_t b, uint16_t r)
{
    return (uint8_t)((int32_t)a +
                     (((int32_t)b - (int32_t)a) * (int32_t)r) / 65535);
}

static float blendf(float a, float b, uint16_t r)
{
    return a + (b - a) * ((float)r / 65535.0f);
}

static ps_swf_rgba blend_rgba(ps_swf_rgba a, ps_swf_rgba b, uint16_t r)
{
    ps_swf_rgba out;

    out.r = blend8(a.r, b.r, r);
    out.g = blend8(a.g, b.g, r);
    out.b = blend8(a.b, b.b, r);
    out.a = blend8(a.a, b.a, r);
    return out;
}

int ps_swf_morph_shape_init(const ps_swf_morph *mo, ps_swf_shape *out)
{
    memset(out, 0, sizeof *out);

    /* Sized from counts the parser already bounded against the input length,
     * never from a field. Separated from the blend itself so that stepping a
     * morph across a hundred frames is a hundred blends and one allocation. */
    out->fills = ps_swf_alloc((size_t)mo->nfill * sizeof *out->fills);
    out->lines = ps_swf_alloc((size_t)mo->nline * sizeof *out->lines);
    out->grads = ps_swf_alloc((size_t)mo->ngrad * sizeof *out->grads);
    out->edges = ps_swf_alloc((size_t)mo->nedge * sizeof *out->edges);
    if(!out->fills || !out->lines || !out->grads || !out->edges) {
        ps_swf_morph_shape_free(out);
        return -1;
    }
    out->id     = mo->id;
    out->nfill  = mo->nfill;
    out->nline  = mo->nline;
    out->ngrad  = mo->ngrad;
    out->nedge  = mo->nedge;
    out->nlayer = 1;
    return 0;
}

void ps_swf_morph_shape_free(ps_swf_shape *out)
{
    ps_swf_dealloc(out->fills);
    ps_swf_dealloc(out->lines);
    ps_swf_dealloc(out->grads);
    /* Always NULL today - a blend has no bitmap fills to hold - and released
     * anyway, so that the day one appears the leak is not waiting for it. This
     * function has to free every array ps_swf_shape names, not every array it
     * happens to fill in. */
    ps_swf_dealloc(out->bfills);
    ps_swf_dealloc(out->edges);
    memset(out, 0, sizeof *out);
}

void ps_swf_morph_at(const ps_swf_morph *mo, uint16_t ratio, ps_swf_shape *out)
{
    uint32_t i, k;

    out->xmin = blend32(mo->xmin, mo->exmin, ratio);
    out->xmax = blend32(mo->xmax, mo->exmax, ratio);
    out->ymin = blend32(mo->ymin, mo->eymin, ratio);
    out->ymax = blend32(mo->ymax, mo->eymax, ratio);

    for(i = 0; i < mo->nfill; i++) {
        /* Type, bitmap id and gradient index are equal at both ends by
         * construction - one record declared them - so the start side is the
         * whole answer for everything except the colour. */
        out->fills[i]       = mo->a.fills[i];
        out->fills[i].color = blend_rgba(mo->a.fills[i].color,
                                         mo->b.fills[i].color, ratio);
    }
    for(i = 0; i < mo->nline; i++) {
        out->lines[i].width = (uint16_t)blend32(mo->a.lines[i].width,
                                                mo->b.lines[i].width, ratio);
        out->lines[i].color = blend_rgba(mo->a.lines[i].color,
                                         mo->b.lines[i].color, ratio);
    }
    for(i = 0; i < mo->ngrad; i++) {
        const ps_swf_gradient *ga = &mo->a.grads[i];
        const ps_swf_gradient *gb = &mo->b.grads[i];
        ps_swf_gradient       *g  = &out->grads[i];

        for(k = 0; k < 6; k++)
            g->mat[k] = blendf(ga->mat[k], gb->mat[k], ratio);
        g->nstop = ga->nstop;
        for(k = 0; k < g->nstop; k++) {
            /* Stop n against stop n, not by position along the ramp. The
             * format pairs them in one record for exactly this reason: a ramp
             * whose stops slide past each other is the effect the file asked
             * for, and matching by ratio instead would refuse to draw it. */
            g->ratio[k] = (uint8_t)blend32(ga->ratio[k], gb->ratio[k], ratio);
            g->color[k] = blend_rgba(ga->color[k], gb->color[k], ratio);
        }
    }
    for(i = 0; i < mo->nedge; i++) {
        const ps_swf_edge   *e = &mo->edges[i];
        const ps_swf_mpoint *p = &mo->bpts[i];
        ps_swf_edge         *o = &out->edges[i];

        *o    = *e;
        o->x0 = blend32(e->x0, p->x0, ratio);
        o->y0 = blend32(e->y0, p->y0, ratio);
        o->cx = blend32(e->cx, p->cx, ratio);
        o->cy = blend32(e->cy, p->cy, ratio);
        o->x1 = blend32(e->x1, p->x1, ratio);
        o->y1 = blend32(e->y1, p->y1, ratio);
    }
}
