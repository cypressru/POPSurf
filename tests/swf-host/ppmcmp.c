/* A frame captured off the Dreamcast against the same frame rendered here.
 *
 * This is the end of the loop the rest of tools/dccheck.sh sets up, and it is
 * the part that turns "it looks wrong" into a number. Two wrong diagnoses in
 * one session came from describing a correct render from the sofa - a test
 * font whose only glyph is a filled square, a gradient testbed with a
 * deliberate hard step in it - and both would have died here in a second.
 *
 *   ./ppmcmp capture.ppm reference.ppm [-r x,y,w,h] [-R x,y] [-t N]
 *                                      [-b RRGGBB] [-o diff.ppm] [-q]
 *
 * Three things have to be reconciled before the two pictures are even
 * comparable, and each of them is a place a careless comparison would report a
 * fault that is not there.
 *
 * Where. The console renders a whole 640x480 page with a toolbar under it and
 * the movie somewhere inside a table cell; swfrender renders the movie alone at
 * its stage size. Layout is the only thing that knows where the movie ended up,
 * so the browser writes the rectangle into the capture's PPM header and this
 * reads it back - no log to scrape and nothing to keep in step by hand. The
 * reference has a one-pixel margin of its own (see fit() in swfrender.c, which
 * translates by +1 so an edge exactly on the boundary is still visible), which
 * is what -R defaults to.
 *
 * How deep. The console's framebuffer is sixteen bits a pixel, and KOS's
 * screenshot expands 5 and 6 bit channels by shifting rather than replicating,
 * so an exact match against an 8-bit reference is not merely unlikely, it is
 * arithmetically impossible for most colours. Both sides are therefore
 * quantised the same way before anything is counted, and the default tolerance
 * is one step of the coarsest channel - because the tile accelerator is free to
 * round where the host truncates and being one 565 step apart says nothing.
 * -q compares at full depth, which is only useful against a capture from a
 * 32-bit mode.
 *
 * What is behind it. Neither the player nor its PVR backend paints the movie's
 * own background colour - the stage is transparent and the page shows through,
 * which is what a browser plugin does - while swfrender fills its canvas with
 * the file's declared background, because a frame on its own is meant to be
 * looked at. So the two disagree over every pixel the movie did not draw. -b
 * gives the colour the page paints there and re-colours the reference's
 * background to match. It is stated rather than guessed, and the count of
 * pixels it moved is printed, because a substitution that quietly hit half the
 * artwork would otherwise look like agreement.
 *
 * The comparison itself is ps_ppmdiff, shared with tricmp: interior pixels are
 * faults, boundary pixels are antialiasing, and the reference is the side that
 * gets to say which is which. See ps_ppmdiff.h.
 */
#include "ps_ppmdiff.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *px;
    int      w, h;
    int      rx, ry, rw, rh;   /* the "# region" comment, -1 when absent */
} image;

/* PPM reading, with the comments kept rather than skipped.
 *
 * The header popsurf writes carries the address it loaded and the rectangle
 * the movie occupies, which is the whole reason this reads comments at all: a
 * capture that cannot say what it is a capture of has to be paired up with a
 * log by hand, and pairing things up by hand is what this loop exists to
 * abolish. */
static int read_ppm(const char *path, image *im)
{
    FILE *f = fopen(path, "rb");
    int   c, field[3], nfield = 0;

    im->px = NULL;
    im->rx = im->ry = im->rw = im->rh = -1;

    if(!f) {
        fprintf(stderr, "cannot open %s\n", path);
        return -1;
    }
    if(fgetc(f) != 'P' || fgetc(f) != '6') {
        fprintf(stderr, "%s: not a P6 PPM\n", path);
        fclose(f);
        return -1;
    }

    while(nfield < 3) {
        c = fgetc(f);
        if(c == EOF) {
            fprintf(stderr, "%s: truncated header\n", path);
            fclose(f);
            return -1;
        }
        if(c == '#') {
            char line[256];

            if(fgets(line, sizeof line, f)) {
                int x, y, w, h;

                if(sscanf(line, " region %d %d %d %d", &x, &y, &w, &h) == 4) {
                    im->rx = x; im->ry = y; im->rw = w; im->rh = h;
                }
            }
            continue;
        }
        if(c == ' ' || c == '\t' || c == '\n' || c == '\r')
            continue;
        ungetc(c, f);
        if(fscanf(f, "%d", &field[nfield]) != 1) {
            fprintf(stderr, "%s: bad header\n", path);
            fclose(f);
            return -1;
        }
        nfield++;
    }
    /* Exactly one whitespace character separates the header from the data. */
    fgetc(f);

    if(field[2] != 255) {
        fprintf(stderr, "%s: maxval %d, only 255 is handled\n", path, field[2]);
        fclose(f);
        return -1;
    }
    im->w = field[0];
    im->h = field[1];
    if(im->w <= 0 || im->h <= 0 || im->w > 8192 || im->h > 8192) {
        fprintf(stderr, "%s: %dx%d is not a picture\n", path, im->w, im->h);
        fclose(f);
        return -1;
    }

    im->px = malloc((size_t)im->w * im->h * 3);
    if(!im->px || fread(im->px, 1, (size_t)im->w * im->h * 3, f) !=
                  (size_t)im->w * im->h * 3) {
        fprintf(stderr, "%s: short read\n", path);
        free(im->px);
        im->px = NULL;
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

static int write_ppm(const char *path, const uint8_t *px, int w, int h)
{
    FILE *f = fopen(path, "wb");

    if(!f)
        return -1;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    fwrite(px, 1, (size_t)w * h * 3, f);
    fclose(f);
    return 0;
}

/* What the console's framebuffer can hold, and nothing finer.
 *
 * Masking rather than rounding, because that is what KOS's screenshot does
 * coming back the other way: it shifts a 5-bit channel up by three and leaves
 * the low bits zero. Rounding here would put the reference half a step away
 * from every value a capture can contain. */
static void quantise_565(uint8_t *px, long npx)
{
    long k;

    for(k = 0; k < npx; k++) {
        px[k * 3 + 0] &= 0xf8;
        px[k * 3 + 1] &= 0xfc;
        px[k * 3 + 2] &= 0xf8;
    }
}

static uint8_t *crop(const image *im, int x0, int y0, int w, int h)
{
    uint8_t *out = malloc((size_t)w * h * 3);
    int      y;

    if(!out)
        return NULL;
    for(y = 0; y < h; y++)
        memcpy(out + (size_t)y * w * 3,
               im->px + ((size_t)(y0 + y) * im->w + x0) * 3, (size_t)w * 3);
    return out;
}

static int parse_rect(const char *s, int *x, int *y, int *w, int *h)
{
    return sscanf(s, "%d,%d,%d,%d", x, y, w, h) == 4;
}

int main(int argc, char **argv)
{
    const char       *cap_path = NULL, *ref_path = NULL, *diff_path = NULL;
    int               rx = -1, ry = -1, rw = -1, rh = -1;
    int               ox = 1, oy = 1;          /* swfrender's margin */
    int               tol = 8, quant = 1, have_bg = 0;
    unsigned          bg = 0;
    image             cap, ref;
    uint8_t          *a = NULL, *b = NULL, *map = NULL;
    ps_ppmdiff_result d;
    long              npx, moved = 0;
    int               i, w, h;

    for(i = 1; i < argc; i++) {
        if(!strcmp(argv[i], "-r") && i + 1 < argc) {
            if(!parse_rect(argv[++i], &rx, &ry, &rw, &rh)) {
                fprintf(stderr, "-r wants x,y,w,h\n");
                return 2;
            }
        }
        else if(!strcmp(argv[i], "-R") && i + 1 < argc) {
            if(sscanf(argv[++i], "%d,%d", &ox, &oy) != 2) {
                fprintf(stderr, "-R wants x,y\n");
                return 2;
            }
        }
        else if(!strcmp(argv[i], "-t") && i + 1 < argc) tol = atoi(argv[++i]);
        else if(!strcmp(argv[i], "-o") && i + 1 < argc) diff_path = argv[++i];
        else if(!strcmp(argv[i], "-q"))                 quant = 0;
        else if(!strcmp(argv[i], "-b") && i + 1 < argc) {
            bg = (unsigned)strtoul(argv[++i], NULL, 16);
            have_bg = 1;
        }
        else if(argv[i][0] == '-') {
            fprintf(stderr, "unknown option %s\n", argv[i]);
            return 2;
        }
        else if(!cap_path) cap_path = argv[i];
        else               ref_path = argv[i];
    }
    if(!cap_path || !ref_path) {
        fprintf(stderr,
                "usage: %s <capture.ppm> <reference.ppm> [-r x,y,w,h]"
                " [-R x,y] [-t N] [-b RRGGBB] [-o diff.ppm] [-q]\n", argv[0]);
        return 2;
    }

    if(read_ppm(cap_path, &cap) < 0 || read_ppm(ref_path, &ref) < 0)
        return 1;

    /* The browser's own answer unless the caller overrode it. Without either,
     * the whole capture is compared, which is right for two pictures of the
     * same thing and wrong for a page with chrome - so it says so. */
    if(rw < 0) {
        if(cap.rw > 0) {
            rx = cap.rx; ry = cap.ry; rw = cap.rw; rh = cap.rh;
        }
        else {
            rx = ry = 0; rw = cap.w; rh = cap.h;
            fprintf(stderr, "note: no region in %s and no -r; comparing the"
                            " whole frame\n", cap_path);
        }
    }

    /* Clamped rather than refused: a movie can hang off the bottom of the
     * viewport, and the part that is on screen is still worth comparing. */
    if(rx < 0) { rw += rx; ox -= rx; rx = 0; }
    if(ry < 0) { rh += ry; oy -= ry; ry = 0; }
    w = rw;
    h = rh;
    if(rx + w > cap.w) w = cap.w - rx;
    if(ry + h > cap.h) h = cap.h - ry;
    if(ox + w > ref.w) w = ref.w - ox;
    if(oy + h > ref.h) h = ref.h - oy;
    if(w <= 0 || h <= 0 || ox < 0 || oy < 0) {
        fprintf(stderr, "nothing to compare: region %d,%d %dx%d against a"
                        " %dx%d reference at %d,%d\n",
                rx, ry, rw, rh, ref.w, ref.h, ox, oy);
        return 1;
    }

    a = crop(&ref, ox, oy, w, h);
    b = crop(&cap, rx, ry, w, h);
    npx = (long)w * h;
    map = malloc((size_t)npx);
    if(!a || !b || !map) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    /* The reference's own background, taken from its margin where no artwork
     * can reach, replaced by whatever the page shows through the stage. */
    if(have_bg) {
        uint8_t was[3];
        long    k;

        was[0] = ref.px[0];
        was[1] = ref.px[1];
        was[2] = ref.px[2];
        for(k = 0; k < npx; k++) {
            if(a[k * 3 + 0] == was[0] && a[k * 3 + 1] == was[1] &&
               a[k * 3 + 2] == was[2]) {
                a[k * 3 + 0] = (uint8_t)(bg >> 16);
                a[k * 3 + 1] = (uint8_t)(bg >> 8);
                a[k * 3 + 2] = (uint8_t)bg;
                moved++;
            }
        }
        printf("background %02x%02x%02x -> %06x on %ld of %ld pixels\n",
               was[0], was[1], was[2], bg, moved, npx);
    }

    if(quant) {
        quantise_565(a, npx);
        quantise_565(b, npx);
    }

    ps_ppmdiff(a, b, w, h, tol, map, &d);

    printf("capture   %s  %dx%d  region %d,%d %dx%d\n",
           cap_path, cap.w, cap.h, rx, ry, rw, rh);
    printf("reference %s  %dx%d  stage at %d,%d\n",
           ref_path, ref.w, ref.h, ox, oy);
    printf("compared  %dx%d = %ld pixels, %s, tolerance %d\n",
           w, h, npx, quant ? "565-quantised" : "full depth", tol);
    printf("  interior  %8ld   <- a fault\n", d.interior);
    printf("  boundary  %8ld   <- antialiasing\n", d.boundary);
    printf("  over one step %5ld\n", d.worse);
    printf("  worst channel difference %d at %d,%d\n",
           d.worst, d.worst_x, d.worst_y);

    if(diff_path) {
        /* The capture, dimmed, with the disagreements painted on it: red for
         * an interior pixel and blue for a boundary one. A count cannot say
         * whether forty differing pixels are one hole or forty edges. */
        uint8_t *img = malloc((size_t)npx * 3);
        long     k;

        if(img) {
            for(k = 0; k < npx; k++) {
                if(map[k] == PS_PPMDIFF_INTERIOR) {
                    img[k * 3 + 0] = 255; img[k * 3 + 1] = 0; img[k * 3 + 2] = 0;
                }
                else if(map[k] == PS_PPMDIFF_BOUNDARY) {
                    img[k * 3 + 0] = 0; img[k * 3 + 1] = 80; img[k * 3 + 2] = 255;
                }
                else {
                    img[k * 3 + 0] = (uint8_t)(b[k * 3 + 0] / 3);
                    img[k * 3 + 1] = (uint8_t)(b[k * 3 + 1] / 3);
                    img[k * 3 + 2] = (uint8_t)(b[k * 3 + 2] / 3);
                }
            }
            if(write_ppm(diff_path, img, w, h) == 0)
                printf("  differences -> %s\n", diff_path);
            free(img);
        }
    }

    free(a);
    free(b);
    free(map);
    free(cap.px);
    free(ref.px);

    if(d.interior) {
        printf("MISMATCH\n");
        return 1;
    }
    printf("OK\n");
    return 0;
}
