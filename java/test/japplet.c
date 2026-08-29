/* Runs a compiled applet's paint() and writes the result to a PPM.
 *
 * This is the whole stack end to end: class file in, bytecode interpreted,
 * java.awt.Graphics forwarded to the rasteriser, pixels out. No Dreamcast
 * involved, which is the point of the surface being a plain buffer.
 */
#include "ps_jvm.h"
#include "ps_applet.h"
#include "ps_jar.h"
#include "ps_joff.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include "hostfont.inc"

/* stb_image's inflate, which ps_jar needs, and its decoders, which ps_applet
 * needs. Pulled in here for the same reason jrun.c does it: the host build has
 * no ps_image.o, and without this translation unit the documented link line
 * fails on stbi_zlib_decode_noheader_malloc. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include "stb_image.h"

/* Loads Foo for a reference to "Foo", from the applet directory. */
static char g_dir[512];

static uint8_t *loader(void *user, const char *name, size_t *out_len)
{
    char     path[768];
    FILE    *f;
    long     sz;
    uint8_t *buf;

    (void)user;
    snprintf(path, sizeof path, "%s/%s.class", g_dir, name);
    f = fopen(path, "rb");
    if(!f)
        return NULL;
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    buf = (uint8_t *)malloc((size_t)sz + 1);
    if(!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    *out_len = (size_t)sz;
    return buf;
}

static int write_ppm(const char *path, const ps_jsurface *s)
{
    FILE *f = fopen(path, "wb");
    int   i, n = s->w * s->h;

    if(!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", s->w, s->h);
    for(i = 0; i < n; i++) {
        uint32_t p = s->px[i];
        unsigned char rgb[3] = { (unsigned char)(p >> 16),
                                 (unsigned char)(p >> 8), (unsigned char)p };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    const char  *cls_path = argc > 1 ? argv[1] : "applets/Chart.class";
    const char  *out      = argc > 2 ? argv[2] : "applet.ppm";
    const char  *ttf      = argc > 3 ? argv[3] : "../../cd/font.ttf";
    int          frames   = argc > 4 ? atoi(argv[4]) : 0;
    host_font    font;
    ps_jtext_ops ops;
    ps_jsurface  surf;
    ps_jgfx      g;
    ps_jvm       vm;
    ps_jclass   *c;
    uint8_t     *img;
    size_t       n = 0;
    const char  *slash, *dot;
    char         base[256];
    int          rc;

    /* Split "applets/Chart.class" into a directory to load from and a class
     * name to start at, the way a browser splits a codebase from an applet
     * attribute. */
    slash = strrchr(cls_path, '/');
    snprintf(g_dir, sizeof g_dir, "%.*s",
             slash ? (int)(slash - cls_path) : 1, slash ? cls_path : ".");
    snprintf(base, sizeof base, "%s", slash ? slash + 1 : cls_path);
    dot = strrchr(base, '.');
    if(dot) *(char *)dot = '\0';

    if(host_font_load(&font, ttf) != 0)
        fprintf(stderr, "warn: no font at %s; drawString will be blank\n", ttf);
    host_font_ops(&font, &ops);

    if(ps_jsurface_init(&surf, 300, 200) != 0) {
        fprintf(stderr, "surface alloc failed\n");
        return 1;
    }
    ps_jsurface_clear(&surf, 0x00000000u);      /* alpha 0 = never painted */
    ps_jgfx_init(&g, &surf, &ops);

    ps_jvm_init(&vm, &g);
    ps_jvm_set_loader(&vm, loader, NULL);

    img = loader(NULL, base, &n);
    if(!img) {
        fprintf(stderr, "cannot read %s\n", cls_path);
        return 1;
    }

    c = ps_jvm_define(&vm, img, n);
    if(!c) {
        fprintf(stderr, "load failed: %s\n", vm.err);
        return 1;
    }

    printf("class      %s extends %s\n", c->name,
           c->super_name ? c->super_name : "-");

    rc = ps_jvm_run_applet(&vm, c, &g);
    if(rc != 0) {
        fprintf(stderr, "run failed: %s\n", vm.err);
        write_ppm(out, &surf);   /* keep the partial frame; it shows how far */
        return 1;
    }

    printf("objects    %ld (%ld bytes)\n", vm.objects, vm.bytes);
    printf("budget     %ld instructions left\n", vm.budget);
    printf("thread     %s\n", ps_jvm_has_thread(&vm) ? "running" : "none");

    if(write_ppm(out, &surf) != 0) {
        fprintf(stderr, "cannot write %s\n", out);
        return 1;
    }
    printf("wrote      %s\n", out);

    /* Animation. The browser drives this off its frame clock; here it is a
     * fixed 40ms step so the output is reproducible - a golden image of an
     * animated applet is worth nothing if it depends on how fast the host
     * happens to be. */
    if(frames > 0) {
        int f;

        for(f = 1; f <= frames; f++) {
            char path[300];
            int  steps = 0;

            /* Advance until the applet asks to be repainted, or it is clear
             * it never will. Several pumps per frame is normal: a sleep
             * returns immediately and the loop has to come round again. */
            while(steps++ < 64 && !vm.repaint && ps_jvm_has_thread(&vm))
                ps_jvm_pump(&vm, 40, 200000);

            if(!ps_jvm_take_repaint(&vm)) {
                printf("frame %-3d  no repaint requested; stopping\n", f);
                break;
            }

            if(ps_jvm_paint(&vm, &g) != 0) {
                fprintf(stderr, "repaint %d failed: %s\n", f, vm.err);
                break;
            }

            snprintf(path, sizeof path, "%.*s_%02d.ppm",
                     (int)(strrchr(out, '.') ? strrchr(out, '.') - out
                                             : (long)strlen(out)), out, f);
            write_ppm(path, &surf);
            printf("frame %-3d  %s\n", f, path);
        }
    }

    /* Before ps_jvm_free, which is what ps_applet.c does for the same reason:
     * the offscreen buffers are found by walking this VM's heap. */
    ps_joff_release(&vm);
    ps_jvm_free(&vm);
    ps_jsurface_free(&surf);
    host_font_free(&font);
    return 0;
}
