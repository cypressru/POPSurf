/* DefineEditText (tag 37, SWF 4): a text field.
 *
 * The name is misleading about what it is for. Editing is the rare case; what
 * pages of this era overwhelmingly use a field for is a score, a caption or a
 * counter that ActionScript writes into, because a field's contents can be
 * changed at run time and a DefineText's cannot. So what a renderer owes this
 * tag is a correct picture of the text it declares, laid out inside its own
 * bounds - and nothing else.
 *
 * Editing and focus are therefore out of scope here, deliberately and not for
 * now: a field's live value is a variable, and variables belong to
 * ActionScript. What is in scope is everything that decides where the ink
 * lands - the font and its height, word wrapping inside the bounds, the three
 * alignment modes, and the margins, indent and leading that shift them.
 *
 * Layout happens at parse time and produces the same ps_swf_glyphref array a
 * DefineText produces, so the field is drawn by the glyph path that already
 * exists. That is the whole reason the layout is here rather than in the
 * renderer: a positioned glyph is a positioned glyph, and there is no second
 * text renderer in this player.
 */
#ifndef PS_SWF_EDIT_H
#define PS_SWF_EDIT_H

#include "ps_swf.h"

/* The Align field's own numbering. Justify is parsed and then laid out as
 * left: justifying means stretching the spaces of every line but the last,
 * which needs the word breaks kept rather than the glyph positions, and no
 * Flash 4 field this has met asks for it. */
#define PS_SWF_ALIGN_LEFT    0
#define PS_SWF_ALIGN_RIGHT   1
#define PS_SWF_ALIGN_CENTER  2
#define PS_SWF_ALIGN_JUSTIFY 3

/* Two pixels of dead space inside the bounds on every side. This is not in the
 * specification anywhere - it is what Flash does, and a layout without it puts
 * every line two pixels left and two pixels high of where the authoring tool
 * showed it, which on a caption in a 20-pixel box is a tenth of the height. */
#define PS_SWF_EDIT_GUTTER 40      /* twips */

struct ps_swf_edittext {
    uint16_t    id;
    int32_t     xmin, xmax, ymin, ymax;   /* the field's box, twips */

    uint16_t    font_id;
    uint16_t    height;                   /* twips */
    ps_swf_rgba color;

    uint16_t    left_margin, right_margin, indent;   /* twips */
    int16_t     leading;                  /* twips, and legally negative */
    uint16_t    max_length;
    uint8_t     align;

    uint8_t     wordwrap, multiline, password, readonly;
    uint8_t     autosize, noselect, border;
    /* Parsed and recorded, never acted on. A field flagged HTML carries markup
     * in its text - <b>, <font>, <a> - and rendering a subset of that is worse
     * than rendering none: half-applied markup produces a picture that is
     * wrong in a way nobody can tell from a layout bug, whereas text with the
     * tags visible in it points straight at this line. When it is done
     * properly it needs an inline model with per-run fonts and colours, which
     * is a different structure from the one above. */
    uint8_t     html;

    /* The declared text, laid out. Positioned exactly as DefineText's glyphs
     * are, in the character's own space, so the stage draws both the same way. */
    ps_swf_glyphref *glyphs;
    uint32_t         nglyph;
    uint32_t         nline;
};

/* `fonts` is the table as it stands at this point in the tag stream, which is
 * why it is passed rather than found: a field is laid out while the movie is
 * still being read, and the movie's own font array does not exist yet.
 *
 * 0 on success, -1 if the record is malformed or too short to hold what its
 * flags claim. A field naming a font that has not been defined is not an
 * error - it lays out to nothing, exactly as a DefineText in the same position
 * draws nothing. */
[[nodiscard]] int ps_swf_edit_parse(const uint8_t *body, size_t blen,
                                    const ps_swf_font *fonts, uint32_t nfont,
                                    ps_swf_edittext *t, uint32_t cap);
void ps_swf_edit_free(ps_swf_edittext *t);

/* DefineFontInfo (tag 13): the code table a DefineFont has nowhere to put.
 *
 * It lives here because an edit text is the only thing that needs it - a
 * DefineText names glyphs by index and never asks what character one is - and
 * because without it a DefineFont cannot serve a field at all. Attaches to an
 * already-defined font and returns -1 if there is none, which loses the codes
 * and nothing else. */
[[nodiscard]] int ps_swf_font_info(const uint8_t *body, size_t blen,
                                   ps_swf_font *fonts, uint32_t nfont);

#endif /* PS_SWF_EDIT_H */
