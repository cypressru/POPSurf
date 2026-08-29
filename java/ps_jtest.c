/* Runs a real applet on the console it has to run on.
 *
 * Everything about this subsystem was developed and checked on a host, which
 * is a good way to be wrong about a machine with a different word size, a
 * different endianness of nothing in particular, a 16KB instruction cache and
 * an FPU that would rather not see a double. This is the answer to "does it
 * actually work on the Dreamcast" - it loads a class off the disc, interprets
 * it, and prints a checksum of the pixels it drew.
 *
 * Build with:  make JAVA_SELFTEST=1
 */
#include "ps_jvm.h"
#include "ps_jdc.h"

#include <kos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_root[16];

static uint8_t *dc_loader(void *user, const char *name, size_t *out_len)
{
    char     path[96];
    void    *buf = NULL;
    ssize_t  len;

    (void)user;
    snprintf(path, sizeof path, "%s/%s.class", g_root, name);

    len = fs_load(path, &buf);
    if(len <= 0 || !buf) {
        free(buf);
        return NULL;
    }
    *out_len = (size_t)len;
    return (uint8_t *)buf;
}

/* A cheap hash over the frame. Two runs of the same applet must agree, and
 * the host's value and the console's must agree too - which is the whole
 * point of checking here rather than looking at the television. */
static uint32_t frame_hash(const ps_jsurface *s)
{
    uint32_t h = 2166136261u;
    int      i, n = s->w * s->h;

    for(i = 0; i < n; i++) {
        h ^= s->px[i];
        h *= 16777619u;
    }
    return h;
}

/* Every applet the disc carries, not just one. Chart exercises the
 * straight-line path; Modes exercises tableswitch, multianewarray and 64-bit
 * arithmetic, which are exactly the opcodes a host with a different word size
 * is most likely to disagree about. */
static const char *g_applets[] = { "Chart", "Modes", NULL };

static void run_one(ps_text_cache *text, const char *name);

void ps_java_selftest(ps_text_cache *text)
{
    static const char *roots[] = { "/pc", "/cd", NULL };
    int a;

    printf("popsurf: java selftest\n");

    for(a = 0; roots[a]; a++) {
        file_t f;

        snprintf(g_root, sizeof g_root, "%s", roots[a]);
        f = fs_open(g_root, O_RDONLY | O_DIR);
        if(f == FILEHND_INVALID)
            continue;
        fs_close(f);
        break;
    }

    for(a = 0; g_applets[a]; a++)
        run_one(text, g_applets[a]);
}

static void run_one(ps_text_cache *text, const char *applet_name)
{
    ps_jsurface  surf;
    ps_jgfx      g;
    ps_jtext_ops ops;
    ps_jvm       vm;
    ps_jclass   *c;
    uint8_t     *img = NULL;
    size_t       n = 0;
    uint64_t     t0, t1;
    int          lit = 0, px;

    img = dc_loader(NULL, applet_name, &n);
    if(!img) {
        printf("popsurf:   %s.class not on %s; skipping\n", applet_name,
               g_root);
        return;
    }
    printf("popsurf:   %s.class from %s, %d bytes\n", applet_name, g_root,
           (int)n);

    if(ps_jsurface_init(&surf, 300, 200) != 0) {
        printf("popsurf:   surface alloc failed\n");
        free(img);
        return;
    }
    /* Transparent, not magenta.
     *
     * The sentinel was 0xffff00ff, which is also Color.magenta - so an applet that
     * drew in magenta had those pixels counted as never painted. Every draw path
     * writes an opaque alpha, so a zero alpha is a value no drawing can produce
     * and the count means what it says. */
    ps_jsurface_clear(&surf, 0x00000000u);

    ps_jdc_text_ops(&ops, text);
    ps_jgfx_init(&g, &surf, &ops);

    ps_jvm_init(&vm, &g);
    ps_jvm_set_loader(&vm, dc_loader, NULL);

    c = ps_jvm_define(&vm, img, n);
    if(!c) {
        printf("popsurf:   load failed: %s\n", vm.err);
        goto out;
    }

    t0 = timer_ms_gettime64();
    if(ps_jvm_run_applet(&vm, c, &g) != 0) {
        printf("popsurf:   run failed: %s\n", vm.err);
        goto out;
    }
    t1 = timer_ms_gettime64();

    /* Any pixel that is still magenta was never painted. A frame that comes
     * out entirely unpainted is the failure this catches, and it is not
     * visible in a checksum alone. */
    for(px = 0; px < surf.w * surf.h; px++) {
        if((surf.px[px] >> 24) != 0)
            lit++;
    }

    printf("popsurf:   ran in %d ms, %ld instructions, %ld objects (%ld bytes)\n",
           (int)(t1 - t0), 20L * 1000 * 1000 - vm.budget, vm.objects, vm.bytes);
    printf("popsurf:   %d/%d pixels painted, frame hash %08lx\n",
           lit, surf.w * surf.h, (unsigned long)frame_hash(&surf));

out:
    ps_jvm_free(&vm);
    ps_jsurface_free(&surf);
}
