/* Client-side image maps implemented as a litehtml image subclass. */
#ifndef PS_IMAGEMAP_H
#define PS_IMAGEMAP_H

#include <litehtml.h>
#include <el_image.h>

#include <cstdlib>
#include <cstring>
#include <strings.h>   /* strncasecmp: not in <cstring> under -std=c++17 */
#include <string>
#include <vector>

#include "ps_controls.h"   /* psctl::host */

namespace psmap {

enum area_shape {
    AREA_RECT = 0,
    AREA_CIRCLE,
    AREA_POLY,
    AREA_DEFAULT
};

/* Parses a coords attribute: a comma or space separated list of integers. */
inline void parse_coords(const char *s, std::vector<int> &out)
{
    out.clear();
    if(!s)
        return;

    while(*s) {
        while(*s && (*s == ',' || *s == ' ' || *s == '\t' || *s == '\n'))
            s++;
        if(!*s)
            break;

        out.push_back((int)strtol(s, (char **)&s, 10));

        /* strtol stops on anything non-numeric; if it consumed nothing we
         * would spin forever on malformed input. */
        if(*s && *s != ',' && *s != ' ' && *s != '\t' && *s != '\n')
            s++;
    }
}

inline bool hit_rect(const std::vector<int> &c, int x, int y)
{
    if(c.size() < 4)
        return false;

    int x0 = c[0] < c[2] ? c[0] : c[2];
    int x1 = c[0] < c[2] ? c[2] : c[0];
    int y0 = c[1] < c[3] ? c[1] : c[3];
    int y1 = c[1] < c[3] ? c[3] : c[1];

    return x >= x0 && x < x1 && y >= y0 && y < y1;
}

inline bool hit_circle(const std::vector<int> &c, int x, int y)
{
    if(c.size() < 3)
        return false;

    long dx = x - c[0], dy = y - c[1], r = c[2];
    return dx * dx + dy * dy <= r * r;
}

/* Even-odd crossing test using integer arithmetic. */
inline bool hit_poly(const std::vector<int> &c, int x, int y)
{
    size_t n = c.size() / 2;
    bool   in = false;

    if(n < 3)
        return false;

    for(size_t i = 0, j = n - 1; i < n; j = i++) {
        long xi = c[i * 2], yi = c[i * 2 + 1];
        long xj = c[j * 2], yj = c[j * 2 + 1];

        if((yi > y) == (yj > y))
            continue;

        /* Compare cross-multiplied to avoid dividing by (yj - yi). */
        long dy = yj - yi;
        long lhs = (long)(x - xi) * dy;
        long rhs = (xj - xi) * (long)(y - yi);

        if(dy > 0 ? lhs < rhs : lhs > rhs)
            in = !in;
    }
    return in;
}

/* Finds the <map> with this name anywhere in the tree. Both "#name" and "name"
 * are accepted, and the leading hash is what pages actually write. */
inline litehtml::element::ptr find_map(const litehtml::element::ptr &root,
                                       const char *usemap)
{
    if(!root || !usemap || !*usemap)
        return nullptr;

    if(*usemap == '#')
        usemap++;

    if(root->tag() == litehtml::_map_) {
        const char *n = root->get_attr("name");

        if(!n)
            n = root->get_attr("id");
        if(n && !strcmp(n, usemap))
            return root;
    }

    for(const auto &c : root->children()) {
        auto found = find_map(c, usemap);
        if(found)
            return found;
    }
    return nullptr;
}

class el_image_map : public litehtml::el_image {
public:
    el_image_map(const litehtml::document::ptr &doc, psctl::host *h)
        : litehtml::el_image(doc), m_host(h)
    {
    }

    void parse_attributes() override
    {
        const char *u = get_attr("usemap");

        m_usemap = u ? u : "";
        litehtml::el_image::parse_attributes();
    }

    void on_click() override
    {
        if(m_usemap.empty()) {
            litehtml::el_image::on_click();
            return;
        }

        auto doc = get_document();
        if(!doc)
            return;

        litehtml::element::ptr map = find_map(doc->root(), m_usemap.c_str());
        if(!map)
            return;

        /* The recorded click is in document coordinates, and so is the
         * element's placement, so the difference is the point within the
         * image that the map's coords are measured against. */
        litehtml::position box = get_placement();
        int rx = m_host->click_x() - (int)box.x;
        int ry = m_host->click_y() - (int)box.y;

        const char *href = area_at(map, rx, ry);
        if(href)
            m_host->navigate(href);
    }

private:
    static const char *area_at(const litehtml::element::ptr &map, int x, int y)
    {
        const char      *fallback = nullptr;
        std::vector<int> coords;

        for(const auto &a : map->children()) {
            if(a->tag() != litehtml::_area_)
                continue;

            const char *href = a->get_attr("href");
            const char *sh   = a->get_attr("shape", "rect");

            /* nohref areas are deliberate holes and must block the areas
             * behind them rather than fall through. */
            if(!href && !a->get_attr("nohref"))
                continue;

            area_shape shape = AREA_RECT;
            if(sh) {
                if(!strncasecmp(sh, "circ", 4))       shape = AREA_CIRCLE;
                else if(!strncasecmp(sh, "poly", 4))  shape = AREA_POLY;
                else if(!strncasecmp(sh, "default", 7)) shape = AREA_DEFAULT;
            }

            if(shape == AREA_DEFAULT) {
                /* Covers the whole image, but only if nothing specific
                 * matched, so it is remembered rather than taken. */
                fallback = href;
                continue;
            }

            parse_coords(a->get_attr("coords"), coords);

            bool hit = (shape == AREA_CIRCLE) ? hit_circle(coords, x, y)
                     : (shape == AREA_POLY)   ? hit_poly(coords, x, y)
                                              : hit_rect(coords, x, y);

            if(hit)
                return href;   /* may be null for nohref: swallows the click */
        }
        return fallback;
    }

    psctl::host *m_host;
    std::string  m_usemap;
};

} /* namespace psmap */

#endif /* PS_IMAGEMAP_H */
