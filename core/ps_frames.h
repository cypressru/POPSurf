/* Frameset geometry.
 *
 * C++ internal header, included only by ps_document.cpp.
 *
 * litehtml knows the tag names and nothing else, so a frameset page renders as
 * an empty document. Frames were everywhere on the web these browsers were
 * built for, and a site whose whole navigation is a left frame is unusable
 * without them.
 *
 * This half is pure geometry: walk the frameset tree and turn it into a flat
 * list of rectangles with URLs. Each frame becomes its own document, which is
 * what the rest of ps_document does with the result.
 */
#ifndef PS_FRAMES_H
#define PS_FRAMES_H

#include <litehtml.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "ps_types.h"

namespace psfrm {

/* A frameset page can nest arbitrarily; this is the ceiling on how deep we
 * follow it before treating the rest as leaves. Real pages use two levels. */
#define PS_FRAME_MAX_DEPTH 8
#define PS_FRAME_MAX       16

struct frame_def {
    ps_rect     rect;
    std::string src;
    std::string name;
    int         scrolling = 1;   /* 0 = no, 1 = auto/yes */
    int         border    = 1;
};

/* One entry of a rows= or cols= list. */
struct track {
    int  value;      /* pixels, percent, or star weight */
    char kind;       /* 'p' pixels, '%' percent, '*' relative */
};

inline void parse_tracks(const char *spec, std::vector<track> &out)
{
    out.clear();

    /* No attribute means a single full-size track, which is what a frameset
     * with only rows= does for its columns. */
    if(!spec || !*spec) {
        out.push_back({ 1, '*' });
        return;
    }

    while(*spec) {
        while(*spec == ' ' || *spec == ',' || *spec == '\t')
            spec++;
        if(!*spec)
            break;

        track t = { 0, 'p' };
        char *end = nullptr;
        long  v   = strtol(spec, &end, 10);

        if(end == spec) {
            /* A bare "*" carries no number and means weight 1. */
            if(*spec == '*') {
                t.value = 1;
                t.kind  = '*';
                spec++;
            }
            else {
                spec++;
                continue;
            }
        }
        else {
            spec = end;
            t.value = (int)v;
            if(*spec == '%')      { t.kind = '%'; spec++; }
            else if(*spec == '*') { t.kind = '*'; spec++; }
            else                    t.kind = 'p';
        }

        out.push_back(t);
        if(out.size() >= PS_FRAME_MAX)
            break;
    }

    if(out.empty())
        out.push_back({ 1, '*' });
}

/* Resolves a track list against an available extent.
 *
 * Fixed pixels and percentages are taken first, then whatever is left is
 * divided between the star tracks by weight. That order matters: a
 * "100,*" sidebar layout must give the sidebar exactly 100 and the rest to
 * the content, not split the difference. */
inline void resolve_tracks(const std::vector<track> &tr, int extent,
                           std::vector<int> &out)
{
    long fixed = 0, stars = 0;
    size_t i;

    out.assign(tr.size(), 0);

    for(i = 0; i < tr.size(); i++) {
        if(tr[i].kind == 'p') {
            out[i] = tr[i].value;
            fixed += out[i];
        }
        else if(tr[i].kind == '%') {
            out[i] = (int)((long)extent * tr[i].value / 100);
            fixed += out[i];
        }
        else {
            stars += tr[i].value > 0 ? tr[i].value : 1;
        }
    }

    long left = extent - fixed;
    if(left < 0)
        left = 0;

    for(i = 0; i < tr.size(); i++) {
        if(tr[i].kind != '*')
            continue;
        int w = tr[i].value > 0 ? tr[i].value : 1;
        out[i] = stars > 0 ? (int)(left * w / stars) : 0;
    }

    /* Rounding loses a pixel or two across several tracks; giving the
     * remainder to the last flexible one avoids a permanent gap at the edge
     * of the screen. */
    long total = 0;
    for(i = 0; i < out.size(); i++)
        total += out[i];

    if(total < extent) {
        for(i = out.size(); i-- > 0;) {
            if(tr[i].kind == '*' || out.size() == 1) {
                out[i] += (int)(extent - total);
                break;
            }
        }
    }
}

inline int attr_bool(const litehtml::element::ptr &el, const char *name,
                     int dflt)
{
    const char *v = el->get_attr(name);

    if(!v)
        return dflt;
    if(!strcasecmp(v, "no") || !strcasecmp(v, "0") || !strcasecmp(v, "off"))
        return 0;
    return 1;
}

/* Recursively splits rect among a frameset's children. */
inline void walk(const litehtml::element::ptr &fs, const ps_rect &rect,
                 int depth, std::vector<frame_def> &out)
{
    if(!fs || depth > PS_FRAME_MAX_DEPTH || out.size() >= PS_FRAME_MAX)
        return;

    const char *rows = fs->get_attr("rows");
    const char *cols = fs->get_attr("cols");

    /* rows and cols together would be a grid; pages of this era nest instead,
     * and rows wins when both appear. */
    bool vertical = rows != nullptr;

    std::vector<track> tr;
    parse_tracks(vertical ? rows : cols, tr);

    std::vector<int> sizes;
    resolve_tracks(tr, vertical ? (rect.y1 - rect.y0) : (rect.x1 - rect.x0),
                   sizes);

    size_t idx = 0;
    int    pos = vertical ? rect.y0 : rect.x0;

    for(const auto &child : fs->children()) {
        litehtml::string_id t = child->tag();

        if(t != litehtml::_frame_ && t != litehtml::_frameset_)
            continue;
        if(idx >= sizes.size() || out.size() >= PS_FRAME_MAX)
            break;

        int      len = sizes[idx++];
        ps_rect  r   = rect;

        if(vertical) {
            r.y0 = (int16_t)pos;
            r.y1 = (int16_t)(pos + len);
        }
        else {
            r.x0 = (int16_t)pos;
            r.x1 = (int16_t)(pos + len);
        }
        pos += len;

        if(t == litehtml::_frameset_) {
            walk(child, r, depth + 1, out);
        }
        else {
            frame_def f;
            const char *s = child->get_attr("src");
            const char *n = child->get_attr("name");

            f.rect      = r;
            f.src       = s ? s : "";
            f.name      = n ? n : "";
            f.scrolling = attr_bool(child, "scrolling", 1);
            f.border    = attr_bool(child, "frameborder", 1);
            out.push_back(f);
        }
    }
}

/* Finds the frameset element of a parsed document, if it is a frameset page.
 * A frameset replaces body, so it hangs off html. */
inline litehtml::element::ptr find_frameset(const litehtml::element::ptr &root)
{
    if(!root)
        return nullptr;

    if(root->tag() == litehtml::_frameset_)
        return root;

    for(const auto &c : root->children()) {
        /* Only descend through structural elements; a frameset nested inside
         * body is not a frameset page. */
        if(c->tag() == litehtml::_body_)
            continue;
        auto found = find_frameset(c);
        if(found)
            return found;
    }
    return nullptr;
}

inline void build(const litehtml::element::ptr &frameset, int w, int h,
                  std::vector<frame_def> &out)
{
    ps_rect full = { 0, 0, (int16_t)w, (int16_t)h };

    out.clear();
    walk(frameset, full, 0, out);
}

} /* namespace psfrm */

#endif /* PS_FRAMES_H */
