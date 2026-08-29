/* Affine and colour transforms.
 *
 * Small enough to be inline and deliberately is not. Every one of these is
 * called once per placed character rather than once per pixel - the renderer
 * sees only the finished product - so the cost that matters is getting the
 * composition order right, not the multiply. Keeping them here means there is
 * exactly one place that knows SWF's cell order, and the two places that used
 * to hand-roll a matrix inverse now do not.
 */
#include "ps_swf.h"

void ps_swf_xform_identity(ps_swf_xform *x)
{
    x->m[0] = x->m[3] = 1.0f;
    x->m[1] = x->m[2] = 0.0f;
    x->m[4] = x->m[5] = 0.0f;
}

void ps_swf_xform_scale(ps_swf_xform *x, float sx, float sy,
                        float tx, float ty)
{
    x->m[0] = sx;  x->m[1] = 0.0f;
    x->m[2] = 0.0f; x->m[3] = sy;
    x->m[4] = tx;  x->m[5] = ty;
}

void ps_swf_xform_mul(ps_swf_xform *out, const ps_swf_xform *outer,
                      const ps_swf_xform *inner)
{
    const float *o = outer->m, *i = inner->m;
    float        r[6];

    /* Through a local rather than straight into out, so a caller may pass the
     * same struct as a destination and an operand - which is what accumulating
     * a transform down a display list nest does on every step. */
    r[0] = o[0] * i[0] + o[2] * i[1];
    r[1] = o[1] * i[0] + o[3] * i[1];
    r[2] = o[0] * i[2] + o[2] * i[3];
    r[3] = o[1] * i[2] + o[3] * i[3];
    r[4] = o[0] * i[4] + o[2] * i[5] + o[4];
    r[5] = o[1] * i[4] + o[3] * i[5] + o[5];

    out->m[0] = r[0]; out->m[1] = r[1]; out->m[2] = r[2];
    out->m[3] = r[3]; out->m[4] = r[4]; out->m[5] = r[5];
}

int ps_swf_xform_invert(ps_swf_xform *out, const ps_swf_xform *in)
{
    const float *m = in->m;
    float        det = m[0] * m[3] - m[1] * m[2];
    float        r[6];

    /* An exporter that scaled something to nothing produces a singular matrix,
     * and it is common enough that this has to be a return value rather than
     * an assertion. The threshold is on the determinant rather than on the
     * cells because a matrix can have large cells and still be singular. */
    if(det > -1e-12f && det < 1e-12f)
        return -1;

    r[0] =  m[3] / det;
    r[1] = -m[1] / det;
    r[2] = -m[2] / det;
    r[3] =  m[0] / det;
    r[4] = (m[2] * m[5] - m[3] * m[4]) / det;
    r[5] = (m[1] * m[4] - m[0] * m[5]) / det;

    out->m[0] = r[0]; out->m[1] = r[1]; out->m[2] = r[2];
    out->m[3] = r[3]; out->m[4] = r[4]; out->m[5] = r[5];
    return 0;
}

void ps_swf_cxform_identity(ps_swf_cxform *c)
{
    int i;

    for(i = 0; i < 4; i++) {
        c->mult[i] = 256;
        c->add[i]  = 0;
    }
}

/* Composing two colour transforms: applying `inner` and then `outer` is
 * multiply the multipliers, and put the inner's addend through the outer's
 * multiply before adding the outer's own.
 *
 * Deliberately not clamped here. Clamping is what the final apply does, and
 * doing it at every level of a nested sprite would turn a transform that dims
 * and then brightens back into one that has lost the top of its range. The
 * intermediate is allowed to be out of gamut; only the pixel is not. */
void ps_swf_cxform_mul(ps_swf_cxform *out, const ps_swf_cxform *outer,
                       const ps_swf_cxform *inner)
{
    int i;

    for(i = 0; i < 4; i++) {
        long mul = (long)outer->mult[i] * inner->mult[i] / 256;
        long add = (long)outer->mult[i] * inner->add[i] / 256 + outer->add[i];

        if(mul >  32767) mul =  32767;
        if(mul < -32768) mul = -32768;
        if(add >  32767) add =  32767;
        if(add < -32768) add = -32768;
        out->mult[i] = (int16_t)mul;
        out->add[i]  = (int16_t)add;
    }
}

ps_swf_rgba ps_swf_cxform_apply(const ps_swf_cxform *c, ps_swf_rgba in)
{
    int         v[4], i;
    ps_swf_rgba out;

    v[0] = in.r; v[1] = in.g; v[2] = in.b; v[3] = in.a;
    for(i = 0; i < 4; i++) {
        long t = ((long)v[i] * c->mult[i]) / 256 + c->add[i];

        if(t < 0)   t = 0;
        if(t > 255) t = 255;
        v[i] = (int)t;
    }
    out.r = (uint8_t)v[0];
    out.g = (uint8_t)v[1];
    out.b = (uint8_t)v[2];
    out.a = (uint8_t)v[3];
    return out;
}
