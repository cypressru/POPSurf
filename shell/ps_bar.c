#include "ps_bar.h"
#include "ps_theme.h"
#include "ps_skin.h"
#include "ps_http.h"

#include <string.h>
#include <stdio.h>
#include <time.h>

/* --- geometry ----------------------------------------------------------- */

#define BAR_VIEW_W 640
#define BAR_VIEW_H 480

/* The band runs to the bottom of the picture; the row of controls is centred
 * in the part of it a viewer can see.
 *
 * That distinction is the whole layout. The band's bottom PS_SAFE_Y is skirt -
 * colour bleeding into the overscan a tube throws away - so it is not spacing
 * and cannot be counted as spacing. Centring in the full band would push
 * everything down into the part of the picture that is not there; centring
 * between the rule and the title-safe line puts it where it looks centred. */
#define BAR_Y  (BAR_VIEW_H - PS_BAR_BAND_H)
#define SAFE_B (BAR_VIEW_H - PS_SAFE_Y)

#define ROW_H  PS_BAR_ROW_H
#define ROW_Y  (BAR_Y + PS_BAR_RULE + \
                ((SAFE_B - (BAR_Y + PS_BAR_RULE)) - ROW_H) / 2)

#define BTN_Y  ROW_Y
#define BTN_H  ROW_H
#define BTN_W  48
#define BTN_GAP 6

/* The hamburger is its own group - it opens a different kind of thing from the
 * two arrows - so it gets a wider gap after it than the arrows get between
 * them. */
#define MENU_X  PS_SAFE_X
#define BACK_X  (MENU_X + BTN_W + 14)
#define FWD_X   (BACK_X + BTN_W + BTN_GAP)

/* Fixed rather than measured, so the address field has a stable right edge and
 * does not resize itself every time the minute rolls over - a field that
 * twitches on the hour is a bug people report as flicker. Wide enough for
 * "12:34 AM" at PS_FONT_TITLE with room to spare. */
#define CLOCK_W 118

#define URL_X (FWD_X + BTN_W + 12)
#define URL_R (BAR_VIEW_W - PS_SAFE_X - CLOCK_W - 10)
#define URL_W (URL_R - URL_X)

/* The toolbar reads at a glance rather than being studied, so it takes the
 * larger of the two chrome sizes. PS_FONT_UI at this distance, on a composite
 * signal, is a row of grey smudges. */
#define BAR_FONT PS_FONT_TITLE

/* Baseline for BAR_FONT centred in the row. */
#define ROW_BASELINE (ROW_Y + ROW_H / 2 + 9)

/* There is no reload button, and that is deliberate. The paint layer draws
 * rectangles and nothing else, and every rectangular approximation of a
 * circular arrow at this size reads as a smudge on a composite signal; a text
 * caption wide enough to be legible would be wider than both arrows together
 * and unbalance the row. Reload keeps its menu entry, where it has room for a
 * word. */

static void widget_rect(ps_bar_widget w, ps_rect *out)
{
    int x = 0, wide = 0;

    switch(w) {
    case PS_BAR_MENU: x = MENU_X; wide = BTN_W; break;
    case PS_BAR_BACK: x = BACK_X; wide = BTN_W; break;
    case PS_BAR_FWD:  x = FWD_X;  wide = BTN_W; break;
    case PS_BAR_URL:  x = URL_X;  wide = URL_W; break;
    default:
        out->x0 = out->y0 = out->x1 = out->y1 = 0;
        return;
    }

    out->x0 = (int16_t)x;
    out->y0 = (int16_t)BTN_Y;
    out->x1 = (int16_t)(x + wide);
    out->y1 = (int16_t)(BTN_Y + BTN_H);
}

static int widget_enabled(const ps_bar *b, ps_bar_widget w)
{
    switch(w) {
    case PS_BAR_BACK: return b->can_back;
    case PS_BAR_FWD:  return b->can_fwd;
    default:          return 1;
    }
}

/* --- state -------------------------------------------------------------- */

/* Re-read cadence for the clock. Under a second, so the displayed minute is
 * never more than that late, and far enough above a frame that the round trip
 * never shows up in the frame time. */
#define CLOCK_POLL_MS 500

static void clock_read(ps_bar *b)
{
    time_t     now = time(NULL);
    struct tm *lt  = localtime(&now);

    if(!lt)
        return;

    b->clock_hh = lt->tm_hour;
    b->clock_mm = lt->tm_min;
}

void ps_bar_init(ps_bar *b)
{
    memset(b, 0, sizeof *b);
    b->visible = 1;
    clock_read(b);
}

int ps_bar_height(const ps_bar *b)
{
    return b->visible ? PS_BAR_BAND_H : 0;
}

void ps_bar_set_visible(ps_bar *b, int on)
{
    b->visible = on ? 1 : 0;
    if(!b->visible) {
        b->hot     = PS_BAR_NONE;
        b->pressed = PS_BAR_NONE;
    }
}

void ps_bar_set_url(ps_bar *b, const char *url)
{
    snprintf(b->url, sizeof b->url, "%s", url ? url : "");
}

void ps_bar_set_nav(ps_bar *b, int can_back, int can_fwd)
{
    b->can_back = can_back ? 1 : 0;
    b->can_fwd  = can_fwd ? 1 : 0;

    /* A button that just went dead must not stay lit, or the next press
     * activates something that is no longer there. */
    if(b->hot != PS_BAR_NONE && !widget_enabled(b, b->hot))
        b->hot = PS_BAR_NONE;
    if(b->pressed != PS_BAR_NONE && !widget_enabled(b, b->pressed))
        b->pressed = PS_BAR_NONE;
}

void ps_bar_set_loading(ps_bar *b, int loading)
{
    b->loading = loading ? 1 : 0;
}

void ps_bar_tick(ps_bar *b, int dt_ms)
{
    b->anim_ms += (uint32_t)dt_ms;

    b->clock_ms += dt_ms;
    if(b->clock_ms >= CLOCK_POLL_MS) {
        b->clock_ms = 0;
        clock_read(b);
    }
}

ps_bar_widget ps_bar_at(const ps_bar *b, int x, int y)
{
    static const ps_bar_widget order[] = {
        PS_BAR_MENU, PS_BAR_BACK, PS_BAR_FWD, PS_BAR_URL
    };
    int i;

    if(!b->visible || y < BAR_Y)
        return PS_BAR_NONE;

    for(i = 0; i < (int)(sizeof order / sizeof order[0]); i++) {
        ps_rect r;

        if(!widget_enabled(b, order[i]))
            continue;

        widget_rect(order[i], &r);

        /* Horizontal extent only: vertically the whole band counts, not just
         * the control's own height. A cursor on a stick is imprecise and the
         * band holds nothing else, so being a few pixels low should still hit
         * the button rather than land on bare panel. */
        if(x >= r.x0 && x < r.x1)
            return order[i];
    }

    return PS_BAR_NONE;
}

void ps_bar_point(ps_bar *b, int x, int y)
{
    b->hot = ps_bar_at(b, x, y);
}

void ps_bar_press(ps_bar *b)
{
    b->pressed = b->hot;
}

ps_bar_widget ps_bar_release(ps_bar *b)
{
    ps_bar_widget w = b->pressed;

    b->pressed = PS_BAR_NONE;

    if(w == PS_BAR_NONE || w != b->hot)
        return PS_BAR_NONE;
    if(!widget_enabled(b, w))
        return PS_BAR_NONE;

    return w;
}

/* --- drawing ------------------------------------------------------------ */

static void draw_button(ps_paint *p, const ps_bar *b, ps_bar_widget w)
{
    ps_rect  r;
    int      off  = !widget_enabled(b, w);
    int      hot  = (b->hot == w) && !off;
    int      down = (b->pressed == w);
    ps_color ink  = ps_skin_ink(hot, off);

    widget_rect(w, &r);
    ps_skin_key(p, &r, down, hot, off);

    if(w == PS_BAR_MENU) {
        ps_skin_burger(p, &r, ink);
        return;
    }

    /* Sized off the button rather than off constants, so the glyph stays
     * centred and proportionate if the row height is ever retuned again. */
    {
        int gw = BTN_W / 3;
        int gh = BTN_H / 2;

        ps_skin_tri(p, r.x0 + (BTN_W - gw) / 2, r.y0 + (BTN_H - gh) / 2,
                    gw, gh, w == PS_BAR_BACK ? -1 : +1, ink);
    }
}

/* What to put in the field, which is not quite what was navigated to.
 *
 * "http://" is seven characters of a field twenty-one characters wide, and it
 * is the same seven characters on very nearly every page this browser will
 * ever open. Dropping them is worth a third of the field. Every other scheme
 * stays: file:// says the page came off the disc rather than the network, and
 * that is worth knowing precisely because it is unusual. */
static const char *display_url(const char *url)
{
    static const char http[] = "http://";

    if(!strncmp(url, http, sizeof http - 1))
        return url + sizeof http - 1;
    return url;
}

static void draw_address(ps_paint *p, const ps_bar *b, ps_text_cache *text)
{
    ps_rect r;
    char    shown[PS_URL_MAX];
    int     pad = 10;

    widget_rect(PS_BAR_URL, &r);
    ps_skin_well(p, r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0);

    /* Accent ring rather than a fill change, the same way a focused form
     * control is marked out on the page: the field's own colours already carry
     * meaning and recolouring them would say something different. */
    if(b->hot == PS_BAR_URL)
        ps_skin_ring(p, &r, PS_C_FOCUS);

    ps_skin_text_elide(text, BAR_FONT, display_url(b->url), URL_W - pad * 2,
                       shown, sizeof shown);

    ps_paint_push_clip(p, &r);
    ps_skin_text(p, text, BAR_FONT, r.x0 + pad, ROW_BASELINE, shown,
                 PS_C_TEXT);
    ps_paint_pop_clip(p);
}

static void draw_clock(ps_paint *p, const ps_bar *b, ps_text_cache *text)
{
    char buf[16];
    int  h12 = b->clock_hh % 12;
    int  w;

    if(h12 == 0)
        h12 = 12;

    /* Twelve hour with a suffix. This is a machine that lives in a living
     * room, and the clock is here to be glanced at from a sofa rather than
     * read precisely. */
    snprintf(buf, sizeof buf, "%d:%02d %s", h12, b->clock_mm,
             b->clock_hh < 12 ? "AM" : "PM");

    w = ps_skin_text_w(text, BAR_FONT, buf);

    /* Clipped to its own reserved zone. The width above is a constant chosen
     * against this font, and a font swap that made the clock wider would
     * otherwise have it run backwards over the address field - a layout bug
     * that only shows up on the one machine with the different font. */
    {
        ps_rect zone = { (int16_t)(URL_R + 10), (int16_t)ROW_Y,
                         (int16_t)(BAR_VIEW_W - PS_SAFE_X),
                         (int16_t)(ROW_Y + ROW_H) };

        ps_paint_push_clip(p, &zone);
        ps_skin_text(p, text, BAR_FONT, BAR_VIEW_W - PS_SAFE_X - w,
                     ROW_BASELINE, buf, PS_C_TEXT);
        ps_paint_pop_clip(p);
    }
}

/* The rule along the top of the bar doubles as the page's load progress.
 *
 * A progress bar somewhere else on screen would be a second thing saying the
 * same thing, and this is already a two-pixel accent line sitting exactly on
 * the boundary between the page and the chrome - which is where a browser's
 * progress has always lived. */
static void draw_rule(ps_paint *p, const ps_bar *b)
{
    size_t got = 0, total = 0;
    int    x0 = 0, x1 = BAR_VIEW_W;

    ps_skin_fill(p, 0, BAR_Y, BAR_VIEW_W, PS_BAR_RULE, PS_C_ACCENT_DIM);

    if(!b->loading)
        return;

    ps_http_progress(&got, &total);

    if(total) {
        int w = (int)((uint64_t)BAR_VIEW_W * got / total);

        x1 = w < 2 ? 2 : w;
    }
    else {
        /* Length unknown, which for most of the pages this browser targets is
         * always. A block that sweeps still reads as work in progress. */
        int span = BAR_VIEW_W / 5;

        x0 = (int)((b->anim_ms / 4) % (uint32_t)BAR_VIEW_W);
        x1 = x0 + span > BAR_VIEW_W ? BAR_VIEW_W : x0 + span;
    }

    ps_skin_fill(p, x0, BAR_Y, x1 - x0, PS_BAR_RULE, PS_C_ACCENT);
}

void ps_bar_draw(ps_paint *p, const ps_bar *b, ps_text_cache *text)
{
    if(!b->visible)
        return;

    /* Flat body, no specular. The line under the top edge belongs on a panel
     * that floats over the page; on a band welded to the bottom of the screen
     * it is a bright stripe directly under the amber rule with nothing to
     * explain it. */
    ps_skin_band(p, 0, BAR_Y + PS_BAR_RULE, BAR_VIEW_W,
                 PS_BAR_BAND_H - PS_BAR_RULE);
    draw_rule(p, b);

    draw_button(p, b, PS_BAR_MENU);
    draw_button(p, b, PS_BAR_BACK);
    draw_button(p, b, PS_BAR_FWD);
    draw_address(p, b, text);
    draw_clock(p, b, text);
}
