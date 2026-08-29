/* Unattended hardware runs: boot arguments, and a frame brought back.
 *
 * Every hardware check until now has been a person with a controller
 * describing what they saw, and that loop has produced wrong diagnoses in both
 * directions - a test font whose only glyph is a filled square read as "text
 * renders as green squares", a gradient testbed with a deliberate hard step in
 * it read as "gradients are two flat blocks". Both were correct renders, and
 * both would have been settled in a second by putting the console's pixels
 * next to the software renderer's. This is what makes that possible without a
 * human in the room.
 *
 * Two halves, and they are here together because neither is any use alone.
 *
 * Where to go. KOS drops dcload's argv - main() is called with (0, NULL), see
 * kernel/init.c and upstream PR #1250 - so the address to open cannot be
 * passed on the command line. The established answer in this ecosystem is a
 * file in the directory dcload maps to /pc, which the host writes before the
 * upload and the program reads at startup; BBALL2027/dcload.sh does exactly
 * this. So does this: <disc root>/.bootargs, one setting per line.
 *
 * What came back. The finished frame is written to a PPM under /pc, where the
 * host can compare it against tests/swf-host/swfrender's output for the same
 * content. A capture that is half of one frame and half of the next is worse
 * than no capture at all, because it looks exactly like a rendering fault, so
 * the timing is the part of this file worth reading: see ps_probe_after_frame.
 *
 * None of it exists on a shipped disc. ps_probe_init returns before it opens
 * anything unless the disc root is /pc - a pressed disc cannot carry a file
 * the developer wrote a minute ago, and has nowhere to write a PPM back to -
 * so the cost of all of this to a real build is one string compare at startup.
 * There is deliberately no button that triggers it: the entire point is that
 * nobody is at the console.
 */
#ifndef PS_PROBE_H
#define PS_PROBE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Reads <root>/.bootargs, if there is one. Returns the address to open instead
 * of the home page, or NULL to behave exactly as an ordinary boot does.
 *
 * root is what disc_root() answered, and NULL is a valid thing to pass. */
const char *ps_probe_init(const char *root);

/* Whether the pointer should be left out of this frame.
 *
 * A cursor parked in the middle of the screen lands on top of whatever is
 * being measured, and a bright arrow over a movie is precisely the kind of
 * thing that gets reported as a hole in the renderer. It is only ever hidden
 * while a capture is pending, and only in a run that asked for one. */
int ps_probe_hide_cursor(void);

/* The movie frame this run is to hold still on, or -1 for "play normally".
 *
 * A capture is compared against one frame rendered on the host, so a movie
 * that keeps playing turns the comparison into a picture of an arbitrary
 * frame - and an arbitrary frame of a testbed built to show a different answer
 * on every one of them looks exactly like a rendering fault. Stating the frame
 * is what makes two runs of the same page produce the same numbers. */
int ps_probe_frame(void);

/* The mask path this run is to take (a PS_SWF_MASK_* value), or -1 for the
 * build's own default. See ps_swf_pvr.h: what the hardware does with a
 * modifier volume takes several captures of one build that differ in one thing
 * each, and nobody is at the console to pick. */
int ps_probe_swfmask(void);

/* The pvr_init profile this run wants (a PS_PVR_CFG_* value), or -1 for the
 * browser's own. Read before gfx init, which is why every setting is now read
 * at the top of main: a value consulted before its file has been opened is
 * exactly the fault that cost three captures. */
int ps_probe_pvrcfg(void);

/* Whether this run wants dithering: 1 on, 0 off, -1 leave the hardware alone.
 *
 * An unattended run answers 0, because a dithered framebuffer holds a colour
 * as an alternating pair of the two the hardware can store and a comparison
 * against an exact renderer reads that as a fault - which it did, twice, on
 * two pages, as single pixels on one scanline at a regular stride. See
 * ps_gfx_pvr.h for why turning it off removes noise rather than coverage. */
int ps_probe_dither(void);

/* 0 to turn off the vertical smoothing KOS applies on a television cable, -1
 * to leave it alone. An unattended run answers 0: rendering the scene through
 * a 0.999 vertical scale makes every framebuffer row a blend of two, which is
 * a pleasant picture and a ramp two or three rows deep at every horizontal
 * edge - the last thing standing between t_stroke and zero. */
int ps_probe_vsmooth(void);

/* The part of the screen worth comparing, in screen pixels.
 *
 * The console draws a whole 640x480 page with chrome around it; the host
 * renders one movie at its stage size. Something has to say where in the frame
 * that movie ended up, and layout is the only thing that knows. Recorded into
 * the capture's PPM header so the comparison needs nothing but the file. */
void ps_probe_set_region(int x, int y, int w, int h);

/* Called once per drawn frame, immediately after the scene has been submitted
 * and before anything else is.
 *
 * `busy` is anything that means the page has not settled - a page, an asset or
 * a soundbank still in flight. Returns non-zero when the shell should exit,
 * which is how the run ends by itself and the host's dc-tool returns. */
int ps_probe_after_frame(int busy, int dt_ms);

#ifdef __cplusplus
}
#endif

#endif
