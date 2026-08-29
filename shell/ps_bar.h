/* Shell toolbar: menu, history, address, progress, and clock. */
#ifndef PS_BAR_H
#define PS_BAR_H

#include "ps_types.h"
#include "ps_paint.h"
#include "ps_text.h"
#include "ps_url.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PS_BAR_NONE = 0,
    PS_BAR_MENU,
    PS_BAR_BACK,
    PS_BAR_FWD,
    PS_BAR_URL
} ps_bar_widget;

typedef struct {
    int visible;

    char url[PS_URL_MAX];

    int can_back;
    int can_fwd;
    int loading;

    /* Widget under the pointer, and the one A is currently held on. Pressed is
     * tracked so a button visibly goes down, which on a controller is the only
     * feedback that the press registered at all. */
    ps_bar_widget hot;
    ps_bar_widget pressed;

    /* Wall clock, re-read from the RTC about once a second rather than every
     * frame. On this machine the clock lives on the AICA, across the G2 bus,
     * and G2 reads are slow enough that sixty of them a second for a display
     * that changes once a minute would be a genuinely poor trade. */
    int clock_hh, clock_mm;
    int clock_ms;

    /* Drives the indeterminate sweep on the progress rule. Owned here rather
     * than taken from the caller's frame clock so drawing needs no time
     * argument, and so the sweep keeps its speed if the shell ever draws the
     * bar from somewhere else. */
    uint32_t anim_ms;
} ps_bar;

void ps_bar_init(ps_bar *b);

/* Lines the bar occupies along the bottom: PS_BAR_BAND_H, or 0 when hidden.
 * The page viewport is whatever is left. */
int  ps_bar_height(const ps_bar *b);

static inline int ps_bar_is_visible(const ps_bar *b) { return b->visible; }
void ps_bar_set_visible(ps_bar *b, int on);

void ps_bar_set_url(ps_bar *b, const char *url);
void ps_bar_set_nav(ps_bar *b, int can_back, int can_fwd);
void ps_bar_set_loading(ps_bar *b, int loading);

void ps_bar_tick(ps_bar *b, int dt_ms);

/* Widget at a screen point, PS_BAR_NONE when the point is not on the bar or
 * the widget there is unavailable. An unavailable widget reporting NONE is
 * what stops a dead Back button from swallowing a click the page could have
 * had. */
ps_bar_widget ps_bar_at(const ps_bar *b, int x, int y);

/* Moves the highlight to whatever is under the pointer. */
void ps_bar_point(ps_bar *b, int x, int y);
static inline ps_bar_widget ps_bar_hot(const ps_bar *b) { return b->hot; }

/* Press and release, edge triggered by the caller. Release returns the widget
 * activated, or PS_BAR_NONE if the pointer left the one that was pressed -
 * which is the standard let-go-somewhere-else escape. */
void          ps_bar_press(ps_bar *b);
ps_bar_widget ps_bar_release(ps_bar *b);

void ps_bar_draw(ps_paint *p, const ps_bar *b, ps_text_cache *text);

#ifdef __cplusplus
}
#endif

#endif /* PS_BAR_H */
