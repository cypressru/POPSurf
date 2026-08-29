/* java.awt's geometry objects, and the font metrics that go with them.
 *
 * Split out of ps_jre.c because it is a different kind of thing: everything in
 * there forwards a call to something that already exists, and these five
 * classes have to *be* something first. An applet asks how big it is in the
 * first few lines of init(), and what comes back is an object with two public
 * fields on it, so the object layout is the design and the methods are the
 * easy part.
 */
#ifndef PS_JGEOM_H
#define PS_JGEOM_H

#include "ps_jvm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Same contract as ps_jre_call, and offered the call first: returns 0 having
 * set *handled when it recognised the method, 0 with *handled clear when it
 * did not, and -1 only when something genuinely failed. */
int ps_jgeom_call(ps_jvm *vm, const char *cls, const char *name,
                  const char *desc, ps_jslot *args, int nargs, ps_jslot *ret,
                  int *handled);

/* Constructors for the rest of the runtime. MouseEvent.getPoint() and anything
 * else that has to hand an applet a geometry object should come through these
 * rather than building one, so there is one definition of the layout. */
ps_jobj *ps_jgeom_point(ps_jvm *vm, int32_t x, int32_t y);
ps_jobj *ps_jgeom_dimension(ps_jvm *vm, int32_t w, int32_t h);
ps_jobj *ps_jgeom_rect(ps_jvm *vm, int32_t x, int32_t y, int32_t w, int32_t h);

/* And back out again. Non-zero when `o` really is one of those classes, in
 * which case the outputs are filled in; zero leaves them untouched.
 *
 * ps_jawt.c is the caller: a widget handed setSize(Dimension) has to record
 * the same two numbers getSize() would later report, and the field layout that
 * says which slot is which is defined here and nowhere else. Reading slot 0
 * over there is how the two files start disagreeing. */
int ps_jgeom_dim_of(const ps_jobj *o, int32_t *w, int32_t *h);
int ps_jgeom_rect_of(const ps_jobj *o, int32_t *x, int32_t *y,
                     int32_t *w, int32_t *h);

#ifdef __cplusplus
}
#endif

#endif /* PS_JGEOM_H */
