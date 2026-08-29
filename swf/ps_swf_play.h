/* A movie with a playhead, and the map from its stage onto a box on screen.
 *
 * ps_swf.h stops at "draw frame N of this movie through this sink", which is
 * pure and has no notion of time. Something has to decide which N, and that
 * decision is the same on every backend: a file declares a frame rate, a host
 * hands over elapsed milliseconds, and frames advance when enough of them have
 * accumulated. Putting it here rather than in the shell means the host tools
 * can step a movie the same way the console does, and means the one piece of
 * arithmetic that is easy to get subtly wrong - carrying the remainder rather
 * than dropping it - exists once.
 *
 * The playhead is not the movie's own state. ps_swf_render_frame replays from
 * frame zero on every call, so this holds a number and not a display list, and
 * asking for the same frame twice draws the same picture. That is the property
 * the whole player is built around; see ps_swf_stage.c.
 */
#ifndef PS_SWF_PLAY_H
#define PS_SWF_PLAY_H

#include "ps_swf.h"

typedef struct {
    ps_swf_movie movie;
    int          loaded;

    uint32_t     frame;
    /* Milliseconds owed to the next frame. Kept rather than rounded away
     * because a 12fps movie at 83.33ms a frame driven by a 60Hz loop lands on
     * a frame boundary every third tick at best - dropping the remainder would
     * run the movie slow by however much the two rates disagree, which is
     * cumulative and so eventually obvious. */
    int          acc_us;
    int          us_per_frame;
} ps_swf_play;

/* Parses `data`, which need not outlive the call. Returns 0, or -1 with a
 * reason in err - a refused file is the ordinary case here, not an exception:
 * anything compressed, anything truncated past the first tag, anything that is
 * not a SWF at all. */
[[nodiscard]] int ps_swf_play_load(ps_swf_play *p, const uint8_t *data,
                                   size_t len, char *err, size_t errlen);
void ps_swf_play_free(ps_swf_play *p);

/* Advances the playhead by wall-clock milliseconds. Returns non-zero when the
 * frame number changed, so a caller holding a retained list knows to drop it.
 *
 * dt is clamped by the caller, not here - the shell already bounds it, and a
 * player that clamped again would disagree with everything else on the frame
 * about how much time passed. */
int ps_swf_play_tick(ps_swf_play *p, int dt_ms);

/* Stage size in pixels: twips at one to twenty, which is what the declared
 * frame rect means and what an authoring tool put in the HTML's width and
 * height. Zero for a movie that did not load. */
int ps_swf_play_stage_w(const ps_swf_play *p);
int ps_swf_play_stage_h(const ps_swf_play *p);

/* The root transform that lands the stage inside a `w` by `h` box whose top
 * left corner is at (ox, oy), preserving aspect and centring the slack.
 *
 * Aspect is preserved because a Flash movie is artwork and not a photograph:
 * an <embed> whose declared size disagrees with the stage - which is common,
 * since authors round - would otherwise stretch text and turn circles into
 * ovals. Centring rather than pinning to a corner is what Flash's own default
 * scale mode does. */
void ps_swf_play_fit(const ps_swf_play *p, int w, int h, float ox, float oy,
                     ps_swf_xform *out);

#endif /* PS_SWF_PLAY_H */
