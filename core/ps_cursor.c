#include "ps_cursor.h"
#include "ps_theme.h"

#include <stdlib.h>
#include <string.h>

/* --- baked set ---------------------------------------------------------- */

/* Format documented in tools/README.md and written by tools/curbake.c. */

#define PSC_HDR_SIZE   16
#define PSC_ROLE_SIZE  16
#define PSC_FRAME_SIZE 16

typedef struct {
    int first, count;
} psc_role;

struct ps_cursor_set {
    const ps_gfx_backend *gfx;
    psc_role              roles[PS_CUR_ROLE_COUNT];
    ps_cursor_frame      *frames;
    int                   nframes;
};

static int is_pow2(int v)
{
    return v > 0 && (v & (v - 1)) == 0;
}

/* The PVR's valid texture dimensions are 8 through 1024. Padding to a power of
 * two is not sufficient on its own: a 1 or 4 pixel wide texture is not
 * representable at all, and uploading one produces garbage rather than an
 * error. One-pixel strips are common - they are how the era drew gradient
 * bars - so this is a real case, not a hypothetical. */
#define PS_TEX_MIN_DIM 8

static int next_pow2(int v)
{
    int p = PS_TEX_MIN_DIM;

    while(p < v)
        p <<= 1;
    return p;
}

/* Frames arrive as tightly packed ARGB4444 at their true size. The PVR needs
 * power-of-two for a twiddled upload, so anything else is padded and the used
 * fraction reported through u1/v1 - same contract as the image cache. */
static int upload_frame(ps_cursor_set *set, ps_cursor_frame *f,
                        const uint8_t *px, int w, int h)
{
    int       pw = next_pow2(w), ph = next_pow2(h);
    uint16_t *tmp;
    int       y, x;

    if(pw > 256 || ph > 256)
        return -1;

    if(is_pow2(w) && is_pow2(h)) {
        /* Common case: 32x32 needs no padding, but the source is byte data
         * that may be unaligned, so it still goes through the accessor. */
        tmp = (uint16_t *)malloc((size_t)w * h * 2);
        if(!tmp)
            return -1;
        for(y = 0; y < w * h; y++)
            tmp[y] = ps_rd_u16le(px + (size_t)y * 2);
        pw = w;
        ph = h;
    }
    else {
        tmp = (uint16_t *)calloc((size_t)pw * ph, sizeof(uint16_t));
        if(!tmp)
            return -1;
        for(y = 0; y < h; y++)
            for(x = 0; x < w; x++)
                tmp[y * pw + x] = ps_rd_u16le(px + ((size_t)y * w + x) * 2);
    }

    f->tex = set->gfx->upload_texture(set->gfx->self, tmp, pw, ph,
                                      PS_FMT_ARGB4444);
    free(tmp);

    if(f->tex == PS_TEXTURE_NONE)
        return -1;

    f->u1 = (float)w / (float)pw;
    f->v1 = (float)h / (float)ph;
    return 0;
}

ps_cursor_set *ps_cursor_set_load(const ps_gfx_backend *gfx,
                                  const void *data, size_t len)
{
    const uint8_t *b = (const uint8_t *)data;
    ps_cursor_set *set;
    uint32_t       n_roles, n_frames;
    size_t         role_off, frame_off;
    int            i;

    if(!gfx || !b || len < PSC_HDR_SIZE)
        return NULL;

    if(b[0] != 'P' || b[1] != 'S' || b[2] != 'C' || b[3] != '1')
        return NULL;
    if(ps_rd_u32le(b + 4) != 1)
        return NULL;

    n_roles  = ps_rd_u32le(b + 8);
    n_frames = ps_rd_u32le(b + 12);

    /* Hostile or truncated input must fail here, not halfway through the
     * upload loop with textures already leaked. */
    if(n_roles != PS_CUR_ROLE_COUNT || n_frames == 0 || n_frames > 4096)
        return NULL;

    role_off  = PSC_HDR_SIZE;
    frame_off = role_off + (size_t)n_roles * PSC_ROLE_SIZE;

    if(len < frame_off + (size_t)n_frames * PSC_FRAME_SIZE)
        return NULL;

    set = (ps_cursor_set *)calloc(1, sizeof *set);
    if(!set)
        return NULL;

    set->gfx     = gfx;
    set->nframes = (int)n_frames;
    set->frames  = (ps_cursor_frame *)calloc(n_frames, sizeof *set->frames);
    if(!set->frames) {
        free(set);
        return NULL;
    }

    for(i = 0; i < PS_CUR_ROLE_COUNT; i++) {
        const uint8_t *r = b + role_off + (size_t)i * PSC_ROLE_SIZE;
        uint32_t cnt   = ps_rd_u32le(r);
        uint32_t first = ps_rd_u32le(r + 4);

        if(cnt == 0 || first + cnt > n_frames) {
            set->roles[i].count = 0;
            continue;
        }
        set->roles[i].count = (int)cnt;
        set->roles[i].first = (int)first;
    }

    for(i = 0; i < (int)n_frames; i++) {
        const uint8_t *e = b + frame_off + (size_t)i * PSC_FRAME_SIZE;
        ps_cursor_frame *f = &set->frames[i];
        int      w   = (int)ps_rd_u16le(e);
        int      h   = (int)ps_rd_u16le(e + 2);
        uint32_t off = ps_rd_u32le(e + 12);

        f->w        = (int16_t)w;
        f->h        = (int16_t)h;
        f->hot_x    = ps_rd_s16le(e + 4);
        f->hot_y    = ps_rd_s16le(e + 6);
        f->delay_ms = (int32_t)ps_rd_u32le(e + 8);

        if(w <= 0 || h <= 0 || w > 256 || h > 256 ||
           (size_t)off + (size_t)w * h * 2 > len) {
            ps_cursor_set_free(set);
            return NULL;
        }

        if(upload_frame(set, f, b + off, w, h) != 0) {
            /* Out of VRAM partway: keep what loaded and drop the rest, so a
             * cursor still appears rather than nothing at all. */
            set->nframes = i;
            break;
        }
    }

    return set;
}

void ps_cursor_set_free(ps_cursor_set *set)
{
    int i;

    if(!set)
        return;

    for(i = 0; i < set->nframes; i++) {
        if(set->frames[i].tex != PS_TEXTURE_NONE)
            set->gfx->free_texture(set->gfx->self, set->frames[i].tex);
    }
    free(set->frames);
    free(set);
}

/* --- role resolution ---------------------------------------------------- */

ps_cursor_role ps_cursor_role_from_css(const char *n)
{
    if(!n || !*n)
        return PS_CUR_DEFAULT;

    if(!strcmp(n, "pointer"))       return PS_CUR_POINTER;
    if(!strcmp(n, "text"))          return PS_CUR_TEXT;
    if(!strcmp(n, "vertical-text")) return PS_CUR_TEXT;
    if(!strcmp(n, "wait"))          return PS_CUR_WAIT;
    if(!strcmp(n, "progress"))      return PS_CUR_PROGRESS;
    if(!strcmp(n, "help"))          return PS_CUR_HELP;
    if(!strcmp(n, "crosshair"))     return PS_CUR_CROSSHAIR;
    if(!strcmp(n, "cell"))          return PS_CUR_CROSSHAIR;
    if(!strcmp(n, "move"))          return PS_CUR_MOVE;
    if(!strcmp(n, "all-scroll"))    return PS_CUR_MOVE;
    if(!strcmp(n, "grab"))          return PS_CUR_MOVE;
    if(!strcmp(n, "grabbing"))      return PS_CUR_MOVE;
    if(!strcmp(n, "not-allowed"))   return PS_CUR_NOT_ALLOWED;
    if(!strcmp(n, "no-drop"))       return PS_CUR_NOT_ALLOWED;
    if(!strcmp(n, "context-menu"))  return PS_CUR_CONTEXT_MENU;
    if(!strcmp(n, "alias"))         return PS_CUR_CONTEXT_MENU;
    if(!strcmp(n, "copy"))          return PS_CUR_CONTEXT_MENU;

    /* Resize cursors collapse onto the four the theme actually provides: a
     * north-west arrow and a south-east arrow are the same diagonal. */
    if(!strcmp(n, "e-resize")  || !strcmp(n, "w-resize") ||
       !strcmp(n, "ew-resize") || !strcmp(n, "col-resize"))
        return PS_CUR_EW_RESIZE;
    if(!strcmp(n, "n-resize")  || !strcmp(n, "s-resize") ||
       !strcmp(n, "ns-resize") || !strcmp(n, "row-resize"))
        return PS_CUR_NS_RESIZE;
    if(!strcmp(n, "ne-resize") || !strcmp(n, "sw-resize") ||
       !strcmp(n, "nesw-resize"))
        return PS_CUR_NESW_RESIZE;
    if(!strcmp(n, "nw-resize") || !strcmp(n, "se-resize") ||
       !strcmp(n, "nwse-resize"))
        return PS_CUR_NWSE_RESIZE;

    return PS_CUR_DEFAULT;
}

/* Falls back down a chain so a partial set still shows something sensible:
 * an animated role degrades to its static twin, everything else to the
 * default arrow, and finally to the built-in vector one. */
static int resolve_role(const ps_cursor_set *set, ps_cursor_role role)
{
    ps_cursor_role chain[3];
    int            n = 0, i;

    chain[n++] = role;
    if(role == PS_CUR_WAIT)
        chain[n++] = PS_CUR_WAIT_STATIC;
    else if(role == PS_CUR_PROGRESS)
        chain[n++] = PS_CUR_PROGRESS_STATIC;
    chain[n++] = PS_CUR_DEFAULT;

    for(i = 0; i < n; i++) {
        if(set->roles[chain[i]].count > 0)
            return (int)chain[i];
    }
    return -1;
}

/* --- movement ----------------------------------------------------------- */

void ps_cursor_init(ps_cursor *c, int view_w, int view_h)
{
    memset(c, 0, sizeof *c);
    c->view_w = view_w;
    c->view_h = view_h;
    c->x      = (float)view_w * 0.5f;
    c->y      = (float)view_h * 0.5f;
    c->role   = PS_CUR_DEFAULT;
}

void ps_cursor_set_art(ps_cursor *c, const ps_cursor_set *set)
{
    c->set   = set;
    c->frame = 0;
    c->elapsed_ms = 0;
}

void ps_cursor_set_role(ps_cursor *c, ps_cursor_role role)
{
    if(c->role == role)
        return;

    c->role       = role;
    c->frame      = 0;
    c->elapsed_ms = 0;
}

/* Deadzone, then a squared response curve. Squared gives fine control near
 * centre for picking out a link and full speed at the edge for crossing the
 * screen, which linear cannot do at once. */
static float axis_curve(int raw)
{
    float n;

    if(raw > -PS_CURSOR_DEADZONE && raw < PS_CURSOR_DEADZONE)
        return 0.0f;

    /* Rescale so the curve starts at zero just outside the deadzone rather
     * than jumping. */
    if(raw > 0)
        n = (float)(raw - PS_CURSOR_DEADZONE) / (127.0f - PS_CURSOR_DEADZONE);
    else
        n = (float)(raw + PS_CURSOR_DEADZONE) / (128.0f - PS_CURSOR_DEADZONE);

    if(n > 1.0f)
        n = 1.0f;
    if(n < -1.0f)
        n = -1.0f;

    return n * n * (n < 0.0f ? -1.0f : 1.0f);
}

void ps_cursor_update(ps_cursor *c, int joy_x, int joy_y, int dt_ms)
{
    float dt = (float)dt_ms / 1000.0f;

    /* A long stall must not teleport the cursor across the screen. */
    if(dt > 0.1f)
        dt = 0.1f;

    c->x += axis_curve(joy_x) * PS_CURSOR_MAX_SPEED * dt;
    c->y += axis_curve(joy_y) * PS_CURSOR_MAX_SPEED * dt;

    if(c->x < 0.0f)
        c->x = 0.0f;
    if(c->y < 0.0f)
        c->y = 0.0f;
    if(c->x > (float)(c->view_w - 1))
        c->x = (float)(c->view_w - 1);
    if(c->y > (float)(c->view_h - 1))
        c->y = (float)(c->view_h - 1);

    /* Animation is wall-clock driven so a spinner keeps its authored speed at
     * 50Hz PAL as well as 60Hz NTSC. */
    if(c->set) {
        int r = resolve_role(c->set, c->role);

        if(r >= 0 && c->set->roles[r].count > 1) {
            const psc_role *ri = &c->set->roles[r];

            if(c->frame >= ri->count)
                c->frame = 0;

            c->elapsed_ms += dt_ms;
            for(;;) {
                int d = c->set->frames[ri->first + c->frame].delay_ms;

                if(d <= 0)
                    d = 100;
                if(c->elapsed_ms < d)
                    break;
                c->elapsed_ms -= d;
                c->frame = (c->frame + 1) % ri->count;
            }
        }
    }
}

/* --- drawing ------------------------------------------------------------ */

static void fill(ps_paint *p, int x, int y, int w, int h, ps_color col)
{
    ps_rect r;

    if(w <= 0 || h <= 0)
        return;

    r.x0 = (int16_t)x;
    r.y0 = (int16_t)y;
    r.x1 = (int16_t)(x + w);
    r.y1 = (int16_t)(y + h);
    ps_paint_rect(p, &r, col);
}

/* Arrow profile: for each row, the x offset and width of the filled span.
 *
 * The built-in fallback, used when no set is loaded or the role is missing.
 * Built from horizontal runs rather than a texture because it costs no VRAM
 * and is exact at 480i where a scaled bitmap crawls. */
typedef struct {
    unsigned char x, w;
} ps_arrow_row;

static const ps_arrow_row arrow[] = {
    {  0,  2 }, {  0,  3 }, {  0,  4 }, {  0,  5 }, {  0,  6 }, {  0,  7 },
    {  0,  8 }, {  0,  9 }, {  0, 10 }, {  0, 11 }, {  0, 12 }, {  0, 13 },
    {  0, 14 }, {  0, 15 }, {  0, 16 }, {  0, 17 }, {  0, 10 }, {  0,  8 },
    {  6,  7 }, {  7,  7 }, {  8,  7 }, {  9,  7 }, { 10,  6 }, { 11,  5 },
    { 12,  4 }, { 13,  3 }
};

#define ARROW_ROWS ((int)(sizeof arrow / sizeof arrow[0]))

static void draw_vector_arrow(ps_paint *p, int x, int y, ps_cursor_role role)
{
    ps_color body;
    int      i;

    switch(role) {
    case PS_CUR_POINTER:  body = PS_C_ACCENT;   break;
    case PS_CUR_WAIT:
    case PS_CUR_PROGRESS: body = PS_C_TEXT_DIM; break;
    default:              body = PS_ARGB(255, 244, 246, 252); break;
    }

    /* Outline first, one pixel proud on every side. The cursor sits over page
     * content we do not control, so it has to carry its own contrast: a white
     * arrow vanishes on a white page and an accent arrow vanishes on the
     * yellow ones this browser was built for. */
    for(i = 0; i < ARROW_ROWS; i++)
        fill(p, x + arrow[i].x - 1, y + i - 1, arrow[i].w + 2, 3, PS_C_OUTLINE);

    for(i = 0; i < ARROW_ROWS; i++)
        fill(p, x + arrow[i].x, y + i, arrow[i].w, 1, body);
}

void ps_cursor_draw(ps_paint *p, const ps_cursor *c)
{
    int x = (int)c->x;
    int y = (int)c->y;
    int r;

    if(!c->set) {
        draw_vector_arrow(p, x, y, c->role);
        return;
    }

    r = resolve_role(c->set, c->role);
    if(r < 0) {
        draw_vector_arrow(p, x, y, c->role);
        return;
    }

    {
        const psc_role        *ri = &c->set->roles[r];
        int                    fi = ri->count > 1 ? c->frame % ri->count : 0;
        const ps_cursor_frame *f  = &c->set->frames[ri->first + fi];
        ps_rect                dst;

        /* The hotspot is the pixel the cursor actually points at, so the art
         * is offset by it rather than drawn from its top-left. */
        dst.x0 = (int16_t)(x - f->hot_x);
        dst.y0 = (int16_t)(y - f->hot_y);
        dst.x1 = (int16_t)(dst.x0 + f->w);
        dst.y1 = (int16_t)(dst.y0 + f->h);

        ps_paint_image(p, f->tex, &dst, 0.0f, 0.0f, f->u1, f->v1,
                       PS_ARGB(255, 255, 255, 255));
    }
}

/* Hover ring.
 *
 * This, not the cursor, is what tells you what you are about to click. At
 * couch distance a 3px ring around the target reads instantly where a cursor
 * tip does not. Drawn dark-then-accent so it survives any page colour. */
void ps_cursor_draw_hover(ps_paint *p, const ps_rect *r)
{
    int x0 = r->x0, y0 = r->y0, x1 = r->x1, y1 = r->y1;
    int w  = x1 - x0, h = y1 - y0;
    int o;

    if(w <= 0 || h <= 0)
        return;

    for(o = 3; o >= 1; o--) {
        ps_color col = (o == 3) ? PS_C_OUTLINE : PS_C_ACCENT;

        fill(p, x0 - o, y0 - o, w + o * 2, 1, col);
        fill(p, x0 - o, y1 + o - 1, w + o * 2, 1, col);
        fill(p, x0 - o, y0 - o, 1, h + o * 2, col);
        fill(p, x1 + o - 1, y0 - o, 1, h + o * 2, col);
    }
}
