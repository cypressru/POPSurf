/* CRT screensaver shown after an idle timeout. */
#ifndef PS_SAVER_H
#define PS_SAVER_H

#include "ps_types.h"
#include "ps_paint.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int active;
    int idle_ms;

    /* Sub-pixel position keeps motion consistent at 50 and 60 Hz. */
    float x, y;
    float dx, dy;

    int view_w, view_h;
} ps_saver;

void ps_saver_init(ps_saver *s, int view_w, int view_h);

/* Advances the idle timer and the animation.
 *
 * `input` is non-zero on any frame the user did something - any button, any
 * trigger, any real stick deflection. `busy` is non-zero when something is
 * playing or loading and the screen must stay up regardless of the timer.
 *
 * Returns non-zero while the saver is up, in which case the caller should draw
 * it instead of the page. */
int ps_saver_tick(ps_saver *s, int dt_ms, int input, int busy);

static inline int ps_saver_is_active(const ps_saver *s) { return s->active; }

/* Drops the saver and restarts the timer. Called for the press that wakes it,
 * which is deliberately swallowed rather than delivered: the first thing you
 * press to see the screen again should not also click whatever the pointer was
 * left sitting on. */
void ps_saver_wake(ps_saver *s);

/* Blacks out the viewport with the mark cut out of it.
 *
 * The caller paints the page first. This covers it everywhere except the mark,
 * so the apple is a moving window onto the page rather than a shape drawn over
 * it. It should be the last thing drawn, and the caller should skip the cursor
 * and any chrome - both would show through the window as static bright shapes,
 * which is the one thing a screensaver is trying not to leave on a tube. */
void ps_saver_draw(ps_paint *p, const ps_saver *s);

#ifdef __cplusplus
}
#endif

#endif /* PS_SAVER_H */
