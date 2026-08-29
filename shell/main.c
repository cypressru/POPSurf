/* Dreamcast browser shell. */
#include <kos.h>
#include <arch/arch.h>
#include <unistd.h>
#include <malloc.h>
#include <dc/sound/sound.h>
#include <dc/cdrom.h>
#include <dc/maple/keyboard.h>

#include "ps_types.h"
#include "ps_config.h"
#include "ps_theme.h"
#include "ps_paint.h"
#include "ps_text.h"
#include "ps_document.h"
#include "ps_audio.h"
#include "ps_adxstream.h"
#include "ps_voice.h"
#include "ps_cursor.h"
#include "ps_osk.h"
#include "ps_saver.h"
#include "ps_gfx.h"
#include "ps_url.h"
#include "ps_http.h"
#include "ps_loader.h"
#include "ps_menu.h"
#include "ps_probe.h"
#include "ps_bar.h"
#include "ps_marks.h"
#include "ps_applet.h"
#include "ps_swf_play.h"
#include "ps_swf_track.h"
#include "pvr/ps_swf_pvr.h"
/* For the vertex buffer's own account of itself, which the movie's cost line
 * prints beside the movie's share of it. */
#include "pvr/ps_gfx_pvr.h"
#include "ps_jdc.h"
#ifdef PS_JAVA_SELFTEST
#include "ps_jdc.h"
#endif

extern uint8 romdisk[];
KOS_INIT_FLAGS(INIT_DEFAULT | INIT_NET);

#define PS_VIEW_W 640
#define PS_VIEW_H 480

#define PS_SCROLL_STEP 12

/* Page height after subtracting the optional toolbar. */
static int page_h(void);

/* The browser starts on the Dreamcast community portal for now. Development
 * and hardware-test builds can override this with make HOME_URL=... without
 * changing source. */
#ifndef PS_HOME_URL
#define PS_HOME_URL "http://dc99.net/dc/index2.html"
#endif

#define PS_HISTORY_MAX PS_CFG_HISTORY

/* Default MIDI soundbank URL, selected at startup and loaded on demand. */
static char g_default_bank[PS_URL_MAX];

/* Avoid GD-ROM reads when no disc is present; they can retry indefinitely. */
static int disc_present(void)
{
    int status = 0, disc_type = 0;

    if(cdrom_get_status(&status, &disc_type) < 0)
        return 0;
    return status != CD_STATUS_NO_DISC && status != CD_STATUS_OPEN;
}

/* Returns /pc during dc-load development, /cd on disc, or NULL. */
static const char *disc_root(void)
{
    file_t f = fs_open("/pc", O_RDONLY | O_DIR);

    if(f != FILEHND_INVALID) {
        fs_close(f);
        return "/pc";
    }
    return disc_present() ? "/cd" : NULL;
}

#ifdef PS_ASSET_BASE_URL
static ssize_t asset_over_http(const char *name, void **out)
{
    char             url_s[256];
    ps_url           u;
    ps_http_response res;

    snprintf(url_s, sizeof url_s, "%s%s", PS_ASSET_BASE_URL, name);
    if(ps_url_parse(&u, url_s) != 0)
        return -1;
    if(ps_http_get(&u, &res) != PS_HTTP_OK || res.status != 200) {
        free(res.body);
        return -1;
    }
    *out = res.body;
    return (ssize_t)res.body_len;
}
#endif

/* Loads a boot asset from local media, then the configured HTTP fallback. */
static ssize_t load_boot_asset(const char *name, void **out)
{
    const char *root = disc_root();
    char        path[64];
    ssize_t     len;

    *out = NULL;

    if(root) {
        snprintf(path, sizeof path, "%s/%s", root, name);
        len = fs_load(path, out);
        if(len > 0 && *out) {
            printf("popsurf: %s from %s, %d bytes\n", name, root, (int)len);
            return len;
        }
        *out = NULL;
    }

#ifdef PS_ASSET_BASE_URL
    len = asset_over_http(name, out);
    if(len > 0 && *out) {
        printf("popsurf: %s from network, %d bytes\n", name, (int)len);
        return len;
    }
    *out = NULL;
#endif

    printf("popsurf: %s unavailable (disc root %s)\n",
           name, root ? root : "none");
    return -1;
}

/* Selects the local bank when available, otherwise the configured URL. */
static void choose_default_bank(void)
{
    const char *root = disc_root();

    if(root) {
        char    path[64];
        file_t  f;

        snprintf(path, sizeof path, "%s/gmbank.psb", root);
        f = fs_open(path, O_RDONLY);
        if(f != FILEHND_INVALID) {
            fs_close(f);
            snprintf(g_default_bank, sizeof g_default_bank, "file://%s", path);
            printf("popsurf: default soundbank %s\n", g_default_bank);
            return;
        }
    }

#ifdef PS_SOUNDBANK_URL
    snprintf(g_default_bank, sizeof g_default_bank, "%s", PS_SOUNDBANK_URL);
    printf("popsurf: configured soundbank %s (no local copy)\n", g_default_bank);
#else
    g_default_bank[0] = '\0';
    printf("popsurf: no default soundbank available\n");
#endif
}

static ps_paint        g_paint;
static ps_text_cache  *g_text;
static ps_document    *g_doc;
static ps_image_cache *g_images;
static ps_applet_cache *g_applets;
static ps_jtext_ops    g_applet_text;
static ps_loader      *g_loader;
static ps_cursor       g_cursor;
static ps_cursor_set  *g_cursor_art;
static ps_menu         g_menu;
static ps_bar          g_bar;
static ps_osk          g_osk;
static ps_saver        g_saver;
static ps_marks        g_marks;
static ps_audio       *g_audio;
static ps_adxstream   *g_stream;
/* What an unattended run asked to open, read before anything else runs. */
static const char     *g_boot_url;

static ps_swf_play     g_swf;
static ps_swf_pvr     *g_swf_pvr;
/* SWF audio is parsed independently from its display list. */
static ps_swf_track   *g_swf_track;
static char            g_swf_url[PS_URL_MAX];   /* what has been requested */
static int             g_swf_ready;
static int             g_swf_first_frame;       /* print the cost once */
static char            g_bgsound_url[PS_URL_MAX];
static char            g_pending_midi_url[PS_URL_MAX];
static char            g_playing_midi_url[PS_URL_MAX]; /* what is sounding */
static char            g_bank_url[PS_URL_MAX];      /* bank currently loaded */
static char            g_bank_wanted[PS_URL_MAX];   /* bank being fetched */
static int             g_bank_loading;              /* blocking overlay is up */
static int             g_bank_frame_drawn;          /* overlay is on screen */
static int             g_bank_arming;               /* overlay up, not fetching yet */
static int             g_quiesce_drawn;             /* held frame is on screen */

/* A navigation waits for one frame to reach the screen before its request is
 * issued. See begin_load_post. */
#define PS_ARM_IDLE          0
#define PS_ARM_WAITING_FRAME 1
#define PS_ARM_READY         2

static char g_arm_url[PS_URL_MAX];
static char g_arm_body[PS_LOADER_BODY_MAX];
static int  g_arm_is_post;
static int  g_arm_state;

/* Defer texture uploads until network activity is idle. */
#define PS_DEFER_MAX PS_CFG_IMAGE_CACHE

typedef struct {
    char   url[PS_URL_MAX];
    char  *data;
    size_t len;
    int    ok;
} ps_deferred_image;

static ps_deferred_image g_deferred[PS_DEFER_MAX];
static int               g_deferred_count;

static void deferred_flush(void);
static void deferred_drop(void);

static int page_h(void)
{
    return PS_VIEW_H - ps_bar_height(&g_bar);
}

/* Audio does not inhibit the screensaver. */
static int media_is_playing(void)
{
    return 0;
}


/* Non-zero when the keyboard is editing the address bar rather than a form
 * field, so commit navigates instead of writing back into the page. */
static int g_osk_is_address;

/* Session history with a cursor; -1 means no page has loaded. */
static char g_hist[PS_HISTORY_MAX][PS_URL_MAX];
static int  g_hist_len;
static int  g_hist_pos = -1;

/* Set while a back or forward step is in flight, so the page arriving is
 * understood as the one already recorded at g_hist_pos rather than as a new
 * destination to append. Cleared by every other kind of navigation. */
static int g_hist_moving;

static int can_go_back(void)    { return g_hist_pos > 0; }
static int can_go_forward(void) { return g_hist_pos >= 0 &&
                                         g_hist_pos + 1 < g_hist_len; }

static char g_current[PS_URL_MAX];
static char g_pending[PS_URL_MAX];
static char g_pending_post[PS_LOADER_BODY_MAX];
static int  g_pending_is_post;
static int  g_have_pending;

/* Non-zero between requesting a page and its HTML arriving. */
static int g_loading_page;

static int g_doc_height;
static int g_scroll_y;

/* --- retained page ---------------------------------------------------------
 *
 * Drawing a page costs ten milliseconds of a sixteen millisecond frame, and
 * measurement said ninety-six percent of that is deciding what to draw rather
 * than drawing it - 240 quads that take under half a millisecond to hand over.
 * So an unchanged page is replayed from the display list instead of being
 * walked again.
 *
 * The whole risk is here rather than in ps_paint: a missed invalidation shows
 * a stale page, which is far worse than a wasted redraw. So this errs heavily
 * towards redrawing, and anything that might have moved a box sets it.
 *
 * What deliberately does not: an applet or an animated GIF painting into a
 * texture it already owns. The recorded quads name the texture, not its
 * contents, so a replay shows the new pixels for nothing - which is the case
 * that makes this worth having at all. */
static int g_page_dirty = 1;
static int g_last_scroll = -1;

static void page_dirty(void)
{
    g_page_dirty = 1;
}

/* --- keys for applets ----------------------------------------------------
 *
 * Java 1.0's key values: a character is itself, and the keys with no character
 * are numbered from 1000. An applet compares against Event.LEFT, so those are
 * the numbers that have to come out of here.
 */
#define PS_JKEY_HOME  1000
#define PS_JKEY_END   1001
#define PS_JKEY_PGUP  1002
#define PS_JKEY_PGDN  1003
#define PS_JKEY_UP    1004
#define PS_JKEY_DOWN  1005
#define PS_JKEY_LEFT  1006
#define PS_JKEY_RIGHT 1007
#define PS_JKEY_F1    1008

/* A Dreamcast keyboard key as Java would number it, or 0 for one that has no
 * Java meaning. The printable keys go through the region's own table so a
 * European layout produces its own characters rather than a US guess. */
static int jkey_of(kbd_key_t k, kbd_region_t region, kbd_mods_t mods,
                   kbd_leds_t leds)
{
    char c;

    switch(k) {
    case KBD_KEY_UP:     return PS_JKEY_UP;
    case KBD_KEY_DOWN:   return PS_JKEY_DOWN;
    case KBD_KEY_LEFT:   return PS_JKEY_LEFT;
    case KBD_KEY_RIGHT:  return PS_JKEY_RIGHT;
    case KBD_KEY_HOME:   return PS_JKEY_HOME;
    case KBD_KEY_END:    return PS_JKEY_END;
    case KBD_KEY_PGUP:   return PS_JKEY_PGUP;
    case KBD_KEY_PGDOWN: return PS_JKEY_PGDN;
    default: break;
    }

    /* F1..F12 are contiguous in both numberings, which is the only reason
     * this is arithmetic rather than another twelve cases. */
    if(k >= KBD_KEY_F1 && k <= KBD_KEY_F12)
        return PS_JKEY_F1 + (k - KBD_KEY_F1);

    c = kbd_key_to_ascii(k, region, mods, leds);
    return c ? (int)(unsigned char)c : 0;
}

/* Every key the keyboard reported this frame, handed to the focused applet.
 *
 * A press and a release rather than a typed character, because that is what
 * an applet built around keyDown/keyUp needs: a game holds a key down and
 * expects to keep moving until it comes up. Nothing happens when no applet
 * has focus, so the address bar and the on-screen keyboard are unaffected. */
static void kbd_event(maple_device_t *dev, kbd_key_t key, key_state_t state,
                      kbd_mods_t mods, kbd_leds_t leds, void *ud)
{
    kbd_state_t *ks = kbd_get_state(dev);
    int          jk;

    (void)ud;

    /* Edges only. The handler also fires for a key that is simply still held,
     * and an applet wants keyDown once rather than sixty times a second. */
    if(state.is_down == state.was_down)
        return;

    jk = jkey_of(key, ks ? ks->region : KBD_REGION_US, mods, leds);
    if(!jk)
        return;

    if(ps_document_applet_key(g_doc, jk, state.is_down))
        page_dirty();
}

static const char *g_offline_html =
    "<html><body bgcolor='#101018' text='#e0e0e8'>"
    "<h1>Offline</h1><p>Could not load the page.</p></body></html>";

/* Frame sources are fetched exactly like images: resolved against the page
 * base, queued on the loader, and delivered back when they arrive. The frame
 * index rides along in the URL table below, since the loader only knows
 * URLs. */
#define PS_FRAME_MAX_TRACK 16
static char g_frame_url[PS_FRAME_MAX_TRACK][PS_URL_MAX];
static int  g_frame_tracked;

/* Whether a background-music URL names a streamed track rather than a MIDI.
 *
 * By extension, because there is nothing else to go on until the bytes arrive
 * and the decision has to be made before the request goes out: a MIDI needs a
 * megabyte of instruments fetched first and an ADX needs none, so getting this
 * wrong either stalls a page behind a download it never uses or starts a tune
 * with no instruments to play it. The query string is stripped first; period
 * pages hang cache-busters off audio URLs like everything else. */
static int url_is_adx(const char *url)
{
    const char *end = strpbrk(url, "?#");
    size_t      n   = end ? (size_t)(end - url) : strlen(url);

    return n >= 4 && !strncasecmp(url + n - 4, ".adx", 4);
}

/* Requests the page's background music once, after it loads. */
/* <meta http-equiv="refresh"> countdown.
 *
 * Started when a page finishes loading, and cancelled by anything that
 * navigates - clicking a link, going back, opening the directory. Without
 * that, arriving somewhere and then being dragged away by a timer belonging
 * to the page before it is worse than not supporting refresh at all.
 *
 * The hop count exists because two pages can point a refresh at each other,
 * and a browser that follows that forever needs unplugging. It is reset by
 * any navigation the user actually asked for, so the limit only ever bites a
 * chain the page built by itself.
 *
 * A floor on the delay is separate from the cap. Content of "0" is common and
 * legitimate - it means "this page is only here to send you somewhere else" -
 * but honouring it literally would fire the load inside the same frame that
 * finished the last one, before anything is drawn, so the address bar never
 * shows where you were and a chain becomes a freeze rather than a sequence of
 * pages. A short floor keeps it visibly a redirect. */
#define PS_REFRESH_MAX_HOPS 8
#define PS_REFRESH_MIN_MS   250

static void begin_load(const char *url);

static int  g_refresh_ms   = -1;   /* counting down; -1 when idle */
static char g_refresh_url[PS_URL_MAX];
static int  g_refresh_hops;

/* Set for exactly one begin_load, the one a firing refresh performs, so that
 * load can be told apart from a navigation the user asked for. */
static int  g_refresh_following;

static void refresh_cancel(void)
{
    g_refresh_ms      = -1;
    g_refresh_url[0]  = '\0';
}

/* Called after a page settles. */
static void refresh_arm(void)
{
    int ms = ps_document_refresh_ms(g_doc);

    refresh_cancel();

    if(ms < 0)
        return;

    if(g_refresh_hops >= PS_REFRESH_MAX_HOPS) {
        printf("popsurf: refresh chain stopped after %d hops\n",
               g_refresh_hops);
        return;
    }

    {
        const char *url = ps_document_refresh_url(g_doc);

        /* No address means the page asked to reload itself, which only the
         * shell can name - the document does not know where it came from
         * after redirects. */
        snprintf(g_refresh_url, sizeof g_refresh_url, "%s",
                 (url && *url) ? url : g_current);
    }

    if(!g_refresh_url[0])
        return;

    g_refresh_ms = (ms < PS_REFRESH_MIN_MS) ? PS_REFRESH_MIN_MS : ms;
    printf("popsurf: refresh in %d ms -> %s\n", g_refresh_ms, g_refresh_url);
}

static void pump_refresh(int dt_ms)
{
    if(g_refresh_ms < 0)
        return;

    g_refresh_ms -= dt_ms;
    if(g_refresh_ms > 0)
        return;

    {
        char url[PS_URL_MAX];

        snprintf(url, sizeof url, "%s", g_refresh_url);
        refresh_cancel();
        g_refresh_hops++;
        g_refresh_following = 1;
        begin_load(url);
    }
}

/* Drops whatever movie is loaded, textures included. Called when the page
 * changes and when it names a different movie: VRAM belongs to the page on
 * screen, the same rule the image cache and the applet cache follow. */
static void swf_drop(void)
{
    if(g_swf_pvr)
        ps_swf_pvr_unbind(g_swf_pvr);
    /* Sound goes with the picture. A tune outliving the movie that owns it is
     * the audio version of a texture outliving its page. */
    ps_swf_track_stop(g_swf_track);
    ps_swf_play_free(&g_swf);
    g_swf_ready  = 0;
    g_swf_url[0] = '\0';
}

static void pump_swf(void)
{
    const char *url = ps_document_swf(g_doc);

    if(!url || !*url) {
        if(g_swf_url[0])
            swf_drop();
        return;
    }
    if(!strcmp(url, g_swf_url))
        return;

    swf_drop();
    snprintf(g_swf_url, sizeof g_swf_url, "%s", url);
    ps_loader_request(g_loader, PS_JOB_SWF, url);
}

/* Depth steps a movie may take out of the paint order.
 *
 * One per pass, and a pass is one fill style of one layer of one character -
 * the sample's largest shape is twenty-three of them, and a whole frame of a
 * busy movie is a few hundred. Beyond this the passes share the last depth,
 * which loses the ones on top of each other rather than climbing into the
 * range reserved for the toolbar. */
#define PS_SWF_Z_STEPS 1024

/* Submits the page's movie, if it has one.
 *
 * Not part of the retained list, and it cannot be: the list is quads with a
 * texture handle, and this is triangles going to the tile accelerator through
 * a path of their own. So it is redrawn every frame from the display list,
 * between the page and the chrome, which is also why a movie makes a page cost
 * what an unrecorded one costs. */
static void draw_swf(void)
{
    ps_rect      box, vis;
    ps_swf_xform root;
    float        z;
    uint64_t     t0;

    if(!g_swf_ready || !g_swf_pvr || !ps_document_swf_rect(g_doc, &box))
        return;

    box.y0 = (int16_t)(box.y0 - g_scroll_y);
    box.y1 = (int16_t)(box.y1 - g_scroll_y);

    vis = box;
    if(vis.y0 < 0)
        vis.y0 = 0;
    if(vis.y1 > (int16_t)page_h())
        vis.y1 = (int16_t)page_h();
    if(ps_rect_empty(&vis))
        return;

    /* Applied here rather than once at startup, because once at startup was
     * once too early: the bootargs are not read until after this object
     * exists. Per frame it is an integer compare and it cannot be sequenced
     * wrongly - and if the mode ever fails to take, the cost line below says
     * which one actually ran rather than which one was asked for. */
    if(ps_probe_swfmask() >= 0)
        ps_swf_pvr_set_mask_mode(g_swf_pvr, ps_probe_swfmask());

    /* The batch has to be on its way before these triangles are, because both
     * go to the same list and that list is in submission order. */
    ps_paint_flush(&g_paint);
    z = ps_paint_reserve_z(&g_paint, PS_SWF_Z_STEPS);

    ps_swf_play_fit(&g_swf, box.x1 - box.x0, box.y1 - box.y0,
                    (float)box.x0, (float)box.y0, &root);

    /* Where the stage itself landed on screen, for an unattended capture to
     * record. The element's box would be the easy answer and the wrong one:
     * a movie whose aspect does not match the box it was given is centred
     * inside it, and a comparison lined up against the box would be off by
     * that margin - which reads exactly like the renderer drawing in the wrong
     * place. */
    {
        const float *m  = root.m;
        float        sw = (float)(g_swf.movie.xmax - g_swf.movie.xmin);
        float        sh = (float)(g_swf.movie.ymax - g_swf.movie.ymin);

        ps_probe_set_region(
            (int)(m[0] * (float)g_swf.movie.xmin +
                  m[2] * (float)g_swf.movie.ymin + m[4] + 0.5f),
            (int)(m[1] * (float)g_swf.movie.xmin +
                  m[3] * (float)g_swf.movie.ymin + m[5] + 0.5f),
            (int)(sw * m[0] + 0.5f), (int)(sh * m[3] + 0.5f));
    }

    t0 = timer_us_gettime64();
    ps_swf_pvr_begin(g_swf_pvr, &vis, z, PS_Z_STEP, PS_SWF_Z_STEPS);
    (void)ps_swf_render_frame(&g_swf.movie, g_swf.frame, &root,
                              ps_swf_pvr_stage_sink(), g_swf_pvr);
    ps_swf_pvr_end(g_swf_pvr);

    /* The number this whole exercise turns on: how long one frame of one movie
     * costs on the machine it has to run on. Unconditional, and once a second
     * rather than behind a build flag, because it is the measurement the whole
     * feature is currently blocked on and a number nobody can read is not a
     * measurement. One line a second, and only on a page carrying a movie.
     *
     * Split rather than totalled, because a total cannot distinguish the three
     * things it could be - see ps_swf_pvr.h. `walk` is what is left after the
     * tessellator, which is the display list itself: replaying frames, blending
     * morphs, composing matrices. */
    {
        static uint64_t acc_us, acc_draw, acc_state;
        static uint64_t acc_tris, acc_mask, acc_strips, acc_pass, acc_draws;
        static uint64_t acc_n, at;
        uint64_t        us = timer_us_gettime64() - t0;

        acc_us     += us;
        acc_draw   += ps_swf_pvr_us_draw(g_swf_pvr);
        acc_state  += ps_swf_pvr_us_state(g_swf_pvr);
        acc_tris   += (uint64_t)ps_swf_pvr_tris(g_swf_pvr);
        acc_mask   += (uint64_t)ps_swf_pvr_mask_tris(g_swf_pvr);
        acc_strips += (uint64_t)ps_swf_pvr_strips(g_swf_pvr);
        acc_pass   += (uint64_t)ps_swf_pvr_passes(g_swf_pvr);
        acc_draws  += (uint64_t)ps_swf_pvr_draws(g_swf_pvr);
        acc_n++;

        if(g_swf_first_frame || timer_us_gettime64() - at > 1000000ull) {
            uint64_t n = acc_n;

            printf("popsurf: swf %lu us/f x%lu = tess %lu (state %lu) + "
                   "walk %lu | %lu draws %lu passes %lu tris "
                   "(%lu mask) %lu strips\n",
                   (unsigned long)(acc_us / n), (unsigned long)n,
                   (unsigned long)((acc_draw - acc_state) / n),
                   (unsigned long)(acc_state / n),
                   (unsigned long)((acc_us - acc_draw) / n),
                   (unsigned long)(acc_draws / n),
                   (unsigned long)(acc_pass / n),
                   (unsigned long)(acc_tris / n),
                   (unsigned long)(acc_mask / n),
                   (unsigned long)(acc_strips / n));

            /* The masking and the budget, on their own line and only from the
             * frame just drawn, because both are properties of one frame and
             * an average over a second of them would hide the frame that ran
             * out. This is the only channel a headless run has: a capture says
             * what the picture was, and this says which of the paths produced
             * it - which mask went to a volume, which fell back to its box,
             * and whether the vertex buffer stopped the movie early. */
            /* One name per mode, and the compiler is told to check that:
             * the log line is the only thing standing between a run in the
             * wrong mode and a number that looks like a result. */
            static const char *const mask_name[PS_SWF_MASK_MODES] = {
                "box", "vol", "volonly", "modonly"
            };
            int want = ps_probe_swfmask();
            int have = ps_swf_pvr_mask_mode(g_swf_pvr);

            if(want >= PS_SWF_MASK_MODES) want = -1;
            if(have < 0 || have >= PS_SWF_MASK_MODES) have = 0;

            /* A run that silently rendered in a mode nobody asked for is worse
             * than a run that failed: three captures of the default once came
             * back identical and every one of them looked like the experiment
             * succeeding. So the mode that drew is named on every line, and a
             * mode that did not take says so in the words a search will find. */
            if(want >= 0 && want != have)
                printf("popsurf: swf mask MODE NOT APPLIED: asked for %s, "
                       "ran %s\n", mask_name[want], mask_name[have]);

            printf("popsurf: swf mask %s: %ld volume tris, %d by box, "
                   "%d textured | %ld flat dropped\n",
                   mask_name[have],
                   ps_swf_pvr_vol_tris(g_swf_pvr),
                   ps_swf_pvr_mask_inexact(g_swf_pvr),
                   ps_swf_pvr_mask_untextured(g_swf_pvr),
                   ps_swf_pvr_flat_tris(g_swf_pvr));

            /* Three numbers rather than one, because the first version of this
             * printed only the budget and a budget of zero says nothing about
             * which of its two inputs was wrong. `page` is what the tile
             * accelerator says the rest of the frame took, `counted` what the
             * submitters think they wrote, and the two disagreeing is itself
             * the finding. */
            printf("popsurf: swf vertex %lu B of %lu, page %lu (counted %lu) "
                   "of %lu%s\n",
                   (unsigned long)ps_swf_pvr_vtx(g_swf_pvr),
                   (unsigned long)ps_swf_pvr_vtx_budget(g_swf_pvr),
                   (unsigned long)ps_swf_pvr_vtx_page(g_swf_pvr),
                   (unsigned long)ps_pvr_vtx_counted(),
                   (unsigned long)ps_pvr_vtx_capacity(),
                   ps_swf_pvr_vtx_full(g_swf_pvr) ? "  CUT - budget spent"
                                                  : "");

            g_swf_first_frame = 0;
            acc_us = acc_draw = acc_state = 0;
            acc_tris = acc_mask = acc_strips = acc_pass = acc_draws = 0;
            acc_n = 0;
            at = timer_us_gettime64();
        }
    }
}

static void pump_bgsound(void)
{
    const char *url = ps_document_bgsound(g_doc);

    if(!url || !*url || (!g_audio && !g_stream))
        return;
    if(!strcmp(url, g_bgsound_url))
        return;

    snprintf(g_bgsound_url, sizeof g_bgsound_url, "%s", url);

    /* A stream carries its own audio; there is no soundbank to wait for. */
    if(url_is_adx(url)) {
        ps_loader_request(g_loader, PS_JOB_AUDIO, url);
        return;
    }

    /* A page may ship its own instruments; otherwise the session default
     * stands. One bank serves every MIDI ever written, so this is fetched once
     * and only replaced when a page actually asks for something different. */
    {
        const char *want = ps_document_soundbank(g_doc);

        if(!want || !*want)
            want = g_default_bank;

        if(!want || !*want) {
            printf("popsurf: MIDI page has no soundbank; page stays silent\n");
            return;
        }

        if(strcmp(want, g_bank_url)) {
            /* Wrong bank, or none yet. Hold the tune until it arrives. */
            snprintf(g_pending_midi_url, sizeof g_pending_midi_url, "%s", url);

            if(strcmp(want, g_bank_wanted)) {
                /* Armed, not started. The request goes out only once the
                 * overlay is on screen, because the frame that draws it would
                 * otherwise be submitted while the transfer is already
                 * running - and one scene during a bulk transfer is enough to
                 * take the machine down. */
                snprintf(g_bank_wanted, sizeof g_bank_wanted, "%s", want);
                g_bank_loading     = 1;
                g_bank_arming      = 1;
                g_bank_frame_drawn = 0;
            }
            return;
        }
    }

    ps_loader_request(g_loader, PS_JOB_AUDIO, url);
}

static void pump_frames(void)
{
    int n, i;

    if(!ps_document_is_frameset(g_doc))
        return;

    n = ps_document_frame_count(g_doc);
    for(i = 0; i < n && i < PS_FRAME_MAX_TRACK; i++) {
        ps_url base, ref;
        char   abs[PS_URL_MAX];

        if(!ps_document_frame_pending(g_doc, i))
            continue;

        if(ps_url_parse(&base, g_current) != 0)
            continue;
        if(ps_url_resolve(&ref, &base, ps_document_frame_src(g_doc, i)) != 0)
            continue;
        if(ps_url_format(&ref, abs, sizeof abs) < 0)
            continue;

        snprintf(g_frame_url[i], sizeof g_frame_url[i], "%s", abs);
        if(i >= g_frame_tracked)
            g_frame_tracked = i + 1;

        ps_document_frame_mark_requested(g_doc, i, abs);
        ps_loader_request(g_loader, PS_JOB_FRAME, abs);
    }
}

static void image_request(void *user, const char *url)
{
    (void)user;
    ps_loader_request(g_loader, PS_JOB_IMAGE, url);
}

static void applet_request(void *user, const char *url)
{
    (void)user;
    printf("popsurf: applet %s\n", url);
    ps_loader_request(g_loader, PS_JOB_APPLET, url);
}

static void on_navigate(void *user, const char *url, const char *post_body)
{
    (void)user;

    if(!url || strlen(url) >= PS_URL_MAX)
        return;
    if(post_body && strlen(post_body) >= PS_LOADER_BODY_MAX)
        return;

    /* Recorded, not acted on: this fires deep inside litehtml's traversal and
     * loading here would destroy the document it is still walking. */
    strcpy(g_pending, url);
    g_pending_is_post = post_body ? 1 : 0;
    if(post_body)
        strcpy(g_pending_post, post_body);
    g_have_pending = 1;
}

/* Starts a navigation. Returns immediately; the page arrives via the loader. */
static void begin_load_post(const char *url, const char *body)
{
    /* Anything still in flight belongs to the page we are leaving. */
    ps_loader_cancel_all(g_loader);
    deferred_drop();
    g_frame_tracked = 0;

    /* A pending refresh belongs to the page being left, so it goes with it.
     * The hop count survives only while the chain is the page's own doing;
     * any navigation that did not come from a refresh clears it, so the limit
     * can never accumulate across a session and strand someone. */
    refresh_cancel();
    if(!g_refresh_following)
        g_refresh_hops = 0;
    g_refresh_following = 0;

    /* Music belongs to the page that asked for it, and a movie's soundtrack
     * belongs to it twice over. */
    ps_audio_stop(g_audio);
    ps_adx_stream_stop(g_stream);
    ps_swf_track_stop(g_swf_track);
    g_bgsound_url[0]      = '\0';
    g_pending_midi_url[0] = '\0';
    g_playing_midi_url[0] = '\0';

    strcpy(g_current, url);
    g_loading_page = 1;
    g_scroll_y     = 0;

    /* Any history step that was in flight is cancelled along with its
     * request; whatever arrives now belongs to this navigation. nav_history
     * re-arms it immediately after calling us, which is the only correct
     * order - setting it first would let a link click inherit the flag. */
    g_hist_moving = 0;

    /* The address shows where you are going, from the moment you ask, the way
     * a browser's does. Waiting for the response would leave the bar naming
     * the previous page for the whole of a slow load. */
    ps_bar_set_url(&g_bar, url);

    printf("popsurf: %s %s%s%s\n", body ? "POST" : "loading", url,
           body ? " body=" : "", body ? body : "");

    ps_menu_toast(&g_menu, url);
    ps_cursor_set_role(&g_cursor, PS_CUR_WAIT);

    /* Armed, not sent. The request goes out only after a frame has been put
     * on screen, because submitting a scene while a transfer is running is
     * what takes the machine down - and the frame that shows the toast and
     * the wait cursor would otherwise be submitted just after the fetch
     * started. Same ordering the soundbank uses. */
    snprintf(g_arm_url, sizeof g_arm_url, "%s", url);
    if(body)
        snprintf(g_arm_body, sizeof g_arm_body, "%s", body);
    g_arm_is_post = body ? 1 : 0;
    g_arm_state   = PS_ARM_WAITING_FRAME;
}

static void begin_load(const char *url)
{
    begin_load_post(url, NULL);
}

static void show_html(const char *html, size_t len, const char *base)
{
#ifndef PS_NO_TEXFREE
    /* VRAM from the previous page goes back before the new one asks for any. */
    ps_image_cache_clear(g_images);
    ps_applet_cache_clear(g_applets);
#endif

    /* A movie belongs to the page that embedded it, and holds both heap and
     * VRAM. Dropped here rather than when the next page names a different one,
     * because a page with no movie at all would otherwise leave the last one
     * loaded and paying for itself. */
    swf_drop();

    if(base)
        ps_document_set_base(g_doc, base);

    page_dirty();

    if(ps_document_load_memory(g_doc, html, len) != 0)
        ps_document_load_memory(g_doc, g_offline_html, strlen(g_offline_html));

    g_doc_height = ps_document_height(g_doc);
}

static void history_push(const char *url)
{
    /* A back or forward step landed. The entry is already in the list and
     * g_hist_pos already points at it; recording it again would append a
     * duplicate and make the next Back a no-op. */
    if(g_hist_moving) {
        g_hist_moving = 0;
        return;
    }

    if(g_hist_pos >= 0 && !strcmp(g_hist[g_hist_pos], url))
        return;

    /* A new destination ends the forward run. Reload and Home come through
     * here too, and both should: once you have gone somewhere else, the pages
     * you could have gone forward to are no longer reachable from here. */
    g_hist_len = g_hist_pos + 1;

    if(g_hist_len == PS_HISTORY_MAX) {
        memmove(g_hist[0], g_hist[1], sizeof g_hist[0] * (PS_HISTORY_MAX - 1));
        g_hist_len--;
        g_hist_pos--;
    }

    strcpy(g_hist[g_hist_len++], url);
    g_hist_pos = g_hist_len - 1;
}

/* Moves the history cursor to an absolute position. */
static void nav_history_to(int np)
{
    if(np < 0 || np >= g_hist_len)
        return;

    begin_load(g_hist[np]);

    /* After, not before: begin_load clears the flag so that an ordinary
     * navigation can never be mistaken for a history step. */
    g_hist_pos    = np;
    g_hist_moving = 1;
}

/* Steps the history cursor. delta is -1 for back, +1 for forward. */
static void nav_history(int delta)
{
    if(g_hist_pos < 0)
        return;

    nav_history_to(g_hist_pos + delta);
}

/* Labels for the menu's list pages.
 *
 * Pointers into the arrays that already hold the data, rebuilt each frame
 * rather than copied: sixteen pointer stores per frame against sixteen
 * kilobytes of duplicated strings, on a machine with twelve megabytes.
 *
 * History reads newest first, which is the order somebody looking for "the
 * page I was on a minute ago" scans in - and the reverse of the order it is
 * stored in, so the mapping back to a position is not the identity. */
static const char *g_hist_labels[PS_HISTORY_MAX];
static const char *g_mark_labels[PS_MARKS_MAX];

static int hist_label_pos(int label_index)
{
    return g_hist_len - 1 - label_index;
}

static void refresh_menu_lists(void)
{
    int i;

    for(i = 0; i < g_hist_len; i++)
        g_hist_labels[i] = g_hist[hist_label_pos(i)];

    for(i = 0; i < ps_marks_count(&g_marks); i++)
        g_mark_labels[i] = ps_marks_title(&g_marks, i);

    ps_menu_set_lists(&g_menu, g_hist_labels, g_hist_len,
                      g_mark_labels, ps_marks_count(&g_marks),
                      ps_marks_count(&g_marks) >= PS_MARKS_MAX,
                      ps_marks_find(&g_marks, g_current) >= 0);
}

/* Shows or hides the toolbar and hands the page the difference.
 *
 * The document has to be told, not just drawn around: it lays out against the
 * viewport height, and a page left measuring 480 lines into a 414-line band
 * would put its footer under the bar and let you scroll past its own end. */
static void set_bar_visible(int on)
{
    if(ps_bar_is_visible(&g_bar) == !!on)
        return;

    ps_bar_set_visible(&g_bar, on);
    ps_document_set_view_h(g_doc, page_h());
    g_doc_height = ps_document_height(g_doc);
    page_dirty();
}

/* Raises the keyboard on the address rather than on a form field. Three
 * different gestures do this - the Y button, the menu entry, and clicking the
 * address field - and all three have to leave the same state behind or commit
 * writes the typed text into whatever the page had focused. */
static void open_address(void)
{
    if(ps_osk_is_open(&g_osk))
        return;

    g_osk_is_address = 1;
    ps_osk_open(&g_osk, "Address", g_current);
}

/* The directory: a page of places to go that ships with the browser.
 *
 * Distinct from bookmarks on purpose. Bookmarks are the owner's, live on the
 * memory card, and can be deleted; this list is ours, lives on the disc, and
 * is the same for everyone. Mixing them would mean either spending the card's
 * eight slots on addresses nobody chose, or offering rows that cannot be
 * removed - and a bookmark you cannot delete is somebody else's bookmark.
 *
 * It is a page rather than another menu screen because the renderer is
 * already here. A menu would need its own list, scrolling and drawing code to
 * show less than the page does, and could not be edited without a compiler.
 *
 * The location follows the rest of the disc's contents - /pc when a
 * development host is mapped, /cd from a real disc - so it is built rather
 * than fixed. */
static void open_directory(void)
{
    const char *root = disc_root();
    char        url[64];

    if(!root) {
        /* Neither a disc nor a host. Better to say nothing happened than to
         * navigate to a page that cannot load and blame the network. */
        printf("popsurf: no directory page available\n");
        return;
    }

    snprintf(url, sizeof url, "file://%s/sites.html", root);
    begin_load(url);
}

/* Bookmarks the page on screen, and writes the card straight away.
 *
 * Saving on the spot rather than at exit costs a second of maple traffic at a
 * moment the user has just asked for something and is expecting a result. The
 * alternative is losing the lot when the console is switched off at the wall,
 * which is how a Dreamcast is switched off. */
static void bookmark_current(void)
{
    const char *title = ps_document_title(g_doc);

    if(!g_current[0])
        return;

    if(ps_marks_add(&g_marks, g_current, title) != 0) {
        ps_menu_toast(&g_menu, "Could not bookmark this page");
        return;
    }

    if(!ps_marks_have_card(&g_marks))
        ps_menu_toast(&g_menu, "Bookmarked (no card - this session only)");
    else if(ps_marks_save(&g_marks) == 0)
        ps_menu_toast(&g_menu, "Bookmarked");
    else
        ps_menu_toast(&g_menu, "Bookmarked, but the card could not be written");
}

static void unbookmark_current(void)
{
    int i = ps_marks_find(&g_marks, g_current);

    if(i < 0)
        return;

    ps_marks_remove(&g_marks, i);
    ps_marks_save(&g_marks);
    ps_menu_toast(&g_menu, "Bookmark removed");
}

/* Drains everything the loader has finished this frame. */
static void deferred_flush(void)
{
    int i, relayout = 0;

    for(i = 0; i < g_deferred_count; i++) {
        if(ps_image_deliver(g_images, g_deferred[i].url, g_deferred[i].ok,
                            g_deferred[i].data, g_deferred[i].len))
            relayout = 1;
        free(g_deferred[i].data);
        g_deferred[i].data = NULL;
    }
    g_deferred_count = 0;

    if(relayout) {
        ps_document_relayout(g_doc);
        g_doc_height = ps_document_height(g_doc);
    }
}

static void deferred_drop(void)
{
    int i;

    for(i = 0; i < g_deferred_count; i++) {
        free(g_deferred[i].data);
        g_deferred[i].data = NULL;
    }
    g_deferred_count = 0;
}

static void pump_loader(void)
{
    ps_job_result res;
    int           relayout = 0;

    while(ps_loader_poll(g_loader, &res)) {
        if(res.kind == PS_JOB_PAGE) {
            if(res.ok) {
                show_html(res.data, res.len, res.final_url);
                history_push(g_current);
                {
                    /* Heap high-water per page. There is no slab allocator
                     * yet (plan 10), so a long page can quietly walk into
                     * the 16MB ceiling; printing it turns "it crashed" into
                     * a number, and it is the same figure the go/no-go RSS
                     * gate needs. */
                    struct mallinfo mi = mallinfo();

                    /* Headroom, not just usage. "heap 6MB" says nothing on
                     * its own; what matters is how much room is left before
                     * an allocation fails, because that is where a page load
                     * turns into a panic. */
                    uint32_t top  = (uint32_t)_arch_mem_top;
                    uint32_t brk  = (uint32_t)sbrk(0);
                    int      free_k = (int)((top - brk) / 1024);

                    printf("popsurf: '%s' height %d, heap %dK used / %dK arena, "
                           "%dK headroom\n",
                           ps_document_title(g_doc), g_doc_height,
                           mi.uordblks / 1024, mi.arena / 1024, free_k);
                }

                /* Armed only once the page has actually settled, so the delay
                 * a page asks for is time it is on screen rather than time
                 * spent fetching it. */
                refresh_arm();
            }
            else {
                printf("popsurf: page failed (HTTP %d)\n", res.status);
                show_html(g_offline_html, strlen(g_offline_html), NULL);
                ps_menu_toast(&g_menu, "Could not load page");

                /* The step still happened as far as history is concerned - the
                 * cursor moved and the offline page stands in for that entry -
                 * so the arming flag has done its job and must not survive
                 * into whatever loads next. */
                g_hist_moving = 0;
            }
            g_loading_page = 0;
        }
        else if(res.kind == PS_JOB_BANK) {
            g_bank_loading     = 0;
            g_bank_frame_drawn = 0;
            g_bank_arming      = 0;
            if(res.ok && g_audio &&
               ps_audio_set_bank(g_audio, res.data, res.len) == 0) {
                printf("popsurf: soundbank %d KB loaded\n",
                       (int)(res.len / 1024));
                res.data = NULL;   /* the bank owns it now */
                snprintf(g_bank_url, sizeof g_bank_url, "%s", res.url);

                if(g_pending_midi_url[0]) {
                    ps_loader_request(g_loader, PS_JOB_AUDIO,
                                      g_pending_midi_url);
                    /* Consumed. Leaving it set means a second bank result -
                     * a retry, or another page naming the same bank - starts
                     * the tune again a few notes in, which sounds like the
                     * music changing its mind. */
                    g_pending_midi_url[0] = '\0';
                }
            }
            else {
                /* Not retried while this page is up - re-requesting on every
                 * relayout would hammer a dead URL. But the next page gets to
                 * try again: a bank that failed once because the link dropped
                 * should not mean silence for the rest of the session, which
                 * is what keeping g_bank_wanted set used to guarantee. */
                g_bank_wanted[0] = '\0';
                printf("popsurf: soundbank unavailable; page stays silent\n");
            }
        }
        else if(res.kind == PS_JOB_AUDIO) {
            /* Only the tune this page asked for, and only if it is not the
             * one already sounding. Duplicate deliveries are possible - a
             * page can name its bgsound more than once - and restarting a
             * playing tune is audible. */
            if(res.ok && ps_swf_track_has_soundtrack(g_swf_track)) {
                /* The movie on this page is already its music, and two tunes
                 * at once in two keys is not a feature. The movie wins because
                 * its audio is tied to what is on screen: a soundtrack out of
                 * step with its own animation is a visible defect, while a
                 * <bgsound> is ambience nothing looks wrong without. */
                printf("popsurf: %s not started; the movie is the "
                       "soundtrack\n", res.url);
            }
            else if(res.ok && !strcmp(res.url, g_bgsound_url) &&
                    strcmp(res.url, g_playing_midi_url)) {
                snprintf(g_playing_midi_url, sizeof g_playing_midi_url, "%s",
                         res.url);

                if(url_is_adx(res.url)) {
                    /* The stream owns the compressed bytes for as long as it
                     * plays, so they must not be freed below with the rest of
                     * the result - and ps_adx_stream_play frees them itself on
                     * every path it refuses. */
                    if(ps_adx_stream_play(g_stream, res.data, res.len,
                                          ps_document_bgsound_loop(g_doc)) == 0)
                        ps_audio_stop(g_audio);
                    res.data = NULL;
                }
                else if(g_audio) {
                    ps_audio_play_midi(g_audio, res.data, res.len,
                                       ps_document_bgsound_loop(g_doc));
                }
            }
        }
        else if(res.kind == PS_JOB_APPLET) {
            /* Running an applet uploads a texture, so it is held back for the
             * same reason a decoded image is: writing VRAM while the network
             * adapter is moving data is what takes the machine down. Unlike an
             * image this is not queued - a class file is the last thing a page
             * asks for and the loader is normally idle by the time it lands -
             * but the check has to be here regardless. */
            if(ps_applet_deliver(g_applets, res.url, res.ok, res.data,
                                 res.len))
                relayout = 1;
        }
        else if(res.kind == PS_JOB_SWF) {
            /* Same rule the applet path states: a texture upload must not
             * overlap the network adapter. Binding a movie uploads its
             * gradients and bitmaps, so it waits for the queue to drain the
             * way a decoded image does. */
            if(res.ok && !strcmp(res.url, g_swf_url) && g_swf_pvr) {
                char err[96];

                err[0] = '\0';
                if(ps_swf_play_load(&g_swf, (const uint8_t *)res.data,
                                    res.len, err, sizeof err) == 0) {
                    (void)ps_swf_pvr_bind(g_swf_pvr, &g_swf.movie);
                    g_swf_ready       = 1;
                    g_swf_first_frame = 1;
                    printf("popsurf: swf %dx%d %d frames %.0f fps, "
                           "%d KB parsed, %d KB VRAM\n",
                           ps_swf_play_stage_w(&g_swf),
                           ps_swf_play_stage_h(&g_swf),
                           (int)g_swf.movie.root.nframe, (double)g_swf.movie.fps,
                           (int)(ps_swf_mem_live() / 1024),
                           (int)(ps_swf_pvr_vram(g_swf_pvr) / 1024));
                }
                else {
                    printf("popsurf: swf refused: %s\n", err);
                }
            }

            /* A second walk over the same bytes, for sound alone, and
             * deliberately not conditional on the first one having worked. The
             * two failures are independent by design: a movie whose art is
             * truncated still has a soundtrack, and one bad sound tag still
             * leaves the picture. See ps_swf_sound.h. */
            if(res.ok && !strcmp(res.url, g_swf_url) && g_swf_track) {
                char serr[96];

                serr[0] = '\0';
                if(ps_swf_track_load(g_swf_track, (const uint8_t *)res.data,
                                     res.len, serr, sizeof serr) < 0) {
                    printf("popsurf: swf audio refused: %s\n", serr);
                }
                else if(ps_swf_track_has_soundtrack(g_swf_track)) {
                    /* The movie is the page's music from here. Stopping what
                     * was playing is the whole reason has_soundtrack answers
                     * at load time rather than at the first sample. */
                    ps_audio_stop(g_audio);
                    ps_adx_stream_stop(g_stream);
                    g_playing_midi_url[0] = '\0';
                }
            }
        }
        else if(res.kind == PS_JOB_FRAME) {
            int i;

            for(i = 0; i < g_frame_tracked; i++) {
                if(strcmp(g_frame_url[i], res.url))
                    continue;
                if(res.ok)
                    ps_document_frame_load(g_doc, i, res.data, res.len,
                                           res.final_url);
                break;
            }
        }
        else {
            /* Images land after layout ran with them at zero size, so a
             * successful one changes the page's measurements.
             *
             * Held back while anything else is still transferring, because
             * decoding uploads a texture and that must not overlap the
             * adapter. */
            if(ps_loader_pending(g_loader) > 0 &&
               g_deferred_count < PS_DEFER_MAX) {
                ps_deferred_image *d = &g_deferred[g_deferred_count++];

                snprintf(d->url, sizeof d->url, "%s", res.url);
                d->data  = res.data;
                d->len   = res.len;
                d->ok    = res.ok;
                res.data = NULL;      /* ownership moved to the queue */
            }
            else if(ps_image_deliver(g_images, res.url, res.ok,
                                     res.data, res.len)) {
                relayout = 1;
            }
        }

        free(res.data);
    }

    if(relayout) {
        ps_document_relayout(g_doc);
        g_doc_height = ps_document_height(g_doc);
        page_dirty();
    }

    /* Quiet again: decode everything that was held back. */
    if(g_deferred_count && ps_loader_pending(g_loader) == 0)
        deferred_flush();
}

/* Three dots cycling, drawn bottom-right inside title-safe.
 *
 * Not a rotating arc: at 480i a spinning shape crawls along the scanlines and
 * reads as noise, whereas discrete blocks changing state stay legible. */
/* Two different waits want two different shapes.
 *
 * A soundbank is a megabyte or more and arrives once per session; it is worth
 * a panel that says so, and worth holding input, because navigating away
 * cancels the fetch and leaves the page silent for good.
 *
 * A page is kilobytes and arrives constantly. Blocking the screen on every
 * click would make the browser feel slower than it is, so pages get a thin
 * bar along the top instead - visible, but never in the way. */
static void draw_bank_overlay(ps_paint *p, ps_text_cache *text,
                              uint64_t now_ms)
{
    size_t   got = 0, total = 0;
    ps_rect  panel, track, fill;
    int      w = 300, h = 62;
    int      x = (PS_VIEW_W - w) / 2, y = (PS_VIEW_H - h) / 2;
    int      pct;
    char     label[64];

    ps_http_progress(&got, &total);
    pct = total ? (int)((uint64_t)got * 100 / total) : -1;

    panel.x0 = (int16_t)x;         panel.y0 = (int16_t)y;
    panel.x1 = (int16_t)(x + w);   panel.y1 = (int16_t)(y + h);
    ps_paint_rect(p, &panel, PS_C_OUTLINE);

    {
        ps_rect inner = { (int16_t)(x + 2), (int16_t)(y + 2),
                          (int16_t)(x + w - 2), (int16_t)(y + h - 2) };
        ps_paint_rect_v(p, &inner, PS_C_PANEL_EDGE, PS_C_PANEL);
    }

    if(pct >= 0)
        snprintf(label, sizeof label, "Loading...  %d%%", pct);
    else
        snprintf(label, sizeof label, "Loading%s",
                 (now_ms / 400) % 3 == 0 ? "." :
                 (now_ms / 400) % 3 == 1 ? ".." : "...");
    {
        ps_font *f = ps_text_font(text, PS_FONT_UI);

        if(f)
            ps_font_draw(p, f, x + 16, y + 24, label, strlen(label),
                         PS_C_TEXT);
    }

    track.x0 = (int16_t)(x + 16);      track.y0 = (int16_t)(y + 36);
    track.x1 = (int16_t)(x + w - 16);  track.y1 = (int16_t)(y + 48);
    ps_paint_rect(p, &track, PS_C_OUTLINE);

    fill = track;
    fill.x0 = (int16_t)(track.x0 + 1);
    fill.y0 = (int16_t)(track.y0 + 1);
    fill.y1 = (int16_t)(track.y1 - 1);
    if(pct >= 0) {
        fill.x1 = (int16_t)(fill.x0 + (track.x1 - track.x0 - 2) * pct / 100);
    }
    else {
        /* Unknown length: a block that sweeps, so it still reads as work in progress. */
        int span = (track.x1 - track.x0 - 2) / 4;
        int off  = (int)((now_ms / 12) % (uint64_t)(track.x1 - track.x0 - 2));

        fill.x0 = (int16_t)(track.x0 + 1 + off);
        fill.x1 = (int16_t)(fill.x0 + span);
        if(fill.x1 > track.x1 - 1)
            fill.x1 = (int16_t)(track.x1 - 1);
    }
    if(fill.x1 > fill.x0)
        ps_paint_rect(p, &fill, PS_C_ACCENT);
}

/* Thin, top-of-screen, non-blocking: the page-load counterpart to the bank
 * panel. Falls back to a sweeping block until the server says how big the
 * document is, which for most of the pages this browser targets is never. */
static void draw_page_bar(ps_paint *p, uint64_t now_ms)
{
    size_t  got = 0, total = 0;
    ps_rect bar;
    int     w;

    ps_http_progress(&got, &total);

    bar.x0 = 0;
    bar.y0 = 0;
    bar.x1 = (int16_t)PS_VIEW_W;
    bar.y1 = 3;
    ps_paint_rect(p, &bar, PS_C_OUTLINE);

    bar.y1 = 3;
    if(total) {
        w = (int)((uint64_t)PS_VIEW_W * got / total);
        bar.x0 = 0;
        bar.x1 = (int16_t)(w < 2 ? 2 : w);
    }
    else {
        int span = PS_VIEW_W / 5;
        int off  = (int)((now_ms / 4) % (uint64_t)PS_VIEW_W);

        bar.x0 = (int16_t)off;
        bar.x1 = (int16_t)(off + span > PS_VIEW_W ? PS_VIEW_W : off + span);
    }
    ps_paint_rect(p, &bar, PS_C_ACCENT);
}

static void draw_progress(ps_paint *p, int pending, uint64_t now_ms,
                          int bottom_inset)
{
    int i;
    int x = PS_VIEW_W - PS_SAFE_X - (3 * 18);
    int y = bottom_inset ? PS_VIEW_H - bottom_inset - 14 - PS_PAD
                         : PS_VIEW_H - PS_SAFE_Y - 14;
    int phase;

    if(!pending)
        return;

    phase = (int)((now_ms / 180) % 3);

    for(i = 0; i < 3; i++) {
        ps_rect r;
        ps_color c = (i == phase) ? PS_C_ACCENT : PS_C_ACCENT_DIM;

        r.x0 = (int16_t)(x + i * 18);
        r.y0 = (int16_t)y;
        r.x1 = (int16_t)(r.x0 + 12);
        r.y1 = (int16_t)(y + 12);

        /* Outlined, because this sits over page content of unknown colour. */
        ps_rect o = { (int16_t)(r.x0 - 2), (int16_t)(r.y0 - 2),
                      (int16_t)(r.x1 + 2), (int16_t)(r.y1 + 2) };
        ps_paint_rect(p, &o, PS_C_OUTLINE);
        ps_paint_rect(p, &r, c);
    }
}

/* Ends the program on the A+B+X+Y+Start combination. */
static void quit_combo(uint8_t addr, uint32_t btns)
{
    (void)addr;
    (void)btns;

    /* Silence before leaving. arch_exit() does not unwind, so any voice still
     * keyed goes on sounding after the program is gone - a held note screaming
     * over the loader is a poor way to end a session. */
    if(g_audio)
        ps_audio_stop(g_audio);
    ps_adx_stream_stop(g_stream);
    ps_swf_track_stop(g_swf_track);

    arch_exit();
}

int main(int argc, char **argv)
{
    const ps_gfx_backend *gfx = ps_gfx_pvr();
    void                 *ttf = NULL;
    ssize_t               ttf_len;
    uint64_t              last_ms;
    int                   prev_a = 0, prev_b = 0, prev_start = 0;
    int                   prev_up = 0, prev_down = 0;
    int                   prev_left = 0, prev_right = 0;
    int                   prev_x = 0, prev_y = 0;
    int                   prev_ltrig = 0, prev_rtrig = 0;

    (void)argc;
    (void)argv;

    /* dc-load's console is not a tty, so newlib hands stdout a 4KB buffer and
     * nothing reaches the host until it fills. When the program then hangs,
     * the last line on the console is not the last line that ran, which sends
     * you looking in entirely the wrong place. Unbuffered costs nothing at
     * this rate and keeps the console honest. */
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("popsurf: main entered\n");

    /* A+B+X+Y+Start ends the program, the standard Dreamcast combination.
     *
     * A browser has no natural exit, so without this there is no way back out
     * of it. That matters most during development over dcload: the loader can
     * only accept a new upload once it is back in control, and its reset
     * command is fire-and-forget with no acknowledgement - it reports success
     * whether or not a running program was listening. So a browser that never
     * exits means every single iteration needs someone to physically press
     * reset on the console.
     *
     * arch_exit() returns to whatever loaded us (ARCH_EXIT_RETURN is KOS's
     * default), so on a burned disc this drops to the system menu and under
     * dcload it hands control back to the loader. */
    cont_btn_callback(0, CONT_A | CONT_B | CONT_X | CONT_Y | CONT_START,
                      quit_combo);

    /* Registered whether or not a keyboard is plugged in: maple hotplugs, and
     * this way one attached later just starts working. Costs nothing when
     * there is none, since nothing ever calls it. */
    kbd_set_event_handler(kbd_event, NULL);

    /* The run's own settings, read before anything acts on one.
     *
     * They used to be read three hundred lines further down, after the PVR was
     * up and the SWF backend built, which meant a setting could be consulted
     * before the file carrying it had been opened - and one was, silently, for
     * three captures. Nothing here needs the video hardware: it is a file
     * under /pc and a handful of integers. Reading it first makes "was this
     * setting available yet" a question that cannot be asked. */
    g_boot_url = ps_probe_init(disc_root());
    ps_pvr_set_init_profile(ps_probe_pvrcfg());

    if(gfx->init(gfx->self, PS_VIEW_W, PS_VIEW_H) < 0) {
        printf("popsurf: PVR init failed\n");
        return 1;
    }

    /* After the mode exists, because the register is rebuilt from it, and
     * after pvr_init, which is what chose the scale being undone here. Both of
     * these are the display path making a picture pleasant, and both of them
     * are noise in a measurement. */
    if(ps_probe_dither() >= 0)
        ps_pvr_set_dither(ps_probe_dither());
    if(ps_probe_vsmooth() == 0)
        ps_pvr_disable_vsmooth();
    printf("popsurf: pvr ok\n");
    ps_paint_init(&g_paint, gfx);
    /* The cursor roams the whole screen, not just the page: the toolbar is
     * reached by pointing at it, so it has to be somewhere the pointer can
     * go. */
    ps_cursor_init(&g_cursor, PS_VIEW_W, PS_VIEW_H);
    ps_menu_init(&g_menu);
    ps_bar_init(&g_bar);
    ps_osk_init(&g_osk);
    ps_saver_init(&g_saver, PS_VIEW_W, PS_VIEW_H);
    printf("popsurf: ui init ok\n");

    /* Before the first page loads, so a bookmark can be the first thing you
     * navigate to. Needs the maple bus, which KOS has up by now. */
    ps_marks_init(&g_marks);

    /* Whether the network came up decides which boot-asset source can work at
     * all, so it is worth one line before either is tried. */
    if(net_default_dev)
        printf("popsurf: net up, %d.%d.%d.%d\n",
               net_default_dev->ip_addr[0], net_default_dev->ip_addr[1],
               net_default_dev->ip_addr[2], net_default_dev->ip_addr[3]);
    else
        printf("popsurf: no network device\n");

    ttf_len = load_boot_asset("font.ttf", &ttf);
    if(ttf_len < 0 || !ttf) {
        printf("popsurf: font load failed\n");
        return 1;
    }

    g_text = ps_text_create(gfx, ttf, (size_t)ttf_len);
    if(!g_text) {
        printf("popsurf: font parse failed\n");
        return 1;
    }

    /* Cursor art is optional: without it the built-in vector arrow is used, so
     * a missing or corrupt set costs appearance, never usability. */
    {
        void   *psc = NULL;
        ssize_t psc_len = load_boot_asset("cursors.psc", &psc);

        if(psc_len > 0 && psc) {
            g_cursor_art = ps_cursor_set_load(gfx, psc, (size_t)psc_len);
            free(psc);
        }
        if(g_cursor_art)
            ps_cursor_set_art(&g_cursor, g_cursor_art);
        else
            printf("popsurf: no cursor art, using built-in arrow\n");
    }

    /* The sound driver has to be up before any sample reaches SPU RAM. */
    {
        /* If the AICA driver fails to come up everything downstream silently
         * no-ops, so the result is worth knowing. */
        int rc = snd_init();

        printf("popsurf: snd_init=%d, spu free %u bytes\n", rc,
               (unsigned)snd_mem_available());
    }
    g_audio = ps_audio_create();
    if(!g_audio)
        printf("popsurf: audio unavailable; pages will be silent\n");
    g_stream = ps_adx_stream_create();
    g_swf_track = ps_swf_track_create();
    choose_default_bank();
#ifdef PS_AUDIO_SELFTEST
    ps_voice_selftest();
#endif
#ifdef PS_JAVA_SELFTEST
    ps_java_selftest(g_text);
#endif

    g_loader = ps_loader_create();
    if(!g_loader) {
        printf("popsurf: loader thread failed\n");
        return 1;
    }

    g_images = ps_image_cache_create(gfx, image_request, NULL);

    /* Applets paint into main memory with the CPU, so their drawString needs
     * a glyph rasteriser rather than the PVR atlas the page uses. Same font,
     * different path. */
    ps_jdc_text_ops(&g_applet_text, g_text);
    g_applets = ps_applet_cache_create(gfx, applet_request, NULL);
    if(g_applets)
        ps_applet_cache_set_text(g_applets, &g_applet_text);
    else
        printf("popsurf: applet support unavailable\n");

    /* No VRAM taken here: the sink allocates only when a movie binds, and
     * gives it all back when the page changes. A build with no movie on any
     * page it visits pays a struct. */
    g_swf_pvr = ps_swf_pvr_create();
    if(!g_swf_pvr)
        printf("popsurf: flash support unavailable\n");
    /* The mask path a run asked for is not applied here, twenty lines before
     * ps_probe_init reads the file that carries it. It was, and the answer was
     * three captures of the default that all looked like the experiment
     * working. It is applied per frame in draw_swf instead, where no ordering
     * can be got wrong. */

    g_doc    = ps_document_create(&g_paint, g_text, g_images,
                                  PS_VIEW_W, page_h());
    if(!g_doc) {
        printf("popsurf: document create failed\n");
        return 1;
    }
    ps_document_set_navigate_cb(g_doc, on_navigate, NULL);
    ps_document_set_applets(g_doc, g_applets);

    show_html(g_offline_html, strlen(g_offline_html), NULL);
    /* Where an unattended run was told to go, read at the top of main with the
     * rest of its settings. KOS drops dcload's argv, so a file next to the
     * assets is the only way to say it; absent, which is every ordinary boot,
     * this is NULL. */
    begin_load(g_boot_url ? g_boot_url : PS_HOME_URL);

#ifdef PS_OSK_DEMO
    /* Raises the keyboard at boot so its look can be captured without a
     * controller. Build-flag only; never on in a normal build. */
    g_osk_is_address = 1;
    ps_osk_open(&g_osk, "Address", "http://example.com/");
#endif

    last_ms = timer_ms_gettime64();

    for(;;) {
        maple_device_t *cont   = NULL;
        uint64_t        now_ms = timer_ms_gettime64();
        int             dt_ms  = (int)(now_ms - last_ms);
        int             a = 0, b = 0, start = 0, up = 0, down = 0;
        int             x_btn = 0, y = 0, left = 0, right = 0;
        int             joyx = 0, joyy = 0;
        int             ltrig = 0, rtrig = 0;
        int             max_scroll, pending;
        int             on_bar = 0;
        int             saver_up = 0, ui_live = 1;
        ps_rect         hover;

        last_ms = now_ms;

        /* While a transfer is in flight the browser touches no peripheral at
         * all: it submits nothing to the tile accelerator and does not poll
         * the controller.
         *
         * Both of those, independently, are enough to take the machine down
         * during a bulk transfer - an empty scene per frame does it, and so
         * does reading the pad, while the same program doing neither streams
         * megabytes cleanly. tests/pvr-net-conflict has the measurements.
         * Throttling does not help either; it is not about how much work,
         * only whether any happens while the adapter is busy.
         *
         * One frame is drawn when a transfer starts - so the progress bar and
         * any layout change reach the screen - and then everything holds
         * until the last job finishes. Navigations arm themselves and wait
         * for that frame before their request goes out, for the same reason.
         */
        if(ps_loader_pending(g_loader) > 0) {
            if(g_quiesce_drawn) {
                thd_sleep(20);
                continue;
            }
            g_quiesce_drawn = 1;
        }
        else {
            g_quiesce_drawn = 0;
        }

        cont = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);


        /* A frame that took longer than this did not render slowly, it
         * stalled - parsing and laying out a page blocks here, and so does
         * holding the picture through a transfer. Time must not be made up
         * afterwards: handing the real elapsed value to the sequencer makes
         * it advance by every tick it missed and fire all of those notes at
         * once. Thirty seconds of stall is a quarter of a song arriving in a
         * single frame. Animations would jump the same way.
         *
         * Clamping loses a little wall-clock accuracy in the tune, which
         * nobody can hear, and avoids an avalanche, which everybody can. */
        if(dt_ms > 100)
            dt_ms = 100;
        if(dt_ms < 0)
            dt_ms = 0;

        if(g_arm_state == PS_ARM_READY) {
            g_arm_state = PS_ARM_IDLE;
            if(g_arm_is_post)
                ps_loader_request_post(g_loader, g_arm_url, g_arm_body);
            else
                ps_loader_request(g_loader, PS_JOB_PAGE, g_arm_url);
        }

        /* The overlay has been drawn and nothing else will be submitted, so
         * it is now safe to start the transfer. */
        if(g_bank_arming && g_bank_frame_drawn) {
            g_bank_arming = 0;
            printf("popsurf: soundbank %s\n", g_bank_wanted);
            ps_loader_request(g_loader, PS_JOB_BANK, g_bank_wanted);
        }

        pump_loader();
        pump_frames();
        pump_bgsound();
        pump_swf();
        pump_refresh(dt_ms);

        /* Off the loop's own clamped dt, and never a thread. A movie's
         * playhead is page state, and page state is only ever touched between
         * frames here - a timeline advancing under the renderer would tear a
         * frame the display list is in the middle of replaying, on a machine
         * with no memory protection to catch it.
         *
         * An unattended run names the frame it wants and the playhead is held
         * there instead. Nothing else can make a capture repeatable: the host
         * renders one stated frame to compare against, and a movie that plays
         * while the page settles arrives at whichever frame the clock landed
         * on. See ps_probe_frame. */
        if(ps_probe_frame() >= 0)
            g_swf.frame = (uint32_t)ps_probe_frame();
        else
            (void)ps_swf_play_tick(&g_swf, dt_ms);
        /* An image or applet frame changing does not need a redraw: the quad
         * names the texture and the texture now holds different pixels. */
        ps_image_cache_tick(g_images, dt_ms);

        /* Scrolling no longer redraws, so it no longer has a chance to say
         * what came into view. The window is handed over every frame instead
         * and the cache works it out from the boxes the last draw recorded. */
        /* Nothing is on screen while the screensaver is up, so nothing is
         * worth painting. The applets keep their state and resume where they
         * left off. */
        /* The saver's own state, not this frame's saver_up - the ticks run
         * before the pad is read, so saver_up is still zero here and reading
         * it would have made this do nothing at all. One frame stale is
         * exactly right for a thirty second blank. */
        ps_applet_set_view(g_applets, g_scroll_y,
                           ps_saver_is_active(&g_saver) ? -1 : page_h());
        ps_applet_cache_tick(g_applets, dt_ms);

        /* A page with a marquee, a blink or a caret on it moves boxes rather
         * than texture contents, and has to be walked every frame. */
        if(ps_document_tick(g_doc, dt_ms))
            page_dirty();
        ps_audio_tick(g_audio, dt_ms);
        /* Refilled here and nowhere else. The frame is held for the whole of
         * any transfer, so a stream running through a page's images repeats
         * the second and a half its ring already holds until the loader goes
         * quiet again. That is the deliberate trade: writing sound memory
         * while the adapter has the G2 bus is the failure that takes the
         * machine down, and a repeat is only a glitch. */
        ps_adx_stream_tick(g_stream, dt_ms);
        /* Same frame, same rule, same reason: the movie's soundtrack writes
         * sound memory too, and the only safe moment to do that is here,
         * between frames, with the loader quiet. The frame number is the one
         * the playhead just settled on, so a stream block and the picture it
         * belongs to are decided together. */
        ps_swf_track_tick(g_swf_track, dt_ms, g_swf.frame);
        ps_menu_tick(&g_menu, dt_ms);
        ps_bar_tick(&g_bar, dt_ms);

        /* Both surfaces show the same two facts, so they are told once, here,
         * rather than each working them out. */
        ps_bar_set_nav(&g_bar, can_go_back(), can_go_forward());
        ps_bar_set_loading(&g_bar, g_loading_page);
        ps_menu_set_state(&g_menu, can_go_back(), can_go_forward(),
                          ps_bar_is_visible(&g_bar));
        refresh_menu_lists();

        if(cont) {
            cont_state_t *st = (cont_state_t *)maple_dev_status(cont);

            if(st) {
                a     = (st->buttons & CONT_A) != 0;
                x_btn = (st->buttons & CONT_X) != 0;
                y     = (st->buttons & CONT_Y) != 0;
                left  = (st->buttons & CONT_DPAD_LEFT) != 0;
                right = (st->buttons & CONT_DPAD_RIGHT) != 0;
                b     = (st->buttons & CONT_B) != 0;
                start = (st->buttons & CONT_START) != 0;
                up    = (st->buttons & CONT_DPAD_UP) != 0;
                down  = (st->buttons & CONT_DPAD_DOWN) != 0;
                joyx  = st->joyx;
                joyy  = st->joyy;
                ltrig = st->ltrig;
                rtrig = st->rtrig;
            }
        }

        /* Screensaver.
         *
         * Any button, either trigger or a real stick deflection counts as
         * being awake - the raw button word rather than the ones bound to
         * something, because pressing a button the browser ignores is still a
         * person sitting there.
         *
         * `busy` is anything that must hold the picture: a tune sounding, a
         * page or an asset still coming in. There is nothing to watch yet, but
         * a video element would answer this same question and nothing else
         * would need to change. */
        {
            int busy = media_is_playing() || g_loading_page ||
                       g_bank_loading || ps_loader_pending(g_loader) > 0;
            int any  = a || b || start || up || down || left || right ||
                       x_btn || y ||
                       ltrig > 32 || rtrig > 32 ||
                       joyx > PS_CURSOR_DEADZONE ||
                       joyx < -PS_CURSOR_DEADZONE ||
                       joyy > PS_CURSOR_DEADZONE ||
                       joyy < -PS_CURSOR_DEADZONE;
            int was  = ps_saver_is_active(&g_saver);

            saver_up = ps_saver_tick(&g_saver, dt_ms, any, busy);

            /* The press that dismisses it is spent doing so. Waking the screen
             * and clicking whatever the pointer happened to be left on are not
             * the same request. The button state still reaches prev_* at the
             * end of the frame, so holding it down does not fire an edge on
             * the next one either. */
            ui_live = !saver_up && !was;
        }

        /* Y raises the keyboard on whatever is focused, or on the address bar
         * if nothing is. It closes it too, which is why commit is what a Y
         * press produces rather than a separate cancel. */
        if(!ui_live) {
            /* The screensaver owns the frame, or this press is dismissing it.
             * Nothing else sees the pad either way. */
        }
        else if(y && !prev_y && !ps_osk_is_open(&g_osk) &&
                !ps_menu_is_open(&g_menu)) {
            if(ps_document_focused_editable(g_doc)) {
                g_osk_is_address = 0;
                ps_osk_open(&g_osk, ps_document_focused_label(g_doc),
                            ps_document_focused_value(g_doc));
            }
            else {
                open_address();
            }
        }
        else if(ps_osk_is_open(&g_osk)) {
            ps_osk_input oin;
            ps_osk_result res;

            memset(&oin, 0, sizeof oin);
            oin.up    = up;
            oin.down  = down;
            oin.left  = left;
            oin.right = right;
            oin.joy_x = joyx;
            oin.joy_y = joyy;
            oin.a     = a && !prev_a;
            oin.b     = b && !prev_b;
            oin.x     = x_btn && !prev_x;
            oin.y     = y && !prev_y;
            oin.start = start && !prev_start;

            res = ps_osk_update(&g_osk, &oin, dt_ms);

            if(res == PS_OSK_COMMIT) {
                if(g_osk_is_address) {
                    char url[PS_URL_MAX];

                    snprintf(url, sizeof url, "%s", ps_osk_text(&g_osk));
                    ps_osk_close(&g_osk);

                    /* A bare host is what people actually type. Refuse rather
                     * than truncate: silently dropping the tail of a long
                     * address loads a different page than the one asked for,
                     * which is worse than saying no.
                     *
                     * A leading slash is the other thing people type, and it
                     * is never a host: "/sd" is a card, not a site. ps_url
                     * reads it as local media, so it is left alone here. */
                    if(url[0] != '/' && !strstr(url, "://")) {
                        char tmp[PS_URL_MAX];

                        if(strlen(url) + 7 >= sizeof tmp) {
                            ps_menu_toast(&g_menu, "Address too long");
                            url[0] = '\0';
                        }
                        else {
                            snprintf(tmp, sizeof tmp, "http://%s", url);
                            snprintf(url, sizeof url, "%s", tmp);
                        }
                    }
                    if(url[0])
                        begin_load(url);
                }
                else {
                    ps_document_set_focused_value(g_doc, ps_osk_text(&g_osk));
                    g_doc_height = ps_document_height(g_doc);
                    page_dirty();
                    ps_osk_close(&g_osk);
                }
            }
            else if(res == PS_OSK_CANCEL) {
                ps_osk_close(&g_osk);
            }
        }
        else if(start && !prev_start) {
            ps_menu_toggle(&g_menu);
        }

        if(!ui_live) {
            /* See above. */
        }
        else if(ps_osk_is_open(&g_osk)) {
            /* Keyboard owns input; the page and menu see nothing. */
        }
        else if(ps_menu_is_open(&g_menu)) {
            /* The menu swallows input so the page never sees a click meant for
             * it, and the cursor holds still underneath. */
            ps_menu_action act = ps_menu_input(&g_menu, up && !prev_up,
                                               down && !prev_down,
                                               a && !prev_a, b && !prev_b);

            switch(act) {
            case PS_MENU_BACK:
                nav_history(-1);
                break;
            case PS_MENU_FORWARD:
                nav_history(+1);
                break;
            case PS_MENU_RELOAD:
                begin_load(g_current);
                break;
            case PS_MENU_HOME:
                begin_load(PS_HOME_URL);
                break;
            case PS_MENU_DIRECTORY:
                open_directory();
                break;
            case PS_MENU_ADDRESS:
                open_address();
                break;
            case PS_MENU_TOOLBAR:
                set_bar_visible(!ps_bar_is_visible(&g_bar));
                break;
            case PS_MENU_GO_HISTORY:
                nav_history_to(hist_label_pos(ps_menu_index(&g_menu)));
                break;
            case PS_MENU_GO_BOOKMARK: {
                const char *u = ps_marks_url(&g_marks, ps_menu_index(&g_menu));

                if(*u)
                    begin_load(u);
                break;
            }
            case PS_MENU_BOOKMARK_ADD:
                bookmark_current();
                break;
            case PS_MENU_BOOKMARK_DEL:
                unbookmark_current();
                break;
            case PS_MENU_QUIT:
                goto done;
            default:
                break;
            }
        }
        else {
            ps_cursor_update(&g_cursor, joyx, joyy, dt_ms);

            /* Where the pointer is, so an applet can be told it was entered or
             * left. Every frame, because the page scrolls under a pointer that
             * has not moved and that crosses boundaries just as truly as
             * moving the pointer does. */
            if(!g_bank_loading)
                ps_document_applet_hover(g_doc, ps_cursor_x(&g_cursor),
                                         ps_cursor_y(&g_cursor), g_scroll_y);

            /* The toolbar gets first refusal on the pointer. Its band is not
             * page, so a press there must never also reach the document -
             * which is what would happen if both were offered the click and
             * the page happened to have something laid out underneath. */
            ps_bar_point(&g_bar, ps_cursor_x(&g_cursor),
                         ps_cursor_y(&g_cursor));
            on_bar = ps_bar_is_visible(&g_bar) &&
                     ps_cursor_y(&g_cursor) >= page_h();

            /* An applet that has been clicked takes the d-pad as arrow keys.
             *
             * There is no other key the controller can produce, and arrows are
             * what a period applet wants - the games in the corpus are all
             * driven by Event.LEFT and Event.RIGHT. It costs the d-pad's
             * scrolling while an applet is focused, which is why a press
             * anywhere outside gives it up again: that is the only way back,
             * and it wants to be the obvious one. Edge triggered, so holding a
             * direction is one press and one release rather than sixty. */
            if(!g_bank_loading && ps_document_applet_focused(g_doc)) {
                int hit = 0;

                if(up != prev_up)
                    hit |= ps_document_applet_key(g_doc, PS_JKEY_UP, up);
                if(down != prev_down)
                    hit |= ps_document_applet_key(g_doc, PS_JKEY_DOWN, down);
                if(left != prev_left)
                    hit |= ps_document_applet_key(g_doc, PS_JKEY_LEFT, left);
                if(right != prev_right)
                    hit |= ps_document_applet_key(g_doc, PS_JKEY_RIGHT, right);
                if(hit)
                    page_dirty();

                /* Swallowed: scrolling with them would fight the applet. */
                up = down = left = right = 0;
            }

            if(ps_document_is_frameset(g_doc)) {
                /* Each frame scrolls itself, and the one under the cursor is
                 * the one you mean. */
                int fi = ps_document_frame_at(g_doc, ps_cursor_x(&g_cursor),
                                              ps_cursor_y(&g_cursor));
                if(fi >= 0) {
                    if(down)
                        ps_document_frame_scroll(g_doc, fi, PS_SCROLL_STEP);
                    if(up)
                        ps_document_frame_scroll(g_doc, fi, -PS_SCROLL_STEP);
                }
            }
            else {
                if(down)
                    g_scroll_y += PS_SCROLL_STEP;
                if(up)
                    g_scroll_y -= PS_SCROLL_STEP;
            }

            max_scroll = g_doc_height - page_h();
            if(max_scroll < 0)
                max_scroll = 0;
            if(g_scroll_y > max_scroll)
                g_scroll_y = max_scroll;
            if(g_scroll_y < 0)
                g_scroll_y = 0;

            if(on_bar) {
                /* Pointing at chrome. The page keeps whatever it was hovering
                 * rather than being told the pointer left, so nothing on it
                 * flickers off while you reach for Back. */
                ps_cursor_set_role(&g_cursor,
                                   ps_bar_hot(&g_bar) != PS_BAR_NONE
                                       ? PS_CUR_POINTER : PS_CUR_DEFAULT);

                if(!g_bank_loading && a && !prev_a)
                    ps_bar_press(&g_bar);

                if(!g_bank_loading && !a && prev_a) {
                    switch(ps_bar_release(&g_bar)) {
                    case PS_BAR_MENU: ps_menu_open(&g_menu); break;
                    case PS_BAR_BACK: nav_history(-1); break;
                    case PS_BAR_FWD:  nav_history(+1); break;
                    case PS_BAR_URL:  open_address(); break;
                    default: break;
                    }
                }
            }
            else {
                /* Hover can restyle a link, so a changed hit test is a
                 * changed page. */
                if(ps_document_mouse_move(g_doc, ps_cursor_x(&g_cursor),
                                          ps_cursor_y(&g_cursor), g_scroll_y))
                    page_dirty();

                /* A page load outranks whatever is under the cursor: while one
                 * is in flight the pointer should say so. Otherwise the role
                 * comes from the page's own computed CSS cursor, so text
                 * fields get an I-beam and resize handles get the right arrow
                 * for free. */
                if(g_loading_page)
                    ps_cursor_set_role(&g_cursor, PS_CUR_WAIT);
                else if(ps_loader_pending(g_loader))
                    ps_cursor_set_role(&g_cursor, PS_CUR_PROGRESS);
                else
                    ps_cursor_set_role(&g_cursor,
                                       ps_cursor_role_from_css(
                                           ps_document_cursor_css(g_doc)));

                /* Edge triggered: holding A must not fire a click every
                 * frame.
                 *
                 * An applet gets first refusal on a press inside its box. It
                 * is a program with its own idea of what a click means, and
                 * letting the page follow a link underneath it would be the
                 * same bug as a click passing through a form control. */
                if(!g_bank_loading && a && !prev_a) {
                    if(!ps_document_applet_input(g_doc, ps_cursor_x(&g_cursor),
                                                 ps_cursor_y(&g_cursor),
                                                 g_scroll_y, 1, 0)) {
                        if(ps_document_mouse_down(g_doc,
                                                  ps_cursor_x(&g_cursor),
                                                  ps_cursor_y(&g_cursor),
                                                  g_scroll_y))
                            page_dirty();
                    }
                }
                if(!g_bank_loading && !a && prev_a) {
                    if(!ps_document_applet_input(g_doc, ps_cursor_x(&g_cursor),
                                                 ps_cursor_y(&g_cursor),
                                                 g_scroll_y, 0, 0)) {
                        if(ps_document_mouse_up(g_doc, ps_cursor_x(&g_cursor),
                                                ps_cursor_y(&g_cursor),
                                                g_scroll_y))
                            page_dirty();
                    }
                }
            }

            /* History steps, wherever the pointer happens to be. These are the
             * fast path that the toolbar buttons are the discoverable version
             * of.
             *
             * The triggers are analog, so "pressed" is a threshold rather than
             * a bit. A third of travel is past the slack at the top and well
             * short of where somebody holding the controller normally rests. */
            if(!g_bank_loading && b && !prev_b)
                nav_history(-1);
            if(!g_bank_loading && ltrig > 80 && prev_ltrig <= 80)
                nav_history(-1);
            if(!g_bank_loading && rtrig > 80 && prev_rtrig <= 80)
                nav_history(+1);
        }

        prev_a     = a;
        prev_x     = x_btn;
        prev_y     = y;
        prev_b     = b;
        prev_start = start;
        prev_up    = up;
        prev_down  = down;
        prev_left  = left;
        prev_right = right;
        prev_ltrig = ltrig;
        prev_rtrig = rtrig;

#ifdef PS_NAV_STRESS
        /* Reproduces "clicked a link while the page was still coming in".
         * Timing matters: navigating the instant after a request goes out is
         * a different case from navigating once the transfer is underway, and
         * only the second is what a person actually does. So this waits until
         * a load has been in flight for PS_NAV_STRESS milliseconds - partway
         * through, with subresources still arriving - and only then jumps. */
        {
            static int load_ms = 0, idle_ms = 0, which = 0;
            static const char *urls[] = {
                "file:///pc/test.html",
                "file:///pc/test_swf.html",
                "file:///pc/applet.html",
            };

            if(g_loading_page || ps_loader_pending(g_loader)) {
                idle_ms = 0;
                load_ms += dt_ms;
                if(load_ms >= PS_NAV_STRESS) {
                    load_ms = 0;
                    printf("stress: interrupting load -> %s (loading=%d pending=%d)\n",
                           urls[which % 3], g_loading_page,
                           ps_loader_pending(g_loader));
                    fflush(stdout);
                    begin_load(urls[which++ % 3]);
                }
            }
            else {
                load_ms = 0;
                idle_ms += dt_ms;
                if(idle_ms >= 1200) {
                    idle_ms = 0;
                    begin_load(urls[which++ % 3]);
                }
            }
        }
#endif

        /* A navigation raised before the bank finished would cancel it. */
        if(g_bank_loading)
            g_have_pending = 0;

        if(g_have_pending) {
            char url[PS_URL_MAX];
            char body[PS_LOADER_BODY_MAX];
            int  is_post = g_pending_is_post;

            strcpy(url, g_pending);
            if(is_post)
                strcpy(body, g_pending_post);
            g_have_pending    = 0;
            g_pending_is_post = 0;

            begin_load_post(url, is_post ? body : NULL);
        }

        pending = ps_loader_pending(g_loader);

        /* Nothing is submitted to the tile accelerator while a bulk transfer
         * is in flight. Driving the PVR while the network adapter is moving
         * megabytes takes the machine down - see tests/pvr-net-conflict,
         * where an empty scene per frame is enough to do it, and throttling
         * the rate does not help. So the last frame stays on screen, with the
         * panel already drawn on it, until the bank has landed.
         *
         * This is why the browser appeared to freeze during the soundbank
         * fetch: it was not hung, it was the two subsystems colliding. */
        ps_paint_begin(&g_paint, PS_ARGB(255, 16, 16, 24));

        /* The page, then the veil, then the mark. The saver dims what is on
         * screen rather than covering it, so the page has to be painted first
         * for there to be anything to see through.
         *
         * The chrome is not painted. It is the part of the screen that never
         * changes and therefore the part that actually burns, and hiding it is
         * free here - a toolbar is no use to somebody who is not at the
         * controller. The cursor goes for the same reason, with one of its own:
         * a pointer parked over a screensaver is a small bright thing that
         * never moves at all. */
        if(saver_up) {
            /* Replayed, not redrawn.
             *
             * ps_document_draw draws at -scroll_y, so the applet elements
             * record their boxes in screen space - and since the page is now
             * recorded unscrolled, the cache reads those as document
             * coordinates. Scrolled down the page they come out negative, the
             * applets are judged off screen, and they never start again when
             * the saver lifts. That is the freeze.
             *
             * Replaying touches no elements at all, so nothing records a box
             * and the ones from the last real draw stand. It is also far
             * cheaper, which a screensaver should be. */
            if(ps_paint_can_replay(&g_paint))
                ps_paint_replay_offset(&g_paint, (float)-g_scroll_y);
            else
                ps_document_draw(g_doc, g_scroll_y);
            ps_saver_draw(&g_paint, &g_saver);
            ps_paint_end(&g_paint);
            if(g_arm_state == PS_ARM_WAITING_FRAME)
                g_arm_state = PS_ARM_READY;
            continue;
        }

        /* Scrolling no longer redraws.
         *
         * The page is recorded once at its own coordinates - the whole of it,
         * not the screenful that happened to be visible - so a scroll is the
         * same quads at a different offset. Quads that land off screen are
         * the tile accelerator's problem, and tiling is how it solves that.
         *
         * This is what makes the retained list worth more than the static
         * case: traversal cost scales with the whole DOM, and a 2306 pixel
         * page was being walked in full, sixty times a second, to put 394
         * pixels on screen. */
        g_last_scroll = g_scroll_y;

        if(!g_page_dirty && ps_paint_can_replay(&g_paint)) {
            ps_paint_replay_offset(&g_paint, (float)-g_scroll_y);
        }
        else {
#ifdef PS_APPLET_PROFILE
            static uint64_t doc_us, doc_n, doc_at;
            uint64_t t0 = timer_us_gettime64();
#endif
            /* Only a real draw can say which applets are on screen, so the
             * flags are cleared here rather than in the tick. */
            ps_applet_cache_page_begin(g_applets);

            /* Recorded at document coordinates and unclipped by the
             * viewport, which needs ps_paint's base clip widened to match -
             * otherwise everything below the fold is clamped away and the
             * recording is only good for the scroll position it was made
             * at. */
            ps_paint_record_begin(&g_paint);
            ps_paint_set_bounds(&g_paint, PS_VIEW_W,
                                g_doc_height > page_h() ? g_doc_height
                                                        : page_h());
            ps_document_draw_all(g_doc, g_doc_height);
            ps_paint_set_bounds(&g_paint, 0, 0);
            ps_paint_record_end(&g_paint);

            /* A page too big to record keeps redrawing, which is what it did
             * before this existed. */
            if(ps_paint_can_replay(&g_paint)) {
                g_page_dirty = 0;
                ps_paint_replay_offset(&g_paint, (float)-g_scroll_y);
            }
            else {
                ps_document_draw(g_doc, g_scroll_y);
            }
#ifdef PS_APPLET_PROFILE
            doc_us += timer_us_gettime64() - t0;
            doc_n++;
            if(timer_us_gettime64() - doc_at > 1000000ull) {
                extern uint64_t ps_paint_prof_submit_us, ps_paint_prof_quads;

                printf("popsurf: page redraw %lu us x%lu/s = traverse %lu + "
                       "submit %lu, %lu quads\n",
                       (unsigned long)(doc_n ? doc_us / doc_n : 0),
                       (unsigned long)doc_n,
                       (unsigned long)(doc_n ? (doc_us -
                            ps_paint_prof_submit_us) / doc_n : 0),
                       (unsigned long)(doc_n ? ps_paint_prof_submit_us / doc_n
                                             : 0),
                       (unsigned long)(doc_n ? ps_paint_prof_quads / doc_n
                                             : 0));
                doc_us = 0;
                doc_n = 0;
                ps_paint_prof_submit_us = 0;
                ps_paint_prof_quads = 0;
                doc_at = timer_us_gettime64();
            }
#endif
        }

        draw_swf();

        /* Not while the pointer is on the toolbar: the page's hover is left
         * standing so it does not flicker, but ringing a link you are no
         * longer pointing at would claim A still activates it. */
        if(!ps_menu_is_open(&g_menu) && !g_loading_page && !on_bar &&
           ps_document_cursor_is_link(g_doc) &&
           ps_document_hover_rect(g_doc, g_scroll_y, &hover))
            ps_cursor_draw_hover(&g_paint, &hover);

        /* Chrome before the panels that sit over it: the toolbar is part of
         * the page's frame, the menu and keyboard are raised above it. */
        ps_bar_draw(&g_paint, &g_bar, g_text);

        ps_menu_draw_toast(&g_paint, &g_menu, g_text, ps_bar_height(&g_bar));
        draw_progress(&g_paint, pending, now_ms, ps_bar_height(&g_bar));
        ps_menu_draw(&g_paint, &g_menu, g_text);
        ps_osk_draw(&g_paint, &g_osk, g_text);

        /* The toolbar shows load progress on its own top rule, so the
         * free-floating bar is only needed when there is no toolbar. */
        if(g_loading_page && !ps_bar_is_visible(&g_bar))
            draw_page_bar(&g_paint, now_ms);
        if(g_bank_loading) {
            draw_bank_overlay(&g_paint, g_text, now_ms);
            g_bank_frame_drawn = 1;
        }
        /* A pointer parked in the middle of the screen sits on top of whatever
         * an unattended run was sent to look at. Only ever hidden in such a
         * run, and only until the capture is taken. */
        if(!ps_probe_hide_cursor())
            ps_cursor_draw(&g_paint, &g_cursor);
        ps_paint_end(&g_paint);

        /* The scene is submitted and nothing else will be this iteration,
         * which is the only moment a capture can be sure of reading one whole
         * frame rather than halves of two. */
        if(ps_probe_after_frame(g_loading_page || g_bank_loading || pending > 0,
                                dt_ms))
            quit_combo(0, 0);

        if(g_arm_state == PS_ARM_WAITING_FRAME)
            g_arm_state = PS_ARM_READY;
    }

done:
    ps_loader_destroy(g_loader);
    ps_adx_stream_destroy(g_stream);
    ps_swf_track_destroy(g_swf_track);
    ps_audio_destroy(g_audio);
    ps_document_destroy(g_doc);
    ps_cursor_set_free(g_cursor_art);
    ps_swf_play_free(&g_swf);
    ps_swf_pvr_destroy(g_swf_pvr);
    ps_applet_cache_destroy(g_applets);
    ps_image_cache_destroy(g_images);
    ps_text_destroy(g_text);
    gfx->shutdown(gfx->self);
    return 0;
}
