/* java.awt.image: the producer/consumer pipeline, as much of it as a sprite
 * sheet needs.
 *
 * One idiom pays for this file, and it is the whole of how an applet of the
 * period cuts a sheet of artwork into tiles:
 *
 *     Image         sheet = getImage(getCodeBase(), "tiles.gif");
 *     ImageFilter   f     = new CropImageFilter(x, y, w, h);
 *     ImageProducer p     = new FilteredImageSource(sheet.getSource(), f);
 *     Image         tile  = createImage(p);
 *
 * Karl Hoernell's entire games catalogue is built on those four lines, and so
 * are Christian Hvid's; the compatibility corpus ranks it third by applets blocked,
 * behind only the layout managers and java.awt.Polygon. Nothing else in that
 * corpus needs producer/consumer plumbing at all.
 *
 * --- what this is not ----------------------------------------------------
 *
 * The real pipeline is asynchronous and multi-threaded: a producer hands a
 * consumer setDimensions, setColorModel, then setPixels in whatever order the
 * decoder happens to finish scanlines in, from a thread the applet never sees.
 * None of that is reproduced here and none of it should be. Images in this
 * browser are decoded to ARGB8888 in one go by ps_applet.c, and there is one
 * interpreter thread. What has to be right is the *object model* - getSource()
 * gives something FilteredImageSource accepts, createImage() gives back an
 * Image - so that applet code written against the real API runs unchanged.
 *
 * Production is therefore direct: given a producer, walk it and fill a pixel
 * block. No consumer object is ever created, and an applet that supplies its
 * own ImageConsumer, or its own ImageFilter overriding setPixels, is refused
 * by name rather than answered wrongly. See the note above filter_kind.
 *
 * --- lazy, not eager ------------------------------------------------------
 *
 * createImage(producer) records the recipe and returns; the pixels are made on
 * first use. That is forced by the browser rather than chosen: ps_applet.c
 * requests an image over the network when the applet calls getImage() and
 * delivers it some frames later, so at the moment init() builds its tiles the
 * sheet has not arrived and an eager crop would produce an empty picture that
 * never corrected itself. Deferring to the first drawImage - which happens in
 * paint(), by which time the sheet is there - is also what the real API does,
 * and it is why an applet may legally call createImage before its artwork has
 * loaded.
 *
 * A production that finds its source still missing leaves the image
 * unmaterialised and is retried on the next frame, so a slow fetch costs a
 * blank tile for a frame or two rather than for the applet's life.
 */
#ifndef PS_JIMG_H
#define PS_JIMG_H

#include "ps_jvm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The ps_jre_call signature, for the classes this file owns. Sets *handled
 * when it took the call and leaves it clear otherwise, so ps_jre.c's own
 * tables still see everything else - in particular Images that came off the
 * network and Graphics methods that are not drawImage.
 *
 * Must be offered the call after ps_joff.c and before ps_jre.c's own Image and
 * Graphics branches: ps_joff.c owns the createImage(int,int) surfaces and
 * declines anything that is not one of them, and ps_jre.c's branches read an
 * Image's length field as a network handle, which an image made here is not. */
int ps_jimg_call(ps_jvm *vm, const char *cls, const char *name,
                 const char *desc, ps_jslot *args, int nargs, ps_jslot *ret,
                 int *handled);

#ifdef __cplusplus
}
#endif

#endif /* PS_JIMG_H */
