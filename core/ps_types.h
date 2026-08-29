/* POPSurf core types. Layout uses signed 26.6 fixed point. */
#ifndef PS_TYPES_H
#define PS_TYPES_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed point: 26.6. Range +/-524288 px, and divisions become shifts. */
typedef int32_t ps_unit;

#define PS_FRAC_BITS 6
#define PS_ONE       (1 << PS_FRAC_BITS)
#define PS_PX(x)     ((ps_unit)((x) * PS_ONE))
#define PS_TO_PX(u)  ((int)((u) >> PS_FRAC_BITS))

/* Round to nearest whole pixel. Bias then truncate; negative-safe because the
 * shift is arithmetic. */
#define PS_ROUND_PX(u) ((int)(((u) + (PS_ONE / 2)) >> PS_FRAC_BITS))

/* Device-space rectangle, whole pixels, x1/y1 exclusive. */
typedef struct {
    int16_t x0, y0, x1, y1;
} ps_rect;

static inline int ps_rect_empty(const ps_rect *r)
{
    return r->x1 <= r->x0 || r->y1 <= r->y0;
}

/* Intersection. Result may be empty; callers must check. */
static inline ps_rect ps_rect_isect(const ps_rect *a, const ps_rect *b)
{
    ps_rect r;
    r.x0 = a->x0 > b->x0 ? a->x0 : b->x0;
    r.y0 = a->y0 > b->y0 ? a->y0 : b->y0;
    r.x1 = a->x1 < b->x1 ? a->x1 : b->x1;
    r.y1 = a->y1 < b->y1 ? a->y1 : b->y1;
    return r;
}

/* Unaligned-safe readers for little-endian asset formats. */
static inline uint16_t ps_rd_u16le(const void *p)
{
    const uint8_t *b = (const uint8_t *)p;
    return (uint16_t)(b[0] | ((uint16_t)b[1] << 8));
}

static inline uint32_t ps_rd_u32le(const void *p)
{
    const uint8_t *b = (const uint8_t *)p;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
}

static inline int16_t ps_rd_s16le(const void *p)
{
    return (int16_t)ps_rd_u16le(p);
}

static inline int32_t ps_rd_s32le(const void *p)
{
    return (int32_t)ps_rd_u32le(p);
}

/* Packed ARGB8888. The PVR consumes this directly as a vertex color. */
typedef uint32_t ps_color;

#define PS_ARGB(a, r, g, b) \
    (((ps_color)(a) << 24) | ((ps_color)(r) << 16) | \
     ((ps_color)(g) << 8)  |  (ps_color)(b))

#define PS_COLOR_A(c) (((c) >> 24) & 0xff)
#define PS_COLOR_R(c) (((c) >> 16) & 0xff)
#define PS_COLOR_G(c) (((c) >> 8)  & 0xff)
#define PS_COLOR_B(c) ( (c)        & 0xff)

#ifdef __cplusplus
}
#endif

#endif /* PS_TYPES_H */
