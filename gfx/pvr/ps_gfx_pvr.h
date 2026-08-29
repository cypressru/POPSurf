/* What one PVR file needs from the other, and nothing a portable caller may
 * touch.
 *
 * ps_gfx.h is the backend-neutral interface and neither of these belongs in it:
 * both are facts about the tile accelerator that only exist because the page
 * and the SWF player submit into the same scene, through the same store queue,
 * out of the same vertex buffer.
 */
#ifndef PS_GFX_PVR_H
#define PS_GFX_PVR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The TA's vertex buffer, and how much of it this frame has spent.
 *
 * The buffer is a fixed allocation made once at pvr_init and it cannot be
 * resized while the PVR is up: pvr_init lays the frame buffers, the object
 * pointer buffers and the vertex buffer out from the bottom of VRAM and hands
 * everything above them to pvr_mem_malloc, so growing it means pvr_shutdown,
 * pvr_init and every texture on the page uploaded again. Overflowing it is
 * worse than either - the TA stops accepting data mid-scene and what comes out
 * is a torn frame, not a caught error.
 *
 * So a submitter that can produce an unbounded amount of geometry - which is
 * the SWF player and nothing else here - has to know what is left rather than
 * hope.
 *
 * `used` is the hardware's own answer: PVR_TA_VERTBUF_POS is the top of the
 * vertex buffer as the TA has actually filled it, and the frame's spend is that
 * against where it stood when the scene opened. It is read rather than counted
 * because the counted version was wrong on the console in the way that matters
 * - it reported the whole buffer spent on a page that had drawn a few hundred
 * quads, and the movie, being the one thing that checks, was the only thing
 * deleted by it. A number that decides whether to draw has to come from the
 * thing being measured.
 *
 * `counted` is what the submitters believe they wrote, kept so the two can be
 * printed side by side. They are not the same quantity and are not expected to
 * agree exactly: one is bytes pushed through the store queue, the other is
 * what the tile accelerator chose to store having read them, and the TA
 * rewrites what it is given into its own per-object form. On the console the
 * pair reads 9224 against 11392 for one page - close enough to be the same
 * frame, far enough apart that treating the counted figure as the size of the
 * buffer's contents was always wrong. The hardware decides; the counter is
 * there to show when the two stop tracking each other at all, which is how
 * this was caught. */
size_t ps_pvr_vtx_capacity(void);
size_t ps_pvr_vtx_used(void);
size_t ps_pvr_vtx_counted(void);
void   ps_pvr_vtx_charge(size_t bytes);

/* Which set of pvr_init parameters to come up with, chosen before gfx init.
 *
 * Two, and the second one exists to reproduce a fault rather than to offer a
 * choice.
 *
 *   BROWSER  what every page gets: the translucent list and its modifier list
 *            binned, the per-pixel sort on, three overflow blocks.
 *   STRIP    the same with autosort disabled, which is what this browser used
 *            until modifier volumes needed otherwise. Translucent polygons
 *            render in strip order and a modifier volume submitted in that
 *            mode corrupts the scene in bands locked to the tile grid. One
 *            command reproduces that, which is worth more than a paragraph
 *            claiming it.
 *
 * Two theories were tested and failed on the way here, and they are recorded
 * so that nobody spends a capture re-deriving them. Enlarging the modifier
 * list's object pointer buffer - thirty-two words instead of sixteen, seven
 * overflow blocks instead of three - changed nothing: 4484 differing interior
 * pixels before and 4484 after, the same six groups, not one pixel moved. That
 * theory had KOS's own sentence behind it, "geometry flickering in and out of
 * existence along the tile boundaries", which describes the symptom exactly
 * and was not the cause. Matching KOS's example configuration wholesale was
 * never needed once autosort alone was tried.
 *
 * The bins cost video memory once: a word per tile per list over 300 tiles,
 * which is 19KB for the modifier list. */
#define PS_PVR_CFG_BROWSER  0
#define PS_PVR_CFG_STRIP    1
#define PS_PVR_CFG_PROFILES 2

void        ps_pvr_set_init_profile(int profile);
const char *ps_pvr_profile_name(int profile);

/* Dithering on the way out to a 16-bit framebuffer, on or off.
 *
 * On is right for a television and it is what a person gets: five bits of red
 * and blue step by eight, so a colour like 0x24 does not exist and the display
 * alternates 0x20 and 0x28 between neighbours to average out to it. Off is
 * right for a measurement, and only because of what a measurement is - a
 * capture is compared pixel by pixel against a renderer that stores the colour
 * exactly, so the alternation reads as a fault at whichever pixels the
 * comparison's one-step tolerance happens not to cover. That is where two
 * counts of "wrong pixels on one scanline at a regular stride" came from, on
 * t_hole and t_stroke, and neither was a rendering fault at all.
 *
 * Turning it off for captures removes noise rather than coverage: this is a
 * fixed function of the video hardware applied to the finished framebuffer,
 * not a path anything here renders through, and the comparison still allows a
 * whole 565 step, so a real error of one step is still caught. */
void        ps_pvr_set_dither(int enable);

/* The other approximation the display path makes, and the same argument.
 *
 * KOS renders the scene to the framebuffer through a vertical scale of 0.999
 * on any cable that is not VGA - one output row per 1024/1025 rendered rows,
 * which means every row is a blend of two - and says why in its own header:
 * "having a value slightly below 1.0f gives the image a pleasant smoothing".
 * It is the flicker filter, it is right for an interlaced television, and it
 * is doing its job invisibly because a flat interior averaged with itself is
 * itself.
 *
 * It is visible in exactly one place: a horizontal edge, which comes out as a
 * ramp two or three rows deep. Vertical edges are untouched, which is what
 * identified it - a stroke measured hard to the pixel across its sides and
 * ramped at its top and bottom. Every other page in the corpus hid it, because
 * a one-row ramp lands inside the comparison's boundary band; a twenty-pixel
 * stroke has edges sharp enough to push the middle row of the ramp into the
 * interior count.
 *
 * So it goes off for a capture and stays on for a television, exactly as the
 * dithering does. Only ever off: 0.999 is KOS's choice for a cable this cannot
 * see, and putting it back is KOS's business rather than this browser's. */
void        ps_pvr_disable_vsmooth(void);

/* Called after the translucent polygon list is closed and before the scene is
 * finished, which is the only window in which another list can be opened.
 *
 * Modifier volumes are the reason this exists. A volume belongs to a list of
 * its own, a list may be submitted only once per scene, and the geometry that
 * decides a volume is discovered while the polygons it applies to are already
 * being written - so the volumes have to be held in RAM and submitted in one
 * run at the end. There is nowhere else in the frame to do that: the shell
 * ends the page's paint and the scene in the same call. */
void ps_pvr_set_list_hook(void (*fn)(void *), void *user);

#ifdef __cplusplus
}
#endif

#endif
