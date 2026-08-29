/* AWT widget and layout shells.
 *
 * Split out of ps_jre.c because it is a different kind of thing: ps_jre.c
 * implements the JDK, and this implements the *absence* of it. See the header
 * comment in ps_jawt.c before believing anything in here draws.
 */
#ifndef PS_JAWT_H
#define PS_JAWT_H

#include "ps_jvm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Same contract as ps_jre_call, which is what lets one line at the top of it
 * delegate here: 0 on success with *handled saying whether the call was
 * claimed, -1 with vm->err set on failure. A call this file does not implement
 * leaves *handled clear so the rest of ps_jre_call still gets its turn - which
 * matters, because Component and Applet are shared with several other pieces
 * of the runtime. */
int ps_jawt_call(ps_jvm *vm, const char *cls, const char *name,
                 const char *desc, ps_jslot *args, int nargs, ps_jslot *ret,
                 int *handled);

/* Is `cls` a name whose calls may be answered as java.awt.Component's?
 *
 * ps_jgeom.c has to decide the same thing - it answers size(), getSize(),
 * getFontMetrics() and the rest for the same set of classes - and it used to
 * decide it from a list of its own. The list had four names in it and this
 * file's had twenty-three, so no Button, TextField or Checkbox ever reached
 * any of those methods; three compatibility applets stop on exactly that.
 * Two lists answering one question caused the same class of coordinate bugs.
 * A predicate cannot drift from
 * itself, so there is one and it lives where the class table does.
 */
int ps_jawt_is_component(const char *cls);

/* Is `o` a widget shell, or a subclass of one, rather than the applet's own
 * surface?
 *
 * The class name a call arrives under cannot answer this: javac writes the
 * *static* type of the receiver into the constant pool, so `comp.setFont(f)`
 * on a Button inside a Fendt panel arrives as java/awt/Component and looks
 * exactly like the applet setting its own font. Only the receiver knows.
 *
 * It matters because the two have different answers. The applet's box is the
 * one the page gave it; a widget is never laid out here and has no box at all
 * until the applet sets one, and telling an applet its Button is 300x200 is
 * the kind of plausible wrong number this runtime is not allowed to invent.
 */
int ps_jawt_is_widget(const ps_jobj *o);

#ifdef __cplusplus
}
#endif

#endif /* PS_JAWT_H */
