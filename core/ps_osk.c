#include "ps_osk.h"
#include "ps_theme.h"
#include "ps_skin.h"

#include <string.h>
#include <stdio.h>

/* --- geometry ----------------------------------------------------------- */

#define PS_OSK_DEADZONE 40

#define OSK_KEY_W   44
#define OSK_KEY_H   38
#define OSK_GAP     4
#define OSK_PAD     10
#define OSK_FIELD_H 40

#define OSK_W (PS_OSK_COLS * OSK_KEY_W + (PS_OSK_COLS - 1) * OSK_GAP + OSK_PAD * 2)
#define OSK_H (PS_OSK_ROWS * OSK_KEY_H + (PS_OSK_ROWS - 1) * OSK_GAP + \
               OSK_PAD * 2 + OSK_FIELD_H + OSK_GAP)

/* Anchored to the bottom inside title-safe: the keyboard is what you look at
 * while typing, and the field being edited is usually above it. */
#define OSK_X ((640 - OSK_W) / 2)
#define OSK_Y (480 - PS_SAFE_Y - OSK_H)

/* --- key tables --------------------------------------------------------- */

/* Special key codes, above any printable character. */
enum {
    K_NONE  = 0,
    K_SHIFT = 0x100,
    K_SYM,
    K_SPACE,
    K_BKSP,
    K_ENTER,
    K_DOTCOM,
    K_WWW,
    K_SLASH
};

typedef struct {
    unsigned short code;
    unsigned char  span;   /* width in grid cells */
} osk_key;

/* Row 4 is shared by every layer; only the character rows change. Eleven
 * columns lets the number row sit one key per column with no half-offsets,
 * which keeps d-pad movement predictable. */
static const osk_key row_bottom[] = {
    { K_SHIFT, 2 }, { K_SYM, 1 }, { K_SPACE, 4 }, { K_DOTCOM, 2 },
    { K_BKSP, 1 }, { K_ENTER, 1 }
};

static const char *layer_rows[PS_OSK_LAYER_COUNT][4] = {
    { "1234567890-", "qwertyuiop", "asdfghjkl", "zxcvbnm,." },
    { "!@#$%^&*()_", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM<>" },
    { "1234567890+", "~`|\\{}[]\"'", ";:/?=-_", "<>,.!&%$#" }
};

static int row_len(ps_osk_layer layer, int row)
{
    if(row >= 4)
        return (int)(sizeof row_bottom / sizeof row_bottom[0]);
    return (int)strlen(layer_rows[layer][row]);
}

static osk_key key_at(ps_osk_layer layer, int row, int col)
{
    osk_key k = { K_NONE, 1 };

    if(row >= 4) {
        int n = (int)(sizeof row_bottom / sizeof row_bottom[0]);

        if(col >= 0 && col < n)
            return row_bottom[col];
        return k;
    }

    {
        const char *r = layer_rows[layer][row];
        int         n = (int)strlen(r);

        if(col >= 0 && col < n)
            k.code = (unsigned char)r[col];
    }
    return k;
}

/* Grid columns are uniform, but the bottom row's keys span several, so its
 * pixel layout is accumulated rather than indexed. */
static void key_rect(ps_osk_layer layer, int row, int col, ps_rect *out)
{
    int x = OSK_X + OSK_PAD;
    int y = OSK_Y + OSK_PAD + OSK_FIELD_H + OSK_GAP +
            row * (OSK_KEY_H + OSK_GAP);
    int i;

    if(row >= 4) {
        for(i = 0; i < col; i++)
            x += row_bottom[i].span * OSK_KEY_W +
                 (row_bottom[i].span - 1) * OSK_GAP + OSK_GAP;

        out->x0 = (int16_t)x;
        out->x1 = (int16_t)(x + row_bottom[col].span * OSK_KEY_W +
                            (row_bottom[col].span - 1) * OSK_GAP);
    }
    else {
        /* Character rows get shorter down the keyboard, so they are centred
         * rather than left-aligned - the classic staggered QWERTY look. */
        int n     = row_len(layer, row);
        int total = n * OSK_KEY_W + (n - 1) * OSK_GAP;
        int avail = OSK_W - OSK_PAD * 2;

        x += (avail - total) / 2 + col * (OSK_KEY_W + OSK_GAP);
        out->x0 = (int16_t)x;
        out->x1 = (int16_t)(x + OSK_KEY_W);
    }

    out->y0 = (int16_t)y;
    out->y1 = (int16_t)(y + OSK_KEY_H);
}

/* --- lifecycle ---------------------------------------------------------- */

void ps_osk_init(ps_osk *k)
{
    memset(k, 0, sizeof *k);
    k->sel_row = 1;
    k->sel_col = 0;
}

void ps_osk_open(ps_osk *k, const char *label, const char *initial)
{
    k->open  = 1;
    k->layer = PS_OSK_LAYER_LOWER;

    snprintf(k->label, sizeof k->label, "%s", label ? label : "");

    k->len = 0;
    k->text[0] = '\0';
    if(initial) {
        snprintf(k->text, sizeof k->text, "%s", initial);
        k->len = (int)strlen(k->text);
    }

    k->repeat_dir = 0;
    k->repeat_ms  = 0;
    k->caret_ms   = 0;
}

void ps_osk_close(ps_osk *k)
{
    k->open = 0;
}

/* --- input -------------------------------------------------------------- */

static void insert_char(ps_osk *k, char c)
{
    if(k->len + 1 >= PS_OSK_TEXT_MAX)
        return;
    k->text[k->len++] = c;
    k->text[k->len]   = '\0';
}

static void insert_str(ps_osk *k, const char *s)
{
    while(*s)
        insert_char(k, *s++);
}

static void backspace(ps_osk *k)
{
    if(k->len > 0)
        k->text[--k->len] = '\0';
}

static void clamp_sel(ps_osk *k)
{
    int n;

    if(k->sel_row < 0)
        k->sel_row = PS_OSK_ROWS - 1;
    if(k->sel_row >= PS_OSK_ROWS)
        k->sel_row = 0;

    n = row_len(k->layer, k->sel_row);
    if(n <= 0)
        n = 1;
    if(k->sel_col < 0)
        k->sel_col = n - 1;
    if(k->sel_col >= n)
        k->sel_col = n - 1;
}

/* Press-and-hold repeat: one cell immediately, then a slower stream. Without
 * it, crossing eleven columns is eleven separate presses. */
#define OSK_REPEAT_FIRST 380
#define OSK_REPEAT_NEXT  90

static int step_dir(ps_osk *k, int dir, int dt_ms)
{
    if(dir == 0) {
        k->repeat_dir = 0;
        k->repeat_ms  = 0;
        return 0;
    }

    if(dir != k->repeat_dir) {
        k->repeat_dir = dir;
        k->repeat_ms  = OSK_REPEAT_FIRST;
        return 1;
    }

    k->repeat_ms -= dt_ms;
    if(k->repeat_ms <= 0) {
        k->repeat_ms = OSK_REPEAT_NEXT;
        return 1;
    }
    return 0;
}

ps_osk_result ps_osk_update(ps_osk *k, const ps_osk_input *in, int dt_ms)
{
    int dir = 0;
    int dx = 0, dy = 0;

    if(!k->open)
        return PS_OSK_IDLE;

    k->caret_ms += dt_ms;

    /* Y closes, matching the button that opened it. Enter also closes, via
     * the key itself below. */
    if(in->y)
        return PS_OSK_COMMIT;
    if(in->start)
        return PS_OSK_CANCEL;

    /* B is backspace: it is the universal "back" button and deleting is what
     * you want most often while typing. */
    if(in->b)
        backspace(k);

    if(in->left  || in->joy_x < -PS_OSK_DEADZONE) { dir = 1; dx = -1; }
    if(in->right || in->joy_x >  PS_OSK_DEADZONE) { dir = 2; dx = +1; }
    if(in->up    || in->joy_y < -PS_OSK_DEADZONE) { dir = 3; dy = -1; }
    if(in->down  || in->joy_y >  PS_OSK_DEADZONE) { dir = 4; dy = +1; }

    if(step_dir(k, dir, dt_ms)) {
        if(dy) {
            /* Keep the horizontal position roughly where it was when moving
             * between rows of different lengths. */
            int old_n = row_len(k->layer, k->sel_row);
            int frac  = old_n > 1 ? (k->sel_col * 1000) / (old_n - 1) : 0;
            int new_n;

            k->sel_row += dy;
            clamp_sel(k);
            new_n = row_len(k->layer, k->sel_row);
            k->sel_col = new_n > 1 ? (frac * (new_n - 1) + 500) / 1000 : 0;
        }
        if(dx)
            k->sel_col += dx;
        clamp_sel(k);
    }

    if(in->x) {
        /* X is a quick shift toggle without travelling to the key. */
        k->layer = (k->layer == PS_OSK_LAYER_UPPER) ? PS_OSK_LAYER_LOWER
                                                    : PS_OSK_LAYER_UPPER;
        clamp_sel(k);
    }

    if(in->a) {
        osk_key key = key_at(k->layer, k->sel_row, k->sel_col);

        switch(key.code) {
        case K_SHIFT:
            k->layer = (k->layer == PS_OSK_LAYER_UPPER) ? PS_OSK_LAYER_LOWER
                                                        : PS_OSK_LAYER_UPPER;
            clamp_sel(k);
            break;
        case K_SYM:
            k->layer = (k->layer == PS_OSK_LAYER_SYM) ? PS_OSK_LAYER_LOWER
                                                      : PS_OSK_LAYER_SYM;
            clamp_sel(k);
            break;
        case K_SPACE:  insert_char(k, ' ');   break;
        case K_BKSP:   backspace(k);          break;
        case K_DOTCOM: insert_str(k, ".com"); break;
        case K_WWW:    insert_str(k, "www."); break;
        case K_SLASH:  insert_char(k, '/');   break;
        case K_ENTER:  return PS_OSK_COMMIT;
        case K_NONE:   break;
        default:
            if(key.code < 0x100)
                insert_char(k, (char)key.code);
            break;
        }
    }

    return PS_OSK_EDITING;
}

int ps_osk_hit(const ps_osk *k, int x, int y)
{
    if(!k->open)
        return 0;
    return x >= OSK_X && x < OSK_X + OSK_W && y >= OSK_Y && y < OSK_Y + OSK_H;
}

void ps_osk_point(ps_osk *k, int x, int y)
{
    int r, c;

    if(!k->open)
        return;

    for(r = 0; r < PS_OSK_ROWS; r++) {
        int n = row_len(k->layer, r);

        for(c = 0; c < n; c++) {
            ps_rect kr;

            key_rect(k->layer, r, c, &kr);
            if(x >= kr.x0 && x < kr.x1 && y >= kr.y0 && y < kr.y1) {
                k->sel_row = r;
                k->sel_col = c;
                return;
            }
        }
    }
}

/* --- drawing ------------------------------------------------------------ */

/* The metal, the panel and the recessed field all live in ps_skin now. They
 * started here, and moved out when the toolbar needed the same material: two
 * copies of a look is how two things that should match stop matching. */

static const char *key_caption(unsigned short code, char *buf)
{
    switch(code) {
    case K_SHIFT:  return "Shift";
    case K_SYM:    return "Sym";
    case K_SPACE:  return "Space";
    case K_BKSP:   return "Del";
    case K_ENTER:  return "Go";
    case K_DOTCOM: return ".com";
    case K_WWW:    return "www.";
    case K_SLASH:  return "/";
    default:
        buf[0] = (char)code;
        buf[1] = '\0';
        return buf;
    }
}

void ps_osk_draw(ps_paint *p, const ps_osk *k, ps_text_cache *tc)
{
    int r, c;
    int fx, fy, fw;

    if(!k->open)
        return;

    /* Scrim, so the page behind cannot compete with the keys. */
    ps_skin_fill(p, 0, 0, 640, 480, PS_C_SCRIM);

    /* Panel: same metal, larger and calmer. */
    ps_skin_panel(p, OSK_X, OSK_Y, OSK_W, OSK_H);

    /* Edit field. */
    fx = OSK_X + OSK_PAD;
    fy = OSK_Y + OSK_PAD;
    fw = OSK_W - OSK_PAD * 2;

    ps_skin_well(p, fx, fy, fw, OSK_FIELD_H);

    {
        ps_font *f = ps_text_font(tc, PS_FONT_UI);
        ps_rect  cr;

        if(k->label[0])
            ps_skin_text_center(p, tc, PS_FONT_UI, fx + 44, fy + 26, k->label,
                                PS_C_TEXT_DIM);

        cr.x0 = (int16_t)(fx + 88);
        cr.y0 = (int16_t)fy;
        cr.x1 = (int16_t)(fx + fw - 4);
        cr.y1 = (int16_t)(fy + OSK_FIELD_H);
        ps_paint_push_clip(p, &cr);

        if(f) {
            int tw = ps_font_measure(f, k->text, (size_t)k->len);
            int tx = fx + 92;

            /* Scroll to keep the caret visible once the text outgrows the
             * field, which it will on any real URL. */
            if(tx + tw > fx + fw - 12)
                tx = fx + fw - 12 - tw;

            ps_font_draw(p, f, tx, fy + 26, k->text, (size_t)k->len,
                         PS_C_TEXT);

            if((k->caret_ms / 500) % 2 == 0)
                ps_skin_fill(p, tx + tw + 1, fy + 8, 2, OSK_FIELD_H - 16,
                             PS_C_ACCENT);
        }
        ps_paint_pop_clip(p);
    }

    /* Keys. */
    for(r = 0; r < PS_OSK_ROWS; r++) {
        int n = row_len(k->layer, r);

        for(c = 0; c < n; c++) {
            osk_key key = key_at(k->layer, r, c);
            ps_rect kr;
            char    buf[2];
            int     active = (r == k->sel_row && c == k->sel_col);
            int     held   = (key.code == K_SHIFT &&
                            k->layer == PS_OSK_LAYER_UPPER) ||
                           (key.code == K_SYM &&
                            k->layer == PS_OSK_LAYER_SYM);

            key_rect(k->layer, r, c, &kr);
            ps_skin_key(p, &kr, held, active, 0);

            ps_skin_text_center(p, tc, PS_FONT_UI, (kr.x0 + kr.x1) / 2,
                                kr.y0 + OSK_KEY_H / 2 + 6,
                                key_caption(key.code, buf),
                                ps_skin_ink(active, 0));
        }
    }
}
