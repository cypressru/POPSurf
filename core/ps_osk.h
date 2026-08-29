/* Controller-driven on-screen keyboard. */
#ifndef PS_OSK_H
#define PS_OSK_H

#include "ps_types.h"
#include "ps_paint.h"
#include "ps_text.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PS_OSK_TEXT_MAX 512

/* Grid is fixed: a full QWERTY plus a modifier row. */
#define PS_OSK_ROWS     5
#define PS_OSK_COLS     11

typedef enum {
    PS_OSK_LAYER_LOWER = 0,
    PS_OSK_LAYER_UPPER,
    PS_OSK_LAYER_SYM,
    PS_OSK_LAYER_COUNT
} ps_osk_layer;

typedef enum {
    PS_OSK_IDLE = 0,   /* closed */
    PS_OSK_EDITING,
    PS_OSK_COMMIT,     /* Enter or Y: caller should take the text */
    PS_OSK_CANCEL
} ps_osk_result;

typedef struct {
    int  open;
    char text[PS_OSK_TEXT_MAX];
    int  len;

    char label[64];

    ps_osk_layer layer;
    int          sel_row, sel_col;

    /* Held-direction repeat, so crossing the grid does not need one press per
     * key while a single tap still moves exactly one cell. */
    int repeat_dir;
    int repeat_ms;

    int caret_ms;
} ps_osk;

void ps_osk_init(ps_osk *k);

/* label is shown above the field, e.g. "Address" or a form field's name. */
void ps_osk_open(ps_osk *k, const char *label, const char *initial);
void ps_osk_close(ps_osk *k);

static inline int ps_osk_is_open(const ps_osk *k) { return k->open; }
static inline const char *ps_osk_text(const ps_osk *k) { return k->text; }

/* Edge-triggered button flags for one frame. */
typedef struct {
    int up, down, left, right;   /* d-pad, level (repeat handled inside) */
    int a, b, x, y;              /* edges */
    int start;
    int joy_x, joy_y;            /* stick also drives selection */
} ps_osk_input;

ps_osk_result ps_osk_update(ps_osk *k, const ps_osk_input *in, int dt_ms);

/* Returns non-zero if the point is inside the panel, so the caller knows the
 * page must not also receive the click. */
int ps_osk_hit(const ps_osk *k, int x, int y);

/* Moves selection to the key under the cursor, for pointer-driven typing. */
void ps_osk_point(ps_osk *k, int x, int y);

void ps_osk_draw(ps_paint *p, const ps_osk *k, ps_text_cache *text);

#ifdef __cplusplus
}
#endif

#endif /* PS_OSK_H */
