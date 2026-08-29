/* Document: parse, layout, paint.
 *
 * This is a C API over litehtml, which is C++. The implementation in
 * ps_document.cpp is the only C++ translation unit in the project and the only
 * place litehtml headers appear; every other file, and the whole public
 * surface, is C. litehtml's document_container is an abstract class, so some
 * C++ is unavoidable, but it stays behind this header.
 *
 * If the engine ever moves off litehtml to gumbo plus our own layout, this
 * header is the seam that does not change.
 */
#ifndef PS_DOCUMENT_H
#define PS_DOCUMENT_H

#include "ps_types.h"
#include "ps_paint.h"
#include "ps_text.h"
#include "ps_image.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ps_document ps_document;

/* images may be NULL, in which case every image lays out as an empty box. */
ps_document *ps_document_create(ps_paint *paint, ps_text_cache *text,
                                ps_image_cache *images,
                                int view_w, int view_h);

/* Base URL for resolving relative subresource and link references. */
void ps_document_set_base(ps_document *doc, const char *base_url);

/* Where <applet> elements get their frames. Optional: without one, applets lay
 * out at their declared size and draw a placeholder saying why they are empty,
 * which is what a build with the Java subsystem compiled out should do. */
struct ps_applet_cache;
void ps_document_set_applets(ps_document *doc, struct ps_applet_cache *a);

/* Offers a pointer event to any applet under the point, in viewport
 * coordinates. Returns non-zero when an applet took it, in which case the page
 * must not also see it - an applet's box is its own.
 *
 * down: 1 press, 0 release, -1 move. */
int ps_document_applet_input(ps_document *doc, int x, int y, int scroll_y,
                             int down, int dragging);

/* Where the pointer is, every frame, so an applet can be told when it was
 * entered and when it was left. Cheap when nothing crossed, which is almost
 * always. */
void ps_document_applet_hover(ps_document *doc, int x, int y, int scroll_y);

/* A key for whichever applet was last clicked. `key` is a Java 1.0 key value -
 * the character itself, or Event.HOME(1000)..Event.F12(1019). Returns non-zero
 * if the applet took it, so the browser knows not to also scroll. */
int ps_document_applet_key(ps_document *doc, int key, int down);

/* Whether any applet currently holds the keyboard. The shell asks so it knows
 * whether the d-pad is scrolling the page or driving an applet. */
int ps_document_applet_focused(const ps_document *doc);
void         ps_document_destroy(ps_document *doc);

/* Parses and lays out in one step. Returns 0 on success. The buffer is not
 * retained. */
int ps_document_load_memory(ps_document *doc, const char *html, size_t len);

/* Re-lay out at a new width, e.g. after a display mode change. */
void ps_document_render(ps_document *doc, int width);

/* Changes the height of the layout viewport and re-lays out.
 *
 * Chrome that appears and disappears along an edge is what needs this: the
 * page has to be told it has fewer lines, or vh units, percentage heights and
 * frameset rows all measure against a viewport that is no longer there. Width
 * is ps_document_render's business; this deliberately does not touch it,
 * because a height change never reflows a line of text and re-running layout
 * at the same width is nearly free by comparison. */
void ps_document_set_view_h(ps_document *doc, int height);

/* --- framesets ------------------------------------------------------------
 *
 * A frameset page is several independent documents side by side, each with its
 * own URL, base, scroll and layout viewport. litehtml has no notion of them at
 * all, so the whole mechanism is ours.
 *
 * The shell drives loading: after a page loads, poll for frames whose source
 * has not been requested yet, resolve and fetch each, then hand the bytes back
 * with ps_document_frame_load.
 */
int         ps_document_is_frameset(const ps_document *doc);
int         ps_document_frame_count(const ps_document *doc);

/* Non-zero when this frame still needs fetching. */
int         ps_document_frame_pending(const ps_document *doc, int i);
const char *ps_document_frame_src(const ps_document *doc, int i);
void        ps_document_frame_mark_requested(ps_document *doc, int i,
                                             const char *abs_url);
int         ps_document_frame_load(ps_document *doc, int i, const char *html,
                                   size_t len, const char *base);

void ps_document_frame_rect(const ps_document *doc, int i, ps_rect *out);
int  ps_document_frame_at(const ps_document *doc, int x, int y);
void ps_document_frame_scroll(ps_document *doc, int i, int dy);

/* What <meta http-equiv="refresh"> asked for.
 *
 * ps_document_refresh_ms returns the requested delay in milliseconds, or -1
 * when the page asked for nothing. Zero is a real answer meaning "go now", so
 * it cannot double as absent.
 *
 * The URL is empty when the page asked to reload itself rather than move on.
 * Resolving that is the shell's job, not this one's: after redirects only the
 * shell knows what the current address actually is.
 *
 * The shell owns the waiting, too. It has to cancel the countdown when the
 * user navigates first - arriving somewhere and then being yanked away by a
 * timer belonging to the page before it is worse than not supporting this at
 * all - and it has to bound how many refreshes may chain, or two pages
 * pointing at each other loop forever. */
int         ps_document_refresh_ms(const ps_document *doc);
const char *ps_document_refresh_url(const ps_document *doc);

/* Background music the page asked for, via <bgsound> or <embed>. Absolute,
 * empty when the page is silent. */
const char *ps_document_bgsound(const ps_document *doc);
int         ps_document_bgsound_loop(const ps_document *doc);

/* The first Flash movie the page embeds, absolute, empty when it embeds none.
 * <embed src>, <object data> and <param name="movie"> are all read, because a
 * page of this era usually carries all three nested inside each other.
 *
 * One per page rather than all of them. A movie is a whole player - a parsed
 * file, a playhead, and a set of textures in a video memory that the page's own
 * images are already spending - and pages that carry a second one carry it as
 * an alternative to the first rather than as a second thing to look at.
 *
 * ps_document_swf_rect gives the box layout put it in, in document coordinates,
 * and returns zero until layout has run. It is asked per frame rather than
 * recorded, because a subresource landing re-runs layout and moves it. */
const char *ps_document_swf(const ps_document *doc);
int         ps_document_swf_rect(const ps_document *doc, ps_rect *out);

/* A page may supply its own instrument set, replacing the session default:
 *   <meta name="soundbank" content="chip.psb">
 *   <link rel="soundbank" href="chip.psb">
 * Absolute, empty when the page is happy with the default. */
const char *ps_document_soundbank(const ps_document *doc);

/* Advances animated content: <marquee> scroll position and <blink> phase.
 * Wall-clock driven, so both keep their authored speed at PAL 50Hz.
 *
 * Returns non-zero if the page contains anything that animates, and therefore
 * cannot be replayed from a retained list. Deliberately answered per page
 * rather than per tick: knowing that a <marquee> moved *this* frame would take
 * threading a changed flag out of every animated element, and a page with a
 * marquee on it is going to be redrawn most frames anyway. A page with none -
 * which is nearly all of them - gets the fast path. */
int ps_document_tick(ps_document *doc, int dt_ms);

/* Re-runs layout at the current width. Called when a subresource arrives and
 * changes what the page measures to. */
void ps_document_relayout(ps_document *doc);

int         ps_document_height(const ps_document *doc);
const char *ps_document_title(const ps_document *doc);

/* Paints the document scrolled by scroll_y. Must sit between
 * ps_paint_begin and ps_paint_end. */
void ps_document_draw(ps_document *doc, int scroll_y);

/* Paints the whole document at its own coordinates, unscrolled and unclipped
 * by the viewport, so it can be recorded once and replayed at any scroll
 * position.
 *
 * The caller must have widened ps_paint's bounds to the document, or every
 * box below the first screenful is clipped away and the recording holds only
 * what happened to be visible - which is the one thing that would make it
 * useless for the case it exists for. */
void ps_document_draw_all(ps_document *doc, int height);

/* Called when a link is activated. The URL is already resolved against the
 * document base. Do not load from inside the callback: it fires deep inside
 * litehtml's own traversal, and destroying the document there would pull the
 * ground out from under it. Record it and act after the frame. */
typedef void (*ps_navigate_fn)(void *user, const char *url,
                               const char *post_body);

void ps_document_set_navigate_cb(ps_document *doc, ps_navigate_fn cb,
                                 void *user);

/* Viewport coordinates; the document applies scroll itself. Each returns
 * non-zero when something visibly changed and a repaint is needed. */
int ps_document_mouse_move(ps_document *doc, int x, int y, int scroll_y);
int ps_document_mouse_down(ps_document *doc, int x, int y, int scroll_y);
int ps_document_mouse_up(ps_document *doc, int x, int y, int scroll_y);

/* Non-zero when the last hit test landed on something clickable, so the shell
 * can show a link cursor. */
int ps_document_cursor_is_link(const ps_document *doc);

/* Computed CSS cursor keyword under the last hit test, e.g. "pointer" or
 * "text". Feed to ps_cursor_role_from_css so the art follows the page. Never
 * NULL; empty means nothing was hit. */
const char *ps_document_cursor_css(const ps_document *doc);

/* Focused form control, for text entry. ps_document_focused_editable returns
 * non-zero when the focus is a field the on-screen keyboard should edit;
 * ps_document_focused_value gives its current contents and label. */
int         ps_document_focused_editable(const ps_document *doc);
const char *ps_document_focused_value(const ps_document *doc);
const char *ps_document_focused_label(const ps_document *doc);
void        ps_document_set_focused_value(ps_document *doc, const char *text);

/* Viewport rect of the element currently under the cursor. Returns non-zero
 * when out was filled. Used to ring the hover target. */
int ps_document_hover_rect(const ps_document *doc, int scroll_y, ps_rect *out);

#ifdef __cplusplus
}
#endif

#endif /* PS_DOCUMENT_H */
