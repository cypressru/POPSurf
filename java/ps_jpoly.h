/* java.awt.Polygon, the Graphics calls that take one, and four singles.
 *
 * Split out of ps_jre.c for the same reason ps_jgeom.c was: Polygon has to
 * *be* something before any of its methods mean anything. Its three public
 * fields are read and written by applets directly at least as often as the
 * methods are called, and a field access has no native path in the
 * interpreter - see the note at the top of ps_jpoly.c.
 *
 * Measured rather than guessed at: the compatibility corpus found it in
 * nine applets across three unrelated authors, the largest single missing
 * class in that corpus and the only entry three authors hit first.
 */
#ifndef PS_JPOLY_H
#define PS_JPOLY_H

#include "ps_jvm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Same contract as ps_jre_call, and offered the call after ps_jawt_call has
 * declined it: returns 0 having set *handled when it recognised the method, 0
 * with *handled clear when it did not, and -1 only when something genuinely
 * failed - which here means an exception was raised and vm->throwing says so.
 */
int ps_jpoly_call(ps_jvm *vm, const char *cls, const char *name,
                  const char *desc, ps_jslot *args, int nargs, ps_jslot *ret,
                  int *handled);

#ifdef __cplusplus
}
#endif

#endif /* PS_JPOLY_H */
