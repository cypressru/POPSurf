#include "ps_jdc.h"

static int dc_measure(void *user, const char *s, size_t len, int px)
{
    return ps_text_measure_px((ps_text_cache *)user, s, len, px);
}

static int dc_ascent(void *user, int px)
{
    return ps_text_ascent_px((ps_text_cache *)user, px);
}

static int dc_descent(void *user, int px)
{
    return ps_text_descent_px((ps_text_cache *)user, px);
}

static void dc_draw(void *user, ps_jsurface *s, int x, int baseline,
                    const char *utf8, size_t len, uint32_t argb, int px,
                    int cx0, int cy0, int cx1, int cy1)
{
    ps_text_blit((ps_text_cache *)user, s->px, s->w, s->h, s->stride,
                 x, baseline, utf8, len, argb, px, cx0, cy0, cx1, cy1);
}

void ps_jdc_text_ops(ps_jtext_ops *ops, ps_text_cache *tc)
{
    ops->user    = tc;
    ops->measure = dc_measure;
    ops->draw    = dc_draw;
    ops->ascent  = dc_ascent;
    ops->descent = dc_descent;
}
