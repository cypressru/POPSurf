#include "ps_menu.h"
#include "ps_theme.h"
#include "ps_skin.h"

#include <string.h>
#include <stdio.h>

#define PS_TOAST_MS 2200

#define PS_MENU_W 340

/* Panel geometry. Fixed height across all three pages: a panel that resized
 * itself as you stepped between them would move the row under the cursor on
 * every transition. */
#define MENU_HEAD_H 40
#define MENU_H      (PS_MENU_VIS * PS_ROW_H + PS_PAD * 2 + MENU_HEAD_H)
#define MENU_X      ((640 - PS_MENU_W) / 2)
#define MENU_Y      ((480 - MENU_H) / 2)

/* --- pages ---------------------------------------------------------------
 *
 * Every page answers the same four questions - how many rows, what does row i
 * say, can it be chosen, and what does choosing it do - so the selection,
 * scrolling and drawing are written once rather than three times.
 */

typedef struct {
    const char    *label;
    ps_menu_action action;
    int            enabled;
} ps_menu_item;

/* Bookmarks needs a card to persist to and History needs somewhere to have
 * been; both are always reachable, and both say so on their own page when they
 * are empty. That is better than a dimmed row here, which tells you a feature
 * exists without telling you why you cannot have it. */
static const ps_menu_item g_root[] = {
    { "Back",       PS_MENU_BACK,    1 },
    { "Forward",    PS_MENU_FORWARD, 1 },
    { "Reload",     PS_MENU_RELOAD,  1 },
    { "Home",       PS_MENU_HOME,    1 },
    { "Address...", PS_MENU_ADDRESS, 1 },
    { "History",    PS_MENU_NONE,    1 },   /* opens a page, see row_activate */
    { "Bookmarks",  PS_MENU_NONE,    1 },
    { "Directory",  PS_MENU_DIRECTORY, 1 },
    { "Toolbar",    PS_MENU_TOOLBAR, 1 },
    { "Quit",       PS_MENU_QUIT,    1 }
};

#define ROOT_COUNT     ((int)(sizeof g_root / sizeof g_root[0]))
#define ROOT_HISTORY   5
#define ROOT_BOOKMARKS 6

/* The bookmarks page leads with one command row; the list starts under it. */
#define MARKS_CMD_ROWS 1

static int page_rows(const ps_menu *m)
{
    switch(m->page) {
    case PS_MENU_PAGE_HISTORY:   return m->hist_count;
    case PS_MENU_PAGE_BOOKMARKS: return MARKS_CMD_ROWS + m->marks_count;
    default:                     return ROOT_COUNT;
    }
}

static const char *page_title(const ps_menu *m)
{
    switch(m->page) {
    case PS_MENU_PAGE_HISTORY:   return "History";
    case PS_MENU_PAGE_BOOKMARKS: return "Bookmarks";
    default:                     return "POPSurf";
    }
}

/* Shown in place of the list when a page has nothing on it. */
static const char *page_empty(const ps_menu *m)
{
    switch(m->page) {
    case PS_MENU_PAGE_HISTORY:   return "Nowhere yet";
    case PS_MENU_PAGE_BOOKMARKS: return "No bookmarks";
    default:                     return "";
    }
}

static const char *row_label(const ps_menu *m, int i)
{
    if(m->page == PS_MENU_PAGE_HISTORY)
        return (i >= 0 && i < m->hist_count) ? m->hist[i] : "";

    if(m->page == PS_MENU_PAGE_BOOKMARKS) {
        if(i == 0) {
            if(m->current_marked)
                return "Remove this page";
            return m->marks_full ? "Bookmarks are full" : "Bookmark this page";
        }
        i -= MARKS_CMD_ROWS;
        return (i >= 0 && i < m->marks_count) ? m->marks[i] : "";
    }

    if(i < 0 || i >= ROOT_COUNT)
        return "";

    /* The only root label that is not fixed: an entry reading "Toolbar" tells
     * you nothing about which way pressing it goes. */
    if(g_root[i].action == PS_MENU_TOOLBAR)
        return m->bar_shown ? "Hide toolbar" : "Show toolbar";

    return g_root[i].label;
}

/* Static permission and current state, combined. */
static int row_enabled(const ps_menu *m, int i)
{
    if(m->page == PS_MENU_PAGE_HISTORY)
        return i >= 0 && i < m->hist_count;

    if(m->page == PS_MENU_PAGE_BOOKMARKS) {
        if(i == 0)
            return m->current_marked || !m->marks_full;
        return (i - MARKS_CMD_ROWS) < m->marks_count;
    }

    if(i < 0 || i >= ROOT_COUNT || !g_root[i].enabled)
        return 0;

    switch(g_root[i].action) {
    case PS_MENU_BACK:    return m->can_back;
    case PS_MENU_FORWARD: return m->can_fwd;
    default:              return 1;
    }
}

/* Runs the selected row. Returns the action to hand back to the shell, and
 * PS_MENU_NONE for rows that only move between pages - stepping into History
 * is navigation inside the menu, not something the browser has to do. */
static ps_menu_action row_activate(ps_menu *m, int i)
{
    if(m->page == PS_MENU_PAGE_HISTORY) {
        m->picked = i;
        m->open   = 0;
        return PS_MENU_GO_HISTORY;
    }

    if(m->page == PS_MENU_PAGE_BOOKMARKS) {
        if(i == 0) {
            /* Stays open: adding a bookmark and immediately seeing it appear
             * in the list underneath is the confirmation that it worked. */
            m->picked = 0;
            return m->current_marked ? PS_MENU_BOOKMARK_DEL
                                     : PS_MENU_BOOKMARK_ADD;
        }
        m->picked = i - MARKS_CMD_ROWS;
        m->open   = 0;
        return PS_MENU_GO_BOOKMARK;
    }

    if(i == ROOT_HISTORY) {
        m->page = PS_MENU_PAGE_HISTORY;
        return PS_MENU_NONE;
    }
    if(i == ROOT_BOOKMARKS) {
        m->page = PS_MENU_PAGE_BOOKMARKS;
        return PS_MENU_NONE;
    }

    m->open = 0;
    return g_root[i].action;
}

/* --- state --------------------------------------------------------------- */

static void move_sel(ps_menu *m, int dir);

void ps_menu_init(ps_menu *m)
{
    memset(m, 0, sizeof *m);
    m->bar_shown = 1;
}

void ps_menu_open(ps_menu *m)
{
    m->open = 1;
    m->page = PS_MENU_PAGE_ROOT;

    /* Not simply zero: Back is first and is unavailable on the very first
     * page, and a menu that opens on a dead entry costs a press before it does
     * anything. */
    m->sel[PS_MENU_PAGE_ROOT] = 0;
    m->top[PS_MENU_PAGE_ROOT] = 0;
    if(!row_enabled(m, 0))
        move_sel(m, +1);
}

void ps_menu_close(ps_menu *m)
{
    m->open = 0;
}

void ps_menu_toggle(ps_menu *m)
{
    if(m->open)
        ps_menu_close(m);
    else
        ps_menu_open(m);
}

void ps_menu_set_state(ps_menu *m, int can_back, int can_fwd, int bar_shown)
{
    m->can_back  = can_back ? 1 : 0;
    m->can_fwd   = can_fwd ? 1 : 0;
    m->bar_shown = bar_shown ? 1 : 0;
}

void ps_menu_set_lists(ps_menu *m,
                       const char *const *hist, int hist_count,
                       const char *const *marks, int marks_count,
                       int marks_full, int current_marked)
{
    m->hist           = hist;
    m->hist_count     = hist_count;
    m->marks          = marks;
    m->marks_count    = marks_count;
    m->marks_full     = marks_full ? 1 : 0;
    m->current_marked = current_marked ? 1 : 0;

    /* A list can shrink under the selection - removing a bookmark is exactly
     * that - and a selection past the end would read off the array. */
    {
        int n = page_rows(m);

        if(m->sel[m->page] >= n)
            m->sel[m->page] = n > 0 ? n - 1 : 0;
    }
}

void ps_menu_toast(ps_menu *m, const char *text)
{
    if(!text)
        return;

    snprintf(m->toast, sizeof m->toast, "%s", text);
    m->toast_ms = PS_TOAST_MS;
}

void ps_menu_tick(ps_menu *m, int dt_ms)
{
    if(m->toast_ms > 0) {
        m->toast_ms -= dt_ms;
        if(m->toast_ms < 0)
            m->toast_ms = 0;
    }
}

/* Keeps the selected row on screen. The window moves by the minimum needed
 * rather than recentring, so a list you are stepping through scrolls one row
 * at a time instead of jumping half a page under your eyes. */
static void scroll_to_sel(ps_menu *m)
{
    int n   = page_rows(m);
    int sel = m->sel[m->page];
    int top = m->top[m->page];

    if(n <= PS_MENU_VIS) {
        m->top[m->page] = 0;
        return;
    }

    if(sel < top)
        top = sel;
    if(sel >= top + PS_MENU_VIS)
        top = sel - PS_MENU_VIS + 1;

    if(top > n - PS_MENU_VIS)
        top = n - PS_MENU_VIS;
    if(top < 0)
        top = 0;

    m->top[m->page] = top;
}

/* Steps past unavailable entries so the selection never rests on one. */
static void move_sel(ps_menu *m, int dir)
{
    int n = page_rows(m);
    int i;

    if(n <= 0)
        return;

    for(i = 0; i < n; i++) {
        m->sel[m->page] += dir;
        if(m->sel[m->page] < 0)
            m->sel[m->page] = n - 1;
        if(m->sel[m->page] >= n)
            m->sel[m->page] = 0;

        if(row_enabled(m, m->sel[m->page]))
            break;
    }

    scroll_to_sel(m);
}

ps_menu_action ps_menu_input(ps_menu *m, int dpad_up, int dpad_down,
                             int a_pressed, int b_pressed)
{
    if(!m->open)
        return PS_MENU_NONE;

    if(dpad_up)
        move_sel(m, -1);
    if(dpad_down)
        move_sel(m, +1);

    if(b_pressed) {
        /* Out of a list page, then out of the menu. B meaning "close" from
         * inside History would make stepping in feel like a trap. */
        if(m->page != PS_MENU_PAGE_ROOT)
            m->page = PS_MENU_PAGE_ROOT;
        else
            m->open = 0;
        return PS_MENU_NONE;
    }

    if(a_pressed && row_enabled(m, m->sel[m->page]))
        return row_activate(m, m->sel[m->page]);

    return PS_MENU_NONE;
}

/* --- drawing -------------------------------------------------------------
 *
 * fill, frame and label used to live here. They are ps_skin's now, shared with
 * the keyboard and the toolbar. */

void ps_menu_draw(ps_paint *p, const ps_menu *m, ps_text_cache *text)
{
    int x = MENU_X, y = MENU_Y;
    int n, top, i;

    if(!m->open)
        return;

    n   = page_rows(m);
    top = m->top[m->page];

    /* Scrim: dims the page so the panel reads as modal, and guarantees
     * contrast over whatever colour the page happens to be. */
    ps_skin_fill(p, 0, 0, 640, 480, PS_C_SCRIM);

    ps_skin_fill(p, x, y, PS_MENU_W, MENU_H, PS_C_PANEL);
    ps_skin_frame(p, x, y, PS_MENU_W, MENU_H, PS_C_PANEL_EDGE);

    ps_skin_text(p, text, PS_FONT_TITLE, x + PS_PAD * 2, y + PS_PAD + 26,
                 page_title(m), PS_C_ACCENT);
    ps_skin_fill(p, x + PS_PAD * 2, y + PS_PAD + 36, PS_MENU_W - PS_PAD * 4,
                 PS_STROKE, PS_C_ACCENT_DIM);

    if(n <= 0) {
        ps_skin_text(p, text, PS_FONT_UI, x + PS_PAD * 2,
                     y + PS_PAD + MENU_HEAD_H + 28, page_empty(m),
                     PS_C_TEXT_DIM);
        return;
    }

    for(i = 0; i < PS_MENU_VIS && top + i < n; i++) {
        int      row = top + i;
        int      ry  = y + PS_PAD + MENU_HEAD_H + i * PS_ROW_H;
        int      on  = row_enabled(m, row);
        int      sel = (row == m->sel[m->page]);
        ps_color col;
        char     shown[96];

        if(!on)
            col = PS_C_TEXT_DIM;
        else if(sel)
            col = PS_C_TEXT_ON_ACC;
        else
            col = PS_C_TEXT;

        if(sel) {
            /* Solid accent bar rather than an outline: at 480i a filled row is
             * unambiguous from across a room, and it cannot be confused with
             * the hover ring used out on the page. */
            ps_skin_fill(p, x + PS_PAD, ry, PS_MENU_W - PS_PAD * 2,
                         PS_ROW_H - 4, on ? PS_C_ACCENT : PS_C_ACCENT_DIM);
        }

        /* History and bookmark rows are URLs and page titles, which are
         * routinely wider than the panel. */
        ps_skin_text_elide(text, PS_FONT_UI, row_label(m, row),
                           PS_MENU_W - PS_PAD * 4, shown, sizeof shown);

        ps_skin_text(p, text, PS_FONT_UI, x + PS_PAD * 2, ry + 27, shown, col);

        /* The two rows that lead somewhere else say so. */
        if(m->page == PS_MENU_PAGE_ROOT &&
           (row == ROOT_HISTORY || row == ROOT_BOOKMARKS)) {
            ps_skin_tri(p, x + PS_MENU_W - PS_PAD * 2 - 10, ry + 13, 10, 14,
                        +1, col);
        }
    }

    /* Scrollbar, only when there is something off screen. A list that might
     * continue below the fold and gives no sign of it is a list people stop at
     * the ninth entry of. */
    if(n > PS_MENU_VIS) {
        int track_y = y + PS_PAD + MENU_HEAD_H;
        int track_h = PS_MENU_VIS * PS_ROW_H - 4;
        int thumb_h = track_h * PS_MENU_VIS / n;
        int thumb_y = track_y + track_h * top / n;

        if(thumb_h < 12)
            thumb_h = 12;

        ps_skin_fill(p, x + PS_MENU_W - PS_PAD, track_y, PS_STROKE * 2,
                     track_h, PS_C_PANEL_EDGE);
        ps_skin_fill(p, x + PS_MENU_W - PS_PAD, thumb_y, PS_STROKE * 2,
                     thumb_h, PS_C_ACCENT);
    }
}

void ps_menu_draw_toast(ps_paint *p, const ps_menu *m, ps_text_cache *text,
                        int bottom_inset)
{
    int      w, h, x, y;
    ps_font *f;
    ps_color fg = PS_C_TEXT;

    if(m->toast_ms <= 0 || !m->toast[0])
        return;

    f = ps_text_font(text, PS_FONT_UI);
    if(!f)
        return;

    w = ps_font_measure(f, m->toast, strlen(m->toast)) + PS_PAD * 2;
    h = 36;
    x = PS_SAFE_X;

    /* With the toolbar up the inset already clears the overscan, so the
     * title-safe margin is not owed twice. */
    y = bottom_inset ? 480 - bottom_inset - h - PS_PAD
                     : 480 - PS_SAFE_Y - h;

    if(w > 640 - PS_SAFE_X * 2)
        w = 640 - PS_SAFE_X * 2;

    /* No fade: alpha ramping over a solid panel bands visibly at 16bpp, and a
     * toast that pops out cleanly is less distracting than one that smears. */
    ps_skin_fill(p, x, y, w, h, PS_C_PANEL);
    ps_skin_frame(p, x, y, w, h, PS_C_PANEL_EDGE);

    ps_paint_push_clip(p, &(ps_rect){ (int16_t)(x + PS_STROKE),
                                      (int16_t)y,
                                      (int16_t)(x + w - PS_STROKE),
                                      (int16_t)(y + h) });
    ps_skin_text(p, text, PS_FONT_UI, x + PS_PAD, y + 25, m->toast, fg);
    ps_paint_pop_clip(p);
}
