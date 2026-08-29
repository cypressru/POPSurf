/* Chrome widgets, drawn once and shared.
 *
 * The on-screen keyboard grew this look first - a metal key, a recessed field,
 * a panel with a specular line - and the toolbar wants exactly the same one.
 * Reimplementing it there would mean two sets of band colours drifting apart
 * over time, and "the toolbar buttons don't quite match the keyboard" is the
 * kind of bug nobody files and everybody notices.
 *
 * So the material lives here and the palette lives in ps_theme.h. Callers pass
 * rectangles and states; nothing in this file knows what a key or a button is
 * for.
 *
 * This is core rather than shell because the keyboard is core: a libpopsurf
 * embedder that draws its own chrome still gets an OSK, and the OSK still has
 * to be made of something.
 */
#ifndef PS_SKIN_H
#define PS_SKIN_H

#include "ps_types.h"
#include "ps_paint.h"
#include "ps_text.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Rect helpers in whole pixels, w/h rather than x1/y1. Every caller was
 * writing this by hand. Non-positive sizes draw nothing rather than
 * degenerate, so callers can subtract padding without checking first. */
void ps_skin_fill(ps_paint *p, int x, int y, int w, int h, ps_color c);
void ps_skin_grad(ps_paint *p, int x, int y, int w, int h, ps_color top,
                  ps_color bottom);

/* Hairline box, PS_STROKE thick, drawn inside the rect. */
void ps_skin_frame(ps_paint *p, int x, int y, int w, int h, ps_color c);

/* Panel body: rim, vertical fall-off, specular line under the top edge. */
void ps_skin_panel(ps_paint *p, int x, int y, int w, int h);

/* The same body without the specular.
 *
 * A specular line says "this is a raised object with a lit top edge", which is
 * true of a panel floating over the page and false of a band welded to the
 * bottom of the screen - there it reads as a stray bright line under the rule
 * with nothing to explain it. */
void ps_skin_band(ps_paint *p, int x, int y, int w, int h);

/* Three stacked bars in the middle of a rect: the menu button. */
void ps_skin_burger(ps_paint *p, const ps_rect *r, ps_color c);

/* Recessed well for a value the user reads or edits - the keyboard's edit
 * field, the toolbar's address field. Lit from below, which is what makes it
 * read as sunk rather than raised. */
void ps_skin_well(ps_paint *p, int x, int y, int w, int h);

/* Metal key or button.
 *
 * The three states are independent on purpose rather than one enum: a held
 * Shift is pressed *and* selected at the same time, and collapsing that into a
 * single value loses the combination the keyboard actually needs. `off`
 * outranks both - an unavailable control is never also lit.
 */
void ps_skin_key(ps_paint *p, const ps_rect *r, int pressed, int active,
                 int off);

/* Ink to draw a caption with, matched to whatever ps_skin_key was passed. */
ps_color ps_skin_ink(int active, int off);

/* Accent ring just outside a control, for "the pointer is on this". Drawn
 * outside so it never eats into the control's own edge. */
void ps_skin_ring(ps_paint *p, const ps_rect *r, ps_color c);

/* Solid triangle, dir -1 pointing left and +1 pointing right.
 *
 * Built from 2px vertical strips because the paint layer has rectangles and
 * nothing else, and because a 1px feature on a 480i field lands on alternate
 * scanlines and crawls. At the sizes chrome uses this is under ten quads. */
void ps_skin_tri(ps_paint *p, int x, int y, int w, int h, int dir, ps_color c);

/* Text at a baseline. Returns the advance width, so callers can lay out left
 * to right without measuring separately. A NULL font draws nothing and still
 * reports 0, which keeps a missing atlas from becoming a crash. */
int ps_skin_text(ps_paint *p, ps_text_cache *tc, int size, int x, int baseline,
                 const char *s, ps_color c);
int ps_skin_text_w(ps_text_cache *tc, int size, const char *s);

/* Centred on cx. */
void ps_skin_text_center(ps_paint *p, ps_text_cache *tc, int size, int cx,
                         int baseline, const char *s, ps_color c);

/* Copies s into out, trimming from the end and appending "..." if it will not
 * fit in max_w pixels. Used for the address field, where a URL is routinely
 * wider than the space it has. */
void ps_skin_text_elide(ps_text_cache *tc, int size, const char *s, int max_w,
                        char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* PS_SKIN_H */
