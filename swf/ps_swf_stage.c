/* The display list: turning a frame number into things to draw.
 *
 * A SWF frame is not a picture, it is a diff. PlaceObject says "put character
 * 7 at depth 12 with this matrix"; the next frame may say only "the thing at
 * depth 12 now has this other matrix" and everything else about it - which
 * character, what colour transform - carries over. So there is no way to draw
 * frame 40 without knowing what frames 0 to 39 did, and that shapes this file:
 * it replays.
 *
 * Replaying from the start on every call is O(frames) and deliberate. A host
 * tool asks for one frame, or dumps all of them in order; a player that cares
 * keeps its own state and steps. The property bought by paying that cost is
 * that rendering frame 40 has no dependence on what was rendered before, which
 * is what makes a wrong frame reproducible on its own instead of only as the
 * fortieth in a sequence.
 *
 * Depth is the z order and it is absolute, not per-timeline: a sprite's
 * contents sort within the sprite, and the sprite as a whole sits at its own
 * depth in the parent. That is why the recursion draws a sprite's display list
 * completely before returning rather than merging it into the parent's.
 *
 * Clip depth rides on the same ordering. A character placed with one is not
 * drawn; it confines what the characters after it in depth order may paint,
 * up to and including the depth it names, and then stops mattering. Since the
 * list is walked in ascending depth, that is a stack: push when the mask is
 * reached, pop when the depth passes what it named.
 *
 * A stack is a decision and not merely an implementation. Two masks whose depth
 * ranges overlap without nesting - 5 to 10 and 7 to 20 - are not a stack, and
 * this treats them as one, so the outer mask stays in force until the inner one
 * ends. Ruffle's renderer arrives at the same behaviour from the same
 * construction. The alternative is to keep every live mask separately so any of
 * them can be dropped in any order, which means storage per mask rather than
 * storage for the current intersection - and per-mask storage is exactly what
 * the interface exists to avoid asking a backend for. Flash exporters emit
 * nested ranges; interleaved ones are pathological.
 */
#include "ps_swf.h"
#include "ps_swf_edit.h"
#include "ps_swf_mem.h"
#include "ps_swf_morph.h"

#include <string.h>

/* One occupied depth. Held in an array kept sorted by depth rather than
 * indexed by it: depth is a sixteen-bit field, so indexing would mean 65536
 * entries at fifty-odd bytes - nearly four megabytes, per nesting level, on a
 * machine with sixteen in total. What a file actually occupies is bounded by
 * how many placements it contains, which the sample puts at eighty-eight for
 * the whole movie, so the sparse form is four orders of magnitude smaller and
 * the binary search costs seven comparisons at that size. */
typedef struct {
    uint16_t      depth;
    uint16_t      id;
    uint16_t      clip_depth;
    uint16_t      ratio;
    ps_swf_xform  mat;
    ps_swf_cxform cx;
    /* The tick this instance appeared on, counted in its parent's ticks and
     * never wrapped. A sprite has its own playhead which starts when it is
     * placed, so a sprite placed on tick 30 is showing its own frame 0 at that
     * moment and its own frame 5 five ticks later.
     *
     * This is the whole of a child's playhead, and it is one word per occupied
     * depth rather than per character - which is what makes per-instance state
     * affordable here. A depth-indexed display list was rejected in this file
     * for costing 3.7MB a nesting level; this is bounded by the placements the
     * timeline actually contains, the same ceiling `list` is already sized to. */
    uint32_t      born;
} slot;

typedef struct {
    const ps_swf_movie      *m;
    const ps_swf_stage_sink *sink;
    void                    *user;
    long                     drawn;
    int                      failed;

    /* The last depth each live mask confines, innermost last. Shared across the
     * whole walk rather than per timeline, even though clip depth is a
     * per-timeline number, because the limit it is checked against is the
     * backend's nesting capacity - and a sprite drawn inside a mask is nested
     * in the backend whether or not its own timeline knows it. */
    uint16_t                 clip_until[PS_SWF_CLIP_DEPTH];
    int                      nclip;
} stage;

static void run_timeline(stage *st, const ps_swf_timeline *tl, uint32_t frame,
                         const ps_swf_xform *parent,
                         const ps_swf_cxform *pcx, int depth);
static void draw_text(stage *st, const ps_swf_text *t,
                      const ps_swf_xform *parent, const ps_swf_cxform *pcx);
static void draw_button(stage *st, const ps_swf_button *b,
                        const ps_swf_xform *parent, const ps_swf_cxform *pcx,
                        int depth);

/* Index of `depth` in a sorted list, or where it would be inserted. The caller
 * distinguishes the two by comparing the depth at the returned index. */
static uint32_t slot_lower(const slot *list, uint32_t n, uint16_t depth)
{
    uint32_t lo = 0, hi = n;

    while(lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;

        if(list[mid].depth < depth)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

/* A glyph outline is filled opaque white, so painting it in the text colour is
 * a colour transform that multiplies by that colour - which is exactly how
 * Flash delivers it, and why the glyph shape can be shared by every piece of
 * text that uses it regardless of colour.
 *
 * The multiplier is 8.8, so full scale is 256 while a colour channel tops out
 * at 255, and the two do not divide evenly. Rounding to nearest is the obvious
 * choice and is wrong: the transform truncates, so a multiplier that is
 * nearest to the ideal ratio lands a channel one below the colour asked for on
 * about half of all values. Rounding up instead - the ceiling of c*256/255 -
 * is exact for every c from 0 to 255, which matters because text is the one
 * place a colour arrives through a multiply rather than as itself. */
static void text_paint(ps_swf_rgba c, ps_swf_cxform *out)
{
    int      i;
    unsigned v[4];

    v[0] = c.r; v[1] = c.g; v[2] = c.b; v[3] = c.a;
    for(i = 0; i < 4; i++) {
        out->mult[i] = (int16_t)((v[i] * 256u + 254u) / 255u);
        out->add[i]  = 0;
    }
}

/* A run of positioned glyphs, in whatever space `tm` lands them in. Both a
 * DefineText and a DefineEditText reduce to exactly this - the difference
 * between them is entirely in how the positions were arrived at, and that
 * happened at parse time. */
static void draw_glyphs(stage *st, const ps_swf_glyphref *glyphs,
                        uint32_t nglyph, const ps_swf_xform *tm,
                        const ps_swf_cxform *pcx)
{
    uint32_t i;

    for(i = 0; i < nglyph; i++) {
        const ps_swf_glyphref *g = &glyphs[i];
        const ps_swf_font     *f = ps_swf_find_font(st->m, g->font_id);
        ps_swf_xform           gm, gx;
        ps_swf_cxform          col, cx;
        float                  sc;

        if(!f || g->glyph >= f->nglyph)
            continue;

        /* Glyphs live on a 1024-unit em square whatever the original font was,
         * so the height in twips divided by that is the scale, and the pen
         * position is already in twips and is not scaled by it. */
        sc = (float)g->height / (float)PS_SWF_EM;
        ps_swf_xform_scale(&gm, sc, sc, (float)g->x, (float)g->y);
        ps_swf_xform_mul(&gx, tm, &gm);

        text_paint(g->color, &col);
        ps_swf_cxform_mul(&cx, pcx, &col);

        st->sink->draw(st->user, &f->glyphs[g->glyph], &gx, &cx);
        st->drawn++;
    }
}

static void draw_text(stage *st, const ps_swf_text *t,
                      const ps_swf_xform *parent, const ps_swf_cxform *pcx)
{
    ps_swf_xform tm;

    ps_swf_xform_mul(&tm, parent, &t->mat);
    draw_glyphs(st, t->glyphs, t->nglyph, &tm, pcx);
}

/* A morph blended at the instance's own ratio, which is the field PlaceObject2
 * carries and the display list already keeps. The blend allocates because its
 * result is a whole shape and the movie it came from is const - a scratch
 * shape hung off the character would be state shared by every instance of it,
 * and two instances of one morph at two ratios in one frame is the ordinary
 * case rather than the exotic one.
 *
 * The blend goes out through the same `draw` an ordinary shape uses, which is
 * what lets a morph be a mask: the caller has already opened the clip if this
 * placement is one, and nothing here needs to know which side of that it is
 * on. */
static void draw_morph(stage *st, const ps_swf_morph *mo, uint16_t ratio,
                       const ps_swf_xform *xf, const ps_swf_cxform *cx)
{
    ps_swf_shape sh;

    if(ps_swf_morph_shape_init(mo, &sh) < 0) {
        st->failed = 1;
        return;
    }
    ps_swf_morph_at(mo, ratio, &sh);
    st->sink->draw(st->user, &sh, xf, cx);
    st->drawn++;
    ps_swf_morph_shape_free(&sh);
}

static void draw_button(stage *st, const ps_swf_button *b,
                        const ps_swf_xform *parent, const ps_swf_cxform *pcx,
                        int depth)
{
    uint32_t i;

    /* Already sorted by depth at parse time, and already filtered to the up
     * state - which is the only state a renderer can know it is in, since the
     * others are functions of where a pointer that does not exist here is. */
    for(i = 0; i < b->nrec; i++) {
        const ps_swf_btnrec *r  = &b->recs[i];
        const ps_swf_char   *ch = ps_swf_find_char(st->m, r->id);
        ps_swf_xform         xf;
        ps_swf_cxform        cx;

        if(!ch)
            continue;
        ps_swf_xform_mul(&xf, parent, &r->mat);
        ps_swf_cxform_mul(&cx, pcx, &r->cx);

        if(ch->kind == PS_SWF_CHAR_SHAPE && ch->index < st->m->nshape) {
            st->sink->draw(st->user, &st->m->shapes[ch->index], &xf, &cx);
            st->drawn++;
        } else if(ch->kind == PS_SWF_CHAR_TEXT && ch->index < st->m->ntext) {
            draw_text(st, &st->m->texts[ch->index], &xf, &cx);
        } else if(ch->kind == PS_SWF_CHAR_SPRITE &&
                  ch->index < st->m->nsprite) {
            run_timeline(st, &st->m->sprites[ch->index], 0, &xf, &cx,
                         depth + 1);
        } else if(ch->kind == PS_SWF_CHAR_EDIT && ch->index < st->m->nedit) {
            draw_glyphs(st, st->m->edits[ch->index].glyphs,
                        st->m->edits[ch->index].nglyph, &xf, &cx);
        } else if(ch->kind == PS_SWF_CHAR_MORPH && ch->index < st->m->nmorph) {
            /* Ratio zero, and there is no other answer available: a button
             * record carries a matrix and a colour transform and nothing
             * else, so a morph on a button face sits at the start of its
             * blend until ActionScript moves it. */
            draw_morph(st, &st->m->morphs[ch->index], 0, &xf, &cx);
        }
    }
}

static void draw_slot(stage *st, const slot *s, uint32_t local_frame,
                      const ps_swf_xform *parent, const ps_swf_cxform *pcx,
                      int depth)
{
    const ps_swf_char *ch = ps_swf_find_char(st->m, s->id);
    ps_swf_xform       xf;
    ps_swf_cxform      cx;

    if(!ch)
        return;

    /* The instance's own transform sits inside everything above it, so the
     * parent is the outer term. Same for the colour transform: a sprite dimmed
     * to half, holding a shape dimmed to half, draws at a quarter. */
    ps_swf_xform_mul(&xf, parent, &s->mat);
    ps_swf_cxform_mul(&cx, pcx, &s->cx);

    if(ch->kind == PS_SWF_CHAR_SHAPE && ch->index < st->m->nshape) {
        st->sink->draw(st->user, &st->m->shapes[ch->index], &xf, &cx);
        st->drawn++;
    } else if(ch->kind == PS_SWF_CHAR_SPRITE && ch->index < st->m->nsprite) {
        run_timeline(st, &st->m->sprites[ch->index], local_frame,
                     &xf, &cx, depth + 1);
    } else if(ch->kind == PS_SWF_CHAR_TEXT && ch->index < st->m->ntext) {
        draw_text(st, &st->m->texts[ch->index], &xf, &cx);
    } else if(ch->kind == PS_SWF_CHAR_BUTTON && ch->index < st->m->nbutton) {
        draw_button(st, &st->m->buttons[ch->index], &xf, &cx, depth);
    } else if(ch->kind == PS_SWF_CHAR_MORPH && ch->index < st->m->nmorph) {
        draw_morph(st, &st->m->morphs[ch->index], s->ratio, &xf, &cx);
    } else if(ch->kind == PS_SWF_CHAR_EDIT && ch->index < st->m->nedit) {
        /* No matrix of its own: a field's glyphs are already positioned in the
         * character's space, because the bounds they were laid out inside are
         * in that space too. */
        draw_glyphs(st, st->m->edits[ch->index].glyphs,
                    st->m->edits[ch->index].nglyph, &xf, &cx);
    }
}

/* Does this slot name a character this build resolved?
 *
 * Not the same question as whether it has anything to paint. An empty sprite
 * resolves and covers nothing; a DefineShape4 does not resolve at all, because
 * the tag is SWF 8 and the parser declines it by name. The two look identical
 * at draw time - neither emits a span - and they want opposite treatment when
 * the slot is a mask, which is why the test is on the character table rather
 * than on what came out of the renderer. */
static int slot_resolves(const stage *st, const slot *s)
{
    const ps_swf_char *ch = ps_swf_find_char(st->m, s->id);

    if(!ch)
        return 0;
    switch(ch->kind) {
    case PS_SWF_CHAR_SHAPE:  return ch->index < st->m->nshape;
    case PS_SWF_CHAR_SPRITE: return ch->index < st->m->nsprite;
    case PS_SWF_CHAR_TEXT:   return ch->index < st->m->ntext;
    case PS_SWF_CHAR_BUTTON: return ch->index < st->m->nbutton;
    case PS_SWF_CHAR_MORPH:  return ch->index < st->m->nmorph;
    case PS_SWF_CHAR_EDIT:   return ch->index < st->m->nedit;
    /* A bitmap is not placeable - it reaches the stage through a fill style -
     * so a placement naming one is a file we did not understand rather than a
     * character with no geometry. */
    default:                 return 0;
    }
}

/* Is this slot a mask, and can this backend be told so?
 *
 * A clip depth at or below the placement's own depth names an empty range. The
 * character is still a mask and is still not drawn - that is a property of the
 * placement, not of how many characters happen to fall in the range - so this
 * says "not a mask I can act on" and the caller drops it either way. Same for a
 * backend with no clip calls and for nesting past what the backend holds: in
 * every one of those cases the mask is skipped rather than painted, because
 * painting it puts the mask's own artwork on the stage, which is a worse
 * picture than leaving what it should have hidden visible.
 *
 * slot_resolves is the fourth case and it is a different argument. An empty
 * mask hides everything in its range, which is the right reading of a file that
 * says "confine this to nothing" and the safe way round when the file is
 * broken, since the alternative reveals what the author hid. But a character
 * this build could not resolve is not the file saying anything - it is us not
 * having read the tag, and an empty mask then blanks every depth up to the clip
 * depth. Ruffle's visual/simple_shapes/masks is that exact case: one clip depth
 * of 11 over a DefineShape4, four characters submitted, and a uniformly white
 * frame. A picture missing one element beats no picture at all, so a mask we
 * could not build is treated as a mask that is not there. */
static int clip_usable(const stage *st, const slot *s)
{
    return s->clip_depth > s->depth && slot_resolves(st, s) &&
           st->sink->clip_begin && st->nclip < PS_SWF_CLIP_DEPTH;
}

static void run_timeline(stage *st, const ps_swf_timeline *tl, uint32_t frame,
                         const ps_swf_xform *parent,
                         const ps_swf_cxform *pcx, int depth)
{
    slot    *list;
    uint32_t n = 0, f, last, i;
    int      clip_base = st->nclip;

    if(st->failed || depth > 8 || tl->nframe == 0 || tl->nop == 0)
        return;

    /* A place op can occupy at most one depth, so the op count is an exact
     * ceiling on how many depths this timeline can ever have live at once. No
     * growth policy and no allocation sized by anything the file merely
     * claims. */
    list = ps_swf_alloc((size_t)tl->nop * sizeof *list);
    if(!list) {
        st->failed = 1;
        return;
    }

    /* `frame` is a tick count and not a frame index: how many times this
     * timeline has been advanced since it started, never wrapped. Which frame
     * that lands on is this, and the distinction is the whole of the nesting
     * behaviour - a child is handed `frame - born`, so it goes on counting
     * through its parent's loop instead of being dragged back by it.
     *
     * Past the end, a timeline loops rather than sitting on its last frame.
     * The spec says nothing at all about this - it never states which frame a
     * sprite shows when placed, how its playhead relates to its parent's, or
     * what happens at the end - so the behaviour here is Flash's as
     * implemented by Ruffle: one tick per parent tick, and an implicit goto
     * frame 1 at the end unless the clip has only one frame. A two-frame
     * parent driving a three-frame child therefore shows child frame 2 on tick
     * 2, where deriving the child from the parent's wrapped frame showed 0.
     *
     * The simplification is that this loops by replaying from frame 0, where
     * Flash reconciles the display list across the loop boundary and keeps
     * characters that a Move tag carries through it. The two agree except for
     * content that persists across the wrap; making them agree there means
     * modelling the goto properly, which is a player's job rather than a
     * renderer's. */
    if(tl->nframe > 1)
        last = frame % tl->nframe;
    else
        last = 0;

    for(f = 0; f <= last; f++) {
        const ps_swf_frame *fr = &tl->frames[f];
        uint32_t            k;

        for(k = 0; k < fr->nop; k++) {
            const ps_swf_op *op;
            uint32_t         at;
            int              found;
            slot            *s;

            if(fr->first_op + k >= tl->nop)
                break;
            op    = &tl->ops[fr->first_op + k];
            at    = slot_lower(list, n, op->depth);
            found = (at < n && list[at].depth == op->depth);

            if(op->op == PS_SWF_OP_REMOVE) {
                if(found) {
                    memmove(list + at, list + at + 1,
                            (size_t)(n - at - 1) * sizeof *list);
                    n--;
                }
                continue;
            }

            if(!found) {
                if(n >= tl->nop)
                    continue;                  /* cannot happen; cheap to say */
                memmove(list + at + 1, list + at,
                        (size_t)(n - at) * sizeof *list);
                n++;
                memset(&list[at], 0, sizeof list[at]);
                list[at].depth = op->depth;
                ps_swf_xform_identity(&list[at].mat);
                ps_swf_cxform_identity(&list[at].cx);
                /* `f` is a local frame index and `born` is an absolute tick,
                 * and they are the same number for the only pass replayed:
                 * this walks 0..last once, so an instance placed at frame f
                 * first appeared on tick f and has persisted since. That is
                 * the "replay rather than reconcile" simplification above,
                 * seen from the playhead's side. */
                list[at].born = f;
            }
            s = &list[at];

            /* Three cases the format distinguishes by Move and HasCharacter
             * together, and they are not interchangeable:
             *
             *   move=0, id set   place a new character here, from defaults
             *   move=1, id unset edit what is here, keeping everything else
             *   move=1, id set   swap the character, keep the slot
             *
             * The middle one is why a move must not reset: a tag carrying only
             * a matrix has to keep the character and the colour transform that
             * were already at this depth. The first is why a place must reset:
             * it would otherwise inherit whatever the previous occupant left.
             *
             * move=0 with no character is undefined in the spec. It lands here
             * as a slot with no character, which draws nothing - a silent
             * no-op rather than a rejected file, since the rest of the frame
             * is still worth showing. */
            if(found && !op->move) {
                uint16_t keep = s->depth;

                memset(s, 0, sizeof *s);
                s->depth = keep;
                ps_swf_xform_identity(&s->mat);
                ps_swf_cxform_identity(&s->cx);
                s->born = f;
            }
            if(op->id) {
                /* A swap is a new instance, so a sprite's playhead restarts.
                 * Without this a sprite swapped in on frame 60 would begin
                 * sixty frames into its own animation. */
                if(s->id != op->id)
                    s->born = f;
                s->id = op->id;
            }
            if(op->has_matrix)
                s->mat = op->mat;
            if(op->has_cxform)
                s->cx = op->cx;
            if(op->clip_depth)
                s->clip_depth = op->clip_depth;
            s->ratio = op->ratio;
        }
    }

    /* The list is already in ascending depth, which is back to front - the
     * order the format defines and the one a painter's algorithm needs. */
    for(i = 0; i < n; i++) {
        /* Only this timeline's own masks are popped, and only from the top.
         * A mask pushed by a parent timeline confines this whole sprite and
         * has nothing to do with the depths in here. */
        while(st->nclip > clip_base &&
              st->clip_until[st->nclip - 1] < list[i].depth) {
            st->nclip--;
            st->sink->clip_end(st->user);
        }

        if(list[i].clip_depth) {
            if(!clip_usable(st, &list[i]))
                continue;
            st->sink->clip_begin(st->user);
            /* The mask's own artwork, delivered through the same call an
             * ordinary character uses. Where it lands is the backend's
             * business: the host writes coverage into a buffer and the PVR
             * puts the triangles in the modifier volume list, and neither
             * needed a second way of describing a shape to do it.
             *
             * The character resolves - clip_usable has already established
             * that - so a mask that comes out empty here is a file saying so,
             * and hiding its whole range is the reading the format asks for. */
            draw_slot(st, &list[i], frame - list[i].born, parent, pcx, depth);
            st->sink->clip_apply(st->user);
            st->clip_until[st->nclip++] = list[i].clip_depth;
            continue;
        }
        draw_slot(st, &list[i], frame - list[i].born, parent, pcx, depth);
    }

    while(st->nclip > clip_base) {
        st->nclip--;
        st->sink->clip_end(st->user);
    }

    ps_swf_dealloc(list);
}

long ps_swf_render_frame(const ps_swf_movie *m, uint32_t frame,
                         const ps_swf_xform *root,
                         const ps_swf_stage_sink *sink, void *user)
{
    stage         st;
    ps_swf_cxform id;

    memset(&st, 0, sizeof st);
    st.m    = m;
    st.sink = sink;
    st.user = user;
    ps_swf_cxform_identity(&id);

    run_timeline(&st, &m->root, frame, root, &id, 0);
    return st.failed ? -1 : st.drawn;
}
