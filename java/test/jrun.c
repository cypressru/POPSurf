/* Host mirror of the browser's applet path: resolves the class graph, unpacks
 * a jar if given one, runs the applet and writes frames. Uses the same
 * ps_applet cache the browser uses, with a filesystem fetch standing in for
 * the network - so what is exercised here is the real resolution code, not a
 * simplified copy of it. */
#include "ps_applet.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include "hostfont.inc"

/* stb_image's inflate, which ps_jar needs. Pulled in here because the host
 * build has no ps_image.o. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include "stb_image.h"

static char g_dir[512];
static const ps_gfx_backend *g_gfx;

/* A no-op backend: the host has no PVR, and the applet path only needs
 * textures to exist as handles. The pixels are read straight off the surface. */
static ps_texture fake_upload(void *s, const void *p, int w, int h,
                              ps_pixel_format f)
{ (void)s;(void)p;(void)w;(void)h;(void)f; return 1; }
static int fake_update(void *s, ps_texture t, const void *p, int w, int h)
{ (void)s;(void)t;(void)p;(void)w;(void)h; return 1; }
static void fake_free(void *s, ps_texture t) { (void)s;(void)t; }

/* No update_texture_argb: the host has no store queues, so the applet cache
 * takes its staging path - which is the one that has to keep working anyway. */
static ps_gfx_backend g_fake = {
    NULL, NULL, NULL, NULL, fake_upload, fake_update, NULL, fake_free, NULL,
    NULL, 0, 0
};

/* "file" fetches: the cache asks by URL, we answer from the directory. */
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
            printf("  miss  %s\n", url);
            ps_applet_deliver(c, url, 0, NULL, 0);
            did++;
            continue;
        }
        fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
        buf = malloc((size_t)sz + 1);
        if(fread(buf, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(buf); continue; }
        fclose(f);
        printf("  fetch %s (%ld bytes)\n", url, sz);
        ps_applet_deliver(c, url, 1, buf, (size_t)sz);
        free(buf);
        did++;
    }
    return did;
}

/* The page's side of an applet: <param name= value=> children and the URL of
 * the document carrying them, given on the command line as
 *
 *     -p name=value        repeatable
 *     -d http://host/page.html
 *
 * after the positional arguments. Without these there is no way to exercise
 * getParameter from a host test at all, and getParameter is the one thing an
 * applet from the wild is most likely to be configured entirely through. */
static char       g_pname[PS_APPLET_PARAMS][PS_APPLET_PARAM_NAME];
static char       g_pval[PS_APPLET_PARAMS][PS_APPLET_PARAM_VALUE];
static const char *g_pn[PS_APPLET_PARAMS], *g_pv[PS_APPLET_PARAMS];
static int         g_np;
static const char *g_doc_base = "";

static void add_param(const char *arg)
{
    const char *eq = strchr(arg, '=');

    if(!eq || g_np >= PS_APPLET_PARAMS)
        return;

    snprintf(g_pname[g_np], sizeof g_pname[0], "%.*s", (int)(eq - arg), arg);
    snprintf(g_pval[g_np], sizeof g_pval[0], "%s", eq + 1);
    g_pn[g_np] = g_pname[g_np];
    g_pv[g_np] = g_pval[g_np];
    g_np++;
}

int main(int argc, char **argv)
{
    const char  *main_url = argc > 1 ? argv[1] : "applets/Bounce.class";
    const char  *jar      = argc > 2 ? argv[2] : "";
    const char  *out      = argc > 3 ? argv[3] : "bounce.ppm";
    int          frames   = argc > 4 ? atoi(argv[4]) : 4;
    host_font    font;
    ps_jtext_ops ops;
    ps_applet_cache *c;
    int          a;

    (void)g_dir; (void)g_gfx;

    for(a = 5; a < argc; a++) {
        if(!strcmp(argv[a], "-p") && a + 1 < argc)
            add_param(argv[++a]);
        else if(!strcmp(argv[a], "-d") && a + 1 < argc)
            g_doc_base = argv[++a];
    }

    if(host_font_load(&font, "../../cd/font.ttf") != 0)
        fprintf(stderr, "warn: no font\n");
    host_font_ops(&font, &ops);

    c = ps_applet_cache_create(&g_fake, request, NULL);
    ps_applet_cache_set_text(c, &ops);

    printf("resolving %s%s%s\n", main_url, *jar ? " from jar " : "", jar);
    ps_applet_get(c, main_url, jar, 300, 200);

    /* Straight after the request, and before anything is delivered - the same
     * order the document's draw uses, and the order that matters: the applet
     * reads its parameters in init(), which runs the moment its class file
     * lands. */
    ps_applet_set_params(c, main_url, g_doc_base, g_pn, g_pv, g_np);
    printf("params: %d, docBase %s\n", g_np, *g_doc_base ? g_doc_base : "-");

    while(pump_fetches(c))
        ;

    printf("status: %s\n", ps_applet_status(c, main_url));

    {
        const ps_applet_view *v = ps_applet_get(c, main_url, jar, 300, 200);
        int f;

        if(!v) { printf("applet did not run\n"); return 1; }

        /* Put the applet on screen as the browser would.
         *
         * ps_applet_cache_tick only runs a slot it believes is visible, and
         * visibility comes from these two calls - which jrun never made, so
         * every animation thread in every host test had been silently skipped
         * since the day the visibility check was added. The applets in this
         * corpus spend their whole lives in that thread, so the one code path
         * that matters most was the one path with no coverage at all. It hid a
         * dangling vm->gfx that segfaults on hardware. */
        ps_applet_set_box(c, main_url, 0, 200);
        ps_applet_set_view(c, 0, 480);

        for(f = 0; f <= frames; f++) {
            char path[600];

            if(f) {
                int guard = 0;
                while(guard++ < 40 && !ps_applet_cache_tick(c, 40))
                    ;
            }
            snprintf(path, sizeof path, "%.*s_%02d.ppm",
                     (int)(strrchr(out,'.') ? strrchr(out,'.')-out : (long)strlen(out)),
                     out, f);
            ps_applet_dump_ppm(c, main_url, path);
            printf("frame %d -> %s\n", f, path);
        }
    }

    /* Synthesised clicks, to prove the 1.0 handlers are reached and that a
     * handler calling repaint() gets one. */
    {
        static const int pts[][2] = { {60,60}, {150,90}, {230,140}, {90,160} };
        int i;

        for(i = 0; i < 4; i++) {
            int took = ps_applet_mouse(c, main_url, pts[i][0], pts[i][1], 1, 0);

            printf("click (%3d,%3d) -> handled=%d\n", pts[i][0], pts[i][1],
                   took);
            ps_applet_mouse(c, main_url, pts[i][0], pts[i][1], 0, 0);
        }
        ps_applet_mouse(c, main_url, 200, 40, -1, 0);
        ps_applet_cache_tick(c, 16);
        ps_applet_dump_ppm(c, main_url, "click_end.ppm");
        printf("wrote click_end.ppm\n");
    }

    ps_applet_heap_report(c, main_url);

    ps_applet_cache_destroy(c);
    host_font_free(&font);
    return 0;
}
