/* Shell menu, history/bookmark lists, and status toast. */
#ifndef PS_MENU_H
#define PS_MENU_H

#include "ps_types.h"
#include "ps_paint.h"
#include "ps_text.h"
#include "ps_cursor.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PS_MENU_NONE = 0,
    PS_MENU_BACK,
    PS_MENU_FORWARD,
    PS_MENU_RELOAD,
    PS_MENU_HOME,
    PS_MENU_DIRECTORY,
    PS_MENU_ADDRESS,
    PS_MENU_TOOLBAR,
    PS_MENU_QUIT,

    /* These carry a row with them; read it with ps_menu_index. */
    PS_MENU_GO_HISTORY,
    PS_MENU_GO_BOOKMARK,
    PS_MENU_BOOKMARK_ADD,
    PS_MENU_BOOKMARK_DEL
} ps_menu_action;

typedef enum {
    PS_MENU_PAGE_ROOT = 0,
    PS_MENU_PAGE_HISTORY,
    PS_MENU_PAGE_BOOKMARKS,
    PS_MENU_PAGE_COUNT
} ps_menu_page;

/* Rows visible at once. The root list is exactly this long; history is not, so
 * anything past it scrolls under the selection. */
#define PS_MENU_VIS 9

typedef struct {
    int          open;
    ps_menu_page page;

    /* Selected row and the first row on screen, per page, so stepping into
     * history and back out again does not lose your place. */
    int sel[PS_MENU_PAGE_COUNT];
    int top[PS_MENU_PAGE_COUNT];

    /* What the shell can currently do. Back was always drawn live before,
     * which meant a fresh session offered a step backwards it could not take;
     * an item that does nothing is worse than one that says it cannot. */
    int can_back;
    int can_fwd;
    int bar_shown;

    /* Borrowed, not owned: these point at the shell's own arrays and are
     * re-supplied every frame. Nothing here outlives a call. */
    const char *const *hist;
    int                hist_count;
    const char *const *marks;
    int                marks_count;
    int                marks_full;
    int                current_marked;

    /* Row an action refers to, resolved by ps_menu_input. Held separately from
     * the selection because a list page can carry a command row above its list
     * - the bookmarks page leads with add-or-remove - so the selected row and
     * the record it means are not the same number. */
    int picked;

    /* Toast state. */
    char toast[128];
    int  toast_ms;
} ps_menu;

void ps_menu_init(ps_menu *m);

/* Opens on the root page. A menu that reopened on whichever list you were last
 * looking at would hide its own top level. */
void ps_menu_toggle(ps_menu *m);
void ps_menu_open(ps_menu *m);
void ps_menu_close(ps_menu *m);
static inline int ps_menu_is_open(const ps_menu *m) { return m->open; }

/* Kept in step by the shell each frame. Also decides the toolbar entry's
 * wording, which is the only label on the root page that is not fixed. */
void ps_menu_set_state(ps_menu *m, int can_back, int can_fwd, int bar_shown);

/* Labels for the list pages, most recent first. current_marked says whether
 * the page on screen is already bookmarked, which decides whether the
 * bookmarks page offers to add it or to remove it. */
void ps_menu_set_lists(ps_menu *m,
                       const char *const *hist, int hist_count,
                       const char *const *marks, int marks_count,
                       int marks_full, int current_marked);

/* Shows a message for a couple of seconds. */
void ps_menu_toast(ps_menu *m, const char *text);

void ps_menu_tick(ps_menu *m, int dt_ms);

/* D-pad and stick move the selection; A activates; B steps back a page, or
 * closes the menu from the root. Returns the chosen action, or PS_MENU_NONE.
 * Consumes input so the page never sees it while open. */
ps_menu_action ps_menu_input(ps_menu *m, int dpad_up, int dpad_down,
                             int a_pressed, int b_pressed);

/* The row a PS_MENU_GO_* or PS_MENU_BOOKMARK_DEL action refers to, indexed
 * into the array the shell supplied. Undefined for every other action. */
static inline int ps_menu_index(const ps_menu *m) { return m->picked; }

void ps_menu_draw(ps_paint *p, const ps_menu *m, ps_text_cache *text);

/* bottom_inset is what the toolbar has already claimed along the bottom edge,
 * so the toast sits above it rather than behind it. Zero when the toolbar is
 * hidden, which is the shape this had before there was one. */
void ps_menu_draw_toast(ps_paint *p, const ps_menu *m, ps_text_cache *text,
                        int bottom_inset);

#ifdef __cplusplus
}
#endif

#endif /* PS_MENU_H */
