/* Malformed files through the whole player path, reproducibly.
 *
 * Three commit messages in this tree cite fuzzing - 2500 files, then 12,285,
 * then 13,855, all clean under ASan and UBSan. Those runs happened; the
 * harness that produced them did not survive them, so nobody could repeat one,
 * and an assertion nobody can repeat reads as coverage while providing none.
 * This is that harness, written down. The numbers it prints are the numbers,
 * and `-s` and `-c` turn any of them back into the exact bytes that produced
 * it.
 *
 *   ./swffuzz out/t_*.swf              every phase, default seed
 *   ./swffuzz -n 400 out/t_*.swf       400 mutants per seed file
 *   ./swffuzz -s 12345 out/t_*.swf     a different corpus, still reproducible
 *   ./swffuzz -c 90210 out/t_*.swf     that one case and nothing else
 *   ./swffuzz -c 90210 -d bad.swf out/t_*.swf     and write it out
 *   ./swffuzz -b 90211 out/t_*.swf     carry on from just after it
 *   ./swffuzz -N out/t_*.swf           how many cases that would be
 *   ./swffuzz leakcheck                deliberately leak; see below
 *
 * Why the whole path and not just the parser. A file that parses cleanly can
 * still kill the renderer, because the parser's job is to reject bytes it
 * cannot represent, not to reject geometry that is merely absurd. A shape with
 * a fill style index one past the table, a PlaceObject2 matrix with a scale of
 * 1e30, a glyph offset that points at its own header - all three parse. So
 * each case goes parse -> display-list walk -> raster -> tessellate -> free,
 * which is every stage a page would run, and morph blending on top of that,
 * since a blended coordinate is arithmetic rather than a transcribed field and
 * is the one number in the renderer no file states.
 *
 * Why the corpus is generated rather than committed. It has to be: the seeds
 * are mkswf's output, which is a few kilobytes of arithmetic and is rebuilt in
 * milliseconds, and the derived cases are a pure function of (seed, case
 * index). Committing a hundred thousand mutants would be committing a hundred
 * thousand copies of that function's output. It also keeps the promise the
 * .gitignore makes - no .swf in the tree - without depending on anyone
 * remembering it.
 *
 * Why the spans are written unclamped. Both existing harnesses clamp x and y
 * in their span sink before touching a pixel, which is correct for a tool that
 * has to produce a picture and fatal for one looking for a bug: a renderer
 * emitting a span at y = -3 is caught by neither. Here the sink writes where
 * it is told, into a heap buffer, so ASan's redzone is the bound check. The
 * clip path is exercised too and does clamp, deliberately - that is the real
 * path and it has its own planes to overrun.
 *
 * Why there is a leak check at all, and why it is a subcommand. LeakSanitizer
 * is on by default under ASan on Linux and off on several other platforms, and
 * "leaks would have been reported" is exactly the kind of assumption this file
 * exists to stop making. `swffuzz leakcheck` leaks on purpose and is expected
 * to die; the Makefile fails if it exits cleanly. That check runs on every
 * `make fuzz` rather than once in somebody's terminal - and it earned its keep
 * immediately, because the first version of it leaked one block, exited 0, and
 * said LeakSanitizer was off when it was working perfectly. See leak_some.
 *
 * Alongside it, every case asserts ps_swf_mem_live() returns to where it
 * started. That catches what LSan structurally cannot - memory still reachable
 * from a live pointer at exit but never handed back - and unlike LSan it names
 * the case that did it.
 */
#define _POSIX_C_SOURCE 200809L

#include "ps_swf.h"
#include "ps_swf_clip.h"
#include "ps_swf_mem.h"
#include "ps_swf_morph.h"
#include "ps_swf_trisoft.h"

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Small on purpose. The question is whether the code survives the bytes, and
 * a 64x48 canvas asks it exactly as well as a stage-sized one while keeping a
 * case in the tens of microseconds - which is what decides whether this gets
 * run every time or skipped. */
enum { CANVAS_W = 64, CANVAS_H = 48 };

/* Every truncation up to this length, then a sweep to EOF on a stride. The
 * seeds mkswf writes are all well under the bound, so today the sweep never
 * engages and the truncation phase is exhaustive; the stride is here so that
 * pointing this at a larger file degrades into a sweep instead of into an
 * afternoon. Prime, so the sweep does not land on the same field of every
 * fixed-size record. */
enum { TRUNC_FULL = 4096, TRUNC_STRIDE = 61 };

/* A corrupt file can claim more frames, shapes and morphs than it has content
 * for, and rendering all of them is work that finds nothing new after the
 * first few. Frames especially: ps_swf_render_frame replays from frame 0 every
 * call, so asking for n frames is quadratic in n by design. */
enum { MAX_FRAMES = 4, MAX_SHAPES = 24, MAX_MORPHS = 8 };

/* Longer than any case should ever take, short enough that a hang is a
 * finding rather than a hung build. A file that takes ten seconds of a
 * workstation is a file that never finishes on a 200MHz SH-4, so this is a
 * real defect class and not just a harness convenience. */
enum { CASE_SECONDS = 10 };

enum { MUT_CAP = 1u << 16 };

/* --- what case is running ------------------------------------------------ */

/* Formatted before each case and written out by the signal handler below.
 * Under -fno-sanitize-recover a fault ends the process where it happens, so
 * anything printed afterwards by main() is never printed at all, and the one
 * fact worth having - which case - has to survive the death. */
static char g_case[192];
static int  g_case_len;

static void say_case(void)
{
    /* write() rather than printf(): this runs from a signal handler and after
     * a sanitizer has already decided the process is over. */
    (void)!write(2, "\nswffuzz: last case: ", 21);
    (void)!write(2, g_case, (size_t)g_case_len);
    (void)!write(2, "\n", 1);
}

static void on_signal(int sig)
{
    say_case();
    if(sig == SIGALRM)
        (void)!write(2, "swffuzz: that case did not finish in time\n", 42);
    /* Back to the default and re-raise, so the exit status still says what
     * killed it and a core file is still produced. */
    signal(sig, SIG_DFL);
    raise(sig);
}

/* Which signals are worth catching depends on who else is catching them.
 * Under ASan the fault handlers are ASan's, and taking them over would trade
 * its report - the stack, the allocation site, the redzone - for one line of
 * ours. It aborts once it has printed, so SIGABRT alone brings the case number
 * back and nothing is lost. Without ASan there is no report to lose and a bare
 * SIGSEGV would otherwise say nothing at all. */
#if defined(__SANITIZE_ADDRESS__) || \
    (defined(__has_feature) && __has_feature(address_sanitizer))
#define FUZZ_SANITIZED 1
#else
#define FUZZ_SANITIZED 0
#endif

static void catch_faults(void)
{
    signal(SIGABRT, on_signal);
    signal(SIGALRM, on_signal);
#if !FUZZ_SANITIZED
    signal(SIGSEGV, on_signal);
    signal(SIGBUS,  on_signal);
    signal(SIGILL,  on_signal);
    signal(SIGFPE,  on_signal);
#endif
}

/* --- rng ----------------------------------------------------------------- */

/* SplitMix64. Seeded per case from the run seed and the case index rather than
 * carried forward, so `-c N` reproduces case N without replaying the N-1
 * before it - which is the difference between a bug report and a bug story. */
typedef struct {
    uint64_t s;
} rng;

static uint64_t rnd64(rng *r)
{
    uint64_t z = (r->s += 0x9e3779b97f4a7c15ull);

    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static uint32_t rnd_below(rng *r, uint32_t n)
{
    return n ? (uint32_t)(rnd64(r) % n) : 0u;
}

static void rng_init(rng *r, uint64_t seed, uint64_t a, uint64_t b)
{
    r->s = seed ^ (a * 0xff51afd7ed558ccdull) ^ (b * 0xc4ceb9fe1a85ec53ull);
    (void)rnd64(r);
}

/* --- the sink ------------------------------------------------------------ */

typedef struct {
    uint8_t *px;
    int      w, h;
    long     spans;
} canvas;

/* Deliberately unbounded in x and y: see the file header. The arithmetic is
 * the same blend the two real harnesses do, because a sink that discarded its
 * arguments would let a dead-code pass optimise the coverage away. */
static void put_span(void *user, int y, int x0, int x1, uint8_t cov,
                     ps_swf_rgba c)
{
    canvas  *cv = user;
    unsigned a  = (unsigned)cov * (unsigned)c.a / 255u;
    int      x;

    cv->spans++;
    for(x = x0; x < x1; x++) {
        uint8_t *p = cv->px + ((size_t)y * cv->w + x) * 3;

        p[0] = (uint8_t)((p[0] * (255u - a) + c.r * a) / 255u);
        p[1] = (uint8_t)((p[1] * (255u - a) + c.g * a) / 255u);
        p[2] = (uint8_t)((p[2] * (255u - a) + c.b * a) / 255u);
    }
}

typedef struct {
    const ps_swf_view *v;
    ps_swf_clip       *clip;
    int                tess;    /* triangles rather than spans */
    ps_trisoft        *ts;
} stagectx;

static void stage_draw(void *user, const ps_swf_shape *sh,
                       const ps_swf_xform *xf, const ps_swf_cxform *cx)
{
    stagectx *sc = user;

    if(sc->tess)
        (void)ps_swf_tess_shape(sh, sc->v, xf, cx, ps_trisoft_sink(), sc->ts);
    else
        (void)ps_swf_raster_shape(sh, sc->v, xf, cx, ps_swf_clip_span,
                                  sc->clip);
}

static void stage_begin(void *u) { ps_swf_clip_begin(((stagectx *)u)->clip); }
static void stage_apply(void *u) { ps_swf_clip_apply(((stagectx *)u)->clip); }
static void stage_end(void *u)   { ps_swf_clip_end(((stagectx *)u)->clip); }

static const ps_swf_stage_sink stage_sink = {
    stage_draw, stage_begin, stage_apply, stage_end
};

/* --- findings ------------------------------------------------------------ */

static long findings;

static void finding(const char *what)
{
    printf("FAIL %s: %s\n", g_case, what);
    fflush(stdout);
    findings++;
}

/* --- one case ------------------------------------------------------------ */

static struct {
    long cases, loaded, refused, drawn, spans;
} stats;

/* Maps a box of twips onto the canvas.
 *
 * The subtraction is in double because the two ends are int32 straight out of
 * a RECT the file controls, and a mutant that sets one to INT32_MIN and the
 * other to INT32_MAX makes the obvious int subtraction overflow - which is
 * undefined, and which would be this harness's bug reported as the library's.
 * The clamps below are the same idea for the divide. */
static void fit_box(ps_swf_xform *xf, double x0, double y0, double x1,
                    double y1, const ps_swf_view *v)
{
    double w = x1 - x0, h = y1 - y0, s, sy;

    if(!(w > 1.0)) w = 1.0;
    if(!(h > 1.0)) h = 1.0;
    s  = (double)(v->w - 2) / w;
    sy = (double)(v->h - 2) / h;
    if(sy < s)
        s = sy;
    if(!(s > 1e-9)) s = 1e-9;
    if(s > 4.0)     s = 4.0;

    ps_swf_xform_scale(xf, (float)s, (float)s, (float)(1.0 - x0 * s),
                       (float)(1.0 - y0 * s));
}

/* Both renderers over one shape, straight at the canvas with nothing clamping
 * in between. */
static void shape_both_ways(const ps_swf_shape *sh, const ps_swf_view *v,
                            canvas *cv)
{
    ps_swf_xform xf;
    ps_trisoft   ts;

    fit_box(&xf, (double)sh->xmin, (double)sh->ymin, (double)sh->xmax,
            (double)sh->ymax, v);

    (void)ps_swf_raster_shape(sh, v, &xf, nullptr, put_span, cv);

    if(ps_trisoft_init(&ts, v, put_span, cv) < 0)
        return;
    (void)ps_swf_tess_shape(sh, v, &xf, nullptr, ps_trisoft_sink(), &ts);
    ps_trisoft_free(&ts);
}

static void run_stage(const ps_swf_movie *m, const ps_swf_view *v, canvas *cv,
                      int tess)
{
    ps_swf_xform xf;
    ps_swf_clip  clip;
    ps_trisoft   ts;
    stagectx     sc = { v, &clip, tess, &ts };
    uint32_t     nf = m->root.nframe, fr;

    if(nf > MAX_FRAMES)
        nf = MAX_FRAMES;
    if(nf == 0)
        return;

    fit_box(&xf, (double)m->xmin, (double)m->ymin, (double)m->xmax,
            (double)m->ymax, v);

    if(ps_swf_clip_init(&clip, v, put_span, cv) < 0)
        return;
    if(tess && ps_trisoft_init(&ts, v, ps_swf_clip_span, &clip) < 0) {
        ps_swf_clip_free(&clip);
        return;
    }
    for(fr = 0; fr < nf; fr++) {
        long drawn = ps_swf_render_frame(m, fr, &xf, &stage_sink, &sc);

        if(drawn > 0)
            stats.drawn += drawn;
    }
    if(tess)
        ps_trisoft_free(&ts);
    ps_swf_clip_free(&clip);
}

/* Every ratio that divides differently: the two ends, the one either side of
 * the halfway point that is not representable as a half, and one that is not a
 * round fraction of anything. */
static const uint16_t morph_ratios[] = { 0, 1, 32767, 32768, 40961, 65535 };

static void run_morphs(const ps_swf_movie *m, const ps_swf_view *v, canvas *cv)
{
    uint32_t i, r;
    uint32_t n = m->nmorph > MAX_MORPHS ? MAX_MORPHS : m->nmorph;

    for(i = 0; i < n; i++)
        for(r = 0; r < sizeof morph_ratios / sizeof morph_ratios[0]; r++) {
            ps_swf_shape sh;

            if(ps_swf_morph_shape_init(&m->morphs[i], &sh) < 0)
                return;
            ps_swf_morph_at(&m->morphs[i], morph_ratios[r], &sh);
            shape_both_ways(&sh, v, cv);
            ps_swf_morph_shape_free(&sh);
        }
}

static void run_one(const uint8_t *data, size_t len)
{
    ps_swf_movie m;
    ps_swf_view  v = { CANVAS_W, CANVAS_H, 1.0f, 2 };
    canvas       cv = { nullptr, CANVAS_W, CANVAS_H, 0 };
    char         err[160] = "";
    size_t       before = ps_swf_mem_live();
    uint32_t     i, ns;

    stats.cases++;
    alarm(CASE_SECONDS);

    if(ps_swf_load(data, len, &m, err, sizeof err) < 0) {
        stats.refused++;
        /* The refusal paths all sit ahead of the first allocation today. Said
         * out loud here so that stops being true loudly. */
        if(ps_swf_mem_live() != before)
            finding("ps_swf_load held memory after refusing the file");
        alarm(0);
        return;
    }
    stats.loaded++;

    /* Heap rather than a local array so a span written outside the canvas has
     * a redzone to land in rather than another local to quietly corrupt. */
    cv.px = malloc((size_t)CANVAS_W * CANVAS_H * 3);
    if(cv.px) {
        memset(cv.px, 0x20, (size_t)CANVAS_W * CANVAS_H * 3);

        run_stage(&m, &v, &cv, 0);
        run_stage(&m, &v, &cv, 1);

        ns = m.nshape > MAX_SHAPES ? MAX_SHAPES : m.nshape;
        for(i = 0; i < ns; i++)
            shape_both_ways(&m.shapes[i], &v, &cv);

        run_morphs(&m, &v, &cv);

        stats.spans += cv.spans;
        free(cv.px);
    }

    ps_swf_free(&m);
    /* The one check LeakSanitizer structurally cannot make: memory the parser
     * never handed back but that something still points at is not a leak by
     * LSan's definition and is a leak by the only definition that matters on a
     * machine with 16MB and no swap. Measured per case rather than per run, so
     * the report names the file that did it. `before` is a fresh reading each
     * case, which is also what stops one leak being re-reported by every case
     * after it. */
    if(ps_swf_mem_live() != before) {
        char msg[96];

        snprintf(msg, sizeof msg, "ps_swf_free left %ld bytes held",
                 (long)(ps_swf_mem_live() - before));
        finding(msg);
    }
    alarm(0);
}

/* --- the corpus ---------------------------------------------------------- */

typedef struct {
    char    *name;
    uint8_t *data;
    size_t   len;
} seedfile;

/* Values that are one wrong step from a bound: a count of zero, a signed byte
 * at its extremes, an unsigned one at its own. Most of what a length field or
 * a style index has to survive is in here. */
static const uint8_t interesting8[] = {
    0x00, 0x01, 0x02, 0x03, 0x07, 0x0f, 0x10, 0x3f, 0x40, 0x7f, 0x80, 0x81,
    0xbf, 0xc0, 0xfe, 0xff
};

static const uint16_t interesting16[] = {
    0x0000, 0x0001, 0x0002, 0x000f, 0x007f, 0x0080, 0x00ff, 0x0100, 0x03ff,
    0x0400, 0x7fff, 0x8000, 0xfffe, 0xffff
};

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

/* One mutant. Between one and four edits, because a single flip usually lands
 * on a coordinate and stacked ones are what turn a tag into a different tag
 * whose body is the old one's - which is where the field-layout parsing this
 * exists to test actually gets confused.
 *
 * Length-changing edits are in the mix on purpose. A tag header states a
 * length and the file states its own, so inserting three bytes desynchronises
 * every offset after the edit, and offsets into the file are exactly what
 * DefineFont's glyph table and DefineBitsJPEG2's splice are made of. */
static size_t mutate(rng *r, const uint8_t *src, size_t n, uint8_t *dst)
{
    size_t   len = n > MUT_CAP ? MUT_CAP : n;
    unsigned edits = 1u + rnd_below(r, 4u);

    memcpy(dst, src, len);

    while(edits--) {
        uint32_t at = len ? rnd_below(r, (uint32_t)len) : 0u;

        switch(rnd_below(r, 13u)) {
        case 0:                                   /* one bit */
            if(len)
                dst[at] ^= (uint8_t)(1u << rnd_below(r, 8u));
            break;
        case 1:
            if(len)
                dst[at] = interesting8[rnd_below(r,
                              sizeof interesting8 / sizeof interesting8[0])];
            break;
        case 2:
            if(len)
                dst[at] = (uint8_t)rnd64(r);
            break;
        case 3:                                   /* a near miss on a count */
            if(len)
                dst[at] = (uint8_t)(dst[at] + ((rnd64(r) & 1u) ? 1 : 255));
            break;
        case 4:
            if(len >= 2)
                put16(dst + (at > len - 2 ? len - 2 : at),
                      interesting16[rnd_below(r,
                          sizeof interesting16 / sizeof interesting16[0])]);
            break;
        case 5: {                                 /* a run of noise */
            uint32_t k = 1u + rnd_below(r, 16u);

            while(k-- && at < len)
                dst[at++] = (uint8_t)rnd64(r);
            break;
        }
        case 6: {                                 /* move a chunk somewhere */
            uint32_t k = 1u + rnd_below(r, 32u);
            uint32_t to = len ? rnd_below(r, (uint32_t)len) : 0u;

            if(at + k > len) k = (uint32_t)len - at;
            if(to + k > len) k = (uint32_t)len - to;
            if(k)
                memmove(dst + to, dst + at, k);
            break;
        }
        case 7:                                   /* cut it short here */
            len = at;
            break;
        case 8: {                                 /* trailing junk */
            uint32_t k = 1u + rnd_below(r, 32u);

            while(k-- && len < MUT_CAP)
                dst[len++] = (uint8_t)rnd64(r);
            break;
        }
        case 9: {                                 /* delete, shifting everything */
            uint32_t k = 1u + rnd_below(r, 8u);

            if(at + k > len)
                k = (uint32_t)len - at;
            memmove(dst + at, dst + at + k, len - at - k);
            len -= k;
            break;
        }
        case 10: {                                /* insert, shifting everything */
            uint32_t k = 1u + rnd_below(r, 8u), j;

            if(len + k > MUT_CAP)
                k = (uint32_t)(MUT_CAP - len);
            memmove(dst + at + k, dst + at, len - at);
            for(j = 0; j < k; j++)
                dst[at + j] = (uint8_t)rnd64(r);
            len += k;
            break;
        }
        case 11:                                  /* the declared file length */
            if(len >= 8) {
                uint32_t v = (uint32_t)rnd64(r);

                if(rnd64(r) & 1u)
                    v = (uint32_t)len + (uint32_t)rnd_below(r, 8u) - 4u;
                dst[4] = (uint8_t)v;       dst[5] = (uint8_t)(v >> 8);
                dst[6] = (uint8_t)(v >> 16); dst[7] = (uint8_t)(v >> 24);
            }
            break;
        default:                                  /* the version byte */
            if(len >= 4)
                dst[3] = (uint8_t)rnd_below(r, 16u);
            break;
        }
        if(len > MUT_CAP)
            len = MUT_CAP;
    }
    return len;
}

/* A file with a plausible header and nothing else that means anything. The
 * header is real because a wholly random buffer is refused at byte 0 and tests
 * one comparison; past it, the tag walk is the thing being handed garbage. */
static size_t garbage(rng *r, uint8_t *dst)
{
    size_t len = 9u + rnd_below(r, 1024u);
    size_t i;

    memcpy(dst, "FWS", 3);
    dst[3] = (uint8_t)rnd_below(r, 16u);
    for(i = 4; i < len; i++)
        dst[i] = (uint8_t)rnd64(r);
    return len;
}

/* --- driving ------------------------------------------------------------- */

static int name_cmp(const void *a, const void *b)
{
    return strcmp(((const seedfile *)a)->name, ((const seedfile *)b)->name);
}

/* Where a case's bytes are built, and what to do with them once they are.
 * `only` is -1 for a whole run or the single case `-c` asked for; `dump`
 * writes that case out instead of running it, which is how a finding leaves
 * this process and becomes an input for swfrender or a debugger. */
typedef struct {
    uint8_t    *buf;
    long        idx;
    long        from;   /* -b: resume here, having run nothing before it */
    long        only;
    const char *dump;
    int         count;  /* -N: walk the space and run none of it */
    int         done;
} driver;

[[gnu::format(printf, 3, 4)]]
static void emit(driver *d, size_t n, const char *fmt, ...)
{
    va_list ap;
    int     head;

    if(d->count || (d->only >= 0 && d->idx != d->only) || d->idx < d->from) {
        d->idx++;
        return;
    }
    head = snprintf(g_case, sizeof g_case, "case %ld: ", d->idx);
    va_start(ap, fmt);
    g_case_len = head + vsnprintf(g_case + head, sizeof g_case - (size_t)head,
                                  fmt, ap);
    va_end(ap);
    if(g_case_len > (int)sizeof g_case - 1)
        g_case_len = (int)sizeof g_case - 1;
    d->idx++;

    if(d->dump) {
        FILE *f = fopen(d->dump, "wb");

        if(f) {
            fwrite(d->buf, 1, n, f);
            fclose(f);
            printf("%s -> %s (%zu bytes)\n", g_case, d->dump, n);
        }
        d->done = 1;
        return;
    }
    run_one(d->buf, n);
    if(d->only >= 0)
        d->done = 1;
}

/* Every case's number has to mean the same thing on the next run, so the case
 * space is enumerated rather than sampled: phase, then seed file in sorted
 * order, then index within the phase. Nothing here depends on the order the
 * shell handed the files over, and nothing carries RNG state from one case to
 * the next - which is what makes `-c` jump straight to a case instead of
 * replaying the ones before it. */
static long run_all(seedfile *sf, int nsf, uint64_t seed, long mutants,
                    long garbages, long from, long only, const char *dump,
                    int count)
{
    driver d = { malloc(MUT_CAP), 0, from, only, dump, count, 0 };
    int    i;
    long   k;

    if(!d.buf)
        return 0;

    /* Truncations. Every prefix of every seed, which is the one family that
     * has to be exhaustive rather than sampled: a read one byte past a length
     * is invisible unless the byte after the length is the byte after the
     * allocation, and only the exact prefix puts it there. */
    for(i = 0; i < nsf && !d.done; i++) {
        size_t t;

        for(t = 0; t <= sf[i].len && !d.done; t++) {
            if(t > TRUNC_FULL && t != sf[i].len && (t % TRUNC_STRIDE) != 0)
                continue;
            memcpy(d.buf, sf[i].data, t);
            emit(&d, t, "%s truncated to %zu", sf[i].name, t);
        }
    }

    for(i = 0; i < nsf && !d.done; i++)
        for(k = 0; k < mutants && !d.done; k++) {
            rng    r;
            size_t n;

            rng_init(&r, seed, (uint64_t)i + 1u, (uint64_t)k);
            n = mutate(&r, sf[i].data, sf[i].len, d.buf);
            emit(&d, n, "%s mutant %ld (%zu bytes)", sf[i].name, k, n);
        }

    for(k = 0; k < garbages && !d.done; k++) {
        rng    r;
        size_t n;

        rng_init(&r, seed, 0xffffu, (uint64_t)k);
        n = garbage(&r, d.buf);
        emit(&d, n, "garbage %ld (%zu bytes)", k, n);
    }

    free(d.buf);
    return d.idx;
}

static int read_seed(const char *path, seedfile *out)
{
    FILE *f = fopen(path, "rb");
    long  n;

    if(!f) {
        fprintf(stderr, "swffuzz: cannot open %s\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if(n < 0 || n > (long)MUT_CAP) {
        fclose(f);
        fprintf(stderr, "swffuzz: %s is not a usable seed\n", path);
        return -1;
    }
    out->data = malloc((size_t)n ? (size_t)n : 1u);
    if(!out->data || fread(out->data, 1, (size_t)n, f) != (size_t)n) {
        free(out->data);
        fclose(f);
        return -1;
    }
    fclose(f);
    out->len = (size_t)n;
    out->name = strdup(path);
    return out->name ? 0 : -1;
}

/* Deliberately leaks, so the Makefile can prove LeakSanitizer is switched on
 * rather than assume it. It is a subcommand and not a comment because the
 * assumption it replaces is the kind that stays true right up until the
 * platform changes underneath it.
 *
 * Sixty-four blocks and not one, which is not superstition. LSan reports a
 * block only when no word anywhere - registers, globals, live stack, TLS -
 * still holds its address, and a single fresh allocation leaves its pointer in
 * a callee-saved register or a stack slot nothing has overwritten yet, so it
 * is found reachable and never reported. The first version of this check leaked
 * one block, exited 0, and would have certified a LeakSanitizer that was
 * working perfectly - which is the failure mode it exists to rule out, arriving
 * through the check itself.
 *
 * Through ps_swf_alloc rather than malloc: that is the allocator every leak
 * this suite could plausibly find comes from, and a check that proved malloc
 * was watched while the parser's own wrapper was not would prove the wrong
 * thing. */
[[gnu::noinline]] static void leak_some(int n)
{
    int i;

    for(i = 0; i < n; i++) {
        void *p = ps_swf_alloc(1024);

        if(p)
            memset(p, 0xab, 1024);
    }
}

static int leakcheck(void)
{
    leak_some(64);
    printf("swffuzz: leaked 64 KiB on purpose; a live LeakSanitizer reports"
           " it and exits non-zero.\n");
    return 0;
}

int main(int argc, char **argv)
{
    seedfile       *sf;
    int             nsf = 0, i;
    uint64_t        seed = 20260807u;
    long            mutants = 600, garbages = 2000, from = 0, only = -1;
    const char     *dump = nullptr;
    struct timespec t0, t1;
    double          secs;
    long            space;
    int             count = 0;

    if(argc > 1 && !strcmp(argv[1], "leakcheck"))
        return leakcheck();

    sf = calloc((size_t)argc, sizeof *sf);
    if(!sf)
        return 1;

    for(i = 1; i < argc; i++) {
        if(!strcmp(argv[i], "-s") && i + 1 < argc)
            seed = strtoull(argv[++i], nullptr, 0);
        else if(!strcmp(argv[i], "-n") && i + 1 < argc)
            mutants = strtol(argv[++i], nullptr, 0);
        else if(!strcmp(argv[i], "-g") && i + 1 < argc)
            garbages = strtol(argv[++i], nullptr, 0);
        else if(!strcmp(argv[i], "-b") && i + 1 < argc)
            from = strtol(argv[++i], nullptr, 0);
        else if(!strcmp(argv[i], "-N"))
            count = 1;
        else if(!strcmp(argv[i], "-c") && i + 1 < argc)
            only = strtol(argv[++i], nullptr, 0);
        else if(!strcmp(argv[i], "-d") && i + 1 < argc)
            dump = argv[++i];
        else if(argv[i][0] == '-') {
            fprintf(stderr, "usage: %s [-s seed] [-n mutants] [-g garbage]"
                            " [-b first case] [-c case] [-d out.swf] [-N]"
                            " seed.swf...\n", argv[0]);
            return 2;
        } else if(read_seed(argv[i], &sf[nsf]) == 0) {
            nsf++;
        } else {
            return 1;
        }
    }
    if(nsf == 0) {
        fprintf(stderr, "swffuzz: no seed files; run mkswf first\n");
        return 2;
    }
    if(dump && only < 0) {
        fprintf(stderr, "swffuzz: -d needs -c to say which case to write\n");
        return 2;
    }
    qsort(sf, (size_t)nsf, sizeof *sf, name_cmp);
    catch_faults();

    if(only < 0 && !count)
        printf("swffuzz: %d seeds, seed %llu, %ld mutants each, %ld garbage\n",
               nsf, (unsigned long long)seed, mutants, garbages);

    clock_gettime(CLOCK_MONOTONIC, &t0);
    space = run_all(sf, nsf, seed, mutants, garbages, from, only, dump, count);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    secs = (double)(t1.tv_sec - t0.tv_sec) +
           (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

    for(i = 0; i < nsf; i++) {
        free(sf[i].data);
        free(sf[i].name);
    }
    free(sf);

    if(dump)
        return 0;
    /* The size of the case space, which is not the same number as the cases a
     * run got through: a run that stops at a fault reports neither. fuzz.sh
     * needs this one to say how much of the space a resumed sweep covered. */
    if(count) {
        printf("%ld\n", space);
        return 0;
    }

    /* The case count is printed whatever happens, because "no findings" from a
     * hundred inputs and from a hundred thousand are different claims and the
     * sentence alone cannot tell them apart. */
    printf("swffuzz: %ld cases in %.1fs - %ld parsed, %ld refused,"
           " %ld characters drawn, %ld spans\n",
           stats.cases, secs, stats.loaded, stats.refused, stats.drawn,
           stats.spans);
    printf(findings ? "swffuzz: %ld finding(s)\n" : "swffuzz: clean\n",
           findings);
    return findings ? 1 : 0;
}
