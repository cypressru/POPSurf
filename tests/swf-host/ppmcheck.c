/* Counts pixels of a given colour in a PPM and compares against an expected
 * total.
 *
 * The generated test files were chosen so that every answer is an integer
 * anyone can work out: a 100x60 pixel rectangle at scale 2 is 24000 filled
 * pixels and nothing else. Without something to assert that, the test suite
 * is a set of pictures that a person has to remember to look at, and a
 * regression in the winding rule or the style selection would sit there
 * unnoticed until the next time someone happened to open one.
 *
 *   ppmcheck file.ppm RRGGBB count [tolerance [x0 y0 x1 y1]]
 *
 * The tolerance exists for the curved case, where the exact pixel count
 * depends on where the antialiaser puts its boundary and is not an integer
 * anyone can derive on paper.
 *
 * The region exists for text layout, where a whole-image count is nearly
 * blind. Three glyphs left-aligned and three centred are the same number of
 * pixels, and so are two lines of a wrapped field and one line that ran off
 * the edge - so the only way to assert that a layout put the ink where it said
 * is to count inside a box that the wrong layout would miss. Half-open, in
 * pixels, x1 and y1 exclusive.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    FILE    *f;
    int      w, h, maxv, i, c;
    int      rx0 = 0, ry0 = 0, rx1 = -1, ry1 = -1;
    long     want, tol = 0, got = 0;
    unsigned rgb;
    unsigned char px[3];

    if(argc < 4) {
        fprintf(stderr,
                "usage: %s file.ppm RRGGBB count [tolerance [x0 y0 x1 y1]]\n",
                argv[0]);
        return 2;
    }
    rgb  = (unsigned)strtoul(argv[2], NULL, 16);
    want = strtol(argv[3], NULL, 10);
    if(argc > 4)
        tol = strtol(argv[4], NULL, 10);
    if(argc > 8) {
        rx0 = atoi(argv[5]);
        ry0 = atoi(argv[6]);
        rx1 = atoi(argv[7]);
        ry1 = atoi(argv[8]);
    }

    f = fopen(argv[1], "rb");
    if(!f) {
        fprintf(stderr, "%s: cannot open\n", argv[1]);
        return 1;
    }
    if(fscanf(f, "P6 %d %d %d", &w, &h, &maxv) != 3 || maxv != 255) {
        fprintf(stderr, "%s: not an 8-bit P6 PPM\n", argv[1]);
        fclose(f);
        return 1;
    }
    fgetc(f);                              /* the single whitespace byte */

    if(rx1 < 0) rx1 = w;
    if(ry1 < 0) ry1 = h;

    for(i = 0; i < w * h; i++) {
        int x = i % w, y = i / w;

        if(fread(px, 1, 3, f) != 3)
            break;
        if(x < rx0 || x >= rx1 || y < ry0 || y >= ry1)
            continue;
        if(px[0] == ((rgb >> 16) & 0xff) && px[1] == ((rgb >> 8) & 0xff) &&
           px[2] == (rgb & 0xff))
            got++;
    }
    fclose(f);

    c = (got >= want - tol && got <= want + tol);
    printf("%-22s %s %06x %8ld  want %ld", argv[1], c ? "ok  " : "FAIL",
           rgb, got, want);
    if(tol)
        printf(" +/- %ld", tol);
    if(argc > 8)
        printf(" in %d,%d..%d,%d", rx0, ry0, rx1, ry1);
    putchar('\n');
    return c ? 0 : 1;
}
