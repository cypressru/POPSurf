/* Two renderings of the same picture, compared pixel by pixel.
 *
 * The rule this file exists to hold in one place: a differing pixel means two
 * different things depending on where it is, and only one of them is a fault.
 *
 * A pixel whose eight neighbours in the reference are all its own colour is
 * fully covered by one pass. Nothing about the order of float operations, and
 * nothing about which machine did the arithmetic, can change a fully covered
 * pixel - so a difference there is a hole, an overlap, a misplaced primitive or
 * a wrong colour, and it is a fault. A pixel next to a differing neighbour
 * holds a coverage fraction, and a fraction that lands on a rounding tie can
 * fall either way between two implementations of the same edge. That is a
 * property of representing a region as numbers, not a defect, and it is why the
 * two counts are reported separately rather than summed.
 *
 * A pixel on the border of the image cannot be shown to be interior, so it is
 * counted as boundary. That is conservative in the direction that never hides a
 * fault: a real hole is many pixels across and cannot hide in a one-pixel
 * frame.
 *
 * tricmp uses this to check the tessellator against the span renderer, where
 * both sides are exact and the tolerance is zero. ppmcmp uses it to check a
 * frame captured off the Dreamcast against the same span renderer, where the
 * console's framebuffer holds sixteen bits per pixel and the tolerance is one
 * step of the coarsest channel. The rule is the same in both, which is the
 * point of it being here: two comparisons that disagree about what counts as a
 * difference are two comparisons nobody can put side by side.
 */
#ifndef PS_PPMDIFF_H
#define PS_PPMDIFF_H

#include <stdint.h>

typedef struct {
    long interior;   /* differing pixels the reference says are fully covered */
    long boundary;   /* differing pixels on an edge in the reference */
    long worse;      /* differing by more than one level, wherever they are */
    long counted;    /* pixels examined */
    int  worst;      /* largest single-channel difference seen */
    int  worst_x;    /* and where, so a number can be gone and looked at */
    int  worst_y;
} ps_ppmdiff_result;

#define PS_PPMDIFF_SAME     0
#define PS_PPMDIFF_BOUNDARY 1
#define PS_PPMDIFF_INTERIOR 2

/* Both buffers are w*h RGB triples. `tol` is the per-channel difference a pixel
 * is allowed before it counts as differing at all; zero means exact.
 *
 * The neighbourhood is read from `ref`, never from `cmp`: the reference is the
 * side that is known to be right, so it is the side entitled to say which
 * pixels are edges. Classifying by the suspect image would let a renderer that
 * blurred everything declare its own faults to be antialiasing.
 *
 * `map` may be NULL. When it is not, it takes one PS_PPMDIFF_* byte per pixel -
 * which is what turns a count back into a picture, and a count of forty is a
 * very different thing depending on whether the forty are scattered or in one
 * block. */
void ps_ppmdiff(const uint8_t *ref, const uint8_t *cmp, int w, int h,
                int tol, uint8_t *map, ps_ppmdiff_result *out);

#endif
