/* The JDK, as much of it as an applet touches, in C.
 *
 * Every class named in ps_jvm.c's native list is implemented here. They are C
 * rather than bytecode for a reason that is easy to lose sight of: loading a
 * real class library to get drawLine would be most of a megabyte of resident
 * memory and a class loader to match, on a machine with twelve, to end up
 * calling a function this file calls directly.
 *
 * The scope is JDK 1.1's applet surface and nothing beyond it. When an applet
 * asks for something absent, the interpreter reports the exact method it
 * wanted rather than guessing - a browser that silently no-ops an unknown call
 * draws a subtly wrong picture, which is worse than a blank box and a line in
 * the log saying which method to add next.
 */
#include "ps_jvm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ps_applet.h"
#include "ps_jlib.h"
#include "ps_jgeom.h"
#include "ps_joff.h"
#include "ps_jawt.h"
#include "ps_jpoly.h"
#include "ps_jimg.h"

/* Colour lives in the object's length field rather than an allocation: a
 * java.awt.Color is one packed int and applets make a great many of them. */
#define COLOR_OF(o)     ((uint32_t)(o)->len)
#define SET_COLOR(o, v) ((o)->len = (int32_t)(v))

#define ARG_I(n)  (args[n].i)
#define ARG_O(n)  (args[n].o)
#define ARG_D(n)  (args[n].d)

static ps_jgfx *gfx_of(ps_jvm *vm, ps_jobj *o)
{
    if(o && o->native)
        return (ps_jgfx *)o->native;
    return vm->gfx;
}

/* Matches on name and, when it matters, descriptor. Passing NULL for the
 * descriptor accepts any overload, which is right for the calls that have only
 * one shape and keeps the table readable. */
static int is(const char *name, const char *desc, const char *n,
              const char *d)
{
    if(strcmp(name, n))
        return 0;
    return d ? !strcmp(desc, d) : 1;
}

/* --- java.awt.Graphics ---------------------------------------------------
 *
 * The whole of the drawing surface an applet can reach, forwarded to ps_jgfx.
 * Coordinate and inclusive/exclusive semantics are handled there, so this is
 * purely unpacking arguments.
 */
static int graphics(ps_jvm *vm, const char *n, const char *d, ps_jslot *args,
                    int nargs, ps_jslot *ret, int *handled)
{
    ps_jgfx *g = gfx_of(vm, nargs > 0 ? ARG_O(0) : NULL);

    (void)ret;

    if(!g)
        return 0;

    *handled = 1;

    if(is(n, d, "setColor", NULL)) {
        if(ARG_O(1))
            ps_jgfx_set_color(g, COLOR_OF(ARG_O(1)));
        return 0;
    }
    if(is(n, d, "fillRect", NULL)) {
        ps_jgfx_fill_rect(g, ARG_I(1), ARG_I(2), ARG_I(3), ARG_I(4));
        return 0;
    }
    if(is(n, d, "drawRect", NULL)) {
        ps_jgfx_draw_rect(g, ARG_I(1), ARG_I(2), ARG_I(3), ARG_I(4));
        return 0;
    }
    if(is(n, d, "clearRect", NULL)) {
        /* No setBackground yet, so white - which is the applet default
         * background in every browser of the period. */
        ps_jgfx_clear_rect(g, ARG_I(1), ARG_I(2), ARG_I(3), ARG_I(4),
                           0xffffffffu);
        return 0;
    }
    if(is(n, d, "drawLine", NULL)) {
        ps_jgfx_draw_line(g, ARG_I(1), ARG_I(2), ARG_I(3), ARG_I(4));
        return 0;
    }
    if(is(n, d, "drawOval", NULL)) {
        ps_jgfx_draw_oval(g, ARG_I(1), ARG_I(2), ARG_I(3), ARG_I(4));
        return 0;
    }
    if(is(n, d, "fillOval", NULL)) {
        ps_jgfx_fill_oval(g, ARG_I(1), ARG_I(2), ARG_I(3), ARG_I(4));
        return 0;
    }
    if(is(n, d, "drawArc", NULL)) {
        ps_jgfx_draw_arc(g, ARG_I(1), ARG_I(2), ARG_I(3), ARG_I(4),
                         ARG_I(5), ARG_I(6));
        return 0;
    }
    if(is(n, d, "fillArc", NULL)) {
        ps_jgfx_fill_arc(g, ARG_I(1), ARG_I(2), ARG_I(3), ARG_I(4),
                         ARG_I(5), ARG_I(6));
        return 0;
    }
    if(is(n, d, "drawRoundRect", NULL)) {
        ps_jgfx_draw_round_rect(g, ARG_I(1), ARG_I(2), ARG_I(3), ARG_I(4),
                                ARG_I(5), ARG_I(6));
        return 0;
    }
    if(is(n, d, "fillRoundRect", NULL)) {
        ps_jgfx_fill_round_rect(g, ARG_I(1), ARG_I(2), ARG_I(3), ARG_I(4),
                                ARG_I(5), ARG_I(6));
        return 0;
    }
    if(is(n, d, "translate", NULL)) {
        ps_jgfx_translate(g, ARG_I(1), ARG_I(2));
        return 0;
    }
    if(is(n, d, "clipRect", NULL)) {
        ps_jgfx_clip_rect(g, ARG_I(1), ARG_I(2), ARG_I(3), ARG_I(4));
        return 0;
    }
    if(is(n, d, "setClip", NULL)) {
        ps_jgfx_set_clip(g, ARG_I(1), ARG_I(2), ARG_I(3), ARG_I(4));
        return 0;
    }
    if(is(n, d, "copyArea", NULL)) {
        ps_jgfx_copy_area(g, ARG_I(1), ARG_I(2), ARG_I(3), ARG_I(4),
                          ARG_I(5), ARG_I(6));
        return 0;
    }
    if(is(n, d, "drawString", NULL)) {
        size_t      len = 0;
        const char *s   = ps_jvm_string_utf8(ARG_O(1), &len);

        if(s)
            ps_jgfx_draw_string(g, s, len, ARG_I(2), ARG_I(3));
        return 0;
    }
    if(is(n, d, "fillPolygon", "([I[II)V") ||
       is(n, d, "drawPolygon", "([I[II)V")) {
        ps_jobj *xs = ARG_O(1), *ys = ARG_O(2);
        int      cnt = ARG_I(3), i;
        int     *xi, *yi;

        if(!xs || !ys || cnt <= 0 || cnt > xs->len || cnt > ys->len)
            return 0;

        xi = (int *)malloc((size_t)cnt * sizeof(int) * 2);
        if(!xi)
            return 0;
        yi = xi + cnt;

        for(i = 0; i < cnt; i++) {
            xi[i] = ((int32_t *)xs->data)[i];
            yi[i] = ((int32_t *)ys->data)[i];
        }
        if(n[0] == 'f')
            ps_jgfx_fill_polygon(g, xi, yi, cnt);
        else
            ps_jgfx_draw_polygon(g, xi, yi, cnt);
        free(xi);
        return 0;
    }
    /* drawImage(Image, x, y, observer) and the scaled form. The observer is
     * ignored: it exists so an applet can be told when more of the image has
     * arrived, and this browser repaints the applet itself when one does. */
    if(is(n, d, "drawImage", NULL)) {
        int             iw = 0, ih = 0;
        const uint32_t *px;

        /* drawImage returns whether the image is complete, and applets do
         * branch on it - `if(!g.drawImage(...)) return;` is the standard way
         * to skip a frame while a picture is still arriving. Leaving ret
         * untouched left that false forever, so an applet written this way
         * drew nothing at all, permanently. */
        if(!ARG_O(1)) {
            ret->i   = 1;             /* a null image is trivially complete */
            *handled = 1;
            return 0;
        }
        px = ps_applet_image_px(ARG_O(1)->len, &iw, &ih);
        if(!px) {                     /* still in flight; AWT draws nothing */
            ret->i = 0;
            return 0;
        }

        /* Five arguments is (img, x, y, observer); seven is (img, x, y, w, h,
         * observer). The descriptor is the only way to tell them apart. */
        if(nargs >= 7)
            ps_jgfx_draw_image_scaled(g, px, iw, ih, iw, ARG_I(2), ARG_I(3),
                                      ARG_I(4), ARG_I(5));
        else
            ps_jgfx_draw_image(g, px, iw, ih, iw, ARG_I(2), ARG_I(3));
        ret->i = 1;
        return 0;
    }
    if(is(n, d, "setFont", NULL)) {
        if(ARG_O(1))
            ps_jgfx_set_font_size(g, ARG_O(1)->len);
        return 0;
    }
    if(is(n, d, "getColor", NULL)) {
        ps_jobj *c = ps_jvm_new(vm, ps_jvm_class(vm, "java/awt/Color"));

        if(c)
            SET_COLOR(c, g->color);
        ret->o = c;
        return 0;
    }
    if(is(n, d, "dispose", NULL) || is(n, d, "<init>", NULL))
        return 0;

    *handled = 0;
    return 0;
}

/* --- java.awt.Color ------------------------------------------------------ */

/* The sixteen constants java.awt.Color declares. Applets reach for these far
 * more often than they construct a Color, and they are the exact values the
 * JDK defines - a "red" that is not 255,0,0 would be visible. */
static const struct { const char *name; uint32_t argb; } g_colors[] = {
    { "white",     0xffffffffu }, { "lightGray", 0xffc0c0c0u },
    { "gray",      0xff808080u }, { "darkGray",  0xff404040u },
    { "black",     0xff000000u }, { "red",       0xffff0000u },
    { "pink",      0xffffafafu }, { "orange",    0xffffc800u },
    { "yellow",    0xffffff00u }, { "green",     0xff00ff00u },
    { "magenta",   0xffff00ffu }, { "cyan",      0xff00ffffu },
    { "blue",      0xff0000ffu },
    /* The uppercase aliases Java 1.4 added, since a modern javac resolves
     * Color.RED to the same field. */
    { "WHITE",     0xffffffffu }, { "BLACK",     0xff000000u },
    { "RED",       0xffff0000u }, { "GREEN",     0xff00ff00u },
    { "BLUE",      0xff0000ffu }, { "YELLOW",    0xffffff00u },
    { "CYAN",      0xff00ffffu }, { "MAGENTA",   0xffff00ffu },
    { "ORANGE",    0xffffc800u }, { "PINK",      0xffffafafu },
    { "GRAY",      0xff808080u }, { "LIGHT_GRAY",0xffc0c0c0u },
    { "DARK_GRAY", 0xff404040u },
    { NULL, 0 }
};

static uint32_t clamp8(int32_t v)
{
    return (uint32_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

static int color(ps_jvm *vm, const char *n, const char *d, ps_jslot *args,
                 int nargs, ps_jslot *ret, int *handled)
{
    int i;

    (void)vm;
    *handled = 1;

    if(is(n, d, "<init>", "(III)V")) {
        if(nargs >= 4 && ARG_O(0))
            SET_COLOR(ARG_O(0), 0xff000000u | (clamp8(ARG_I(1)) << 16) |
                                (clamp8(ARG_I(2)) << 8) | clamp8(ARG_I(3)));
        return 0;
    }
    if(is(n, d, "<init>", "(I)V")) {
        if(nargs >= 2 && ARG_O(0))
            SET_COLOR(ARG_O(0), 0xff000000u | ((uint32_t)ARG_I(1) & 0xffffffu));
        return 0;
    }
    if(is(n, d, "<init>", "(FFF)V")) {
        if(nargs >= 4 && ARG_O(0))
            SET_COLOR(ARG_O(0),
                      0xff000000u | (clamp8((int32_t)(args[1].f * 255.0f)) << 16) |
                      (clamp8((int32_t)(args[2].f * 255.0f)) << 8) |
                      clamp8((int32_t)(args[3].f * 255.0f)));
        return 0;
    }
    if(is(n, d, "getRGB", NULL)) {
        ret->i = nargs >= 1 && ARG_O(0) ? (int32_t)COLOR_OF(ARG_O(0)) : 0;
        return 0;
    }
    if(is(n, d, "getRed", NULL) || is(n, d, "getGreen", NULL) ||
       is(n, d, "getBlue", NULL)) {
        uint32_t c = nargs >= 1 && ARG_O(0) ? COLOR_OF(ARG_O(0)) : 0;

        ret->i = (int32_t)((n[3] == 'R') ? ((c >> 16) & 0xff)
                         : (n[3] == 'G') ? ((c >> 8) & 0xff) : (c & 0xff));
        return 0;
    }

    /* A static colour constant, read through getstatic. */
    for(i = 0; g_colors[i].name; i++) {
        if(!strcmp(n, g_colors[i].name)) {
            ps_jobj *c = ps_jvm_new(vm, ps_jvm_class(vm, "java/awt/Color"));

            if(!c)
                return -1;
            SET_COLOR(c, g_colors[i].argb);
            ret->o = c;
            return 0;
        }
    }

    *handled = 0;
    return 0;
}

/* --- java.lang.Math ------------------------------------------------------ */

static int jmath(const char *n, const char *d, ps_jslot *args, int nargs,
                 ps_jslot *ret, int *handled)
{
    (void)nargs;
    *handled = 1;

    if(is(n, d, "abs", "(I)I"))  { ret->i = args[0].i < 0 ? -args[0].i : args[0].i; return 0; }
    if(is(n, d, "abs", "(F)F"))  { ret->f = fabsf(args[0].f); return 0; }
    if(is(n, d, "abs", "(D)D"))  { ret->d = fabs(args[0].d); return 0; }
    if(is(n, d, "abs", "(J)J"))  { ret->j = args[0].j < 0 ? -args[0].j : args[0].j; return 0; }
    if(is(n, d, "min", "(II)I")) { ret->i = args[0].i < args[1].i ? args[0].i : args[1].i; return 0; }
    if(is(n, d, "min", "(JJ)J")) { ret->j = args[0].j < args[2].j ? args[0].j : args[2].j; return 0; }
    if(is(n, d, "max", "(JJ)J")) { ret->j = args[0].j > args[2].j ? args[0].j : args[2].j; return 0; }
    if(is(n, d, "min", "(FF)F")) { ret->f = args[0].f < args[1].f ? args[0].f : args[1].f; return 0; }
    if(is(n, d, "max", "(FF)F")) { ret->f = args[0].f > args[1].f ? args[0].f : args[1].f; return 0; }
    if(is(n, d, "max", "(II)I")) { ret->i = args[0].i > args[1].i ? args[0].i : args[1].i; return 0; }

    /* Doubles arrive two slots apart, so the second argument of a (DD)D call
     * is at index 2, not 1. Getting this wrong reads the padding slot and
     * silently computes with zero. */
    if(is(n, d, "min", "(DD)D")) { ret->d = args[0].d < args[2].d ? args[0].d : args[2].d; return 0; }
    if(is(n, d, "max", "(DD)D")) { ret->d = args[0].d > args[2].d ? args[0].d : args[2].d; return 0; }
    if(is(n, d, "pow", "(DD)D")) { ret->d = pow(args[0].d, args[2].d); return 0; }

    if(is(n, d, "sin",  "(D)D")) { ret->d = sin(args[0].d);  return 0; }
    if(is(n, d, "asin", "(D)D")) { ret->d = asin(args[0].d); return 0; }
    if(is(n, d, "acos", "(D)D")) { ret->d = acos(args[0].d); return 0; }
    if(is(n, d, "atan", "(D)D")) { ret->d = atan(args[0].d); return 0; }
    if(is(n, d, "log",  "(D)D")) { ret->d = log(args[0].d);  return 0; }
    if(is(n, d, "exp",  "(D)D")) { ret->d = exp(args[0].d);  return 0; }
    if(is(n, d, "rint", "(D)D")) { ret->d = rint(args[0].d); return 0; }
    if(is(n, d, "IEEEremainder", "(DD)D")) {
        ret->d = remainder(args[0].d, args[2].d);
        return 0;
    }
    /* Degrees and radians are 1.2, but applets of this period write them
     * anyway and they cost two lines. */
    if(is(n, d, "toRadians", "(D)D")) {
        ret->d = args[0].d * (3.14159265358979323846 / 180.0);
        return 0;
    }
    if(is(n, d, "toDegrees", "(D)D")) {
        ret->d = args[0].d * (180.0 / 3.14159265358979323846);
        return 0;
    }
    if(is(n, d, "cos",  "(D)D")) { ret->d = cos(args[0].d);  return 0; }
    if(is(n, d, "tan",  "(D)D")) { ret->d = tan(args[0].d);  return 0; }
    if(is(n, d, "sqrt", "(D)D")) { ret->d = sqrt(args[0].d); return 0; }
    if(is(n, d, "atan2","(DD)D")){ ret->d = atan2(args[0].d, args[2].d); return 0; }
    if(is(n, d, "floor","(D)D")) { ret->d = floor(args[0].d); return 0; }
    if(is(n, d, "ceil", "(D)D")) { ret->d = ceil(args[0].d);  return 0; }
    if(is(n, d, "round","(D)J")) { ret->j = (int64_t)floor(args[0].d + 0.5); return 0; }
    if(is(n, d, "round","(F)I")) { ret->i = (int32_t)floorf(args[0].f + 0.5f); return 0; }

    if(is(n, d, "random", "()D")) {
        /* Deterministic on purpose. A golden-image test that renders an
         * applet twice has to get the same picture, and no applet's drawing
         * is improved by being unreproducible. Seeded per VM would be the
         * fix if one ever needs real variety. */
        static uint32_t s = 0x2545F491u;

        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        ret->d = (double)(s >> 8) / (double)(1u << 24);
        return 0;
    }
    if(!strcmp(n, "PI")) { ret->d = 3.14159265358979323846; return 0; }
    if(!strcmp(n, "E"))  { ret->d = 2.71828182845904523536; return 0; }

    *handled = 0;
    return 0;
}

/* --- java.lang.String ---------------------------------------------------- */

static int jstring(ps_jvm *vm, const char *n, const char *d, ps_jslot *args,
                   int nargs, ps_jslot *ret, int *handled)
{
    size_t      len = 0;
    const char *s   = nargs > 0 ? ps_jvm_string_utf8(ARG_O(0), &len) : NULL;

    (void)vm;
    *handled = 1;

    if(is(n, d, "length", NULL))  { ret->i = (int32_t)len; return 0; }
    if(is(n, d, "charAt", NULL)) {
        int32_t i = nargs > 1 ? ARG_I(1) : 0;

        ret->i = (s && i >= 0 && (size_t)i < len) ? (unsigned char)s[i] : 0;
        return 0;
    }
    if(is(n, d, "isEmpty", NULL)) { ret->i = len == 0; return 0; }
    if(!strcmp(n, "valueOf")) {
        /* Static, so there is no receiver: the value is argument zero. */
        char buf[64];
        int  bl = 0;

        if(strstr(d, "(I)"))       bl = snprintf(buf, sizeof buf, "%d", (int)args[0].i);
        else if(strstr(d, "(J)"))  bl = snprintf(buf, sizeof buf, "%ld", (long)args[0].j);
        else if(strstr(d, "(Z)"))  bl = snprintf(buf, sizeof buf, "%s", args[0].i ? "true" : "false");
        else if(strstr(d, "(C)"))  { buf[0] = (char)args[0].i; buf[1] = 0; bl = 1; }
        else if(strstr(d, "(D)"))  bl = snprintf(buf, sizeof buf, "%g", args[0].d);
        else if(strstr(d, "(F)"))  bl = snprintf(buf, sizeof buf, "%g", (double)args[0].f);
        else { ret->o = nargs ? args[0].o : NULL; return 0; }

        ret->o = ps_jvm_new_string(vm, buf, (size_t)bl);
        return 0;
    }
    if(is(n, d, "equals", NULL)) {
        size_t      ol = 0;
        const char *o  = nargs > 1 ? ps_jvm_string_utf8(ARG_O(1), &ol) : NULL;

        ret->i = (s && o && ol == len && !memcmp(s, o, len)) ? 1 : 0;
        return 0;
    }

    *handled = 0;
    return 0;
}

/* --- the event objects ---------------------------------------------------
 *
 * MouseEvent, KeyEvent, ActionEvent and the 1.0 Event have no class file, so
 * they carry their state in plain instance slots with the layout ps_jvm.h
 * names. The browser fills those in before it delivers; everything here reads
 * them back.
 */

static int32_t evt_int(ps_jslot *args, int nargs, int slot)
{
    ps_jobj *o = nargs >= 1 ? args[0].o : NULL;

    if(!o || !o->fields || !o->cls || slot >= (int)o->cls->inst_slots)
        return 0;
    return o->fields[slot].i;
}

static ps_jobj *evt_ref(ps_jslot *args, int nargs, int slot)
{
    ps_jobj *o = nargs >= 1 ? args[0].o : NULL;

    if(!o || !o->fields || !o->cls || slot >= (int)o->cls->inst_slots)
        return NULL;
    return o->fields[slot].o;
}

/* The VK_ constants worth carrying.
 *
 * The letters and digits are their own ASCII codes, which is why an applet can
 * write `e.getKeyCode() == 'A'` and be right; only the keys with no character
 * need naming. This is the set an applet steering something around a screen
 * reaches for, plus the ones a game loop tests. */
static const struct { const char *name; int32_t value; } g_vkeys[] = {
    { "VK_LEFT",      37 }, { "VK_UP",        38 },
    { "VK_RIGHT",     39 }, { "VK_DOWN",      40 },
    { "VK_ENTER",     10 }, { "VK_TAB",        9 },
    { "VK_BACK_SPACE", 8 }, { "VK_ESCAPE",    27 },
    { "VK_SPACE",     32 }, { "VK_DELETE",   127 },
    { "VK_SHIFT",     16 }, { "VK_CONTROL",   17 },
    { "VK_ALT",       18 }, { "VK_PAUSE",     19 },
    { "VK_PAGE_UP",   33 }, { "VK_PAGE_DOWN", 34 },
    { "VK_END",       35 }, { "VK_HOME",      36 },
    { "VK_INSERT",   155 },
    { "VK_F1",       112 }, { "VK_F2",       113 },
    { "VK_F3",       114 }, { "VK_F4",       115 },
    { "VK_F5",       116 }, { "VK_F6",       117 },
    { "VK_F7",       118 }, { "VK_F8",       119 },
    { "VK_F9",       120 }, { "VK_F10",      121 },
    { "VK_F11",      122 }, { "VK_F12",      123 },
    { "VK_UNDEFINED",  0 },
    { NULL, 0 }
};

/* The static constants the event classes declare, read through getstatic. */
static const struct { const char *name; int32_t value; } g_evconsts[] = {
    { "SHIFT_MASK",   PS_MOD_SHIFT   }, { "CTRL_MASK",    PS_MOD_CTRL },
    { "META_MASK",    PS_MOD_META    }, { "ALT_MASK",     PS_MOD_ALT  },
    { "BUTTON1_MASK", PS_MOD_BUTTON1 }, { "BUTTON2_MASK", PS_MOD_BUTTON2 },
    { "BUTTON3_MASK", PS_MOD_BUTTON3 },

    { "MOUSE_CLICKED",  PS_EV_MOUSE_CLICKED  },
    { "MOUSE_PRESSED",  PS_EV_MOUSE_PRESSED  },
    { "MOUSE_RELEASED", PS_EV_MOUSE_RELEASED },
    { "MOUSE_MOVED",    PS_EV_MOUSE_MOVED    },
    { "MOUSE_ENTERED",  PS_EV_MOUSE_ENTERED  },
    { "MOUSE_EXITED",   PS_EV_MOUSE_EXITED   },
    { "MOUSE_DRAGGED",  PS_EV_MOUSE_DRAGGED  },
    { "KEY_TYPED",      PS_EV_KEY_TYPED      },
    { "KEY_PRESSED",    PS_EV_KEY_PRESSED    },
    { "KEY_RELEASED",   PS_EV_KEY_RELEASED   },
    { "ACTION_PERFORMED", PS_EV_ACTION       },
    { "CHAR_UNDEFINED", PS_CHAR_UNDEFINED    },
    { NULL, 0 }
};

/* One of java.awt.event, by its unqualified name.
 *
 * Leaves *handled at zero for a member it does not know, so an applet reaching
 * for something absent gets the interpreter's message naming the exact method
 * rather than a silent zero - which is the rule the whole of this file follows
 * and the reason an unimplemented call is a line in the log rather than a
 * subtly wrong picture. */
static void event_class(ps_jvm *vm, const char *n, const char *name,
                        const char *desc, ps_jslot *args, int nargs,
                        ps_jslot *ret, int *handled)
{
    int i;

    /* The adapters. Every method is empty by definition, so anything called on
     * one that reaches here is a subclass calling super, or an adapter that
     * overrode nothing - and doing nothing is the correct answer to both. A
     * subclass that *did* override is never routed here: the interpreter
     * resolves a virtual call against the object it has, so the override wins
     * before the name is ever looked up in C. */
    if(!strcmp(n, "MouseAdapter") || !strcmp(n, "MouseMotionAdapter") ||
       !strcmp(n, "KeyAdapter")   || !strcmp(n, "WindowAdapter")) {
        *handled = 1;
        return;
    }

    /* The listener interfaces themselves. Nothing is ever dispatched to one -
     * an interface has no implementation - but an applet's `implements` clause
     * names them and the loader has to be able to resolve the name. */
    if(!strcmp(n, "MouseListener") || !strcmp(n, "MouseMotionListener") ||
       !strcmp(n, "KeyListener")   || !strcmp(n, "ActionListener") ||
       !strcmp(n, "WindowListener")) {
        *handled = is(name, desc, "<init>", NULL);
        return;
    }

    if(is(name, desc, "<init>", NULL)) {
        *handled = 1;
        return;
    }

    /* Accessors, shared across the event classes because the fields are. An
     * applet reaching getX on a KeyEvent gets the zero it deserves rather than
     * a refusal, which is the same shape AWT's own inheritance produces. */
    if(is(name, desc, "getX", NULL))          { ret->i = evt_int(args, nargs, PS_EVF_X); *handled = 1; return; }
    if(is(name, desc, "getY", NULL))          { ret->i = evt_int(args, nargs, PS_EVF_Y); *handled = 1; return; }
    if(!strcmp(name, "x"))                    { ret->i = evt_int(args, nargs, PS_EVF_X); *handled = 1; return; }
    if(!strcmp(name, "y"))                    { ret->i = evt_int(args, nargs, PS_EVF_Y); *handled = 1; return; }
    if(is(name, desc, "getID", NULL))         { ret->i = evt_int(args, nargs, PS_EVF_ID); *handled = 1; return; }
    if(is(name, desc, "getModifiers", NULL) ||
       is(name, desc, "getModifiersEx", NULL)){ ret->i = evt_int(args, nargs, PS_EVF_MODS); *handled = 1; return; }
    if(is(name, desc, "getClickCount", NULL)) { ret->i = evt_int(args, nargs, PS_EVF_CLICKS); *handled = 1; return; }
    if(is(name, desc, "getKeyCode", NULL))    { ret->i = evt_int(args, nargs, PS_EVF_KEY); *handled = 1; return; }
    if(is(name, desc, "getKeyChar", NULL))    { ret->i = evt_int(args, nargs, PS_EVF_CHAR); *handled = 1; return; }
    if(is(name, desc, "getWhen", NULL))       { ret->j = 0; *handled = 1; return; }

    if(is(name, desc, "getSource", NULL) ||
       is(name, desc, "getComponent", NULL)) {
        ret->o = evt_ref(args, nargs, PS_EVF_SOURCE);
        *handled = 1;
        return;
    }
    if(is(name, desc, "getActionCommand", NULL)) {
        ret->o = evt_ref(args, nargs, PS_EVF_CMD);
        *handled = 1;
        return;
    }

    /* getPoint() is a fresh Point every call, as it is in AWT: the event's own
     * coordinates are ints and handing out a Point that aliased them would let
     * an applet write through it into the event. */
    if(is(name, desc, "getPoint", NULL)) {
        ps_jobj *p = ps_jvm_new(vm, ps_jvm_class(vm, "java/awt/Point"));

        /* A Point's own slots, not the event's.
         *
         * PS_EVF_X and PS_EVF_Y are 2 and 3 in the nine-slot layout the event
         * classes share; java/awt/Point is a real two-field class and its x
         * and y are slots 0 and 1. Writing the event indices into it ran
         * sixteen bytes off the end of the allocation - a nondeterministic
         * malloc abort without a sanitiser - and left the Point reading zero,
         * so no 1.1 applet using getPoint ever got coordinates either. Two
         * bugs, one wrong pair of indices.
         *
         * Bounds-checked as well as corrected, because the layout of a class
         * owned by another file is not something this one should assume. */
        if(p && p->fields && p->cls && p->cls->inst_slots >= 2) {
            p->fields[0].i = evt_int(args, nargs, PS_EVF_X);
            p->fields[1].i = evt_int(args, nargs, PS_EVF_Y);
        }
        ret->o = p;
        *handled = 1;
        return;
    }

    if(is(name, desc, "isShiftDown", NULL)) {
        ret->i = (evt_int(args, nargs, PS_EVF_MODS) & PS_MOD_SHIFT) != 0;
        *handled = 1;
        return;
    }
    if(is(name, desc, "isControlDown", NULL)) {
        ret->i = (evt_int(args, nargs, PS_EVF_MODS) & PS_MOD_CTRL) != 0;
        *handled = 1;
        return;
    }
    if(is(name, desc, "isMetaDown", NULL)) {
        ret->i = (evt_int(args, nargs, PS_EVF_MODS) & PS_MOD_META) != 0;
        *handled = 1;
        return;
    }
    if(is(name, desc, "isAltDown", NULL)) {
        ret->i = (evt_int(args, nargs, PS_EVF_MODS) & PS_MOD_ALT) != 0;
        *handled = 1;
        return;
    }
    if(is(name, desc, "consume", NULL) ||
        is(name, desc, "isConsumed", NULL)) {
        /* Consuming stops the event reaching the peer, and there is no peer
         * under this - the browser has already decided the applet has it. */
        ret->i = 0;
        *handled = 1;
        return;
    }

    for(i = 0; g_vkeys[i].name; i++) {
        if(!strcmp(name, g_vkeys[i].name)) {
            ret->i = g_vkeys[i].value;
            *handled = 1;
            return;
        }
    }
    for(i = 0; g_evconsts[i].name; i++) {
        if(!strcmp(name, g_evconsts[i].name)) {
            ret->i = g_evconsts[i].value;
            *handled = 1;
            return;
        }
    }

    /* A VK_ for a letter or a digit, which is the character itself. Spelling
     * out all thirty-six would be a table that says nothing. */
    if(!strncmp(name, "VK_", 3) && name[3] && !name[4]) {
        ret->i = (unsigned char)name[3];
        *handled = 1;
        return;
    }
}

/* --- the rest ------------------------------------------------------------ */

int ps_jre_call(ps_jvm *vm, const char *cls, const char *name,
                const char *desc, ps_jslot *args, int nargs, ps_jslot *ret,
                int *handled)
{
    *handled = 0;

    /* Each of these is offered the call and free to decline, leaving *handled
     * clear so the rest of this file gets its usual turn. Declining has to be
     * per method rather than per class: a name none of them knows must reach
     * the interpreter's "unimplemented" report naming the exact method, which
     * is the failure that says what to write next.
     *
     * Offscreen images go first because they shadow. An Image out of
     * createImage answers getGraphics and getWidth from its own surface; one
     * off the network has to fall through to the branch below.
     *
     * The spellings are not interchangeable. ps_jlib_call reports claiming
     * through its return value and success through *handled; the other three
     * report claiming through *handled and failure through the return value.
     * Collapsing them into one test gets one of the four wrong. */
    if(ps_joff_call(vm, cls, name, desc, args, nargs, ret, handled) != 0)
        return -1;
    if(*handled)
        return 0;

    if(ps_jlib_call(vm, cls, name, desc, args, nargs, ret, handled))
        return *handled ? 0 : -1;

    if(ps_jgeom_call(vm, cls, name, desc, args, nargs, ret, handled) != 0)
        return -1;
    if(*handled)
        return 0;

    if(ps_jawt_call(vm, cls, name, desc, args, nargs, ret, handled) != 0)
        return -1;
    if(*handled)
        return 0;

    if(ps_jimg_call(vm, cls, name, desc, args, nargs, ret, handled) != 0)
        return -1;
    if(*handled)
        return 0;

    if(ps_jpoly_call(vm, cls, name, desc, args, nargs, ret, handled) != 0)
        return -1;
    if(*handled)
        return 0;

    if(!strcmp(cls, "java/awt/Graphics"))
        return graphics(vm, name, desc, args, nargs, ret, handled);

    if(!strcmp(cls, "java/awt/Color"))
        return color(vm, name, desc, args, nargs, ret, handled);

    if(!strcmp(cls, "java/lang/Math"))
        return jmath(name, desc, args, nargs, ret, handled);

    if(!strcmp(cls, "java/lang/String"))
        return jstring(vm, name, desc, args, nargs, ret, handled);

    /* java.awt.Event, the 1.0 event object. Applets of this period read the
     * coordinates from the method arguments, but they read the modifiers and
     * the click count off the Event - `if (evt.clickCount == 2)` and
     * `evt.shiftDown()` are how a 1.0 applet distinguishes a double click and
     * a modified one, and both used to answer zero here. The browser fills the
     * slots in before the handler runs. */
    if(!strcmp(cls, "java/awt/Event")) {
        int slot = -1;

        if(is(name, desc, "<init>", NULL)) {
            *handled = 1;
            return 0;
        }

        if(!strcmp(name, "id"))              slot = PS_EVF_ID;
        else if(!strcmp(name, "x"))          slot = PS_EVF_X;
        else if(!strcmp(name, "y"))          slot = PS_EVF_Y;
        else if(!strcmp(name, "key"))        slot = PS_EVF_KEY;
        else if(!strcmp(name, "modifiers"))  slot = PS_EVF_MODS;
        else if(!strcmp(name, "clickCount")) slot = PS_EVF_CLICKS;

        if(slot >= 0) {
            ret->i = evt_int(args, nargs, slot);
            *handled = 1;
            return 0;
        }
        if(!strcmp(name, "target") || !strcmp(name, "arg")) {
            ret->o = evt_ref(args, nargs, !strcmp(name, "target")
                                          ? PS_EVF_SOURCE : PS_EVF_CMD);
            *handled = 1;
            return 0;
        }

        /* The constants an applet compares evt.key and evt.id against. The
         * navigation keys are the reason a 1.0 applet can steer anything: they
         * are not characters, so there is nothing else to test them by. */
        {
            static const struct { const char *name; int32_t value; } ev[] = {
                { "HOME", 1000 }, { "END",  1001 },
                { "PGUP", 1002 }, { "PGDN", 1003 },
                { "UP",   1004 }, { "DOWN", 1005 },
                { "LEFT", 1006 }, { "RIGHT",1007 },
                { "ESCAPE",    27 }, { "ENTER",      10 },
                { "TAB",        9 }, { "BACK_SPACE",  8 },
                { "DELETE",   127 },
                { "SHIFT_MASK", PS_MOD_SHIFT }, { "CTRL_MASK", PS_MOD_CTRL },
                { "META_MASK",  PS_MOD_META  }, { "ALT_MASK",  PS_MOD_ALT  },
                { "MOUSE_DOWN", PS_EV_MOUSE_PRESSED  },
                { "MOUSE_UP",   PS_EV_MOUSE_RELEASED },
                { "MOUSE_MOVE", PS_EV_MOUSE_MOVED    },
                { "MOUSE_DRAG", PS_EV_MOUSE_DRAGGED  },
                { "MOUSE_ENTER",PS_EV_MOUSE_ENTERED  },
                { "MOUSE_EXIT", PS_EV_MOUSE_EXITED   },
                { "KEY_PRESS",  PS_EV_KEY_PRESSED    },
                { "KEY_RELEASE",PS_EV_KEY_RELEASED   },
                { "ACTION_EVENT", PS_EV_ACTION       },
                { NULL, 0 }
            };
            int i;

            for(i = 0; ev[i].name; i++) {
                if(strcmp(name, ev[i].name))
                    continue;
                ret->i = ev[i].value;
                *handled = 1;
                return 0;
            }
            /* Event.F1 through Event.F12, which are consecutive. */
            if(name[0] == 'F' && name[1] >= '1' && name[1] <= '9') {
                int fn = atoi(name + 1);

                if(fn >= 1 && fn <= 12 && !name[fn < 10 ? 2 : 3]) {
                    ret->i = 1008 + fn - 1;
                    *handled = 1;
                    return 0;
                }
            }
        }

        if(is(name, desc, "shiftDown", NULL)) {
            ret->i = (evt_int(args, nargs, PS_EVF_MODS) & PS_MOD_SHIFT) != 0;
            *handled = 1;
        }
        else if(is(name, desc, "controlDown", NULL)) {
            ret->i = (evt_int(args, nargs, PS_EVF_MODS) & PS_MOD_CTRL) != 0;
            *handled = 1;
        }
        else if(is(name, desc, "metaDown", NULL)) {
            ret->i = (evt_int(args, nargs, PS_EVF_MODS) & PS_MOD_META) != 0;
            *handled = 1;
        }
        return 0;
    }

    /* --- java.awt.event --------------------------------------------------
     *
     * The 1.1 event objects, and the constants applets compare against. The
     * values are the ones a real JDK reports, checked rather than reasoned
     * about: BUTTON2_MASK and ALT_MASK are the same bit and BUTTON3_MASK and
     * META_MASK are the same bit, which looks like a mistake in a table and is
     * not one.
     *
     * getX and e.x reach the same code, because the interpreter hands a field
     * read on a runtime class the same receiver a call would get. */
    if(!strncmp(cls, "java/awt/event/", 15)) {
        event_class(vm, cls + 15, name, desc, args, nargs, ret, handled);
        return 0;
    }
    if(!strcmp(cls, "java/util/EventObject")) {
        event_class(vm, "EventObject", name, desc, args, nargs, ret, handled);
        return 0;
    }

    /* java.awt.Point. Small enough that applets treat it as two ints, which is
     * what getPoint() has to hand back for `e.getPoint().x` to work. */
    if(!strcmp(cls, "java/awt/Point")) {
        if(is(name, desc, "<init>", NULL)) {
            if(nargs >= 3 && args[0].o && args[0].o->fields) {
                args[0].o->fields[PS_EVF_X].i = ARG_I(1);
                args[0].o->fields[PS_EVF_Y].i = ARG_I(2);
            }
            *handled = 1;
        }
        else if(!strcmp(name, "x") || is(name, desc, "getX", NULL)) {
            ret->i = evt_int(args, nargs, PS_EVF_X);
            *handled = 1;
        }
        else if(!strcmp(name, "y") || is(name, desc, "getY", NULL)) {
            ret->i = evt_int(args, nargs, PS_EVF_Y);
            *handled = 1;
        }
        return 0;
    }

    if(!strcmp(cls, "java/awt/Image")) {
        int iw = 0, ih = 0;

        if(is(name, desc, "getWidth", NULL)) {
            ps_applet_image_px(nargs >= 1 && args[0].o ? args[0].o->len : 0,
                               &iw, &ih);
            ret->i = iw;
            *handled = 1;
        }
        else if(is(name, desc, "getHeight", NULL)) {
            ps_applet_image_px(nargs >= 1 && args[0].o ? args[0].o->len : 0,
                               &iw, &ih);
            ret->i = ih;
            *handled = 1;
        }
        else if(is(name, desc, "flush", NULL) ||
                is(name, desc, "getGraphics", NULL)) {
            ret->o = NULL;
            *handled = 1;
        }
        return 0;
    }

    /* --- java.applet.AudioClip -------------------------------------------
     *
     * An interface in the real API, and there is nothing here to distinguish
     * that from a class: the interpreter dispatches a native call on the name
     * in the constant pool, so an invokeinterface on AudioClip lands here
     * whatever the receiver turns out to be. The handle is in the object's
     * length field, the way an Image carries its own.
     *
     * The three methods are the whole interface, and none of them can fail:
     * a clip that cannot be played is documented to do nothing, not to throw.
     * See ps_applet.c for what "do nothing" currently covers. */
    if(!strcmp(cls, "java/applet/AudioClip")) {
        int h = (nargs >= 1 && args[0].o) ? args[0].o->len : 0;

        if(is(name, desc, "play", NULL)) {
            ps_applet_clip_play(h, 0);
            *handled = 1;
        }
        else if(is(name, desc, "loop", NULL)) {
            ps_applet_clip_play(h, 1);
            *handled = 1;
        }
        else if(is(name, desc, "stop", NULL)) {
            ps_applet_clip_stop(h);
            *handled = 1;
        }
        return 0;
    }

    if(!strcmp(cls, "java/awt/Font")) {
        /* Only the size is honoured. One face is baked into the browser and
         * synthesising a second would change metrics without matching any
         * real typeface - the same call the HTML side already makes. */
        if(is(name, desc, "<init>", NULL)) {
            if(nargs >= 4 && args[0].o)
                args[0].o->len = args[3].i;
            *handled = 1;
        }
        else if(is(name, desc, "getSize", NULL)) {
            ret->i = nargs >= 1 && args[0].o ? args[0].o->len : 12;
            *handled = 1;
        }
        return 0;
    }

    /* --- java.lang.StringBuilder -----------------------------------------
     *
     * Not optional, however modern the name looks: `"Score: " + n` compiles to
     * a StringBuilder chain, and an applet that draws a number to the screen
     * is doing string concatenation. Java 1.1 spelled it StringBuffer and
     * javac 8 emits StringBuilder; both land here.
     *
     * Backed by a plain growable C string in the object's native pointer,
     * owned so the heap sweep reclaims it. */
    if(!strcmp(cls, "java/lang/StringBuilder") ||
       !strcmp(cls, "java/lang/StringBuffer")) {
        ps_jobj *self = nargs >= 1 ? args[0].o : NULL;

        if(!self)
            return 0;

        if(is(name, desc, "<init>", NULL)) {
            size_t      il = 0;
            const char *init = (nargs >= 2 && args[1].o)
                             ? ps_jvm_string_utf8(args[1].o, &il) : NULL;

            self->native = malloc(il + 32);
            if(self->native) {
                self->owns_native = 1;
                if(init)
                    memcpy(self->native, init, il);
                ((char *)self->native)[il] = '\0';
                self->len = (int32_t)il;
            }
            *handled = 1;
            return 0;
        }

        if(!strcmp(name, "append")) {
            char        tmp[64];
            const char *add = NULL;
            size_t      al  = 0;
            char       *grown;

            /* The overload is read off the descriptor, which is the only
             * thing that distinguishes append(int) from append(char). */
            if(strstr(desc, "(Ljava/lang/String;)")) {
                add = (nargs >= 2) ? ps_jvm_string_utf8(args[1].o, &al) : NULL;
            }
            else if(strstr(desc, "(I)")) {
                al = (size_t)snprintf(tmp, sizeof tmp, "%d", (int)args[1].i);
                add = tmp;
            }
            else if(strstr(desc, "(J)")) {
                al = (size_t)snprintf(tmp, sizeof tmp, "%ld",
                                      (long)args[1].j);
                add = tmp;
            }
            else if(strstr(desc, "(C)")) {
                tmp[0] = (char)args[1].i;
                tmp[1] = '\0';
                al = 1;
                add = tmp;
            }
            else if(strstr(desc, "(Z)")) {
                add = args[1].i ? "true" : "false";
                al  = strlen(add);
            }
            else if(strstr(desc, "(D)") || strstr(desc, "(F)")) {
                double v = strstr(desc, "(D)") ? args[1].d
                                               : (double)args[1].f;

                al = (size_t)snprintf(tmp, sizeof tmp, "%g", v);
                add = tmp;
            }
            else if(nargs >= 2 && args[1].o) {
                /* append(Object) - the only Object an applet appends is a
                 * String, and toString on anything else has nowhere useful to
                 * go here. */
                add = ps_jvm_string_utf8(args[1].o, &al);
            }

            if(add && self->native) {
                grown = (char *)realloc(self->native,
                                        (size_t)self->len + al + 1);
                if(grown) {
                    self->native = grown;
                    memcpy(grown + self->len, add, al);
                    self->len = (int32_t)((size_t)self->len + al);
                    grown[self->len] = '\0';
                }
            }

            ret->o = self;            /* append returns this, for chaining */
            *handled = 1;
            return 0;
        }

        if(is(name, desc, "toString", NULL)) {
            ret->o = ps_jvm_new_string(vm, self->native ? (char *)self->native
                                                        : "",
                                       (size_t)(self->native ? self->len : 0));
            *handled = 1;
            return 0;
        }
        if(is(name, desc, "length", NULL)) {
            ret->i = self->native ? self->len : 0;
            *handled = 1;
            return 0;
        }
        return 0;
    }

    /* --- the throwable hierarchy -----------------------------------------
     *
     * Applets construct these to throw and almost never inspect what they
     * catch, so what has to work is that they can be made, carry a message,
     * and print themselves. Matched by suffix rather than by an exhaustive
     * list: every name in the runtime's throwable set ends in Exception,
     * Error or Throwable, and an applet that defines its own subclass of one
     * gets a real bytecode class instead and never reaches here. */
    {
        size_t cl = strlen(cls);
        int    is_throwable =
            (cl > 9  && !strcmp(cls + cl - 9,  "Exception")) ||
            (cl > 5  && !strcmp(cls + cl - 5,  "Error")) ||
            (cl > 9  && !strcmp(cls + cl - 9,  "Throwable"));

        if(is_throwable) {
            if(is(name, desc, "<init>", "(Ljava/lang/String;)V")) {
                /* The message rides on the object, borrowed from the String
                 * the applet built. */
                if(nargs >= 2 && args[0].o)
                    args[0].o->native = args[1].o;
                *handled = 1;
            }
            else if(is(name, desc, "<init>", NULL)) {
                *handled = 1;
            }
            else if(is(name, desc, "getMessage", NULL) ||
                    is(name, desc, "getLocalizedMessage", NULL) ||
                    is(name, desc, "toString", NULL)) {
                ret->o = (nargs >= 1 && args[0].o)
                       ? (ps_jobj *)args[0].o->native : NULL;
                *handled = 1;
            }
            else if(is(name, desc, "printStackTrace", NULL) ||
                    is(name, desc, "fillInStackTrace", NULL)) {
                /* There is no stack to print by the time this is called - the
                 * frames the exception passed through are gone. Naming the
                 * class is the honest remainder. */
                printf("applet: %s thrown\n", cls);
                ret->o = nargs >= 1 ? args[0].o : NULL;
                *handled = 1;
            }
            if(*handled)
                return 0;
        }
    }

    /* --- java.lang.Thread ------------------------------------------------
     *
     * One Runnable, cooperatively scheduled. See the note above ps_jvm_pump
     * for why that is enough: every animated applet of the period loops,
     * mutates a field, calls repaint() and sleeps, and none of them depend on
     * running at the same time as anything else.
     */
    if(!strcmp(cls, "java/lang/Thread")) {
        if(is(name, desc, "<init>", "(Ljava/lang/Runnable;)V")) {
            /* new Thread(this). The Runnable is borrowed, not owned - it is
             * the applet, which the VM already holds. */
            if(nargs >= 2 && args[0].o)
                args[0].o->native = args[1].o;
            *handled = 1;
        }
        else if(is(name, desc, "<init>", NULL)) {
            /* new Thread() with no argument, which means a subclass overriding
             * run(). The object is its own Runnable. */
            *handled = 1;
        }
        else if(is(name, desc, "start", NULL) || is(name, desc, "run", NULL)) {
            if(nargs >= 1 && args[0].o) {
                ps_jobj *r = (ps_jobj *)args[0].o->native;

                /* Either the Runnable handed to the constructor, or the Thread
                 * subclass itself. */
                vm->thread_run = r ? r : args[0].o;
                vm->thread_started = 0;
                vm->thread_done = 0;
            }
            *handled = 1;
        }
        else if(is(name, desc, "sleep", NULL)) {
            /* Cannot block: there is a browser waiting for this frame. The
             * duration is recorded and the interpreter hands its frame stack
             * back, to be resumed when the wall clock has caught up. */
            long ms = nargs >= 1 ? (long)args[0].j : 0;

            if(ms < 0)   ms = 0;
            if(ms > 5000) ms = 5000;   /* a page must stay responsive */

            vm->wake_ms  = ms;
            vm->sleeping = 1;
            *handled = 1;
        }
        else if(is(name, desc, "setPriority", NULL) ||
                is(name, desc, "setDaemon", NULL) ||
                is(name, desc, "interrupt", NULL) ||
                is(name, desc, "yield", NULL) ||
                is(name, desc, "join", NULL)) {
            *handled = 1;
        }
        else if(is(name, desc, "isAlive", NULL)) {
            ret->i = vm->thread_run && !vm->thread_done;
            *handled = 1;
        }
        else if(is(name, desc, "currentThread", NULL)) {
            ret->o = NULL;
            *handled = 1;
        }
        return 0;
    }

    if(!strcmp(cls, "java/lang/System")) {
        if(!strcmp(name, "out") || !strcmp(name, "err")) {
            /* One shared stream object; nothing about it is per-instance. */
            ret->o = ps_jvm_new(vm, ps_jvm_class(vm, "java/io/PrintStream"));
            *handled = 1;
        }
        else if(is(name, desc, "currentTimeMillis", NULL)) {
            ret->j = 0;
            *handled = 1;
        }
        /* System.gc is a hint, and honouring it is honest here - this
         * collector is not generational, so a full pass is exactly what the
         * applet asked for. Two games in the corpus call it between levels and
         * died on it, which is a poor way to lose an applet. */
        else if(is(name, desc, "gc", "()V")) {
            ps_jvm_gc(vm);
            *handled = 1;
        }
        else if(is(name, desc, "runFinalization", "()V") ||
                is(name, desc, "setProperty", NULL)) {
            *handled = 1;
        }
        /* getProperty answers null rather than inventing a value. An applet
         * reading "os.name" is deciding what to do about a platform, and every
         * answer we could give is a lie about a machine that is none of them;
         * null is the documented result for a property that is not set, and
         * the branch an applet takes for it is the conservative one. */
        else if(is(name, desc, "getProperty", NULL)) {
            ret->o   = NULL;
            *handled = 1;
        }
        else if(is(name, desc, "exit", "(I)V")) {
            /* An applet has no business ending the process it is a guest in,
             * and on a console there is nothing to exit to. */
            /* int32_t is long on SH-4, so the cast is not decoration. */
            printf("applet called System.exit(%d), ignored\n",
                   (int)(nargs > 0 ? args[0].i : 0));
            *handled = 1;
        }
        return 0;
    }

    if(!strcmp(cls, "java/io/PrintStream")) {
        if(!strncmp(name, "print", 5)) {
            size_t      len = 0;
            const char *s   = nargs > 1 ? ps_jvm_string_utf8(args[1].o, &len)
                                        : NULL;

            /* An applet's console output goes to the browser's log, which on
             * this machine is the dcload console. It is the only debugging
             * channel an applet author has. */
            if(s)
                printf("applet: %.*s\n", (int)len, s);
            else if(nargs > 1 && desc && strstr(desc, "(I)"))
                /* int32_t is long on SH-4 and int on the host, so the cast is
                 * what keeps one format string correct on both. */
                printf("applet: %d\n", (int)args[1].i);
            *handled = 1;
        }
        return 0;
    }

    /* java.lang.Object, and the AWT component hierarchy an Applet extends.
     * Their constructors have nothing to do and the methods an applet calls
     * on them are all about layout, which does not apply to a box the browser
     * has already sized. */
    if(!strcmp(cls, "java/lang/Object")   || !strcmp(cls, "java/applet/Applet") ||
       !strcmp(cls, "java/awt/Component") || !strcmp(cls, "java/awt/Panel") ||
       !strcmp(cls, "java/awt/Container")) {
        if(is(name, desc, "<init>", NULL)) {
            *handled = 1;
            return 0;
        }

        /* The three java.lang.Object methods every class inherits.
         *
         * toString is the one that matters: a class that does not override it
         * still gets concatenated, and reaching an unimplemented Object method
         * stopped the applet outright. The format is the one the JDK
         * documents, with the class name dotted rather than slashed. */
        if(is(name, desc, "toString", "()Ljava/lang/String;")) {
            ps_jobj *self = nargs > 0 ? args[0].o : NULL;
            char     buf[128];
            size_t   i;

            snprintf(buf, sizeof buf, "%s@%lx",
                     (self && self->cls) ? self->cls->name : "java.lang.Object",
                     (unsigned long)(size_t)self);
            for(i = 0; buf[i]; i++)
                if(buf[i] == '/')
                    buf[i] = '.';

            ret->o   = ps_jvm_new_string(vm, buf, strlen(buf));
            *handled = 1;
            return 0;
        }
        if(is(name, desc, "hashCode", "()I")) {
            ps_jobj *self = nargs > 0 ? args[0].o : NULL;

            /* Identity, which is what Object.hashCode is. The value differs
             * from a real JVM's and is allowed to: nothing may depend on a
             * particular number, only on the same object giving the same one. */
            ret->i   = (int32_t)(size_t)self;
            *handled = 1;
            return 0;
        }
        if(is(name, desc, "equals", "(Ljava/lang/Object;)Z")) {
            ret->i   = (nargs > 1 && args[0].o == args[1].o) ? 1 : 0;
            *handled = 1;
            return 0;
        }

        /* repaint() is the applet asking to be painted again. The browser
         * decides when, which is exactly the contract AWT specifies - and it
         * is why an applet that animates without calling repaint() draws
         * nothing, here as everywhere else. */
        if(is(name, desc, "repaint", NULL)) {
            vm->repaint = 1;
            *handled = 1;
            return 0;
        }
        if(is(name, desc, "invalidate", NULL) ||
            is(name, desc, "validate", NULL) || is(name, desc, "resize", NULL) ||
            is(name, desc, "setBackground", NULL) ||
            is(name, desc, "setForeground", NULL) ||
            is(name, desc, "init", NULL) || is(name, desc, "start", NULL) ||
            is(name, desc, "stop", NULL) || is(name, desc, "destroy", NULL)) {
            *handled = 1;
            return 0;
        }

        /* showStatus writes to the browser's status line, which this browser
         * does not have. Accepting and dropping it is the whole fix, and it
         * unblocked five applets across three authors - the cheapest entry on
         * the measured list by some distance, because an applet narrating
         * itself does so from init() and died on the first line.
         *
         * It is printed rather than silently dropped: an applet's status text
         * is usually the most direct account of what it thinks it is doing,
         * which is worth having on the console while the corpus is still this
         * far from running. */
        if(is(name, desc, "showStatus", "(Ljava/lang/String;)V")) {
            size_t      len = 0;
            const char *msg = nargs > 1 ? ps_jvm_string_utf8(args[1].o, &len)
                                        : NULL;

            if(msg)
                printf("applet status: %.*s\n", (int)len, msg);
            *handled = 1;
            return 0;
        }

        /* The two an applet overrides for a browser that offers "About". No
         * browser here asks, but a subclass calling super still has to land
         * somewhere. */
        if(is(name, desc, "getAppletInfo", "()Ljava/lang/String;") ||
           is(name, desc, "getParameterInfo", "()[[Ljava/lang/String;")) {
            ret->o   = NULL;
            *handled = 1;
            return 0;
        }
        /* getImage(URL) and getImage(URL, String). A URL here is whatever
         * getDocumentBase or getCodeBase returned, which this runtime makes a
         * String - so both forms reduce to resolving a name against a base.
         *
         * Resolved rather than concatenated, because getDocumentBase now
         * answers with the page's URL and that ends in index.html, not in a
         * slash. Joining those by hand fetches ".../index.htmlsplash.gif". */
        if(is(name, desc, "getImage", NULL)) {
            char        url[512];
            const char *a = nargs > 1 ? ps_jvm_string_utf8(args[1].o, NULL)
                                      : NULL;
            const char *b = nargs > 2 ? ps_jvm_string_utf8(args[2].o, NULL)
                                      : NULL;
            ps_jobj    *img;

            /* One argument is getImage(URL): the whole location is the base
             * and there is no name to resolve against it. */
            ps_applet_url_join(url, sizeof url, b ? a : NULL, b ? b : a);

            img = ps_jvm_new(vm, ps_jvm_class(vm, "java/awt/Image"));
            if(img)
                img->len = ps_applet_want_image(url);

            ret->o = img;
            *handled = 1;
            return 0;
        }

        /* getAudioClip(URL) and getAudioClip(URL, String), resolved exactly
         * as getImage is. Applet.play(URL[, String]) is the same call with the
         * clip thrown away, which is what the real API documents it as. */
        if(is(name, desc, "getAudioClip", NULL) ||
           is(name, desc, "play", NULL)) {
            char        url[512];
            const char *a = nargs > 1 ? ps_jvm_string_utf8(args[1].o, NULL)
                                      : NULL;
            const char *b = nargs > 2 ? ps_jvm_string_utf8(args[2].o, NULL)
                                      : NULL;
            ps_jobj    *clip;
            int         h;

            ps_applet_url_join(url, sizeof url, b ? a : NULL, b ? b : a);

            h = ps_applet_want_clip(url);

            if(name[0] == 'p') {
                /* play() returns void and, uniquely in this pair, is
                 * documented to do nothing at all if the clip cannot be
                 * found - so there is nothing to report either way. */
                ps_applet_clip_play(h, 0);
                *handled = 1;
                return 0;
            }

            /* Null only when the applet has run out of clip slots. A real
             * getAudioClip hands back an object for a URL that 404s, and the
             * applet finds out by hearing nothing - so failing the fetch is
             * not a reason to return null and send it down a branch it was
             * never written for. */
            clip = h ? ps_jvm_new(vm, ps_jvm_class(vm, "java/applet/AudioClip"))
                     : NULL;
            if(clip)
                clip->len = h;

            ret->o = clip;
            *handled = 1;
            return 0;
        }

        /* getParameter(String). Null when the page did not name it, which is
         * the whole point: applets branch on null to pick a default, and an
         * empty string is a configured empty value rather than an absent one.
         *
         * The name match folds case - see ps_applet_param. */
        if(is(name, desc, "getParameter", NULL)) {
            const char *want = nargs > 1 ? ps_jvm_string_utf8(args[1].o, NULL)
                                         : NULL;
            const char *val  = want ? ps_applet_param(want) : NULL;

            ret->o = val ? ps_jvm_new_string(vm, val, strlen(val)) : NULL;
            *handled = 1;
            return 0;
        }

        /* The code base is the directory the class was fetched from; the
         * document base is the page's own URL, filename and all. That
         * difference is what the real API specifies, and it is only safe now
         * that getImage resolves rather than concatenates. */
        if(is(name, desc, "getCodeBase", NULL)) {
            const char *b = ps_applet_code_base();

            ret->o = ps_jvm_new_string(vm, b, strlen(b));
            *handled = 1;
            return 0;
        }
        if(is(name, desc, "getDocumentBase", NULL)) {
            const char *b = ps_applet_doc_base();

            ret->o = ps_jvm_new_string(vm, b, strlen(b));
            *handled = 1;
            return 0;
        }

        /* The surface is NULL in vector mode - there is no pixel buffer, the
         * drawing goes straight out as geometry - so the box comes from the
         * clip there. Reading s->w unconditionally was a segfault waiting on
         * the PS_APPLET_VECTOR build knob. */
        if(is(name, desc, "getWidth", NULL)) {
            ret->i = !vm->gfx      ? 0
                   : vm->gfx->s    ? vm->gfx->s->w
                   : vm->gfx->cx1 - vm->gfx->cx0;
            *handled = 1;
            return 0;
        }
        if(is(name, desc, "getHeight", NULL)) {
            ret->i = !vm->gfx      ? 0
                   : vm->gfx->s    ? vm->gfx->s->h
                   : vm->gfx->cy1 - vm->gfx->cy0;
            *handled = 1;
            return 0;
        }

        /* --- listener registration ---------------------------------------
         *
         * The 1.1 half of the event model. An applet does this in init(),
         * either with `this` - having declared `implements MouseListener` -
         * or with an anonymous subclass of one of the adapters, which is the
         * shape the authoring tools of the period emitted.
         *
         * Registering more than one is legal and applets do it, so this
         * appends rather than replaces. Registering none leaves the 1.0
         * overrides in charge; registering any retires them for good, which is
         * what AWT does and is the only rule under which an applet cannot be
         * handed the same click twice. */
        {
            static const struct { const char *suffix; int kind; } kinds[] = {
                { "MouseListener",       PS_LSN_MOUSE  },
                { "MouseMotionListener", PS_LSN_MOTION },
                { "KeyListener",         PS_LSN_KEY    },
                { "ActionListener",      PS_LSN_ACTION },
                { NULL, 0 }
            };
            int adding = !strncmp(name, "add", 3);
            int i;

            if(adding || !strncmp(name, "remove", 6)) {
                const char *tail = name + (adding ? 3 : 6);

                for(i = 0; kinds[i].suffix; i++) {
                    if(strcmp(tail, kinds[i].suffix))
                        continue;

                    if(adding)
                        ps_jvm_add_listener(vm, nargs > 0 ? args[0].o : NULL,
                                            nargs > 1 ? args[1].o : NULL,
                                            kinds[i].kind);
                    else
                        ps_jvm_remove_listener(vm, nargs > 0 ? args[0].o : NULL,
                                               nargs > 1 ? args[1].o : NULL,
                                               kinds[i].kind);
                    *handled = 1;
                    return 0;
                }
            }
        }

        /* Accepted and ignored. enableEvents is an applet asking for events
         * without a listener, to catch them by overriding processMouseEvent -
         * the third way of doing this, rarer than either of the other two, and
         * not wired up. It is accepted rather than refused because the call
         * itself has no effect worth having: an applet that goes on to
         * override processMouseEvent gets no events, which is what it gets
         * today either way, and one that calls enableEvents and then registers
         * a listener anyway works. */
        if(is(name, desc, "enableEvents", NULL) ||
           is(name, desc, "disableEvents", NULL) ||
           is(name, desc, "requestFocus", NULL) ||
           is(name, desc, "setFocusable", NULL) ||
           is(name, desc, "addNotify", NULL) ||
           is(name, desc, "removeNotify", NULL)) {
            *handled = 1;
            return 0;
        }
    }

    return 0;
}
