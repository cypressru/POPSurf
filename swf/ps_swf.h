/* SWF 4 player: tag walker, shape parser, timeline, and two ways to draw.
 *
 * "SWF 4" is the target for the player as a whole - Flash 4 era content, which
 * is what the Dreamcast web of the time was serving, and the sample this was
 * developed against declares version 3. It is not a statement about which
 * files this file can read, and the distinction is worth stating because the
 * two are further apart than they look in both directions.
 *
 * Shapes here are already complete for SWF 1 through 7. The static shape tags
 * are DefineShape (SWF 1), DefineShape2 (SWF 2) and DefineShape3 (SWF 3, and
 * the one that adds alpha), and all three are parsed; no version between 4 and
 * 7 adds another. The next one is DefineShape4 in SWF 8, which is where line
 * styles gain caps, joins and miter limits, where a radial gradient gains a
 * focal point, and where a gradient may carry fifteen stops instead of eight.
 * None of that is read, and the two fields in a gradient that are SWF 8 -
 * spread mode and interpolation mode - are read and discarded, which is
 * correct because they are required to be zero before SWF 8.
 *
 * A tag declined that way is named once on the err channel rather than dropped
 * in silence. Five files in the corpus are DefineShape4 and nothing else, and
 * an empty stage with no reason given is the worst failure a browser has: it
 * cannot be told from a blank movie, from a fetch that returned nothing, or
 * from a fault in here.
 *
 * The display list is likewise complete for SWF 1 through 7, in the sense that
 * every tag that can move a character onto or off a timeline in those versions
 * is read: PlaceObject (SWF 1), RemoveObject (SWF 1), PlaceObject2 (SWF 3),
 * RemoveObject2 (SWF 3), DefineSprite (SWF 3), ShowFrame, FrameLabel and
 * SetBackgroundColor. PlaceObject3 is SWF 8 and is not read, which also means
 * no blend modes and no filters. One field inside PlaceObject2 is located
 * and then skipped rather than acted on: ClipActions, which is ActionScript
 * and belongs with the rest of it. ClipDepth is acted on - a character placed
 * with one is a mask rather than something to draw, and confines what the
 * characters above it may paint, up to and including that depth. How a mask
 * reaches a backend is ps_swf_stage_sink at the bottom of this file, and why
 * it is shaped that way rather than as a buffer is ps_swf_clip.c.
 *
 * Bitmaps are complete for SWF 1 through 3, which is every form that exists
 * before SWF 8: DefineBitsLossless and DefineBitsLossless2 in their 8, 15, 24
 * and 32-bit varieties, and DefineBits, DefineBitsJPEG2 and DefineBitsJPEG3.
 * Only the last of those carries alpha on the JPEG side. What is not read is
 * DefineBitsJPEG4 (SWF 10) and the SWF 8 licence to put a PNG or a GIF inside
 * a tag whose name says JPEG - the latter is detected from the signature and
 * refused by name rather than decoded as a broken JPEG.
 *
 * The rest of the character set is complete for SWF 3 and 4 as far as drawing
 * goes. DefineFont, DefineFont2, DefineFontInfo, DefineText and DefineText2
 * give static text; DefineButton and DefineButton2 draw their up state;
 * DefineEditText lays a declared string out inside its own bounds;
 * DefineMorphShape blends two shapes at the ratio PlaceObject2 carries.
 *
 * Two of the readers for a SWF 4 file are deliberately not here. ActionScript
 * is parsed into the blocks below and run by ps_swf_action.h, which nothing
 * that draws a picture links - a fault in an interpreter should not be
 * mistakable for a rendering fault. Sound walks the tag stream itself in
 * ps_swf_sound.h rather than hanging off the display list, for the reason its
 * own header gives. A character that is none of the kinds this file models
 * therefore still places nothing, so an unread one renders as an empty stage
 * rather than as an error - except where that character is a mask, which is
 * the one place the difference between "covers nothing" and "we could not read
 * it" changes the whole frame. ps_swf_stage.c's clip_usable is that argument.
 *
 * One tag inside the stated target is genuinely unread: DefineButtonCxform
 * (SWF 2), which recolours a button's face. It is the only omission below SWF
 * 5 that can change a picture; Protect and FreeCharacter are the others and
 * neither carries anything to draw.
 *
 * Two of the newest carry a boundary worth stating here rather than only at
 * the file that implements them. An edit text is rendered and not edited: a
 * field's live value is an ActionScript variable, and focus, caret and input
 * belong to a player rather than to a renderer - and the HTML flag is read and
 * then ignored, because a subset of markup is worse than none of it. A morph
 * is interpolated into an ordinary ps_swf_shape and drawn by the existing
 * renderer unchanged, which is the whole reason morphs cost nothing below the
 * parser - though that unchanged renderer is now one that draws bitmap fills,
 * and a morph's are not interpolated: see ps_swf_morph.c.
 *
 * Compressed files are refused. That costs nothing at the stated target, since
 * CWS is SWF 6 and later, but it does mean an arbitrary file off the web may
 * be rejected before any of the above matters.
 *
 * The split that matters for later is between geometry and pixels, and there
 * are now two consumers of the geometry rather than one:
 *
 *   ps_swf_raster_shape  sweeps scanlines and emits horizontal runs with
 *                        software coverage antialiasing.
 *   ps_swf_tess_shape    cuts the same filled regions into triangles and
 *                        hands them to a sink.
 *
 * They are siblings, not stages. The PVR is a triangle engine that does
 * coverage in hardware, so a scanline sweep is not a step on the way to it -
 * it is a different algorithm, and half of it would be thrown away. What the
 * two share is everything above the last stage: parse, flatten curves at a
 * stated tolerance, and resolve which fill covers which side of which edge.
 *
 * The span path is kept because it is the oracle. Feed the emitted triangles
 * back through the same sweep and the picture must come out identical, which
 * is how the triangle path gets validated on a workstation instead of on
 * hardware where a wrong picture could be the tessellator, the TA list, the
 * texture format or the parser and there is no way to tell which.
 *
 * Units: SWF geometry is in twips, twentieths of a pixel, as signed integers.
 * They stay in twips through parsing and are converted once, at raster time.
 * A placement matrix is the one thing that is not integral - its scale and
 * skew terms are 16.16 fixed point - so a matrix is held as six floats with
 * the translation still counted in twips.
 */
#ifndef PS_SWF_H
#define PS_SWF_H

#include <stddef.h>
#include <stdint.h>

/* --- parsed model ------------------------------------------------------- */

typedef struct {
    uint8_t r, g, b, a;
} ps_swf_rgba;

/* An affine map, in SWF's own MATRIX cell order: x' = a*x + c*y + tx and
 * y' = b*x + d*y + ty, stored a, b, c, d, tx, ty.
 *
 * One type for every transform in the player, because they compose and there
 * is no useful distinction between them: a placement matrix, a gradient's map
 * out of its square, and the view's map from twips to output pixels are all
 * the same kind of thing, and the renderer only ever sees the product. Keeping
 * them separate types would mean writing the same multiply three times. */
typedef struct {
    float m[6];
} ps_swf_xform;

void ps_swf_xform_identity(ps_swf_xform *x);
void ps_swf_xform_scale(ps_swf_xform *x, float sx, float sy,
                        float tx, float ty);
/* out = outer applied after inner. Aliasing any argument is safe. */
void ps_swf_xform_mul(ps_swf_xform *out, const ps_swf_xform *outer,
                      const ps_swf_xform *inner);
/* 0 on success, -1 if singular - which is not a malformed file, it is an
 * exporter scaling something to nothing, and the caller has to expect it. */
int  ps_swf_xform_invert(ps_swf_xform *out, const ps_swf_xform *in);

/* CXFORM / CXFORMWITHALPHA: multiply then add, per channel, clamped.
 *
 * The multiplier is 8.8 fixed point with 256 meaning "unchanged", which is the
 * detail worth stating: it is not a percentage and it is signed, so a file can
 * legitimately ask for a negative multiplier and Flash clamps the result
 * rather than treating it as an error. Alpha terms only exist in
 * CXFORMWITHALPHA; where the file carries a plain CXFORM the alpha row is left
 * at identity by the parser so every consumer can apply all four the same way. */
typedef struct {
    int16_t mult[4];      /* r, g, b, a - 256 is identity */
    int16_t add[4];
} ps_swf_cxform;

void        ps_swf_cxform_identity(ps_swf_cxform *c);
void        ps_swf_cxform_mul(ps_swf_cxform *out, const ps_swf_cxform *outer,
                              const ps_swf_cxform *inner);
ps_swf_rgba ps_swf_cxform_apply(const ps_swf_cxform *c, ps_swf_rgba in);

/* FILLSTYLE type codes, straight from the spec's numbering. */
#define PS_SWF_FILL_SOLID    0x00
#define PS_SWF_FILL_LINEAR   0x10
#define PS_SWF_FILL_RADIAL   0x12
#define PS_SWF_FILL_BITMAP   0x40   /* 0x40..0x43: tiled/clipped, smoothed or not */

/* DefineShape 1/2/3 allow eight stops; the field that counts them is four bits
 * wide and so can say fifteen. Storing fifteen means a nonconforming file is
 * shaded as it asks rather than clipped, and costs sixty bytes on a structure
 * that only exists once per gradient in the file. */
#define PS_SWF_MAX_STOPS 15

/* A gradient is held out of line, indexed from the fill style, because it is a
 * hundred bytes and a solid fill is eight. Inlining it would multiply the fill
 * table by twelve for every file, to describe files that are mostly solids -
 * and the fill table's size ceiling is derived from the input length, so that
 * multiplier lands directly on the worst-case allocation.
 *
 * `mat` maps the gradient square - 32768 twips on a side, centred on the
 * origin - into shape space. It is the only reason the matrix in a FILLSTYLE
 * has to be read rather than skipped, and getting it wrong tilts or slides the
 * ramp without changing anything else, which is exactly the kind of fault that
 * survives a glance at the picture. Order is a, b, c, d, tx, ty with
 * x' = a*x + c*y + tx and y' = b*x + d*y + ty. */
typedef struct {
    float       mat[6];
    ps_swf_rgba color[PS_SWF_MAX_STOPS];
    uint8_t     ratio[PS_SWF_MAX_STOPS];    /* 0..255 along the ramp */
    uint8_t     nstop;
} ps_swf_gradient;

/* A decoded bitmap character: straight, non-premultiplied RGBA, row major.
 *
 * Four bytes a pixel is the expensive form and it is what the wire formats
 * decode to least badly: the lossless tags carry 8, 15, 24 and 32-bit pixels,
 * the JPEG tags carry three channels plus a separate alpha plane, and the only
 * layout all six land in without a second conversion is this one. Narrowing to
 * the PVR's ARGB4444 is a texture upload's job and belongs with the texture
 * upload, where the size of the destination is known; doing it here would
 * quantise every bitmap whether or not it is ever drawn.
 *
 * DefineBitsLossless2's 32-bit form is premultiplied on the wire. It is
 * un-premultiplied once here rather than at every sample, because the sampler
 * blends against a destination it does not own and cannot know whether the
 * caller wants premultiplied input. */
typedef struct {
    uint16_t     id;
    uint16_t     w, h;
    ps_swf_rgba *px;        /* w * h, row major, straight alpha */
} ps_swf_bitmap;

/* The out-of-line half of a bitmap fill style, held for exactly the reason a
 * gradient's is: the fill table is capped from the file's own length, so
 * anything inlined there is multiplied by that ceiling.
 *
 * `mat` maps bitmap space into shape space, where a texel is twenty units on a
 * side - the same twips the shape is in, so a fill drawn at its natural size
 * has a scale of twenty rather than of one. Order is the same a, b, c, d, tx,
 * ty as everywhere else.
 *
 * `bmp` is resolved once, after the whole file has been walked, and not while
 * parsing: a fill style names a character by ID, definitions may follow the
 * shape that uses them, and the bitmap array is reallocated as it grows, so a
 * pointer taken during the walk would be both premature and stale. NULL means
 * the ID names nothing this build could decode, which draws the fill's fallback
 * colour rather than nothing at all. */
typedef struct {
    float                mat[6];
    const ps_swf_bitmap *bmp;
} ps_swf_bitmapfill;

typedef struct {
    uint8_t     type;
    /* The mean of a gradient's stops, and mid grey for a bitmap. Kept even now
     * that gradients are shaded, because it is what a caller that cannot shade
     * - a hit test, a thumbnail, a PVR list that has run out of texture RAM -
     * should use, and because it is the fallback when the gradient matrix is
     * singular and cannot be inverted. */
    ps_swf_rgba color;
    uint16_t    bitmap_id;
    uint16_t    grad;       /* 1-based into shape->grads; 0 if not a gradient */
    uint16_t    bfill;      /* 1-based into shape->bfills; 0 if not a bitmap */
} ps_swf_fill;

/* DefineShape 1/2/3 line styles carry a width and a colour and nothing else.
 * Caps and joins only become file-specified in LINESTYLE2, which is
 * DefineShape4 and so SWF 8 - before that the renderer's behaviour is Flash's
 * default, round on both, which is what the stroker draws. */
typedef struct {
    uint16_t    width;    /* twips */
    ps_swf_rgba color;
} ps_swf_line;

/* One edge record, already resolved against the style state in force when it
 * was read.
 *
 * fill0/fill1 are 1-based indices into ps_swf_shape.fills, with 0 meaning "no
 * fill on that side". SWF fills are dual-sided: an edge names the style to
 * its left and the style to its right, and the same edge therefore bounds two
 * different fills at once. That representation is the reason a SWF shape does
 * not have to be an ordered list of closed contours - the edges may arrive in
 * any order at all. (Ruffle's render/src/shape_utils.rs calls this "edge
 * soup"; the dual-sided reading and the rule that fill style 0 is the
 * negative side, so its edges must be reversed to point the same way round as
 * style 1's, is taken from there. MIT OR Apache-2.0.)
 *
 * `layer` counts NewStyles records. Each one starts a fresh style table and,
 * more importantly, a fresh drawing layer painted over everything before it. */
typedef struct {
    int32_t  x0, y0;
    int32_t  cx, cy;   /* quadratic control point; meaningless unless curve */
    int32_t  x1, y1;
    uint16_t fill0, fill1, line;
    uint8_t  curve;
    uint8_t  layer;
} ps_swf_edge;

typedef struct {
    uint16_t     id;
    int32_t      xmin, xmax, ymin, ymax;   /* declared bounds, twips */

    ps_swf_fill     *fills;
    uint32_t         nfill;
    ps_swf_line     *lines;
    uint32_t         nline;
    ps_swf_edge     *edges;
    uint32_t         nedge;
    ps_swf_gradient *grads;
    uint32_t         ngrad;
    ps_swf_bitmapfill *bfills;
    uint32_t           nbfill;

    uint32_t         nlayer;
} ps_swf_shape;

/* --- characters, the display list and the timeline ---------------------- */

#define PS_SWF_CHAR_SHAPE   1
#define PS_SWF_CHAR_SPRITE  2
#define PS_SWF_CHAR_TEXT    3
#define PS_SWF_CHAR_BUTTON  4
#define PS_SWF_CHAR_BITMAP  5
#define PS_SWF_CHAR_MORPH   6
#define PS_SWF_CHAR_EDIT    7

/* Declared here and defined in ps_swf_morph.h and ps_swf_edit.h. Only the
 * movie needs to hold them, and holding a pointer needs no more than the name;
 * everything that actually looks inside one includes the header that has it.
 * The two are far larger than the character types above - a morph is two style
 * tables and two geometries, an edit text is a whole paragraph layout - and
 * putting them here would make this file mostly about them. */
typedef struct ps_swf_morph    ps_swf_morph;
typedef struct ps_swf_edittext ps_swf_edittext;

/* Glyphs are always defined on a 1024-unit em square, whatever the font was
 * before it was imported - the spec requires an exporter to rescale to it. A
 * glyph coordinate therefore becomes twips on the stage by multiplying by
 * TextHeight and dividing by this. DefineFont3 uses 20480 instead, but that is
 * SWF 8 and not read here. */
#define PS_SWF_EM 1024

/* A font is a table of outlines, and - only for the sake of DefineEditText -
 * enough metrics to lay a string out.
 *
 * DefineText needs none of them: it carries the size and the advance of every
 * glyph it places, and names glyphs by index, so the font is consulted only
 * for the shape. An edit text carries a string instead, so something has to
 * turn a byte into a glyph index and know how wide it is, and that something
 * can only be the font.
 *
 * The outlines are ps_swf_shape like any other, which is the point - a glyph
 * is a shape record with no style arrays, so giving it a synthetic one-entry
 * fill table at parse time means the entire renderer draws text without
 * knowing text exists. */
typedef struct {
    uint16_t      id;
    ps_swf_shape *glyphs;
    uint32_t      nglyph;

    /* Character code per glyph, and how far the pen moves after it. Both are
     * NULL where the file did not say, and that is the common case at this
     * target: DefineFont2 states both, DefineFont states neither and gets its
     * code table separately from DefineFontInfo. Advances are in em units, so
     * they scale with the text height exactly as the outlines do - which is
     * the opposite of DefineText's advances, which are already in twips at the
     * final size. */
    uint16_t     *code;
    int16_t      *advance;

    /* Em units, from DefineFont2's optional layout block. Zero means the file
     * carried none, and the edit text layout then falls back on the em square
     * rather than inventing a number. */
    int16_t       ascent, descent, leading;
} ps_swf_font;

/* One glyph of a DefineText, already positioned.
 *
 * The format stores this as runs with sticky state and relative advances -
 * a record sets a font and a colour, then each glyph carries only an index and
 * how far to step. Resolving that at parse time to an absolute pen position
 * per glyph costs eight bytes a glyph and means the renderer has no state to
 * carry and no order to preserve. */
typedef struct {
    uint16_t    font_id;
    uint16_t    glyph;      /* index into that font's glyph table */
    uint16_t    height;     /* twips */
    ps_swf_rgba color;
    int32_t     x, y;       /* pen in text space, twips */
} ps_swf_glyphref;

typedef struct {
    uint16_t         id;
    ps_swf_xform     mat;   /* text space -> the character's own space */
    ps_swf_glyphref *glyphs;
    uint32_t         nglyph;
} ps_swf_text;

/* One character of a button's up state, in its own miniature display list.
 *
 * Only the up state is kept. A button has four - up, over, down, and a hit
 * area that is never drawn at all - and which one shows is a function of where
 * the pointer is, which is not a question a renderer can answer. Up is what
 * Flash shows before anything is touched, so it is what a frame looks like. */
typedef struct {
    uint16_t      id;
    uint16_t      depth;
    ps_swf_xform  mat;
    ps_swf_cxform cx;
} ps_swf_btnrec;

typedef struct {
    uint16_t       id;
    ps_swf_btnrec *recs;
    uint32_t       nrec;
} ps_swf_button;

/* A definition tag names its character with an ID that the display list then
 * refers to. IDs are not dense and not ordered - a file may define 3, then
 * 900, then 4 - so this is a lookup table rather than an array indexed by ID.
 * Searched linearly, which is right for the counts involved: the sample
 * defines ten characters, and a table small enough to fit in cache beats a
 * hash whose bucket array would be another allocation sized from the file. */
typedef struct {
    uint16_t id;
    uint8_t  kind;
    uint32_t index;     /* into movie->shapes or movie->sprites */
} ps_swf_char;

#define PS_SWF_OP_PLACE   1
#define PS_SWF_OP_REMOVE  2

/* One edit to the display list, from a Place or Remove tag.
 *
 * The display list is not a per-frame picture, it is a set of edits applied in
 * order - a frame says "put this here" or "change that one's matrix" and
 * everything else stays as it was. That is why a frame cannot be rendered
 * without replaying the frames before it, and why this is stored as ops rather
 * than as a resolved list per frame: a 76-frame movie with 88 placements
 * stores 88 ops, where resolved lists would store the same content 76 times. */
typedef struct {
    uint8_t       op;
    uint8_t       move;        /* PlaceObject2's Move flag: edit, do not add */
    uint8_t       has_matrix;
    uint8_t       has_cxform;
    uint16_t      depth;
    uint16_t      id;          /* character to place; 0 when only editing */
    uint16_t      clip_depth;
    uint16_t      ratio;
    ps_swf_xform  mat;
    ps_swf_cxform cx;
    /* PlaceObject2's Name, owned, NULL when the tag carried none - which is
     * most placements, so the cost is a pointer per op and a string only where
     * the file wrote one. Nothing that draws reads it: it exists so a host can
     * answer SetTarget and a slash path, which name an instance and not a
     * character. Copied rather than pointed into `data` for the reason
     * ps_swf_load states - `data` need only outlive the call. */
    char         *name;
} ps_swf_op;

typedef struct {
    uint32_t first_op, nop;
} ps_swf_frame;

/* A FrameLabel, and the frame it precedes. Zero-based, like everything else
 * here and unlike the file, whose GotoFrame operands are one-based half the
 * time - ps_swf_action.h normalises those at the boundary.
 *
 * Per timeline rather than per movie because a label is scoped to the timeline
 * it was written in: two sprites may both label a frame "loop", and a
 * gotoAndPlay inside one of them means its own. */
typedef struct {
    uint32_t frame;
    char    *name;             /* owned */
} ps_swf_label;

/* One block of ActionScript, copied out of a DoAction or DoInitAction tag.
 *
 * Copied rather than referenced because ps_swf_load's contract is that `data`
 * need only outlive the call, and unlike a shape an action block cannot be
 * decoded into a model up front - it is code, and it means nothing until the
 * frame it sits on is reached. ps_swf_action.h is what runs it.
 *
 * Which of the two fields carries anything follows from which array holds the
 * block, because the two tags differ in when they run and in nothing else. A
 * DoAction runs with the frame it follows and lives in that timeline's `acts`,
 * so `frame` is set and `sprite` is 0. A DoInitAction runs once before
 * anything else to initialise one sprite, belongs to no frame and no timeline,
 * and so lives in the movie's `inits` with `sprite` set and `frame` -1. */
typedef struct {
    int32_t  frame;
    uint16_t sprite;
    uint8_t *code;
    uint32_t len;
} ps_swf_actions;

/* A timeline owns its frames and its ops rather than holding a range into one
 * array per movie. That looks wasteful and is not: a DefineSprite tag can sit
 * between two PlaceObject tags of the root's own frame, so a sprite's frames
 * are interleaved with the root's in file order and a range would have to
 * describe a discontiguous set. Two allocations per timeline is the cost of
 * not needing a second pass. */
typedef struct {
    uint16_t      id;          /* 0 for the root timeline */
    ps_swf_op    *ops;
    uint32_t      nop;
    ps_swf_frame *frames;
    uint32_t      nframe;
    /* Scripts belong to the timeline they were written in, not to the movie: a
     * sprite has its own frame numbering, so a DoAction inside a DefineSprite
     * is on frame 3 of that sprite and not on frame 3 of anything else. A flat
     * movie-level list with a frame number would silently conflate the two. */
    ps_swf_actions *acts;
    uint32_t        nact;
    ps_swf_label   *labels;
    uint32_t        nlabel;
} ps_swf_timeline;

typedef struct {
    int         version;
    int         compressed;
    int32_t     xmin, xmax, ymin, ymax;    /* stage rect, twips */
    float       fps;
    int         frames;                    /* as declared in the header */
    ps_swf_rgba bg;

    ps_swf_shape    *shapes;
    uint32_t         nshape;
    ps_swf_font     *fonts;
    uint32_t         nfont;
    ps_swf_text     *texts;
    uint32_t         ntext;
    ps_swf_button   *buttons;
    uint32_t         nbutton;
    ps_swf_bitmap   *bitmaps;
    uint32_t         nbitmap;
    ps_swf_morph    *morphs;
    uint32_t         nmorph;
    ps_swf_edittext *edits;
    uint32_t         nedit;
    ps_swf_char     *chars;
    uint32_t         nchar;

    ps_swf_timeline  root;
    ps_swf_timeline *sprites;
    uint32_t         nsprite;

    /* DoInitAction blocks, in the order the file gave them, which is the order
     * they run in - before the first frame of anything. Movie-level rather
     * than per-timeline because a sprite's initialiser is written in whichever
     * timeline the exporter felt like putting it in, and it does not run there. */
    ps_swf_actions  *inits;
    uint32_t         ninit;
} ps_swf_movie;

const ps_swf_char   *ps_swf_find_char(const ps_swf_movie *m, uint16_t id);
const ps_swf_font   *ps_swf_find_font(const ps_swf_movie *m, uint16_t id);
const ps_swf_bitmap *ps_swf_find_bitmap(const ps_swf_movie *m, uint16_t id);

/* The frame a label names, zero-based, or -1. Case sensitive, which is what
 * Flash 4 is; the case-insensitive comparison arrived with SWF 7's stricter
 * mode and applies the other way round.
 *
 * Marked because -1 is a real answer and an easy one to drop: a gotoAndPlay to
 * a label that does not exist must do nothing at all, and a caller that treats
 * the return as a frame number goes to frame -1 instead. That is exactly the
 * silent wrong-frame this exists to prevent. */
[[nodiscard]] int ps_swf_find_label(const ps_swf_timeline *tl,
                                    const char *label);

/* Parses in place: `data` must outlive the movie only until ps_swf_load
 * returns, since every field is copied out. Returns 0 on success, -1 on a
 * malformed or truncated file, with a reason in err.
 *
 * A truncated file is not necessarily useless - an intro cut short still has
 * whole shapes at the front - so tags that parse are kept and the first one
 * that does not stops the walk. err then says what stopped it. */
int  ps_swf_load(const uint8_t *data, size_t len, ps_swf_movie *m,
                 char *err, size_t errlen);
void ps_swf_free(ps_swf_movie *m);

/* Bytes currently held, and the high-water mark since the last reset. This
 * exists because the eventual budget is 16MB with no virtual memory and no
 * swap: "how much did that file cost" has to be answerable, and answerable on
 * the host, long before a file is loaded on hardware. */
size_t ps_swf_mem_live(void);
size_t ps_swf_mem_peak(void);
void   ps_swf_mem_reset_peak(void);

/* --- rasteriser --------------------------------------------------------- */

/* Where the output goes and how finely. Deliberately no transform: that is a
 * separate argument everywhere, because a frame draws many characters through
 * one view with a different matrix each, and folding the two together would
 * mean rebuilding the view per character. */
typedef struct {
    int   w, h;         /* target size in pixels */

    /* Curve flattening tolerance, in output pixels: the furthest a chord is
     * allowed to stray from the true curve. This is a parameter and not a
     * constant on purpose. It is the one knob trading quality against cost in
     * the whole renderer - halving it roughly doubles the segment count and
     * so the vertex and span work - and the right value on a 200MHz SH-4
     * driving a PVR is not going to be the right value on a workstation
     * writing a PPM. It wants measuring on hardware, not guessing here. */
    float tol;

    /* Sub-scanlines per pixel row for antialiasing. 1 disables it. Vertical
     * only; horizontal coverage is computed exactly from the span ends. */
    int   samples;
} ps_swf_view;

/* How one pass paints: a flat colour, a gradient ramp, or a bitmap, either of
 * the latter two sampled through an affine map. This is the whole of what a
 * backend needs to set up before the geometry arrives, and it is shaped that
 * way on purpose - on the PVR it is a polygon context, where `color` is the
 * base colour of an untextured list, a gradient is a 256x1 texture and a
 * bitmap is a texture, with `inv` supplying the UVs in both cases.
 *
 * `inv` maps a point in output pixel space to whichever source space is in
 * play, so sx = inv[0]*x + inv[1]*y + inv[2] and sy = inv[3]*x + inv[4]*y +
 * inv[5]. For a gradient that space is the square's own -16384..16384 twips;
 * for a bitmap it is texels, with the twenty units per texel already divided
 * out so the sampler does not repeat that per pixel. It is precomputed because
 * it folds together the fill matrix's inverse and the view transform, and
 * neither changes within a pass. */
typedef struct {
    ps_swf_rgba            color;   /* already recoloured by the cxform */
    const ps_swf_gradient *grad;    /* NULL for a flat fill */
    int                    radial;  /* ramp along the radius, not along x */
    /* NULL unless this is a bitmap fill. Tiled and smoothed are separate fill
     * style IDs in the format rather than flags on one, so they are separate
     * here too - a file distinguishes all four combinations and gets what it
     * asked for. */
    const ps_swf_bitmap   *bitmap;
    int                    tiled;
    int                    smoothed;
    float                  inv[6];
    /* Held rather than pre-applied because a gradient's colour is not known
     * until the pixel is: the stops are interpolated first and recoloured
     * after, which is the order Flash uses and the only one that gets a
     * half-transparent ramp under a dimming transform right. */
    ps_swf_cxform          cx;
    int                    has_cx;
} ps_swf_paint;

/* Called with one finished horizontal run: pixels x0..x1-1 of row y are
 * covered by `color` with coverage cov (0..255, 255 = fully inside). Runs are
 * emitted in increasing y then increasing x, and never overlap within a
 * single call to ps_swf_raster_shape's per-style pass. */
typedef void (*ps_swf_span_fn)(void *user, int y, int x0, int x1,
                               uint8_t cov, ps_swf_rgba color);

/* `xf` maps shape twips to output pixels, and `cx` recolours what is drawn -
 * both of them the product of everything the display list stacked up on the
 * way down to this character. `cx` may be NULL for no recolouring.
 *
 * Returns the number of line segments the shape flattened to, or -1 if it
 * could not allocate. */
long ps_swf_raster_shape(const ps_swf_shape *sh, const ps_swf_view *v,
                         const ps_swf_xform *xf, const ps_swf_cxform *cx,
                         ps_swf_span_fn span, void *user);

/* --- triangles ---------------------------------------------------------- */

typedef struct {
    float x, y;         /* output pixel space, same frame the spans use */
} ps_swf_vtx;

/* Where triangles go. Split into three calls rather than one per-triangle
 * callback carrying a colour, because that is the shape of the hardware: begin
 * is a polygon context and a header submission, tri is three vertices into the
 * TA's FIFO, end is the end-of-list. A host backend uses the same three to
 * batch a pass and rasterise it in one sweep, which is what makes the
 * comparison against the span renderer meaningful - see ps_swf_trisoft.c.
 *
 * Triangles within one pass may share edges but do not overlap, and are all
 * wound the same way. The sink may assume that; the tessellator guarantees it
 * for well-formed geometry and the area check in tricmp is what watches for
 * the cases where it does not. */
typedef struct {
    void (*begin)(void *user, const ps_swf_paint *paint);
    void (*tri)(void *user, const ps_swf_vtx *v);   /* three vertices */
    void (*end)(void *user);
} ps_swf_tri_sink;

/* Returns the number of triangles emitted, or -1 if it could not allocate. */
long ps_swf_tess_shape(const ps_swf_shape *sh, const ps_swf_view *v,
                       const ps_swf_xform *xf, const ps_swf_cxform *cx,
                       const ps_swf_tri_sink *sink, void *user);

/* --- the stage ----------------------------------------------------------- */

/* How deep masks may nest before the walker starts ignoring them.
 *
 * Two is what real content reaches - a masked sprite whose own contents are
 * masked - and the sample reaches zero. The number is here rather than in the
 * backend because the walker has to know it: a mask it pushes and the backend
 * cannot hold would draw the picture unmasked, which is worse than not masking
 * at all, so the walker refuses the mask instead and both sides agree on when.
 * It is small on purpose. Every level costs the host backend a full-resolution
 * buffer, which is the one cost the interface below exists to keep out of the
 * hardware backend. */
#define PS_SWF_CLIP_DEPTH 4

/* One character, resolved: its shape, and the transform and recolouring that
 * the display list accumulated for it. The sink exists so the frame walker
 * does not have to know which renderer is in use - swfrender points it at the
 * span path and tricmp at the triangle path, and the walk is shared, which is
 * the same split the two shape renderers already have one level down.
 *
 * The three clip calls are the same shape as ps_swf_tri_sink's three, and for
 * the same reason: they are what a tile accelerator does, not what a host finds
 * convenient. A mask arrives as ordinary geometry through `draw`, between
 * clip_begin and clip_apply, in the same space as everything else - never as a
 * finished mask buffer, and never as a "is this pixel masked" query the backend
 * has to answer. The PVR path submits those same shapes to the same
 * tessellator and puts the triangles in the modifier volume list instead of the
 * polygon list, which is the whole of its implementation and allocates nothing.
 * ps_swf_clip.c argues that in full.
 *
 * Draws between clip_apply and the matching clip_end are confined to the mask.
 * Masks nest, and nesting intersects: a pixel is painted only where every mask
 * in force covers it. A mask pushed while another is still collecting geometry
 * confines that geometry, which is what makes a masked sprite used as a mask
 * come out as the intersection rather than as either half.
 *
 * A sink that cannot clip leaves all three NULL. The walker then drops mask
 * characters entirely - they are not drawn, and what they should have hidden
 * stays visible, which is what this player did before any of this existed. */
typedef struct {
    void (*draw)(void *user, const ps_swf_shape *sh, const ps_swf_xform *xf,
                 const ps_swf_cxform *cx);
    void (*clip_begin)(void *user);
    void (*clip_apply)(void *user);
    void (*clip_end)(void *user);
} ps_swf_stage_sink;

/* Draws the root timeline as it stands after `frame` ticks, 0-based.
 *
 * A tick count and not a frame index: past the end the root loops, so tick N
 * of a four-frame movie shows frame N mod 4, and a placed sprite is handed the
 * ticks it has been alive for rather than the frame its parent wrapped to.
 * That is what keeps a three-frame child advancing under a two-frame parent -
 * see ps_swf_stage.c, which is also where the one simplification against Flash
 * is stated.
 *
 * This replays every frame from the first, because a frame is a set of edits
 * and not a picture. That is O(frames) per call and deliberately so: a host
 * tool renders one frame or dumps all of them in order, and a player that
 * cares steps its own state forward instead. Doing it this way keeps the
 * function pure, which is what makes rendering frame 40 twice give the same
 * answer as rendering it once.
 *
 * Returns the number of characters drawn - masks included, since a mask is
 * geometry the backend had to process - or -1 if it could not allocate. */
[[nodiscard]] long ps_swf_render_frame(const ps_swf_movie *m, uint32_t frame,
                                       const ps_swf_xform *root,
                                       const ps_swf_stage_sink *sink,
                                       void *user);

#endif /* PS_SWF_H */
