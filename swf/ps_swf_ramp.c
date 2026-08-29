#include "ps_swf_ramp.h"
#include "ps_swf_geom.h"
#include "ps_swf_mem.h"

#include <string.h>

static uint16_t pack4444(ps_swf_rgba c)
{
    return (uint16_t)(((c.a >> 4) << 12) | ((c.r >> 4) << 8) |
                      ((c.g >> 4) << 4) | (c.b >> 4));
}

static uint16_t pack565(ps_swf_rgba c)
{
    return (uint16_t)(((c.r >> 3) << 11) | ((c.g >> 2) << 5) | (c.b >> 3));
}

/* Widened back the way the hardware does, by replicating the high bits into
 * the low ones rather than shifting zeros in. Shifting zeros makes white
 * 0xf8f8f8 and turns every fully saturated ramp end slightly grey, which on a
 * gradient running to white is a visible dirty edge. */
static ps_swf_rgba unpack4444(uint16_t v)
{
    ps_swf_rgba c;
    unsigned    a = (v >> 12) & 15, r = (v >> 8) & 15;
    unsigned    g = (v >> 4) & 15,  b = v & 15;

    c.a = (uint8_t)((a << 4) | a);
    c.r = (uint8_t)((r << 4) | r);
    c.g = (uint8_t)((g << 4) | g);
    c.b = (uint8_t)((b << 4) | b);
    return c;
}

static ps_swf_rgba unpack565(uint16_t v)
{
    ps_swf_rgba c;
    unsigned    r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;

    c.r = (uint8_t)((r << 3) | (r >> 2));
    c.g = (uint8_t)((g << 2) | (g >> 4));
    c.b = (uint8_t)((b << 3) | (b >> 2));
    c.a = 255;
    return c;
}

int ps_swf_ramp_build(ps_swf_ramp *r, const ps_swf_gradient *g, int radial)
{
    ps_swf_paint pt;
    int          x, y, i;

    memset(r, 0, sizeof *r);
    if(!g || g->nstop == 0)
        return -1;

    r->radial = radial;
    r->w      = radial ? PS_SWF_RAMP_RADIAL : PS_SWF_RAMP_LINEAR_W;
    r->h      = radial ? PS_SWF_RAMP_RADIAL : PS_SWF_RAMP_LINEAR_H;

    r->opaque = 1;
    for(i = 0; i < g->nstop; i++)
        if(g->color[i].a != 255)
            r->opaque = 0;

    r->px = ps_swf_alloc((size_t)r->w * (size_t)r->h * sizeof *r->px);
    if(!r->px)
        return -1;

    /* An inverse that turns a texel centre into a point of the gradient
     * square, so ps_paint_at is asked exactly the question the sampler will
     * ask it later through the interpolated coordinate. The half texel is what
     * makes the two line up: the hardware puts texel x's centre at
     * u = (x + 0.5)/w, and this puts the same texel's colour at the same
     * fraction along the square. */
    memset(&pt, 0, sizeof pt);
    pt.grad   = g;
    pt.radial = radial;
    pt.inv[0] = PS_SWF_RAMP_SQUARE / (float)r->w;
    pt.inv[2] = -0.5f * PS_SWF_RAMP_SQUARE + 0.5f * pt.inv[0];
    pt.inv[4] = radial ? PS_SWF_RAMP_SQUARE / (float)r->h : 0.0f;
    pt.inv[5] = radial ? -0.5f * PS_SWF_RAMP_SQUARE + 0.5f * pt.inv[4] : 0.0f;

    for(y = 0; y < r->h; y++)
        for(x = 0; x < r->w; x++) {
            ps_swf_rgba c = ps_paint_at(&pt, (float)x, (float)y);

            r->px[y * r->w + x] = r->opaque ? pack565(c) : pack4444(c);
        }
    return 0;
}

void ps_swf_ramp_free(ps_swf_ramp *r)
{
    ps_swf_dealloc(r->px);
    memset(r, 0, sizeof *r);
}

void ps_swf_ramp_uv(const ps_swf_paint *p, float uv[6])
{
    const float s = 1.0f / PS_SWF_RAMP_SQUARE;

    /* inv maps a screen pixel to a point of the gradient square, in twips
     * running from -16384 to 16384. The texture covers exactly that extent, so
     * the texture coordinate is the same map scaled by the square's width and
     * shifted so the centre lands at a half. */
    uv[0] = p->inv[0] * s;
    uv[1] = p->inv[1] * s;
    uv[2] = p->inv[2] * s + 0.5f;

    if(p->radial) {
        uv[3] = p->inv[3] * s;
        uv[4] = p->inv[4] * s;
        uv[5] = p->inv[5] * s + 0.5f;
    }
    else {
        uv[3] = 0.0f;
        uv[4] = 0.0f;
        uv[5] = 0.5f;
    }
}

ps_swf_rgba ps_swf_ramp_sample(const ps_swf_ramp *r, float u, float v)
{
    float    fx = u * (float)r->w - 0.5f;
    float    fy = v * (float)r->h - 0.5f;
    int      x0, y0, x1, y1, k;
    float    tx, ty;
    unsigned ch[4];
    ps_swf_rgba out;

    /* Clamped at the texel grid, which is PVR_UVCLAMP_UV. It clamps the
     * coordinate, not the colour, so a sample past either end repeats the last
     * texel - which is what a gradient's own "everything past the ends clamps"
     * rule already asks for. */
    x0 = (int)(fx >= 0.0f ? fx : fx - 1.0f);
    y0 = (int)(fy >= 0.0f ? fy : fy - 1.0f);
    tx = fx - (float)x0;
    ty = fy - (float)y0;
    x1 = x0 + 1;
    y1 = y0 + 1;

    if(x0 < 0) x0 = 0;
    if(y0 < 0) y0 = 0;
    if(x1 < 0) x1 = 0;
    if(y1 < 0) y1 = 0;
    if(x0 >= r->w) x0 = r->w - 1;
    if(x1 >= r->w) x1 = r->w - 1;
    if(y0 >= r->h) y0 = r->h - 1;
    if(y1 >= r->h) y1 = r->h - 1;

    {
        ps_swf_rgba c[4];
        unsigned    i;

        c[0] = r->opaque ? unpack565(r->px[y0 * r->w + x0])
                         : unpack4444(r->px[y0 * r->w + x0]);
        c[1] = r->opaque ? unpack565(r->px[y0 * r->w + x1])
                         : unpack4444(r->px[y0 * r->w + x1]);
        c[2] = r->opaque ? unpack565(r->px[y1 * r->w + x0])
                         : unpack4444(r->px[y1 * r->w + x0]);
        c[3] = r->opaque ? unpack565(r->px[y1 * r->w + x1])
                         : unpack4444(r->px[y1 * r->w + x1]);

        for(i = 0; i < 4; i++) {
            const uint8_t *p0 = &((const uint8_t *)&c[0])[i];
            const uint8_t *p1 = &((const uint8_t *)&c[1])[i];
            const uint8_t *p2 = &((const uint8_t *)&c[2])[i];
            const uint8_t *p3 = &((const uint8_t *)&c[3])[i];
            float          a  = (float)*p0 + ((float)*p1 - (float)*p0) * tx;
            float          b  = (float)*p2 + ((float)*p3 - (float)*p2) * tx;
            float          f  = a + (b - a) * ty;

            ch[i] = (unsigned)(f + 0.5f);
            if(ch[i] > 255u)
                ch[i] = 255u;
        }
    }

    for(k = 0; k < 4; k++)
        ((uint8_t *)&out)[k] = (uint8_t)ch[k];
    return out;
}
