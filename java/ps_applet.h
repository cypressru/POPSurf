/* Java applet loading, execution, and rendering. */
#ifndef PS_APPLET_H
#define PS_APPLET_H

#include "ps_types.h"
#include "ps_config.h"
#include "ps_jvm.h"
#include "ps_jgfx.h"
#include "../gfx/ps_gfx.h"
#include "../net/ps_url.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Each live applet owns a JVM and an ARGB framebuffer. */
#define PS_APPLET_MAX 8

/* Ceiling on one applet's box. The surface is 4 bytes a pixel in main memory
 * plus a padded texture in VRAM, and a page is free to ask for 4000x4000. */
#define PS_APPLET_MAX_DIM    512
#define PS_APPLET_MAX_PIXELS (320 * 240)

/* Maximum classes loaded by one applet. */
#define PS_APPLET_CLASSES 32

/* A class name in internal form: "Foo", "com/example/Sprite". */
#define PS_APPLET_NAME_MAX 96

/* Applet images remain in main-memory ARGB for CPU blitting. */
#define PS_APPLET_IMAGES     8
#define PS_APPLET_IMG_PIXELS (256 * 256)

/* Fixed parameter limits bound memory consumed by untrusted markup. */
#define PS_APPLET_PARAMS      16
#define PS_APPLET_PARAM_NAME  32
#define PS_APPLET_PARAM_VALUE 160

/* Audio clips one applet may hold. Period applets load a small fixed set up
 * front - a click, a bounce, a background loop - and keep them for their whole
 * life, so this is a count of distinct sounds and not of plays. */
#define PS_APPLET_CLIPS 4

typedef struct ps_applet_cache ps_applet_cache;

/* Asks for a class file to be fetched. Must return immediately; the bytes come
 * back through ps_applet_deliver. */
typedef void (*ps_applet_request_fn)(void *user, const char *url);

typedef struct {
    ps_texture tex;
    int        w, h;        /* the box the page asked for */
    float      u1, v1;      /* used fraction of the padded texture */
    int        ready;
} ps_applet_view;

ps_applet_cache *ps_applet_cache_create(const ps_gfx_backend *gfx,
                                        ps_applet_request_fn request,
                                        void *user);
void ps_applet_cache_destroy(ps_applet_cache *c);

/* Text backend for applet drawString. Borrowed; must outlive the cache. */
void ps_applet_cache_set_text(ps_applet_cache *c, const ps_jtext_ops *ops);

/* Looks the applet up, requesting it on first sighting. Returns NULL until it
 * has painted a frame. The w/h are the element's box, needed at request time
 * because the surface is allocated before the class arrives.
 *
 * `archive` is the resolved .jar URL when the page named one, empty otherwise.
 * When present it is fetched instead of the class file and every .class inside
 * it is unpacked; `url` still identifies the applet and names its main class.
 *
 * Dependencies resolve against the directory `url` sits in, which is what
 * codebase means once it has been folded into the URL. */
const ps_applet_view *ps_applet_get(ps_applet_cache *c, const char *url,
                                    const char *archive, int w, int h);

/* Hands over the element's <param name= value=> children and the URL of the
 * page carrying the applet.
 *
 * Kept out of ps_applet_get because the slot has to exist before there is
 * anywhere to put a parameter, and get is what creates it - so the document
 * calls this immediately after, on every draw. The first call wins: an applet
 * reads its parameters in init(), and a page laying out again must not change
 * them underneath one that is already running.
 *
 * Names and values are copied. Anything past PS_APPLET_PARAMS is dropped. */
void ps_applet_set_params(ps_applet_cache *c, const char *url,
                          const char *doc_base, const char *const *names,
                          const char *const *values, int n);

/* Paints an applet as geometry, straight into the page.
 *
 * This is the fast path: no pixel buffer, no ARGB4444 pack, no texture upload.
 * The applet's paint() runs during the page's own draw and its drawing becomes
 * quads the tile accelerator fills, which is what the hardware is for.
 *
 * ops/user carry the two shapes everything reduces to - a rectangle and a run
 * of text. x/y place the applet's origin on the page; w/h are its box.
 *
 * Returns non-zero when it painted. Zero means this applet needs the software
 * path - it used copyArea or drew an image - and the caller should fall back
 * to the texture it already has. */
int ps_applet_draw(ps_applet_cache *c, const char *url,
                   const struct ps_jvec_ops *ops, void *user,
                   int x, int y, int w, int h);

/* Clears every applet's on-screen flag, which the page's draw then
 * re-establishes. Must be called only when the page is actually being drawn -
 * on a frame replayed from the retained list nothing draws, so nothing would
 * set the flags again and every applet would stop. */
void ps_applet_cache_page_begin(ps_applet_cache *c);

/* Records where an applet sits in the document. Set during the page's draw,
 * which is the only place its position is known - and now that a page is drawn
 * once and replayed at every scroll position, the answer has to be in document
 * space rather than screen space, or it would only ever be right for the
 * scroll position the recording was made at. */
void ps_applet_set_box(ps_applet_cache *c, const char *url, int doc_y, int h);

/* The window into the document, this frame. Visibility is worked out from this
 * and the boxes above, every frame, because scrolling no longer redraws and so
 * no longer has a chance to say what moved into view. */
void ps_applet_set_view(ps_applet_cache *c, int scroll_y, int view_h);

/* Hands over a fetched class file. Runs the applet and uploads the frame.
 * ok == 0 marks it permanently failed so it is never requested again.
 * Returns non-zero if layout should re-run. */
int ps_applet_deliver(ps_applet_cache *c, const char *url, int ok,
                      const void *data, size_t len);

/* Forgets everything, on navigation. */
void ps_applet_cache_clear(ps_applet_cache *c);

/* Repaints applets that want repainting. Returns non-zero if any frame
 * changed. Currently every applet is static after its first paint - there is
 * no Thread, so nothing asks - and this exists so that the day repaint() does
 * something, the shell already calls it. */
int ps_applet_cache_tick(ps_applet_cache *c, int dt_ms);

/* Why an applet is not on screen, for the placeholder the document draws.
 * A blank box tells the reader nothing; "no class file" and "needs a JVM
 * feature we do not have" are different problems with different fixes. */
const char *ps_applet_status(ps_applet_cache *c, const char *url);

/* --- the bridge the runtime uses -----------------------------------------
 *
 * java.applet.Applet.getImage and java.awt.Graphics.drawImage reach the
 * browser through these. They act on whichever applet is currently executing,
 * which the interpreter guarantees is exactly one - so the alternative would
 * be threading a slot pointer through every native call in ps_jre for the
 * sake of three of them.
 */

/* Resolves rel against the applet's own directory, the way getCodeBase does. */
void ps_applet_image_base(char *out, size_t n, const char *rel);

/* Registers an image URL and returns a one-based handle, or 0. Fetching starts
 * immediately; the applet gets pixels whenever they land, which is the same
 * contract real AWT offers. */
int ps_applet_want_image(const char *url);

/* Decoded pixels for a handle, or NULL while it is in flight. */
const uint32_t *ps_applet_image_px(int handle, int *w, int *h);

/* java.applet.Applet.getParameter, on the running applet. NULL when the page
 * did not name it, which is the answer applets branch on constantly.
 *
 * The match is case-insensitive. Pages of the period spelled a name whichever
 * way suited them - TEXT, Text and text in the same document - and every
 * browser that ran these applets folded case, so an applet asking for "text"
 * expects to find <param name="TEXT">. Matching exactly would leave it with
 * null and its default behaviour, which is exactly the silently-wrong picture
 * this exists to avoid. */
const char *ps_applet_param(const char *name);

/* getCodeBase and getDocumentBase.
 *
 * The first is the directory the applet's class came from; the second is the
 * page's own URL, filename included, which is what the real API returns - so
 * neither can be joined to a relative name by concatenation, and both go
 * through ps_applet_url_join below. Both are absolute in the browser; in a
 * host test they are whatever path the runner was given. */
const char *ps_applet_code_base(void);
const char *ps_applet_doc_base(void);

/* Resolves rel against base the way java.net.URL(URL, String) does, which is
 * what getImage(getCodeBase(), "x.gif") is really asking for: the base's last
 * path component is replaced, an absolute rel wins outright, and a root
 * relative one keeps only the scheme and host. An empty base means the
 * applet's own directory, which is what the runtime used to return for both
 * bases and what a page that passes null gets. */
void ps_applet_url_join(char *out, size_t n, const char *base,
                        const char *rel);

/* --- audio ---------------------------------------------------------------
 *
 * java.applet.AudioClip. Registers a clip by URL and returns a one-based
 * handle, the same shape getImage uses.
 *
 * Nothing is fetched and nothing is played - see the note in ps_applet.c for
 * why, and what would have to be added. The object model is real so that an
 * applet which loads sound runs and draws; before this it stopped dead on a
 * class the runtime had never heard of.
 */
int  ps_applet_want_clip(const char *url);
void ps_applet_clip_play(int handle, int loop);
void ps_applet_clip_stop(int handle);

/* --- input ---------------------------------------------------------------
 *
 * The browser forwards a click or a key inside the applet's box. Coordinates
 * are relative to that box, which is what an applet expects: it was written
 * against a Component that starts at 0,0.
 *
 * Both event models are served, and exactly one of them runs per applet.
 *
 * Java 1.0's callbacks - mouseDown, mouseUp, mouseDrag, mouseMove, keyDown -
 * are plain methods on Applet, so the browser calls the method if the applet
 * defines one and nothing has to be registered. Java 1.1's listeners are
 * registered, and registering any of them retires the 1.0 methods on that
 * component for good. That is AWT's own rule, checked against a real JDK, and
 * it is the only arrangement in which an applet cannot be handed one click
 * twice. Whichever model the applet chose, the call below is the same.
 *
 * `key` is a Java 1.0 key value: the character for anything that has one, and
 * Event.HOME(1000) through Event.F12(1019) for the keys that do not. The 1.1
 * side translates - VK_LEFT is 37 where Event.LEFT is 1006 - so the browser
 * only ever has to speak one of the two.
 *
 * Returns non-zero if the applet handled it, so the browser knows not to treat
 * the press as a page click as well.
 */
int ps_applet_mouse(ps_applet_cache *c, const char *url, int x, int y,
                    int down, int dragging);
int ps_applet_key(ps_applet_cache *c, const char *url, int key, int down);

/* Tells the cache which applet the pointer is inside, in that applet's own
 * coordinates. NULL for none, which is where the pointer is most of the time.
 *
 * Call it every frame. It compares against the applet it was told last time
 * and only does anything when that changed, which is what turns a position
 * the browser has anyway into the mouseEntered and mouseExited the applet is
 * waiting for. Doing it from the rectangle instead would refire on every
 * scroll, because the rectangle moves and the pointer does not. */
void ps_applet_set_hover(ps_applet_cache *c, const char *url, int x, int y);

/* Which applet the keyboard goes to, set by a press inside one.
 *
 * An applet is the only thing in this browser that can want a key, and there
 * is no tab order to reach it with, so a click is the whole focus model. */
void        ps_applet_set_focus(ps_applet_cache *c, const char *url);
const char *ps_applet_focus(const ps_applet_cache *c);

/* Prints the applet's heap and collection counters. Host tests only. */
void ps_applet_heap_report(ps_applet_cache *c, const char *url);

/* Writes an applet's current surface to a PPM. For host tests only: the
 * browser never needs this, and it is the one way to look at what an applet
 * drew without a console attached. */
int ps_applet_dump_ppm(ps_applet_cache *c, const char *url, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* PS_APPLET_H */
