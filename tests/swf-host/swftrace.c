/* Runs a .swf's ActionScript and prints what it traced.
 *
 * This exists for one reason: Ruffle's regression suite ships an expected
 * trace next to most of its test files, and a file with an expected output is
 * not input any more, it is an oracle. Every other test in this directory
 * asserts against a number this project worked out itself, which is the right
 * discipline and also its blind spot - a misreading shared by the generator
 * and the reader cancels out. An answer computed by somebody else's player,
 * from a file built by somebody else's compiler, cannot cancel out.
 *
 *   ./swftrace file.swf [-s] [-v]
 *
 * Stdout is the trace text and nothing else, one line per Trace action, so it
 * diffs directly against Ruffle's output.txt. Everything diagnostic goes to
 * stderr: -v adds a line per block saying how it ended and what it skipped,
 * and the final stderr line is always a summary the corpus runner parses.
 *
 * --- what "running the movie" means here -----------------------------------
 *
 * A player runs a frame's script when its playhead reaches that frame. There
 * is no playhead here - ps_swf_stage.c replays rather than steps, and nothing
 * in this directory owns movie state - so this does the one pass that needs no
 * playhead: every DoInitAction in file order, then every root DoAction in
 * frame order. That is exactly right for a file that plays straight through
 * once, which the SWF 4 action tests are, and it is wrong the moment a script
 * calls GotoFrame and expects the frames to be re-run.
 *
 * So a mismatch against Ruffle is three different things and the corpus runner
 * has to be told which: an action we get wrong, an action we do not implement
 * (the summary counts those separately, with the first opcode), or a control
 * flow this harness does not model. Sprite scripts are the clearest case of
 * the third and are off by default; -s runs them after the root, in file
 * order, which is a guess and is labelled as one.
 *
 * The variable store is the host's, as ps_swf_action.h requires. It is flat:
 * a slash path is a key like any other, so "/box:count" and "count" are two
 * variables rather than one variable on two timelines. A real player resolves
 * the path against the display list; doing that here would mean modelling
 * instance names, which the parser discards.
 */
#include "ps_swf.h"
#include "ps_swf_action.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VARS 256
#define NAME 64
#define TEXT 256

typedef struct {
    char name[VARS][NAME];
    char val[VARS][TEXT];
    int  nvar;
    int  verbose;
} store;

static store S;

static int findvar(const char *name)
{
    int i;

    for(i = 0; i < S.nvar; i++)
        if(strcmp(S.name[i], name) == 0)
            return i;
    return -1;
}

static bool h_get_var(void *u, const char *name, char *out, size_t outlen)
{
    int i = findvar(name);

    (void)u;
    if(i < 0)
        return false;
    snprintf(out, outlen, "%s", S.val[i]);
    return true;
}

static void h_set_var(void *u, const char *name, const char *value)
{
    int i = findvar(name);

    (void)u;
    if(i < 0) {
        if(S.nvar >= VARS)
            return;
        i = S.nvar++;
        snprintf(S.name[i], NAME, "%s", name);
    }
    snprintf(S.val[i], TEXT, "%s", value);
}

/* Trace is the whole point of the file, so it is the one callback that writes
 * to stdout. Flushed per line because the corpus runner reads this through a
 * pipe alongside stderr and an interleaved buffer would make a diff lie. */
static void h_trace(void *u, const char *text)
{
    (void)u;
    printf("%s\n", text);
    fflush(stdout);
}

static void h_goto(void *u, int f)          { (void)u; if(S.verbose) fprintf(stderr, "  goto %d\n", f); }
static void h_step(void *u, int d)          { (void)u; if(S.verbose) fprintf(stderr, "  step %d\n", d); }
static void h_play(void *u, bool p)         { (void)u; if(S.verbose) fprintf(stderr, "  play %d\n", p); }
static void h_target(void *u, const char *t){ (void)u; if(S.verbose) fprintf(stderr, "  target %s\n", t); }

static void h_url(void *u, const char *url, const char *target, unsigned flags)
{
    (void)u;
    if(S.verbose)
        fprintf(stderr, "  url %s -> %s (%u)\n", url, target, flags);
}

/* Absent rather than faked. A property read that succeeds with a made-up
 * number turns a divergence we would have seen into one we would not, and the
 * VM's answer for an absent property - undefined, which reads as 0 and as ""
 * - is at least a documented one. */
static const ps_swf_host HOST = {
    .get_var = h_get_var, .set_var = h_set_var,
    .goto_frame = h_goto, .step_frame = h_step, .set_play = h_play,
    .set_target = h_target, .get_url = h_url, .trace = h_trace,
};

static ps_swf_vm VM;

typedef struct {
    int      blocks;
    int      faulted;
    uint32_t skipped;
    int      first_skipped;     /* -1 until an unknown opcode is seen */
    const char *first_status;   /* the first non-OK status, or NULL */
} tally;

static void run_block(const ps_swf_actions *a, const char *what, tally *t)
{
    ps_swf_action_result r;
    bool                 ok;

    if(!a->code || !a->len)
        return;
    ps_swf_action_init(&VM, &HOST);
    ok = ps_swf_action_run(&VM, a->code, a->len, &r);
    t->blocks++;
    t->skipped += r.skipped;
    if(r.skipped && t->first_skipped < 0)
        t->first_skipped = r.first_skipped;
    if(!ok) {
        t->faulted++;
        if(!t->first_status)
            t->first_status = ps_swf_action_status_name(r.status);
    }
    if(S.verbose)
        fprintf(stderr, "  [%s] %s at %zu, %u steps, %u skipped\n", what,
                ps_swf_action_status_name(r.status), r.pc, r.steps, r.skipped);
}

int main(int argc, char **argv)
{
    const char  *path = NULL;
    int          sprites = 0, i;
    FILE        *f;
    long         len;
    uint8_t     *buf;
    ps_swf_movie m;
    char         err[192] = "";
    tally        t = { 0, 0, 0, -1, NULL };
    uint32_t     fr, k;

    for(i = 1; i < argc; i++) {
        if(!strcmp(argv[i], "-s"))      sprites = 1;
        else if(!strcmp(argv[i], "-v")) S.verbose = 1;
        else if(argv[i][0] != '-')      path = argv[i];
    }
    if(!path) {
        fprintf(stderr, "usage: %s <file.swf> [-s] [-v]\n", argv[0]);
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
    buf = len > 0 ? malloc((size_t)len) : NULL;
    if(!buf || fread(buf, 1, (size_t)len, f) != (size_t)len) {
        fprintf(stderr, "read failed\n");
        fclose(f);
        free(buf);
        return 1;
    }
    fclose(f);

    if(ps_swf_load(buf, (size_t)len, &m, err, sizeof err) < 0) {
        fprintf(stderr, "refused: %s\n", err);
        fprintf(stderr, "summary refused\n");
        free(buf);
        return 1;
    }
    free(buf);

    for(i = 0; i < (int)m.ninit; i++)
        run_block(&m.inits[i], "init", &t);

    /* Frame order rather than file order: a DoAction's frame number is where
     * it runs, and the parser has already numbered them against this
     * timeline's own ShowFrames. */
    for(fr = 0; fr < m.root.nframe; fr++)
        for(k = 0; k < m.root.nact; k++)
            if(m.root.acts[k].frame == (int32_t)fr)
                run_block(&m.root.acts[k], "root", &t);

    if(sprites)
        for(i = 0; i < (int)m.nsprite; i++)
            for(k = 0; k < m.sprites[i].nact; k++)
                run_block(&m.sprites[i].acts[k], "sprite", &t);

    fprintf(stderr, "summary parsed version=%d blocks=%d faulted=%d"
            " skipped=%u first_skipped=%d status=%s note=%s\n",
            m.version, t.blocks, t.faulted, t.skipped, t.first_skipped,
            t.first_status ? t.first_status : "-", err[0] ? err : "-");
    ps_swf_free(&m);
    return 0;
}
