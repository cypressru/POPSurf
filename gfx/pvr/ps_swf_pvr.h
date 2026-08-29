/* The Dreamcast backend for the SWF player's stage sink.
 *
 * ps_swf_trisoft.c is the host sibling of the inner half of this: it takes the
 * same triangles and rasterises them so they can be compared against the span
 * renderer with no tolerance. This one submits them, which is the thing that
 * cannot be checked without a television.
 *
 * The whole of it goes into PVR_LIST_TR_POLY with autosort disabled, which is
 * the list gfx/pvr/ps_gfx_pvr.c already uses for the page, and for the same
 * reason: submission order is paint order, which is what a SWF display list
 * means. There is no second list and no second scene - a movie is submitted in
 * the middle of the page's own frame, between two ps_paint flushes.
 */
#ifndef PS_SWF_PVR_H
#define PS_SWF_PVR_H

#include "../../core/ps_types.h"
#include "../../swf/ps_swf.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ps_swf_pvr ps_swf_pvr;

/* How a mask that is not a rectangle is applied.
 *
 * VOL is the default and it is exact on hardware: t_clipn frame 1 - an eighty
 * by sixty mask with a forty by twenty hole wound out of it, over a hundred by
 * sixty rectangle - measures zero differing interior pixels against the
 * software renderer. Getting there needed the per-pixel sort turned on in
 * pvr_init, which is ps_gfx_pvr.h's story, and the other three modes are what
 * found that out.
 *
 *   BOX      no volume at all. A general mask takes its bounding box, which is
 *            what this file did before volumes existed. Kept because its
 *            answer is arithmetic rather than a hypothesis - 684 differing
 *            interior pixels on that page, one group, the hole showing through
 *            in red - so it is the control a later measurement is taken
 *            against.
 *   VOL      the volume is submitted and the content it confines is drawn with
 *            two colours a vertex, the second used inside the volume.
 *   VOLONLY  the volume is submitted and the content is drawn as ordinary
 *            single-colour polygons that ignore it, which should be BOX's
 *            picture to the pixel.
 *   MODONLY  the content is drawn with two colours and no volume is submitted,
 *            so nothing marks it anywhere and it should vanish entirely: 2820
 *            in one group.
 *
 * Those four are a truth table on two axes - whether the modifier list carries
 * triangles, whether the polygons carry two colours - and they are why the
 * fault was cornered in one round rather than guessed at. Under the broken
 * configuration BOX and MODONLY hit their predicted numbers exactly while VOL
 * and VOLONLY missed, and VOLONLY carries no modified polygon at all, which
 * said the submission was at fault and not the masking. They stay as the
 * regression lever: if a mask goes wrong again these are the four commands,
 * and three of them have their answer known in advance. */
#define PS_SWF_MASK_BOX     0
#define PS_SWF_MASK_VOL     1
#define PS_SWF_MASK_VOLONLY 2
#define PS_SWF_MASK_MODONLY 3
#define PS_SWF_MASK_MODES   4

ps_swf_pvr *ps_swf_pvr_create(void);
void        ps_swf_pvr_destroy(ps_swf_pvr *p);

/* Takes effect at the next ps_swf_pvr_begin. An unknown mode is ignored rather
 * than clamped: a run asking for a mode this build does not have should render
 * the way the build normally does, not the way the nearest number does. */
void        ps_swf_pvr_set_mask_mode(ps_swf_pvr *p, int mode);
int         ps_swf_pvr_mask_mode(const ps_swf_pvr *p);

/* Uploads a movie's gradient ramps and bitmaps. Returns 0 even when some of
 * them did not fit: a fill whose texture is missing draws its own fallback
 * colour, which ps_swf_fill's comment names as the answer for exactly this
 * case. -1 means the movie could not be bound at all.
 *
 * Bound once per movie rather than per frame. The textures are a function of
 * the file and nothing else - a colour transform is applied at the vertex, not
 * baked into the ramp - so rebuilding them per frame would be VRAM churn to
 * reproduce a constant. */
[[nodiscard]] int ps_swf_pvr_bind(ps_swf_pvr *p, const ps_swf_movie *m);
void              ps_swf_pvr_unbind(ps_swf_pvr *p);

/* Opens a submission scope. `clip` is the rectangle on screen the movie may
 * paint in, and `z` the depth its first pass takes; each pass after it takes
 * one step more, because the translucent list is depth-tested with GREATER and
 * two passes at one depth would lose the second. `steps` is how many passes
 * the caller reserved, and passes beyond it share the last depth rather than
 * climbing into whatever was reserved for the chrome.
 *
 * Must sit inside the page's own frame, between ps_paint_begin and
 * ps_paint_end, and the caller must have flushed ps_paint first - the batch
 * and these triangles both go to the same list, and order is everything. */
void ps_swf_pvr_begin(ps_swf_pvr *p, const ps_rect *clip, float z, float zstep,
                      int steps);
void ps_swf_pvr_end(ps_swf_pvr *p);

/* Hand this and the ps_swf_pvr to ps_swf_render_frame. */
const ps_swf_stage_sink *ps_swf_pvr_stage_sink(void);

/* What the last scope cost, split so that the three candidate explanations for
 * a slow frame can be told apart rather than argued about.
 *
 *   tris       triangles the tessellator produced
 *   mask_tris  of those, the ones measured for a mask and never submitted
 *   vol_tris   of those, the ones resubmitted as a modifier volume
 *   flat_tris  of those, the ones dropped for having no area at all
 *   strips     primitives that reached the TA - what the vertex buffer holds
 *   passes     state changes: one polygon context built and compiled each
 *   draws      characters tessellated, each paying ps_swf_tess_shape's setup
 *   us_state   time inside the state change
 *   us_draw    time inside the tessellator, us_state included
 *
 * A total says a frame was slow. These say whether it was slow per pass, per
 * character or per triangle, which are three different fixes. */
long   ps_swf_pvr_tris(const ps_swf_pvr *p);
long   ps_swf_pvr_mask_tris(const ps_swf_pvr *p);
long   ps_swf_pvr_vol_tris(const ps_swf_pvr *p);
long   ps_swf_pvr_flat_tris(const ps_swf_pvr *p);
long   ps_swf_pvr_strips(const ps_swf_pvr *p);
long   ps_swf_pvr_passes(const ps_swf_pvr *p);
long   ps_swf_pvr_draws(const ps_swf_pvr *p);
size_t ps_swf_pvr_vram(const ps_swf_pvr *p);

/* What the movie put in the TA's vertex buffer, what it was allowed, and
 * whether it hit the end.
 *
 * Reported rather than only enforced because the three readings are different
 * bugs: a frame well inside its budget that looks wrong is a rendering fault,
 * a frame that ran out is a picture missing its top layers, and a budget of
 * nearly nothing is the page having spent the buffer before the movie started.
 * Nothing else in the browser can tell those apart from the picture. */
size_t ps_swf_pvr_vtx(const ps_swf_pvr *p);
size_t ps_swf_pvr_vtx_budget(const ps_swf_pvr *p);
size_t ps_swf_pvr_vtx_page(const ps_swf_pvr *p);
int    ps_swf_pvr_vtx_full(const ps_swf_pvr *p);

/* Masks that were neither a rectangle nor the frame's one modifier volume, and
 * so were approximated by their own bounding box. Zero is an exact frame.
 *
 * mask_untextured is the other approximation and is counted apart because it
 * is a different fix: a pass with a texture drawn under a volume, which the
 * two-colour vertex cannot carry and which therefore took the bounding box
 * while the flat passes beside it were masked exactly. */
int    ps_swf_pvr_mask_inexact(const ps_swf_pvr *p);
int    ps_swf_pvr_mask_untextured(const ps_swf_pvr *p);

uint64_t ps_swf_pvr_us_state(const ps_swf_pvr *p);
uint64_t ps_swf_pvr_us_draw(const ps_swf_pvr *p);

#ifdef __cplusplus
}
#endif

#endif /* PS_SWF_PVR_H */
