#include "ps_swf_play.h"

#include <string.h>

/* Bounds on a declared frame rate.
 *
 * The header field is 8.8 fixed point and a file may say zero, which is what
 * an exporter writes when it does not care. Zero would divide, and a very
 * large value would advance dozens of frames per tick and turn a long timeline
 * into a CPU spike. Twelve is what Flash itself substituted for a missing rate
 * in this era, and it is the rate the content this player exists for was
 * authored at. */
#define FPS_FALLBACK 12.0f
#define FPS_MAX      120.0f

int ps_swf_play_load(ps_swf_play *p, const uint8_t *data, size_t len,
                     char *err, size_t errlen)
{
    float fps;

    memset(p, 0, sizeof *p);
    if(ps_swf_load(data, len, &p->movie, err, errlen) < 0)
        return -1;

    fps = p->movie.fps;
    if(!(fps > 0.0f))
        fps = FPS_FALLBACK;
    if(fps > FPS_MAX)
        fps = FPS_MAX;

    p->us_per_frame = (int)(1000000.0f / fps + 0.5f);
    p->loaded       = 1;
    return 0;
}

void ps_swf_play_free(ps_swf_play *p)
{
    if(p->loaded)
        ps_swf_free(&p->movie);
    memset(p, 0, sizeof *p);
}

int ps_swf_play_tick(ps_swf_play *p, int dt_ms)
{
    uint32_t before = p->frame;
    uint32_t n;

    if(!p->loaded || dt_ms <= 0)
        return 0;

    n = p->movie.root.nframe;
    if(n <= 1)
        return 0;

    p->acc_us += dt_ms * 1000;
    while(p->acc_us >= p->us_per_frame) {
        p->acc_us -= p->us_per_frame;
        p->frame++;
    }

    /* Wrapped here rather than left to grow, because the frame number is what
     * the walker replays up to: an unwrapped one would replay a hundred
     * thousand frames' worth of ops to draw the third one. */
    p->frame %= n;
    return p->frame != before;
}

int ps_swf_play_stage_w(const ps_swf_play *p)
{
    if(!p->loaded)
        return 0;
    return (int)((p->movie.xmax - p->movie.xmin) / 20);
}

int ps_swf_play_stage_h(const ps_swf_play *p)
{
    if(!p->loaded)
        return 0;
    return (int)((p->movie.ymax - p->movie.ymin) / 20);
}

void ps_swf_play_fit(const ps_swf_play *p, int w, int h, float ox, float oy,
                     ps_swf_xform *out)
{
    float sw = (float)(p->movie.xmax - p->movie.xmin);
    float sh = (float)(p->movie.ymax - p->movie.ymin);
    float s;

    ps_swf_xform_identity(out);
    if(!p->loaded || w <= 0 || h <= 0 || !(sw > 0.0f) || !(sh > 0.0f))
        return;

    s = (float)w / sw;
    if((float)h / sh < s)
        s = (float)h / sh;

    /* The stage rect's origin is subtracted before the scale, because a file
     * may declare a frame rect that does not start at zero and the artwork is
     * positioned against that origin rather than against the twip grid. */
    ps_swf_xform_scale(out, s, s,
                       ox + ((float)w - sw * s) * 0.5f - (float)p->movie.xmin * s,
                       oy + ((float)h - sh * s) * 0.5f - (float)p->movie.ymin * s);
}
