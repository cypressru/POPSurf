/* Host implementation of the triangle sink. See ps_swf_trisoft.c. */
#ifndef PS_SWF_TRISOFT_H
#define PS_SWF_TRISOFT_H

#include "ps_swf_geom.h"

typedef struct {
    const ps_swf_view *view;
    ps_swf_span_fn     span;
    void              *user;

    ps_swf_paint       paint;
    ps_aedge          *ae;
    long               n, cap;
    ps_crossing       *xs;
    long               xcap;
    float             *cov;

    long               tris;
    double             area_signed, area_abs;
    int                failed;
} ps_trisoft;

int  ps_trisoft_init(ps_trisoft *t, const ps_swf_view *v,
                     ps_swf_span_fn span, void *user);
void ps_trisoft_free(ps_trisoft *t);

const ps_swf_tri_sink *ps_trisoft_sink(void);

#endif /* PS_SWF_TRISOFT_H */
