/* DefineMorphShape (tag 46, SWF 3): two shapes and a ratio.
 *
 * At ratio 0 the character is the start shape, at 65535 the end shape, and in
 * between every coordinate, colour, gradient stop and line width is a linear
 * blend of the two. PlaceObject2's Ratio field is what drives it, which is the
 * whole of the animation: a morph is one character that the display list moves
 * through its own shape over frames, not a sequence of shapes.
 *
 * The structural fact everything here rests on is that the two shapes are not
 * independent. They share one style table, one edge count and one sequence of
 * records, because that is what makes a point-by-point blend definable at all
 * - there is no general algorithm for morphing two arbitrary outlines, so the
 * format requires the exporter to have already put them in correspondence. So
 * the two are parsed in lockstep and a file whose halves disagree is refused
 * rather than guessed at: the alternative is a picture that is wrong in a way
 * no assertion can describe.
 *
 * What is stored is therefore one edge array carrying the shared topology and
 * the start geometry, a second array carrying only the six coordinates of the
 * same edges at the other end, and two style tables. Storing two whole
 * ps_swf_edge arrays would repeat the fill and line indices, the curve flag
 * and the layer on every edge - a third of the largest structure in the file,
 * to hold two copies of numbers that are equal by construction.
 *
 * Interpolation produces an ordinary ps_swf_shape, so nothing downstream of
 * this file knows morphs exist. The rasteriser, the tessellator and the stage
 * all draw the result unchanged.
 */
#ifndef PS_SWF_MORPH_H
#define PS_SWF_MORPH_H

#include "ps_swf.h"

/* One edge's geometry at ratio 65535. The control point is meaningless unless
 * the paired ps_swf_edge says curve, exactly as in ps_swf_edge itself. */
typedef struct {
    int32_t x0, y0;
    int32_t cx, cy;
    int32_t x1, y1;
} ps_swf_mpoint;

/* The style tables at one end. Counts live on the morph rather than here
 * because both ends have the same ones - a MORPHFILLSTYLE is a single record
 * carrying both colours, so the two tables are two views of one parse. */
typedef struct {
    ps_swf_fill     *fills;
    ps_swf_line     *lines;
    ps_swf_gradient *grads;
} ps_swf_morph_side;

struct ps_swf_morph {
    uint16_t id;
    int32_t  xmin, xmax, ymin, ymax;       /* declared start bounds, twips */
    int32_t  exmin, exmax, eymin, eymax;   /* declared end bounds */

    ps_swf_morph_side a, b;
    uint32_t          nfill, nline, ngrad;

    ps_swf_edge   *edges;    /* start geometry, and the topology both share */
    ps_swf_mpoint *bpts;     /* the same edges at the other end */
    uint32_t       nedge;
};

/* 0 on success, -1 if the record is malformed, truncated, or describes two
 * shapes that do not correspond. `cap` is the input-derived element ceiling
 * every other parser here uses. */
[[nodiscard]] int ps_swf_morph_parse(const uint8_t *body, size_t blen,
                                     ps_swf_morph *mo, uint32_t cap);
void ps_swf_morph_free(ps_swf_morph *mo);

/* Sizes `out`'s arrays from the morph's own already-bounded counts, so the
 * same shape can be re-blended at any number of ratios without reallocating.
 * [[nodiscard]] because a caller that ignored the failure would then blend
 * into null arrays. */
[[nodiscard]] int ps_swf_morph_shape_init(const ps_swf_morph *mo,
                                          ps_swf_shape *out);
void ps_swf_morph_shape_free(ps_swf_shape *out);

/* Writes the blend at `ratio` into a shape prepared by the call above. */
void ps_swf_morph_at(const ps_swf_morph *mo, uint16_t ratio,
                     ps_swf_shape *out);

#endif /* PS_SWF_MORPH_H */
