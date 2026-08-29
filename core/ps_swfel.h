/* The box a Flash movie occupies.
 *
 * C++ internal header, included only by ps_document.cpp, the same arrangement
 * ps_appletel.h and ps_controls.h use and hooked in the same way.
 *
 * This is a box and nothing else - it does not draw, does not fetch, and does
 * not know a movie exists. The shell submits the movie's triangles to the tile
 * accelerator directly, after the page has been painted, because they do not go
 * through ps_paint's quad batch and so cannot live in the retained list an
 * element's draw would write into. All this element is for is the one thing
 * layout can answer and nothing else can: where on the page the movie goes.
 *
 * It exists because litehtml has no entry for <embed> or <object> in its master
 * stylesheet, so without it the tag is an anonymous inline with no dimensions,
 * width and height attributes are never read, and the movie has a URL and
 * nowhere to be. That is the same hole <applet> falls into and the same fix.
 *
 * Only claimed for a tag that actually names a .swf. An <embed> is also how the
 * period played background music, and an <object> is how it did a dozen other
 * things; taking every one of them would put a 320 by 240 hole in pages that
 * currently lay out correctly.
 */
#ifndef PS_SWFEL_H
#define PS_SWFEL_H

#include <litehtml.h>
#include <render_item.h>
#include <render_image.h>

#include <cstdlib>

namespace psswf {

class el_movie : public litehtml::html_tag {
public:
    explicit el_movie(const std::shared_ptr<litehtml::document> &doc)
        : litehtml::html_tag(doc)
    {
    }

    bool is_replaced() const override { return true; }

    /* Without a replaced render item litehtml lays this out as a generic block
     * and never asks for the intrinsic size, so the box collapses to nothing
     * and the movie has nowhere to go. render_item_image is the generic
     * replaced-content renderer and is not image specific - same note as
     * ps_appletel.h, same trap. */
    std::shared_ptr<litehtml::render_item> create_render_item(
        const std::shared_ptr<litehtml::render_item> &parent_ri) override
    {
        auto ri = std::make_shared<litehtml::render_item_image>(
            shared_from_this());
        ri->parent(parent_ri);
        return ri;
    }

    void parse_attributes() override
    {
        /* Flash's own default when a page states neither, which is rarer than
         * it sounds: the authoring tool wrote both, and a page that omits them
         * usually meant the movie to fill something else. A visible box is
         * still the better failure, because a movie that loaded and drew
         * nowhere is indistinguishable from one that did not load. */
        m_w = attr_px("width",  320);
        m_h = attr_px("height", 240);
        litehtml::html_tag::parse_attributes();
    }

    void get_content_size(litehtml::size &sz, litehtml::pixel_t) override
    {
        sz.width  = (litehtml::pixel_t)m_w;
        sz.height = (litehtml::pixel_t)m_h;
    }

private:
    int attr_px(const char *name, int dflt) const
    {
        const char *v = get_attr(name, "");
        int         n;

        if(!v || !*v)
            return dflt;
        n = atoi(v);        /* a percentage reads as its leading number */
        return n > 0 ? n : dflt;
    }

    int m_w = 320, m_h = 240;
};

}  /* namespace psswf */

#endif /* PS_SWFEL_H */
