/* A gradient as a texture, and the map from screen pixels into it.
 *
 * This is the hardware's way of shading a gradient and it exists here, beside
 * the player, rather than inside the PVR backend - because the interesting
 * half of it is checkable on a host and the backend is not.
 *
 * Why a texture at all, when the UI's gradients in core/ps_paint.c are
 * per-vertex: a linear ramp's position is an affine function of screen
 * position, so the hardware interpolating a texture coordinate computes
 * exactly what the software sampler would have evaluated per pixel. Per-vertex
 * colour is only equivalent when no stop falls between two vertices - and a
 * stop is precisely the place where the ramp is not linear, so per-vertex is
 * wrong exactly where a gradient is interesting. ps_paint.h's argument runs
 * the other way for a two-colour UI wash, where there are no interior stops
 * and banding is the only thing left to lose.
 *
 * A radial ramp is not an affine function of its own argument - it is a square
 * root of one - so it cannot be a row addressed by an interpolated coordinate.
 * Its texture is a picture of the gradient square instead, which makes u and v
 * affine again and moves the error from "wrong colour" to "finite resolution".
 *
 * The ramp is filled by calling ps_paint_at, the same function the span
 * renderer samples per pixel. That is deliberate and load bearing: a second
 * implementation of the stop walk could agree with the first for years and
 * then not, in the one place no test can see, because the tests have no PVR.
 * Here the two are the same code.
 */
#ifndef PS_SWF_RAMP_H
#define PS_SWF_RAMP_H

#include "ps_swf.h"

/* A linear ramp is sampled along one axis, so it is a row; eight rows because
 * the hardware's smallest useful texture is not one.
 *
 * The radial size is the one number here with a picture behind it. The square
 * is mapped onto the fill, so a texel spans (fill width / this) pixels, and a
 * gradient carrying a hard stop shows that spacing as the softness of its
 * edge. At 64 a 200-pixel-wide fill gave a three-pixel edge, which reads as
 * blurred rather than antialiased; at 128 it is one and a half, which does
 * not. The cost is 32KB against 8KB, paid once per radial gradient in the
 * file - see ps_swf_pvr.c's budget for what that is measured against. */
#define PS_SWF_RAMP_LINEAR_W 256
#define PS_SWF_RAMP_LINEAR_H 8
#define PS_SWF_RAMP_RADIAL   128

/* The gradient square is 32768 twips on a side, centred on the origin. Every
 * coordinate in this file is measured against that, which is why it is named
 * rather than written out four times. */
#define PS_SWF_RAMP_SQUARE 32768.0f

typedef struct {
    int       w, h;
    int       radial;
    /* Every stop opaque, so the ramp fits RGB565 - thirty-two levels of red
     * and blue against sixteen. On a gradient that is the difference between
     * shading and stripes, and it costs nothing: the alpha is the vertex's. */
    int       opaque;
    uint16_t *px;               /* w*h, RGB565 when opaque else ARGB4444 */
} ps_swf_ramp;

[[nodiscard]] int ps_swf_ramp_build(ps_swf_ramp *r, const ps_swf_gradient *g,
                                    int radial);
void              ps_swf_ramp_free(ps_swf_ramp *r);

/* The texture coordinate for a point in output pixel space:
 *
 *   u = uv[0]*x + uv[1]*y + uv[2]
 *   v = uv[3]*x + uv[4]*y + uv[5]
 *
 * Affine, which is the whole point - the hardware interpolates it across a
 * triangle and lands on the same value the sampler would have computed.
 *
 * A linear ramp leaves v at the middle row, because its texture has eight
 * identical ones and only exists as a row because a texture cannot be one
 * pixel tall. */
void ps_swf_ramp_uv(const ps_swf_paint *p, float uv[6]);

/* What the hardware would fetch: bilinear, clamped at both ends, from the
 * quantised texture.
 *
 * A model of the PVR rather than something the PVR calls - nothing on the
 * console links this. It is here so a host test can ask "does the texture path
 * shade this pixel the same colour the span renderer does", which is the only
 * question about the hardware gradient path that can be answered without
 * hardware. See tests/swf-host/ramptest.c. */
ps_swf_rgba ps_swf_ramp_sample(const ps_swf_ramp *r, float u, float v);

#endif /* PS_SWF_RAMP_H */
