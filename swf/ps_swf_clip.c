/* Clipping layers, on the host, and the argument about how they reach the PVR.
 *
 * A SWF mask is a character that is not drawn and instead confines what the
 * characters above it may paint. This file implements that the obvious way for
 * a machine with memory to spare: rasterise the mask's own geometry into a
 * coverage plane, then multiply every span that follows by it. One plane per
 * nesting level, each the size of the output.
 *
 * That is a full-resolution buffer per clip, and it is the wrong shape for the
 * Dreamcast. It is in here rather than anywhere else on purpose - nothing above
 * ps_swf_clip_span knows a plane exists. What the stage hands a backend is
 * ps_swf_stage_sink: a scope, and the mask's geometry through the same draw
 * call an ordinary character uses. Nobody asks the backend for a buffer, and
 * nobody asks it "is pixel x,y masked", which is the query a tile accelerator
 * cannot answer because it never has the whole mask anywhere at once.
 *
 *
 * What the PVR does instead
 * -------------------------
 *
 * Modifier volumes. They exist for exactly this: geometry that is not drawn and
 * that changes how other polygons in the tile are shaded. It is implemented in
 * gfx/pvr/ps_swf_pvr.c and it is exact on hardware; what follows is the
 * mapping it was built from, one correction the API forced, and the two
 * measurements that got it working. The four sink calls map like this.
 *
 *   clip_begin   nothing but a state change. The next draws go to the modifier
 *                list rather than the polygon list.
 *   draw         the mask's shape through ps_swf_tess_shape, exactly as for a
 *                visible character, with the triangles written as
 *                pvr_modifier_vol_t into PVR_LIST_OP_MOD or PVR_LIST_TR_MOD
 *                under a header from pvr_mod_compile. Every triangle but the
 *                last carries PVR_MODIFIER_OTHER_POLY.
 *   clip_apply   close the volume: resubmit the last triangle's header with
 *                PVR_MODIFIER_INCLUDE_LAST_POLY. From here the content is
 *                submitted through pvr_poly_cxt_col_mod and pvr_poly_mod_compile
 *                as pvr_vertex_pcm_t, which carries two colours per vertex -
 *                argb1 used inside the volume, argb0 outside. The mask is
 *                applied by putting the real colour in argb1 and an alpha of
 *                zero in argb0. Outside the mask the polygon still rasterises
 *                and contributes nothing.
 *   clip_end     go back to the single-parameter header.
 *
 * The storage that costs is the modifier list's object pointer buffer, which is
 * opb_sizes[1] and [3] in pvr_init_params_t: sized once at pvr_init for the
 * whole frame, per tile, and the same size whether the frame has one mask or
 * forty. There is no per-clip allocation and no full-screen anything. The
 * deferred architecture is what makes that work rather than what makes it hard -
 * the volume flag is a bit per pixel of the 32x32 tile being resolved, on the
 * chip, and it is gone before the tile is written out. gfx/pvr/ps_gfx_pvr.c
 * takes opb_sizes[3] at sixteen words, which across 20x15 tiles is 19KB of
 * video memory, and that is the whole storage cost of the feature.
 *
 * One thing in that mapping is wrong, and it is the thing that decides how much
 * of masking is expressible at all. The four calls above read as though the
 * volume and the content it confines are interleaved - open a volume, close it,
 * draw through it, open the next. They cannot be. A modifier volume goes to a
 * list of its own, and KOS's pvr_list_begin says a list may be submitted only
 * once per scene; its own modifier example submits the whole polygon list,
 * calls pvr_list_finish, and only then opens the modifier list. So the volumes
 * of a frame are one run of geometry submitted after every polygon that reads
 * them, and the flag they set is one bit per pixel that every modified polygon
 * in the frame reads the same way.
 *
 * One bit per pixel per scene is one mask per scene. Two masks in one frame
 * mark the same flag, so content confined by the first would appear inside the
 * second as well. That is not a thing to discover from a picture, so
 * ps_swf_pvr.c spends the volume on the first mask that needs one and gives
 * every later one its bounding box, and says which it did on the console's own
 * cost line. What nests properly is a rectangle against a volume, because the
 * two mechanisms are independent: the rectangle clips vertices on the CPU
 * before they are written and the volume marks pixels on the chip afterwards,
 * so a rectangle outside a general mask is applied to the volume's own
 * triangles as they are collected - route 2 below, at no cost - and a
 * rectangle inside one is applied to the content. General against general is
 * still the case with no cheap answer.
 *
 * Three details that are not obvious and that would bite an implementation:
 *
 * The polygon must be able to draw nothing outside the volume, and in the
 * opaque list it cannot - PVR_LIST_OP_POLY does not blend. Masked content
 * therefore goes in the translucent list with autosort disabled, so submission
 * order is the painter order the display list already guarantees. That is not
 * an imposition: a SWF composites back to front with per-shape alpha anyway.
 *
 * A volume's interior is decided by parity, so two overlapping triangles in one
 * volume cancel and leave a hole. The tessellator already guarantees the
 * triangles of one pass do not overlap - the header of ps_swf_tri_sink says so
 * and the area column in tricmp is what watches it - so a single-pass mask is
 * safe as one volume. A mask that is several passes, or a sprite holding
 * several overlapping characters, is not: each piece has to close its own
 * volume, and successive inclusion volumes accumulate. That is the load-bearing
 * connection between the tessellator's non-overlap property and this feature,
 * and it is worth stating because nothing about the picture would reveal it -
 * a parity hole looks like a mask that was authored with a hole in it.
 *
 * The flag is one bit, so it expresses one mask. Nesting - the intersection of
 * two masks - has three routes and the first covers most real content:
 *
 *   1. A mask that is an axis-aligned rectangle is not a volume at all, it is a
 *      clip of the content's triangles against a rectangle, done on the CPU
 *      while the vertices are being written. Exact, no volume, no list, and it
 *      composes under nesting because the intersection of two rectangles is a
 *      rectangle. Most Flash masks are a rectangular window over a scrolling
 *      sprite, so this is the case to detect first. The PVR's own user clip
 *      (PVR_USERCLIP_INSIDE with a PVR_HDR_USERCLIP header) does the same job
 *      for free but states its rectangle in 32-pixel tiles, so it is exact only
 *      on tile boundaries - useful as a coarse reject in front of the exact
 *      geometric clip, not as a replacement for it.
 *   2. Rectangle against general shape: clip the general mask's triangles to
 *      the rectangle, submit the result as the volume. Same cost as one mask.
 *   3. General against general: intersect the two triangle sets before
 *      submitting, or render the inner mask into a texture sized by its own
 *      bounding box and sample it as alpha. Both cost real work, and both are
 *      bounded by the mask rather than by the screen. This is the case that
 *      wants measuring on hardware before it is written, and it is why
 *      PS_SWF_CLIP_DEPTH is small rather than generous.
 *
 * Where the two backends genuinely differ is the edge, and only the edge. This
 * file multiplies eight-bit coverage, so a mask antialiases what it cuts; the
 * volume flag is one bit, so the PVR's mask edge is hard. Interior pixels are
 * identical either way, which is the class the tests assert exactly on, and the
 * generated files put every mask edge on an integer pixel boundary so both
 * backends produce the same counts rather than merely similar ones.
 *
 * What the console does with a volume, measured twice on two builds: not what
 * any of this says, and not any kind of mask.
 *
 * t_clipn frame 1 is one mask of one pass, eight volume triangles, the log
 * confirming the collection and the count. It measures 4484 differing interior
 * pixels in six groups, and it measured the same on the build before the vertex
 * budget was fixed, so it is the path and not an artefact of the frame being
 * cut. Fourteen candidate outcomes were built on the host and compared with the
 * same comparator, and none is 4484 in six: the mask marking nothing gives 2820
 * in one group, the flag inverted 3504 in two, the bounding box this replaces
 * 684 in one, the mask painted as artwork 3540 in two, the playhead having
 * missed the frame 2721 in two.
 *
 * The six groups say why none of them fits. The largest is 2820 - the content
 * gone entirely, exactly the "nothing marked" figure. The other five are solid
 * rectangles, and every one of them begins at a stage row congruent to 17
 * modulo 32 and stands eight rows tall: y 49, y 81, y 113, on a stage 120 rows
 * deep. Thirty-two rows is the tile the PVR renders into. So what is being
 * painted is not the mask in the wrong place, not the mask inside out, and not
 * the content anywhere - it is a fixed band of every tile the frame touches,
 * repeated down the screen, in a colour that quantises to 0xf0 in a channel
 * where neither the fill (0xe0) nor the mask (0xff) can land. Geometry locked
 * to the tile grid rather than to the artwork is the tile accelerator being
 * handed something it did not expect, not a mask rule misunderstood.
 *
 * The separation has since been run, and it lands on the submission rather
 * than on anything in this file's argument. Two axes - whether the modifier
 * list carries triangles, and whether the polygons carry two colours a vertex
 * - and all four corners measured against numbers predicted on the host
 * beforehand:
 *
 *   box      no volume, ordinary polygons     684 predicted, 684 measured
 *   modonly  no volume, two-colour polygons  2820 predicted, 2820 measured
 *   volonly  volume, ordinary polygons        684 predicted, 3217 measured
 *   vol      volume, two-colour polygons         0 predicted, 4484 measured
 *
 * The two cells with no volume in them hit their predictions exactly, so the
 * two-colour vertices and the polygon headers are not the fault. Both cells
 * with a volume miss, and volonly misses while containing no modified polygon
 * at all - ordinary polygons that never read a flag, wrong anyway, with the
 * same full-width tile-anchored bands. Submitting the modifier list is what
 * breaks the scene, and every question this file asks about parity, inclusion
 * and inversion is downstream of that.
 *
 * It was autosort. With the per-pixel sort disabled - which this browser did
 * because submission order was paint order and depth was then a constant
 * nobody had to think about - the hardware renders translucent polygons in
 * strip order, and a modifier volume submitted in that mode is not evaluated
 * the way every header here says. Turning the sort on made the same page
 * exact: zero differing interior pixels, a mask with a hole in it, on the
 * console. The picture is unchanged in every other respect, to the pixel,
 * because the z was always there and always right - so what the sort costs is
 * time and not correctness.
 *
 * The other theory was tested and failed, and is written down so that nobody
 * spends a capture re-deriving it from the same sentence: enlarging the
 * modifier list's object pointer buffer, thirty-two words instead of sixteen
 * and seven overflow blocks instead of three, changed nothing at all - 4484
 * before, 4484 after, the same six groups, not one pixel moved. KOS documents
 * too small an OPB as making "geometry flicker in and out of existence along
 * the tile boundaries", which described the symptom exactly and was not the
 * cause. A symptom matching a documented failure mode is a hypothesis, not a
 * diagnosis.
 *
 * Read from the KOS headers and its own example rather than run: everything
 * above comes from dc/pvr.h, dc/pvr/pvr_header.h,
 * examples/dreamcast/pvr/modifier_volume and from what the fields have to mean.
 * The three questions this file used to end on have not all been settled, and
 * saying which is which matters more now that gfx/pvr/ps_swf_pvr.c submits
 * volumes for real.
 *
 * Settled by the API, not by hardware: a volume is a separate list submitted
 * after the polygons, so the flag is one bit per pixel per scene shared by
 * every volume in it. That is what the paragraph above rests on and it is why
 * only one mask a frame gets a volume.
 *
 * Not settled, and it decides only how a mask made of several passes behaves:
 * whether successive inclusion volumes accumulate or replace. A mask of one
 * pass - which is what one filled shape is, and what every mask in the corpus
 * is - closes exactly one volume group and does not care. A mask of several
 * closes one group each, and if groups replace rather than accumulate only the
 * last one will mark. The picture says which: a multi-pass mask showing only
 * its last piece is "replace".
 *
 * Not settled, and load bearing within a group: whether a volume of coplanar
 * triangles marks by parity. It is why one group per pass is what the code
 * emits - the tessellator's non-overlap guarantee holds inside a pass and
 * nowhere else - and a wrong answer here shows as a hole where two pieces of
 * one mask overlap. Also the z: the volume sheet is submitted one depth step in
 * front of everything the movie reserved, on the reading that an inclusion
 * volume marks a pixel when an odd number of its surfaces lie in front of that
 * pixel. A sheet behind the content would mark nothing, and the whole of the
 * masked content would then be invisible, which is the loud failure rather than
 * the quiet one.
 *
 * Not settled and not used: what PVR_MODIFIER_EXCLUDE_LAST_POLY does to a flag
 * an earlier volume set, which is the thing that would decide whether
 * intersecting two general masks has a cheap form.
 *
 * The user clip rectangle's granularity is the fourth: the header carries
 * start_x through end_y as tile coordinates, so route 1 treats it as a coarse
 * reject rather than an exact clip, which is the safe reading whichever way it
 * turns out.
 */
#include "ps_swf_clip.h"
#include "ps_swf_mem.h"

#include <string.h>

/* Coverage times mask, both 0..255.
 *
 * The two ends are the whole reason for the rounding term. A mask that fully
 * covers a pixel has to leave the coverage bit-identical - anything else dims
 * every interior pixel of every masked shape by a step, which would show up as
 * a wall of interior differences in tricmp and would be a real defect, not a
 * rounding one. */
static unsigned mul255(unsigned a, unsigned b)
{
    return (a * b + 127u) / 255u;
}

static_assert((255u * 255u + 127u) / 255u == 255u,
              "a mask that covers a pixel must leave its coverage untouched");
static_assert((255u * 0u + 127u) / 255u == 0u,
              "a mask that misses a pixel must erase it");

/* dest is the innermost mask still collecting geometry, and `by` the outermost
 * thing that confines what is being written. They are recomputed rather than
 * tracked incrementally because the states only change on push and pop, and a
 * four-entry scan there is cheaper than a rule to get wrong. */
static void clip_restate(ps_swf_clip *c)
{
    int k;

    c->dest = -1;
    for(k = c->n - 1; k >= 0; k--)
        if(!c->applied[k]) {
            c->dest = k;
            break;
        }
    /* Only the topmost plane, and only once applied. Every plane already holds
     * its own geometry intersected with whatever was in force when it was
     * pushed, so the innermost one is the whole intersection and the ones under
     * it need not be consulted. That is what keeps the cost of nesting one
     * multiply per pixel rather than one per level. */
    c->by = (c->n > 0 && c->applied[c->n - 1]) ? c->n - 1 : -1;
}

int ps_swf_clip_init(ps_swf_clip *c, const ps_swf_view *v,
                     ps_swf_span_fn out, void *user)
{
    memset(c, 0, sizeof *c);
    c->view = v;
    c->out  = out;
    c->user = user;
    c->dest = -1;
    c->by   = -1;
    if(v->w <= 0 || v->h <= 0)
        return -1;
    return 0;
}

void ps_swf_clip_free(ps_swf_clip *c)
{
    int k;

    for(k = 0; k < PS_SWF_CLIP_DEPTH; k++)
        ps_swf_dealloc(c->plane[k]);
    memset(c, 0, sizeof *c);
}

int ps_swf_clip_failed(const ps_swf_clip *c)
{
    return c->failed;
}

size_t ps_swf_clip_bytes(const ps_swf_clip *c)
{
    size_t total = 0;
    int    k;

    for(k = 0; k < PS_SWF_CLIP_DEPTH; k++)
        if(c->plane[k])
            total += (size_t)c->view->w * (size_t)c->view->h;
    return total;
}

void ps_swf_clip_begin(ps_swf_clip *c)
{
    size_t n = (size_t)c->view->w * (size_t)c->view->h;

    if(c->failed || c->n >= PS_SWF_CLIP_DEPTH) {
        /* The walker checks the same limit before pushing, so reaching here
         * means the two disagree. Refusing rather than clamping keeps that a
         * missing mask instead of an unbalanced push. */
        c->failed = 1;
        return;
    }
    if(!c->plane[c->n]) {
        c->plane[c->n] = ps_swf_alloc(n);
        if(!c->plane[c->n]) {
            c->failed = 1;
            return;
        }
    }
    memset(c->plane[c->n], 0, n);
    c->applied[c->n] = 0;
    c->n++;
    clip_restate(c);
}

void ps_swf_clip_apply(ps_swf_clip *c)
{
    int k;

    if(c->failed || c->n == 0)
        return;
    k = c->n - 1;

    /* Fold in whatever was in force when this mask was pushed, once, here -
     * rather than multiplying by every live plane on every span. Nothing
     * between the push and now can have changed that plane: only deeper masks
     * can be pushed and popped in between, and they sit above it. */
    if(k > 0 && c->applied[k - 1]) {
        size_t   n = (size_t)c->view->w * (size_t)c->view->h;
        size_t   i;
        uint8_t *a = c->plane[k];
        uint8_t *b = c->plane[k - 1];

        for(i = 0; i < n; i++)
            a[i] = (uint8_t)mul255(a[i], b[i]);
    }
    c->applied[k] = 1;
    clip_restate(c);
}

void ps_swf_clip_end(ps_swf_clip *c)
{
    if(c->n == 0)
        return;
    c->n--;
    clip_restate(c);
}

void ps_swf_clip_span(void *user, int y, int x0, int x1, uint8_t cov,
                      ps_swf_rgba color)
{
    ps_swf_clip   *c = user;
    const uint8_t *m;
    uint8_t       *dst;
    int            x, run_start, run_val;

    if(c->failed)
        return;                 /* better a missing shape than an unmasked one */
    if(y < 0 || y >= c->view->h)
        return;
    if(x0 < 0)          x0 = 0;
    if(x1 > c->view->w) x1 = c->view->w;
    if(x0 >= x1)
        return;

    m = c->by >= 0 ? c->plane[c->by] + (size_t)y * c->view->w : NULL;

    if(c->dest >= 0) {
        /* Mask geometry, so what is being written is coverage and not a
         * picture: the colour and the alpha are dropped. A Flash mask is its
         * outline and nothing else - a half-transparent mask shape hides
         * nothing extra, and one filled in any colour masks the same. Overlap
         * unions by taking the larger, which is what two overlapping contours
         * of one mask mean. */
        dst = c->plane[c->dest] + (size_t)y * c->view->w;
        for(x = x0; x < x1; x++) {
            unsigned v = m ? mul255(cov, m[x]) : cov;

            if(v > dst[x])
                dst[x] = (uint8_t)v;
        }
        return;
    }

    if(!m) {
        c->out(c->user, y, x0, x1, cov, color);
        return;
    }

    /* Runs of equal masked coverage, so a run crossing the flat interior of a
     * mask stays one call. The sink's contract - runs in increasing x, never
     * overlapping - survives, since these are the same span cut up. */
    run_start = x0;
    run_val   = (int)mul255(cov, m[x0]);
    for(x = x0 + 1; x <= x1; x++) {
        int v = x < x1 ? (int)mul255(cov, m[x]) : -1;

        if(v != run_val) {
            if(run_val > 0)
                c->out(c->user, y, run_start, x, (uint8_t)run_val, color);
            run_start = x;
            run_val   = v;
        }
    }
}
