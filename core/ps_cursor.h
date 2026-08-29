/* Pointer input and animated cursor assets. */
#ifndef PS_CURSOR_H
#define PS_CURSOR_H

#include "ps_types.h"
#include "ps_paint.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Roles are CSS cursor keywords where one exists, so litehtml's computed
 * cursor value maps straight onto them. Order and count must match the baked
 * asset; tools/curbake.c writes the role table in exactly this order. */
typedef enum {
    PS_CUR_DEFAULT = 0,
    PS_CUR_POINTER,
    PS_CUR_TEXT,
    PS_CUR_WAIT,
    PS_CUR_PROGRESS,
    PS_CUR_HELP,
    PS_CUR_CROSSHAIR,
    PS_CUR_MOVE,
    PS_CUR_NOT_ALLOWED,
    PS_CUR_EW_RESIZE,
    PS_CUR_NS_RESIZE,
    PS_CUR_NESW_RESIZE,
    PS_CUR_NWSE_RESIZE,
    PS_CUR_CONTEXT_MENU,
    PS_CUR_HANDWRITING,
    PS_CUR_LOCATION,
    PS_CUR_PERSON,
    PS_CUR_WAIT_STATIC,
    PS_CUR_PROGRESS_STATIC,
    PS_CUR_ROLE_COUNT
} ps_cursor_role;

#define PS_CURSOR_MAX_FRAMES 64

typedef struct {
    ps_texture tex;
    int16_t    w, h;
    int16_t    hot_x, hot_y;
    int32_t    delay_ms;
    float      u1, v1;
} ps_cursor_frame;

typedef struct ps_cursor_set ps_cursor_set;

/* Parses a baked .psc blob and uploads every frame. The blob is not retained.
 * Returns NULL if the data is not a valid set, which callers must treat as
 * "no art", never as fatal. */
ps_cursor_set *ps_cursor_set_load(const ps_gfx_backend *gfx,
                                  const void *data, size_t len);
void           ps_cursor_set_free(ps_cursor_set *set);

/* Maps a CSS cursor keyword to a role. Unknown keywords give PS_CUR_DEFAULT,
 * which is what a browser does with an unrecognised cursor value. */
ps_cursor_role ps_cursor_role_from_css(const char *css_name);

typedef struct {
    /* Sub-pixel position, so slow stick movement still moves the cursor
     * instead of quantising to nothing. */
    float x, y;
    int   view_w, view_h;

    const ps_cursor_set *set;
    ps_cursor_role       role;

    /* Animation, advanced by ps_cursor_update. */
    int frame;
    int elapsed_ms;
} ps_cursor;

/* Analog readings below this are treated as centred. Dreamcast sticks rest
 * off-centre often enough that without a deadzone the cursor drifts on its
 * own, which reads as a broken controller. */
#define PS_CURSOR_DEADZONE 16

/* Pixels per second at full deflection. */
#define PS_CURSOR_MAX_SPEED 520.0f

void ps_cursor_init(ps_cursor *c, int view_w, int view_h);

void ps_cursor_set_art(ps_cursor *c, const ps_cursor_set *set);

/* joy_x/joy_y are raw -128..127 controller axes. dt_ms keeps travel speed and
 * animation independent of frame rate, which matters at 50Hz PAL. */
void ps_cursor_update(ps_cursor *c, int joy_x, int joy_y, int dt_ms);

/* Changing role restarts its animation, so a spinner always begins at frame
 * zero rather than partway through. */
void ps_cursor_set_role(ps_cursor *c, ps_cursor_role role);

/* Drawn last, on top of the page. */
void ps_cursor_draw(ps_paint *p, const ps_cursor *c);

/* Ring around the element under the cursor. This, rather than the cursor art,
 * is what makes a target readable from across a room. */
void ps_cursor_draw_hover(ps_paint *p, const ps_rect *r);

static inline int ps_cursor_x(const ps_cursor *c) { return (int)c->x; }
static inline int ps_cursor_y(const ps_cursor *c) { return (int)c->y; }

#ifdef __cplusplus
}
#endif

#endif /* PS_CURSOR_H */
