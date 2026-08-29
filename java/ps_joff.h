/* Offscreen images: Component.createImage, Image.getGraphics, and the
 * MediaTracker an applet uses to wait for a picture to arrive.
 *
 * This is one idiom, and it is the same four lines in every animated applet
 * written between 1995 and 2001:
 *
 *     Image    off = createImage(w, h);     // once, in init()
 *     Graphics og  = off.getGraphics();     // draw the frame into og
 *     ...
 *     g.drawImage(off, 0, 0, this);         // blit it in paint()
 *
 * It exists because AWT paints a component by clearing it and calling paint(),
 * so an applet that draws directly flickers. Here the browser composites a
 * finished surface anyway and nothing would flicker either way - but the
 * applets are written against the idiom, and an Image.getGraphics() that
 * returns null makes them throw on their second line.
 *
 * Split out of ps_jre.c because the ownership rules below are the whole of the
 * difficulty and they do not belong scattered through a dispatch table.
 */
#ifndef PS_JOFF_H
#define PS_JOFF_H

#include "ps_jvm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The ps_jre_call signature, for the classes this file owns. Sets *handled
 * when it took the call; leaves it clear so ps_jre.c's own tables get a look
 * at everything else - notably Graphics methods, and Images that came off the
 * network rather than out of createImage. */
int ps_joff_call(ps_jvm *vm, const char *cls, const char *name,
                 const char *desc, ps_jslot *args, int nargs, ps_jslot *ret,
                 int *handled);

/* Frees every offscreen surface belonging to vm.
 *
 * Must be called before ps_jvm_free: after it, vm->gc_head is gone and there
 * is no way left to tell which surfaces were this applet's. */
void ps_joff_release(ps_jvm *vm);

/* The pixels of an offscreen Image, borrowed.
 *
 * ps_jimg.c needs this because Image.getSource() is legal on a back buffer, and
 * an applet that draws a sheet of tiles for itself and then crops them out of
 * it is doing exactly what one that fetched the sheet does. The pointer is into
 * the table above and must not be freed or held across anything that could
 * reap a slot.
 *
 * Returns NULL for anything that is not one of this file's surfaces, which is
 * also how a caller tells an offscreen image from a network one without
 * knowing how either is represented.
 */
const uint32_t *ps_joff_image_px(const ps_jobj *img, int *w, int *h,
                                 int *stride);

/* Text ops for offscreen Graphics contexts that cannot inherit any.
 *
 * A software ps_jgfx carries the glyph callbacks it draws with, and an applet
 * in vector mode has none to lend: ps_jgfx_init_vector takes its text through
 * the vector vtable instead. An offscreen image is always a real pixel buffer,
 * so without this a vector-mode applet's back buffer would silently lose every
 * drawString. */
void ps_joff_set_text(const ps_jtext_ops *ops);

#ifdef __cplusplus
}
#endif

#endif /* PS_JOFF_H */
