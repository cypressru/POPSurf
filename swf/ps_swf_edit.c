/* DefineEditText: reading the record, and laying the text out inside it.
 *
 * The record is the same hazard PlaceObject2 is. Sixteen flag bits decide
 * which of eleven later fields are present, and every field after a
 * mis-handled one is read from the wrong offset - so a field that claims a
 * colour it does not carry does not lose its colour, it loses its margins, its
 * variable name and its text, and what it draws is whatever those bytes
 * happened to be. The flags are therefore read one bit at a time in the order
 * the specification lists them rather than as a mask, and the whole record is
 * checked for overrun once at the end.
 *
 * The layout is the part with real work in it. Word wrapping means measuring a
 * line before placing it, and alignment means knowing its width before its
 * first glyph is positioned - so each line is walked twice: once to find where
 * it breaks and how wide it is, once to emit it. Two walks over the same bytes
 * rather than one walk into a scratch line buffer, because that buffer would
 * be sized by the length of a string in the file, and nothing here is.
 */
#include "ps_swf_edit.h"

#include "ps_swf_bits.h"
#include "ps_swf_mem.h"
#include "ps_swf_read.h"

#include <string.h>

/* An advance is SI16 in em units and FontHeight is UI16 in twips, so their
 * product needs thirty-one bits and clears a signed 32-bit type by about four
 * thousand - one part in five hundred thousand. That margin is too thin to
 * build on: a font stated on a larger em square, which DefineFont3's 20480 is,
 * walks straight through it. So em2tw widens before it multiplies, and this is
 * the bound it is widening against. */
static_assert((int64_t)32768 * 65535 < INT32_MAX &&
              (int64_t)32768 * 65535 > INT32_MAX - 65536,
              "em-to-twip scaling is one font size away from overflowing 32 bits");

/* Advances and outlines are stated on the same em square, so one constant
 * scales both. If a later font tag moved that square - DefineFont3 uses 20480
 * - the advances would have to move with it, and this is where the coupling
 * is written down. */
static_assert(PS_SWF_EM == 1024,
              "glyph advances share the outlines' em square");

/* --- metrics ------------------------------------------------------------- */

static int32_t em2tw(int32_t em, uint16_t height)
{
    return (int32_t)(((int64_t)em * (int64_t)height) / PS_SWF_EM);
}

static const ps_swf_font *find_font(const ps_swf_font *fonts, uint32_t nfont,
                                    uint16_t id)
{
    uint32_t i;

    /* Last definition wins, for the same reason ps_swf_find_char says so. */
    for(i = nfont; i-- > 0; )
        if(fonts[i].id == id)
            return &fonts[i];
    return NULL;
}

/* Character code to glyph index, linearly. A Flash 4 font holds the glyphs one
 * movie actually uses - the sample's two fonts carry 34 and 45 - so a table
 * that fits in a cache line or two beats anything with a bucket array that
 * would be one more allocation sized from the file. */
static int glyph_for(const ps_swf_font *f, unsigned code)
{
    uint32_t i;

    if(!f->code)
        return -1;
    for(i = 0; i < f->nglyph; i++)
        if(f->code[i] == code)
            return (int)i;
    return -1;
}

static int32_t advance_em(const ps_swf_font *f, uint32_t g)
{
    const ps_swf_shape *sh;
    int32_t             max = 0;
    uint32_t            i;

    if(f->advance)
        return f->advance[g];

    /* No advance table, which is every DefineFont - the tag has nowhere to put
     * one. The right edge of the outline's own bounding box is the closest
     * thing the file contains, and it is what Flash falls back on.
     *
     * A glyph with no outline has no width to derive and that is exactly the
     * space, which is the one glyph whose width decides where lines break. A
     * quarter em is the usual figure and being slightly wrong about it is far
     * better than running every word together. */
    sh = &f->glyphs[g];
    for(i = 0; i < sh->nedge; i++) {
        const ps_swf_edge *e = &sh->edges[i];

        if(e->x0 > max) max = e->x0;
        if(e->x1 > max) max = e->x1;
        if(e->curve && e->cx > max) max = e->cx;
    }
    return max ? max : PS_SWF_EM / 4;
}

/* --- line breaking ------------------------------------------------------- */

typedef struct {
    uint32_t end;      /* one past the last byte drawn on this line */
    uint32_t next;     /* where the following line starts */
    int32_t  width;    /* twips, excluding a break space */
} ps_line;

/* Where the line starting at `p` ends, and how wide it is.
 *
 * The break is taken at the last space, and that space is neither drawn nor
 * counted - a right-aligned line ending in a space would otherwise hang a
 * space width off the margin. A space is also never itself the thing that
 * overflows: trailing whitespace is allowed to run past the edge, because the
 * alternative pushes the word in front of it onto the next line for the sake
 * of a gap nobody can see. A word too long for the whole line has no space to
 * break at and is cut where it overflows, which is what Flash does and is the
 * only option that guarantees progress: without it a line that can hold
 * nothing would be measured forever. */
static ps_line measure(const ps_swf_edittext *t, const ps_swf_font *f,
                       const uint8_t *s, uint32_t len, uint32_t p,
                       int wrap, int breaks, int32_t avail)
{
    ps_line  ln = { len, len, 0 };
    int32_t  w = 0, brk_w = 0;
    uint32_t i = p, brk_next = 0;
    long     brk = -1;

    while(i < len) {
        unsigned c = s[i];
        int32_t  aw;
        int      g;

        if(breaks && (c == '\r' || c == '\n')) {
            ln.end  = i;
            ln.next = i + 1;
            /* A CRLF is one break, not two. Flash writes bare CR, but text
             * assigned from a page or an editor arrives with either. */
            if(c == '\r' && i + 1 < len && s[i + 1] == '\n')
                ln.next = i + 2;
            ln.width = w;
            return ln;
        }

        g  = glyph_for(f, t->password ? '*' : c);
        aw = g < 0 ? 0 : em2tw(advance_em(f, (uint32_t)g), t->height);

        if(wrap && c != ' ' && i > p && w + aw > avail) {
            if(brk >= 0) {
                ln.end   = (uint32_t)brk;
                ln.next  = brk_next;
                ln.width = brk_w;
            } else {
                ln.end   = i;
                ln.next  = i;
                ln.width = w;
            }
            return ln;
        }
        w += aw;
        i++;
        if(c == ' ') {
            brk      = (long)i - 1;
            brk_next = i;
            brk_w    = w - aw;
        }
    }
    ln.end   = len;
    ln.next  = len;
    ln.width = w;
    return ln;
}

static int layout(ps_swf_edittext *t, const ps_swf_font *f,
                  const uint8_t *s, uint32_t len, vec *out, uint32_t cap)
{
    int32_t  asc, desc, step, usable, y;
    uint32_t p = 0;
    int      first = 1;
    /* Both only apply to a field that can hold more than one line. A
     * single-line field with WordWrap set does not wrap, it runs on. */
    int      wrap   = t->multiline && t->wordwrap;
    int      breaks = t->multiline;

    if(t->height == 0)
        return 0;

    /* Where the baseline sits under the top of the box. A file with no layout
     * block states no ascent, and then the whole em is the only figure
     * available - which puts the baseline one text height down, and is right
     * for the ordinary case where a font's ascent is most of its em. */
    asc  = f->ascent  ? em2tw(f->ascent,  t->height) : (int32_t)t->height;
    desc = f->descent ? em2tw(f->descent, t->height) : 0;
    step = asc + desc + em2tw(f->leading, t->height) + (int32_t)t->leading;
    /* Leading is signed and a file may drive the step to zero or below, which
     * is a legal design choice for overlapping lines but would stack them all
     * on one baseline here. The text height is the floor. */
    if(step <= 0)
        step = (int32_t)t->height;

    usable = (t->xmax - t->xmin) - 2 * PS_SWF_EDIT_GUTTER
             - (int32_t)t->left_margin - (int32_t)t->right_margin;
    if(usable < 0)
        usable = 0;

    y = t->ymin + PS_SWF_EDIT_GUTTER + asc;

    while(p < len) {
        ps_line  ln;
        int32_t  indent = first ? (int32_t)t->indent : 0;
        int32_t  avail  = usable - indent;
        int32_t  x;
        uint32_t i;

        if(avail < 0)
            avail = 0;
        ln = measure(t, f, s, len, p, wrap, breaks, avail);

        /* Alignment is entirely a choice of where the line's first pen goes.
         * Justify lands on the left arm deliberately - see the header. */
        x = t->xmin + PS_SWF_EDIT_GUTTER + (int32_t)t->left_margin + indent;
        if(t->align == PS_SWF_ALIGN_RIGHT)
            x += avail - ln.width;
        else if(t->align == PS_SWF_ALIGN_CENTER)
            x += (avail - ln.width) / 2;

        for(i = p; i < ln.end; i++) {
            ps_swf_glyphref gr;
            int             g = glyph_for(f, t->password ? '*' : s[i]);

            if(g < 0)
                continue;              /* not in this font: no ink, no advance */
            gr.font_id = t->font_id;
            gr.glyph   = (uint16_t)g;
            gr.height  = t->height;
            gr.color   = t->color;
            gr.x       = x;
            gr.y       = y;
            if(vec_push(out, &gr, cap) < 0)
                return -1;
            x += em2tw(advance_em(f, (uint32_t)g), t->height);
        }
        t->nline++;
        y += step;
        first = 0;

        if(!t->multiline)
            break;
        /* measure guarantees this, but the loop's termination depends on it
         * and the cost of saying so is one comparison per line. */
        p = ln.next > p ? ln.next : p + 1;
    }
    return 0;
}

/* --- the tag ------------------------------------------------------------- */

/* A null-terminated string, returned in place. Nothing is copied: the field's
 * text is laid out before this function returns, and the glyph positions are
 * what survive. */
static const uint8_t *read_string(ps_bits *b, uint32_t *len)
{
    const uint8_t *s;

    ps_bits_align(b);
    s    = b->data + (b->pos < b->len ? b->pos : b->len);
    *len = 0;
    while(!b->over) {
        if(ps_bits_u8(b) == 0)
            break;
        (*len)++;
    }
    return s;
}

int ps_swf_edit_parse(const uint8_t *body, size_t blen,
                      const ps_swf_font *fonts, uint32_t nfont,
                      ps_swf_edittext *t, uint32_t cap)
{
    ps_bits            b;
    vec                g = { NULL, 0, 0, sizeof(ps_swf_glyphref) };
    const ps_swf_font *f;
    const uint8_t     *text = NULL;
    uint32_t           tlen = 0, skip;
    unsigned           has_text, has_color, has_max, has_font, has_class;
    unsigned           has_layout;

    memset(t, 0, sizeof *t);
    t->color.a = 255;                  /* black opaque, the field's default */

    ps_bits_init(&b, body, blen);
    t->id = ps_bits_u16(&b);
    read_rect(&b, &t->xmin, &t->xmax, &t->ymin, &t->ymax);

    /* Sixteen bits, MSB first, in the order the field list uses. Two of them
     * were reserved before SWF 6 and are required to be zero there, so on a
     * SWF 4 file HasFontClass never fires - but they occupy bits in every
     * version and skipping them shifts the twelve behind them. */
    has_text     = ps_bits_ub(&b, 1);
    t->wordwrap  = (uint8_t)ps_bits_ub(&b, 1);
    t->multiline = (uint8_t)ps_bits_ub(&b, 1);
    t->password  = (uint8_t)ps_bits_ub(&b, 1);
    t->readonly  = (uint8_t)ps_bits_ub(&b, 1);
    has_color    = ps_bits_ub(&b, 1);
    has_max      = ps_bits_ub(&b, 1);
    has_font     = ps_bits_ub(&b, 1);
    has_class    = ps_bits_ub(&b, 1);          /* SWF 6 */
    t->autosize  = (uint8_t)ps_bits_ub(&b, 1);
    has_layout   = ps_bits_ub(&b, 1);
    t->noselect  = (uint8_t)ps_bits_ub(&b, 1);
    t->border    = (uint8_t)ps_bits_ub(&b, 1);
    (void)ps_bits_ub(&b, 1);                   /* WasStatic, SWF 6 */
    t->html      = (uint8_t)ps_bits_ub(&b, 1);
    (void)ps_bits_ub(&b, 1);                   /* UseOutlines */

    if(has_font)
        t->font_id = ps_bits_u16(&b);
    if(has_class)
        (void)read_string(&b, &skip);
    /* FontHeight belongs to whichever of the two named the font, and sits
     * behind the class name rather than beside the font id. */
    if(has_font || has_class)
        t->height = ps_bits_u16(&b);
    if(has_color)
        t->color = read_color(&b, 1);
    if(has_max)
        t->max_length = ps_bits_u16(&b);
    if(has_layout) {
        t->align        = ps_bits_u8(&b);
        t->left_margin  = ps_bits_u16(&b);
        t->right_margin = ps_bits_u16(&b);
        t->indent       = ps_bits_u16(&b);
        t->leading      = (int16_t)ps_bits_u16(&b);
    }
    (void)read_string(&b, &skip);              /* VariableName, always present */
    if(has_text)
        text = read_string(&b, &tlen);

    /* One check for the whole record. A tag too short to hold what its flags
     * claim does not fail on the field that ran out - that one reads zeros -
     * it fails on everything after it, so there is nothing to be gained by
     * finding out which field it was. */
    if(b.over)
        return -1;

    f = find_font(fonts, nfont, t->font_id);
    if(text && tlen && f && layout(t, f, text, tlen, &g, cap) < 0) {
        ps_swf_dealloc(g.base);
        return -1;
    }
    t->glyphs = g.base;
    t->nglyph = g.n;
    return 0;
}

void ps_swf_edit_free(ps_swf_edittext *t)
{
    ps_swf_dealloc(t->glyphs);
    t->glyphs = NULL;
    t->nglyph = 0;
}

int ps_swf_font_info(const uint8_t *body, size_t blen, ps_swf_font *fonts,
                     uint32_t nfont)
{
    ps_bits      b;
    ps_swf_font *f;
    uint16_t     id;
    uint16_t    *code;
    unsigned     namelen, flags, wide;
    uint32_t     i;

    ps_bits_init(&b, body, blen);
    id      = ps_bits_u16(&b);
    namelen = ps_bits_u8(&b);
    ps_bits_skip(&b, namelen);
    /* Six of these eight bits describe how the font looks and are of no
     * interest to a renderer that has the outlines. The last one says how wide
     * a code table entry is, and it is the only reason to read the byte. */
    flags = ps_bits_u8(&b);
    wide  = flags & 0x01;
    if(b.over)
        return -1;

    f = NULL;
    for(i = nfont; i-- > 0; )
        if(fonts[i].id == id) {
            f = &fonts[i];
            break;
        }
    /* Codes for a font that is not there, or a second table for one that
     * already has one, are both nothing to act on. */
    if(!f || f->nglyph == 0 || f->code)
        return -1;

    /* One entry per glyph, and the count comes from the font rather than from
     * this tag - DefineFontInfo does not state one. That is what makes this
     * allocation a function of what was already parsed and bounded. */
    code = ps_swf_alloc((size_t)f->nglyph * sizeof *code);
    if(!code)
        return -1;
    for(i = 0; i < f->nglyph; i++) {
        code[i] = (uint16_t)(wide ? ps_bits_u16(&b) : ps_bits_u8(&b));
        if(b.over)
            code[i] = 0;               /* short table: those glyphs go unnamed */
    }
    f->code = code;
    return 0;
}
