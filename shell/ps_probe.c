/* Boot arguments and framebuffer capture. See ps_probe.h for why. */
#include <kos.h>
#include <dc/pvr.h>
#include <dc/video.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ps_probe.h"
#include "ps_url.h"
/* For the mask path names only: the bootargs file is where a run says which
 * one it wants, and the names have to mean the same thing in both places. */
#include "../gfx/pvr/ps_swf_pvr.h"
#include "../gfx/pvr/ps_gfx_pvr.h"

/* The name dcload.sh uses in the sibling project, kept the same on purpose:
 * one convention across two programs is one thing to remember, and anyone
 * looking for it will look for this. */
#define PS_PROBE_ARGS_FILE ".bootargs"

/* Bigger than any settings file that belongs here; a bigger one is a mistake
 * rather than a request, and is refused rather than truncated for the reason
 * ps_url.h gives about truncated addresses. */
#define PS_PROBE_FILE_MAX  (PS_URL_MAX + 64)

static char g_url[PS_URL_MAX];
static char g_shot[160];

/* How long the page has to have been quiet before the frame is worth keeping.
 * Images arrive after layout has already run and trigger a reflow, so "the
 * page load finished" and "the page has stopped changing" are a beat apart,
 * and the first one is the wrong moment. */
static int g_settle_ms = 1200;

/* Shoot anyway after this long, and say so. A run that produces nothing is a
 * run nobody can learn from, and a frame of a page that never finished is
 * still evidence - as long as the file says that is what it is. Under the
 * screensaver's thirty seconds, because a dimmed screen is not evidence. */
static int g_timeout_ms = 25000;

static int g_exit_after = 1;
static int g_want_cursor;

/* The frame of a movie to hold still on, or -1 to let it play.
 *
 * A capture is compared against one frame rendered on the host, so the console
 * has to be showing that frame and not whichever one the playhead reached
 * while the page was settling. It is not a small effect: t_clip is six frames
 * at twelve a second, so 1200ms of settling lands fourteen frames in, on frame
 * two - which is the frame whose mask covers nothing, and which reads as the
 * renderer having clipped the content away entirely. */
static int g_frame = -1;

/* Which mask path the SWF backend is to take this run, or -1 for the build's
 * own default. One capture cannot compare two renderings of the same page, so
 * the way to settle what the hardware does with a modifier volume is several
 * runs of one build that differ in one thing each - and the thing has to
 * arrive from outside, because nobody is at the console to choose it. */
static int g_swfmask = -1;

/* Which pvr_init profile this run wants, or -1 for the browser's own. The
 * volume fault is in submission rather than in masking, and the browser's PVR
 * configuration is the part of submission that differs from the one
 * arrangement known to work. */
static int g_pvrcfg = -1;

/* Dithering: 1 on, 0 off, -1 not stated. Not stated is the ordinary case and
 * means an unattended run turns it off and a person keeps it. */
static int g_dither = -1;

/* The vertical smoothing KOS applies on a television cable, same convention.
 * Asking for it is asking to leave it alone, since only KOS knows what value
 * the cable wants; asking against it turns it off. */
static int g_vsmooth = -1;

static int g_armed;
static int g_quiet_ms;
static int g_age_ms;
static int g_late;
static int g_rx, g_ry, g_rw, g_rh;

static char *trim(char *s)
{
    char *e;

    while(*s == ' ' || *s == '\t')
        s++;
    e = s + strlen(s);
    while(e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
        *--e = '\0';
    return s;
}

/* Every setting the file carried, echoed as it is taken.
 *
 * The host wrote those lines and can check they came back, which closes the
 * half of the path a person cannot see: a key that arrives and is understood
 * now says so, a key that is not understood already said so, and the two
 * together mean a run can no longer be conducted in a state nobody asked for.
 * That is not hypothetical - swfmask parsed correctly for three runs and was
 * read by the shell before the file had been opened, and the captures came
 * back looking like the experiment had worked. */
static void set_option(const char *key, const char *val)
{
    printf("popsurf: bootargs %s=%s\n", key, val);

    if(!strcmp(key, "url"))
        snprintf(g_url, sizeof g_url, "%s", val);
    else if(!strcmp(key, "shot"))
        snprintf(g_shot, sizeof g_shot, "%s", val);
    else if(!strcmp(key, "settle"))
        g_settle_ms = atoi(val);
    else if(!strcmp(key, "timeout"))
        g_timeout_ms = atoi(val);
    else if(!strcmp(key, "exit"))
        g_exit_after = atoi(val);
    else if(!strcmp(key, "cursor"))
        g_want_cursor = atoi(val);
    else if(!strcmp(key, "frame"))
        g_frame = atoi(val);
    else if(!strcmp(key, "swfmask")) {
        /* Named rather than numbered in the file that a person writes and
         * reads: `swfmask=volonly` says what the run is testing, where
         * `swfmask=2` would have to be looked up every time. */
        if(!strcmp(val, "box"))          g_swfmask = PS_SWF_MASK_BOX;
        else if(!strcmp(val, "vol"))     g_swfmask = PS_SWF_MASK_VOL;
        else if(!strcmp(val, "volonly")) g_swfmask = PS_SWF_MASK_VOLONLY;
        else if(!strcmp(val, "modonly")) g_swfmask = PS_SWF_MASK_MODONLY;
        else printf("popsurf: bootargs: swfmask '%s' is not a mask path\n", val);
    }
    else if(!strcmp(key, "dither"))
        g_dither = atoi(val);
    else if(!strcmp(key, "vsmooth"))
        g_vsmooth = atoi(val);
    else if(!strcmp(key, "pvrcfg")) {
        int i;

        g_pvrcfg = -1;
        for(i = 0; i < PS_PVR_CFG_PROFILES; i++)
            if(!strcmp(val, ps_pvr_profile_name(i)))
                g_pvrcfg = i;
        if(g_pvrcfg < 0)
            printf("popsurf: bootargs: pvrcfg '%s' is not a profile\n", val);
    }
    else
        printf("popsurf: bootargs: ignoring '%s'\n", key);
}

/* One setting per line, key=value.
 *
 * A line with no '=' is taken as the address, so `echo http://... > .bootargs`
 * does the obvious thing and matches what the sibling project's script writes.
 * Blank lines and lines starting with '#' are comments, because the script
 * that generates this file should be able to explain itself to whoever reads
 * it next. */
static void parse(char *text)
{
    char *p = text;

    while(*p) {
        char *line = p;
        char *nl   = strchr(p, '\n');
        char *eq;

        if(nl) {
            *nl = '\0';
            p   = nl + 1;
        }
        else {
            p = line + strlen(line);
        }

        line = trim(line);
        if(!*line || *line == '#')
            continue;

        eq = strchr(line, '=');
        if(!eq) {
            snprintf(g_url, sizeof g_url, "%s", line);
            continue;
        }
        *eq = '\0';
        set_option(trim(line), trim(eq + 1));
    }
}

const char *ps_probe_init(const char *root)
{
    char    path[64];
    void   *text = NULL;
    ssize_t len;

    /* Only under dcload. See the header: a pressed disc cannot carry this file
     * and has nowhere to write a capture back to, so a shipped build pays one
     * string compare and stops here. */
    if(!root || strcmp(root, "/pc") != 0)
        return NULL;

    snprintf(path, sizeof path, "%s/%s", root, PS_PROBE_ARGS_FILE);
    len = fs_load(path, &text);
    if(len <= 0 || !text) {
        free(text);
        return NULL;
    }

    if(len >= PS_PROBE_FILE_MAX) {
        printf("popsurf: %s is %d bytes, which is not a settings file\n",
               path, (int)len);
        free(text);
        return NULL;
    }

    /* fs_load does not terminate what it read, and the parser below walks it
     * as a string. The check above is what makes this copy safe. */
    {
        char buf[PS_PROBE_FILE_MAX];

        memcpy(buf, text, (size_t)len);
        buf[len] = '\0';
        free(text);
        parse(buf);
    }

    if(g_shot[0]) {
        g_armed = 1;
        printf("popsurf: capture to %s after %d ms quiet (or %d ms)\n",
               g_shot, g_settle_ms, g_timeout_ms);
    }
    if(g_url[0])
        printf("popsurf: bootargs open %s\n", g_url);

    return g_url[0] ? g_url : NULL;
}

int ps_probe_hide_cursor(void)
{
    return g_armed && !g_want_cursor;
}

int ps_probe_frame(void)
{
    return g_frame;
}

int ps_probe_swfmask(void)
{
    return g_swfmask;
}

int ps_probe_pvrcfg(void)
{
    return g_pvrcfg;
}

/* Off for a run that is going to be measured, untouched for one that is not.
 *
 * A capture is compared against a renderer that stores colours exactly, and
 * dithering answers a colour the framebuffer cannot hold by alternating the two
 * it can - which is the right answer on a television and noise in a
 * comparison. A run with nobody watching has no television to be right for.
 * Stated outright in the file, that wins either way, because the one thing
 * worth being able to do is capture the dithering itself. */
int ps_probe_dither(void)
{
    if(g_dither >= 0)
        return g_dither != 0;
    return g_armed ? 0 : -1;
}

/* 0 to turn the vertical smoothing off, -1 to leave the cable's own setting
 * alone. There is no 1: this browser can remove KOS's smoothing but has no
 * business choosing a value to put back. */
int ps_probe_vsmooth(void)
{
    if(g_vsmooth >= 0)
        return g_vsmooth != 0 ? -1 : 0;
    return g_armed ? 0 : -1;
}

void ps_probe_set_region(int x, int y, int w, int h)
{
    g_rx = x;
    g_ry = y;
    g_rw = w;
    g_rh = h;
}

/* The address the video hardware is scanning out of, read the way a value the
 * vblank handler rewrites has to be read. Without the volatile view the
 * compiler is entitled to hoist it out of the loop below and spin forever. */
static const void *front_buffer(void)
{
    return *(void *volatile *)&vram_l;
}

/* Waits until the frame that was just submitted is the one being displayed.
 *
 * This is the whole reason a capture can be trusted. pvr_scene_finish() only
 * hands the scene over: the render into the back buffer happens afterwards on
 * the hardware's own schedule, and the two buffers are exchanged later still,
 * in the vblank handler once the render has signalled done. Reading the
 * framebuffer straight after finishing a scene therefore returns the *previous*
 * frame, and reading it while the exchange happens returns the top of one frame
 * and the bottom of another - which is indistinguishable, in a PPM, from a
 * renderer that draws half a page.
 *
 * So: wait for the render to start (pvr_wait_ready returns when the tile
 * accelerator has handed our scene on), wait for it to finish, then wait for
 * the exchange itself, which is visible from here as vram_l moving. Nothing
 * else is submitted afterwards, so no further exchange can happen underneath
 * the read that follows.
 *
 * The spin is bounded because none of those three waits is guaranteed on a
 * machine that has gone wrong, and a diagnostic that hangs is worse than one
 * that reports a suspect frame. */
static void wait_for_flip(void)
{
    const void *shown = front_buffer();
    int         i;

    pvr_wait_ready();
    pvr_wait_render_done();

    for(i = 0; i < 8 && front_buffer() == shown; i++)
        vid_waitvbl();
}

/* The frame, as a PPM, with what produced it in the header.
 *
 * The pixels come from KOS's own converter rather than a second copy of the
 * pixel-mode switch: it is the one piece of code that already knows every mode
 * the console can be in, and getting that wrong silently produces a picture
 * with the channels swapped, which reads as a colour bug in the browser. It
 * holds interrupts off for the length of the conversion, which is why this
 * runs once, at the end of a run, and not per frame.
 *
 * The comments carry the address and the region, so the comparison on the host
 * needs the file and nothing else - not a log to scrape, not a note about which
 * page this was. Netpbm allows comments anywhere in the header. */
static int write_shot(const char *path)
{
    uint8_t *px = NULL;
    size_t   len;
    /* Room for the longest address the browser will hold: snprintf reports the
     * length it wanted rather than the length it wrote, so a header that
     * overflowed would be written from past the end of this buffer. */
    char     hdr[PS_URL_MAX + 192];
    int      hlen;
    file_t   f;

    len = vid_screen_shot_data(&px);
    if(!len || !px) {
        printf("popsurf: capture: no framebuffer data\n");
        free(px);
        return -1;
    }

    hlen = snprintf(hdr, sizeof hdr,
                    "P6\n"
                    "# popsurf capture\n"
                    "# url %s\n"
                    "# region %d %d %d %d\n"
                    "# settled %s\n"
                    "%d %d\n255\n",
                    g_url[0] ? g_url : "(home)",
                    g_rx, g_ry, g_rw, g_rh,
                    g_late ? "no (timed out)" : "yes",
                    vid_mode->width, vid_mode->height);
    if(hlen < 0 || hlen >= (int)sizeof hdr)
        hlen = (int)strlen(hdr);

    f = fs_open(path, O_WRONLY | O_TRUNC);
    if(f == FILEHND_INVALID) {
        printf("popsurf: capture: cannot write %s\n", path);
        free(px);
        return -1;
    }

    if(fs_write(f, hdr, (size_t)hlen) != hlen ||
       fs_write(f, px, len) != (ssize_t)len) {
        printf("popsurf: capture: write to %s failed\n", path);
        fs_close(f);
        free(px);
        return -1;
    }

    fs_close(f);
    free(px);

    printf("popsurf: capture %dx%d -> %s, region %d %d %d %d, settled %s\n",
           vid_mode->width, vid_mode->height, path,
           g_rx, g_ry, g_rw, g_rh, g_late ? "no" : "yes");
    return 0;
}

int ps_probe_after_frame(int busy, int dt_ms)
{
    if(!g_armed)
        return 0;

    /* dt_ms is the loop's own clamped delta, and frames held for a transfer
     * never reach here at all, so this counts time the page was actually being
     * drawn. That is the right clock for both of these: a twenty-second
     * soundbank fetch should not spend the timeout, and it is not settling
     * either. */
    g_age_ms += dt_ms;
    g_quiet_ms = busy ? 0 : g_quiet_ms + dt_ms;

    if(g_quiet_ms < g_settle_ms) {
        if(g_age_ms < g_timeout_ms)
            return 0;
        g_late = 1;
    }

    g_armed = 0;
    wait_for_flip();
    write_shot(g_shot);

    return g_exit_after;
}
