/* java.lang and java.util, the parts an applet reaches through rather than
 * draws with.
 *
 * ps_jre.c is the AWT surface - Graphics, Color, Font, the applet lifecycle.
 * This is everything underneath it: String, the primitive wrappers, Vector,
 * Hashtable, Random, StringTokenizer. They are split because they fail
 * differently. An absent drawing call leaves a wrong picture; an absent
 * String.substring stops the applet dead on its first line of parsing, and the
 * corpus reaches for these far more often than it reaches for fillArc.
 */
#ifndef PS_JLIB_H
#define PS_JLIB_H

#include "ps_jvm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Offered every native call before ps_jre.c looks at it.
 *
 * Returns non-zero when it has claimed the call, and then either
 *
 *   *handled = 1   the call ran; ret holds its result
 *   *handled = 0   it raised - vm->throwing or vm->failed says which, and the
 *                  caller must return -1 without looking further
 *
 * and zero when the call is none of its business, leaving *handled untouched
 * so the rest of ps_jre_call gets its turn. Claiming has to be all-or-nothing
 * per method rather than per class: a name this file does not know must fall
 * through to the interpreter's "unimplemented" report naming the exact
 * method, which is the failure that tells you what to write next.
 */
int ps_jlib_call(ps_jvm *vm, const char *cls, const char *name,
                 const char *desc, ps_jslot *args, int nargs, ps_jslot *ret,
                 int *handled);

#ifdef __cplusplus
}
#endif

#endif /* PS_JLIB_H */
