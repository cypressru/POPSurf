/* What one frame of a movie would cost the tile accelerator.
 *
 * The open question about this player is not whether it draws the right
 * picture - tricmp answers that with no tolerance - it is whether the right
 * picture can be produced sixty times a second on a 200MHz SH-4 driving a PVR.
 * Trapezoidal decomposition deliberately emits more triangles than a real
 * triangulator, and the honest reaction to that is to count them rather than to
 * have an opinion about them.
 *
 * So this walks a frame exactly as gfx/pvr/ps_swf_pvr.c walks it - the same
 * stage sink, the same tessellator, the same rule for merging two triangles of
 * a trapezoid into one four-vertex strip - and reports what would have gone
 * into the TA:
 *
 *   draws    characters tessellated, each paying ps_swf_tess_shape's setup
 *   passes   state changes: one polygon context built and compiled each
 *   tris     triangles the tessellator emitted
 *   strips   primitives after the trapezoid merge
 *   TAbytes  the resulting vertices plus one header per pass, at 32 bytes each
 *
 * TAbytes is the number to compare against pvr_init's vertex buffer, which
 * gfx/pvr/ps_gfx_pvr.c currently asks for 512KB of. The first two exist
 * because hardware timings say the fixed costs matter more than the triangles
 * do, and a fixed cost has to be attributed to something countable before it
 * can be reduced - passes and draws are the two candidates, and a file where
 * they differ is what tells them apart. This prints them for any file, so a
 * page can be chosen to vary one while holding the geometry still.
 *
 * It is not a time measurement and cannot be: how long the SH-4 takes is a
 * property of the SH-4. The console prints its own line every second.
 *
 *   ./swfcost file.swf [-w px] [-h px] [-t tol] [-f frame|all]
 */
#include "ps_swf.h"
#include "ps_swf_play.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    ps_swf_view view;

    long tris, strips, verts, passes, draws;

    int   held;
    float hx[3], hy[3];
} cost;

static void flush_held(cost *c)
{
    if(!c->held)
        return;
    c->strips++;
    c->verts += 3;
    c->held = 0;
}

static void c_begin(void *user, const ps_swf_paint *p)
{
    cost *c = user;

    (void)p;
    flush_held(c);
    c->passes++;
}

/* The same pairing gfx/pvr/ps_swf_pvr.c applies, deliberately duplicated
 * rather than shared: this has to agree with the backend by inspection, and a
 * shared implementation would agree by construction even if the backend's rule
 * had drifted from what the hardware sees. */
static void c_tri(void *user, const ps_swf_vtx *v)
{
    cost *c = user;

    c->tris++;

    if(c->held && c->hx[0] == v[0].x && c->hy[0] == v[0].y &&
       c->hx[2] == v[1].x && c->hy[2] == v[1].y) {
        c->held = 0;
        c->strips++;
        c->verts += 4;
        return;
    }

    flush_held(c);
    c->hx[0] = v[0].x; c->hy[0] = v[0].y;
    c->hx[1] = v[1].x; c->hy[1] = v[1].y;
    c->hx[2] = v[2].x; c->hy[2] = v[2].y;
    c->held  = 1;
}

static void c_end(void *user)
{
    flush_held((cost *)user);
}

static const ps_swf_tri_sink g_tri = { c_begin, c_tri, c_end };

static void s_draw(void *user, const ps_swf_shape *sh, const ps_swf_xform *xf,
                   const ps_swf_cxform *cx)
{
    cost *c = user;

    c->draws++;
    (void)ps_swf_tess_shape(sh, &c->view, xf, cx, &g_tri, c);
}

/* Masks are counted, not skipped: a mask's geometry goes through the same
 * tessellator on the way to being measured, so it costs what it costs. */
static void s_nop(void *user) { (void)user; }

static const ps_swf_stage_sink g_stage = { s_draw, s_nop, s_nop, s_nop };

static void run_frame(ps_swf_play *pl, cost *c, int w, int h, uint32_t frame,
                      const char *label)
{
    ps_swf_xform root;
    clock_t      t0;
    double       ms;
    long         bytes;

    c->tris = c->strips = c->verts = c->passes = c->draws = 0;
    c->held = 0;

    ps_swf_play_fit(pl, w, h, 0.0f, 0.0f, &root);

    t0 = clock();
    (void)ps_swf_render_frame(&pl->movie, frame, &root, &g_stage, c);
    ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;

    bytes = (c->verts + c->passes) * 32;
    printf("  %-6s %6ld %6ld %8ld %8ld %9ld %8.1f %8.2f\n", label, c->draws,
           c->passes, c->tris, c->strips, bytes, bytes / 1024.0, ms);
}

int main(int argc, char **argv)
{
    const char  *path = NULL;
    int          w = 0, h = 0, i, all = 0;
    float        tol = 0.5f;
    long         frame = 0, len;
    FILE        *f;
    uint8_t     *buf;
    ps_swf_play  pl;
    cost         c;
    char         err[160] = "";

    for(i = 1; i < argc; i++) {
        if(!strcmp(argv[i], "-w") && i + 1 < argc)      w = atoi(argv[++i]);
        else if(!strcmp(argv[i], "-h") && i + 1 < argc) h = atoi(argv[++i]);
        else if(!strcmp(argv[i], "-t") && i + 1 < argc) tol = (float)atof(argv[++i]);
        else if(!strcmp(argv[i], "-f") && i + 1 < argc) {
            if(!strcmp(argv[i + 1], "all")) all = 1;
            else                            frame = atol(argv[i + 1]);
            i++;
        }
        else if(argv[i][0] != '-')                      path = argv[i];
    }
    if(!path) {
        fprintf(stderr, "usage: %s <file.swf> [-w px] [-h px] [-t tol] "
                        "[-f frame|all]\n", argv[0]);
        return 2;
    }

    f = fopen(path, "rb");
    if(!f) {
        fprintf(stderr, "cannot open %s\n", path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)len);
    if(!buf || fread(buf, 1, (size_t)len, f) != (size_t)len) {
        fprintf(stderr, "read failed\n");
        fclose(f);
        free(buf);
        return 1;
    }
    fclose(f);

    if(ps_swf_play_load(&pl, buf, (size_t)len, err, sizeof err) < 0) {
        fprintf(stderr, "%s: %s\n", path, err);
        free(buf);
        return 1;
    }
    free(buf);

    /* Default to the movie's own stage size, because that is the scale a page
     * embeds it at unless the author said otherwise, and triangle count is a
     * function of scale: the same curve flattened across twice the pixels is
     * flattened into more chords. */
    if(w <= 0) w = ps_swf_play_stage_w(&pl);
    if(h <= 0) h = ps_swf_play_stage_h(&pl);
    if(w <= 0) w = 320;
    if(h <= 0) h = 240;

    memset(&c, 0, sizeof c);
    c.view.w       = w;
    c.view.h       = h;
    c.view.tol     = tol;
    c.view.samples = 1;

    printf("%s  %dx%d  tol %.3f  %u frames  %.0f fps  %u shapes\n", path, w, h,
           (double)tol, (unsigned)pl.movie.root.nframe, (double)pl.movie.fps,
           (unsigned)pl.movie.nshape);
    printf("  %-6s %6s %6s %8s %8s %9s %8s %8s\n", "frame", "draws",
           "passes", "tris", "strips", "TAbytes", "KB", "ms");

    if(all) {
        uint32_t n = pl.movie.root.nframe, k;

        for(k = 0; k < n; k++) {
            char label[16];

            snprintf(label, sizeof label, "%u", (unsigned)k);
            run_frame(&pl, &c, w, h, k, label);
        }
    }
    else {
        run_frame(&pl, &c, w, h, (uint32_t)frame, "-");
    }

    printf("  peak %lu bytes held while parsing\n",
           (unsigned long)ps_swf_mem_peak());
    ps_swf_play_free(&pl);
    return 0;
}
