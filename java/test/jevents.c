/* Drives an applet through the whole input repertoire and prints what it did.
 *
 * jrun already covers press and release, which is all the Java 1.0 model ever
 * needed. The 1.1 model has more shapes than that - a drag between a press and
 * a release cancels the click, a key arrives as three events rather than one -
 * and none of them are reachable through a runner that only knows how to
 * click. So this drives ps_applet's input entry points directly, in a fixed
 * script, and the trace it prints is the thing to compare against a real JDK.
 *
 * Same cache the browser uses, same fetch stand-in as jrun: what is exercised
 * is the real routing code, not a copy of it.
 */
#include "ps_applet.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include "hostfont.inc"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include "stb_image.h"

static ps_texture fake_upload(void *s, const void *p, int w, int h,
                              ps_pixel_format f)
{ (void)s;(void)p;(void)w;(void)h;(void)f; return 1; }
static int fake_update(void *s, ps_texture t, const void *p, int w, int h)
{ (void)s;(void)t;(void)p;(void)w;(void)h; return 1; }
static void fake_free(void *s, ps_texture t) { (void)s;(void)t; }

static ps_gfx_backend g_fake = {
    NULL, NULL, NULL, NULL, fake_upload, fake_update, NULL, fake_free, NULL,
    NULL, 0, 0
};

#define MAXQ 64
static char g_queue[MAXQ][512];
static int  g_nq;

static void request(void *user, const char *url)
{
    (void)user;
    if(g_nq < MAXQ)
        snprintf(g_queue[g_nq++], sizeof g_queue[0], "%s", url);
}

static int pump_fetches(ps_applet_cache *c)
{
    int did = 0;

    while(g_nq > 0) {
        char  url[512];
        FILE *f;
        long  sz;
        void *buf;

        snprintf(url, sizeof url, "%s", g_queue[0]);
        memmove(g_queue[0], g_queue[1], sizeof g_queue[0] * (size_t)(g_nq - 1));
        g_nq--;

        f = fopen(url, "rb");
        if(!f) {
            ps_applet_deliver(c, url, 0, NULL, 0);
            did++;
            continue;
        }
        fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
        buf = malloc((size_t)sz + 1);
        if(!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
            fclose(f); free(buf); continue;
        }
        fclose(f);
        ps_applet_deliver(c, url, 1, buf, (size_t)sz);
        free(buf);
        did++;
    }
    return did;
}

int main(int argc, char **argv)
{
    const char      *url = argc > 1 ? argv[1] : "applets/Click11.class";
    host_font        font;
    ps_jtext_ops     ops;
    ps_applet_cache *c;
    int              took, i;

    if(host_font_load(&font, "../../cd/font.ttf") != 0)
        fprintf(stderr, "warn: no font\n");
    host_font_ops(&font, &ops);

    c = ps_applet_cache_create(&g_fake, request, NULL);
    ps_applet_cache_set_text(c, &ops);

    ps_applet_get(c, url, "", 300, 200);
    while(pump_fetches(c))
        ;

    printf("status: %s\n", ps_applet_status(c, url));
    if(!ps_applet_get(c, url, "", 300, 200)) {
        printf("applet did not run\n");
        return 1;
    }

    /* Put it on screen and leave it there. The cache only gives time to
     * applets it believes are visible, and an applet that never animates is an
     * applet whose collector never runs - which is exactly the condition a
     * listener held only by the browser has to survive. */
    ps_applet_set_box(c, url, 0, 200);
    ps_applet_set_view(c, 0, 480);
    for(i = 0; i < 400; i++)
        ps_applet_cache_tick(c, 20);
    ps_applet_heap_report(c, url);

    /* Coordinates below are component space: the applet's own box with its
     * origin at 0,0, which is what the browser hands over after subtracting
     * the element's document-space position. */

    printf("\n-- a plain click: press then release at the same point --\n");
    took = ps_applet_mouse(c, url, 40, 50, 1, 0);
    printf("   press   -> handled=%d\n", took);
    took = ps_applet_mouse(c, url, 40, 50, 0, 0);
    printf("   release -> handled=%d\n", took);

    printf("\n-- a drag: press, move with the button down, release --\n");
    took = ps_applet_mouse(c, url, 100, 60, 1, 0);
    printf("   press   -> handled=%d\n", took);
    took = ps_applet_mouse(c, url, 130, 80, 1, 1);
    printf("   drag    -> handled=%d\n", took);
    took = ps_applet_mouse(c, url, 160, 100, 0, 0);
    printf("   release -> handled=%d  (no click should follow this one)\n",
           took);

    printf("\n-- a bare move, button up --\n");
    took = ps_applet_mouse(c, url, 210, 30, -1, 0);
    printf("   move    -> handled=%d\n", took);

    printf("\n-- a printable key, down then up --\n");
    took = ps_applet_key(c, url, 'a', 1);
    printf("   down    -> handled=%d\n", took);
    took = ps_applet_key(c, url, 'a', 0);
    printf("   up      -> handled=%d\n", took);

    printf("\n-- a key with no character: Event.LEFT --\n");
    took = ps_applet_key(c, url, 1006, 1);
    printf("   down    -> handled=%d  (no keyTyped should follow)\n", took);

    /* Crossings. The browser has no boundary notion of its own - it says where
     * the pointer is every frame and the cache works out that it moved from
     * one applet to another, or to none. The repeat below is the case that
     * matters: a page scrolling under a still pointer must not refire. */
    printf("\n-- crossings --\n");
    ps_applet_set_hover(c, url, 10, 12);
    printf("   entered\n");
    ps_applet_set_hover(c, url, 11, 13);
    printf("   still inside -> nothing above this line\n");
    ps_applet_set_hover(c, NULL, 11, 13);
    printf("   left\n");
    ps_applet_set_hover(c, NULL, 40, 40);
    printf("   still outside -> nothing above this line\n");
    ps_applet_set_hover(c, url, 20, 20);
    printf("   entered again\n");

    /* Focus. A press takes the keyboard, a release does not, and a press that
     * lands on the page gives it back - which the browser does by clearing it
     * when no applet claimed the click. */
    printf("\n-- focus --\n");
    /* Held already: the presses further up this run took it. */
    printf("   carried in:       %s\n",
           ps_applet_focus(c) ? "held" : "(none)");
    ps_applet_mouse(c, url, 20, 20, 1, 0);
    printf("   after press:      %s\n",
           ps_applet_focus(c) ? "held" : "(none)");
    ps_applet_mouse(c, url, 20, 20, 0, 0);
    printf("   after release:    %s\n",
           ps_applet_focus(c) ? "held" : "(none)");
    ps_applet_set_focus(c, NULL);
    printf("   after page click: %s\n",
           ps_applet_focus(c) ? "held" : "(none)");

    printf("\n");
    ps_applet_cache_tick(c, 16);
    ps_applet_heap_report(c, url);

    ps_applet_cache_destroy(c);
    host_font_free(&font);
    return 0;
}
