/* The <applet> element.
 *
 * C++ internal header, included only by ps_document.cpp so the project keeps a
 * single C++ translation unit and a C public surface - same arrangement as
 * ps_controls.h, and it hooks in the same way, through
 * document_container::create_element.
 *
 * litehtml has no notion of <applet>: it is not in the master stylesheet and
 * there is no element for it, so without this the tag lays out as an anonymous
 * inline and the page shows the <param> tags' whitespace and nothing else.
 * Here it becomes a replaced element with the box the page asked for, exactly
 * like an image.
 *
 * <object> and <embed> also carried applets in the period and are recognised
 * when their type or classid says Java. <object> in particular is what the
 * later authoring tools emitted, so a browser that only knows <applet> misses
 * a good share of what is left.
 */
#ifndef PS_APPLETEL_H
#define PS_APPLETEL_H

#include <litehtml.h>
#include <render_item.h>
#include <render_image.h>

#include <cstring>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

#include "ps_types.h"
#include "ps_theme.h"
#include "ps_paint.h"
#include "ps_text.h"

/* For the parameter bounds only. They are the applet cache's, not this
 * element's: writing them out again here is how the two ends of the same list
 * come to disagree and a <param> disappears between them. */
#include "../java/ps_applet.h"

namespace psapp {

/* The <param name= value=> children, in source order and with duplicates left
 * alone - the first of a repeated name is what a browser answers with, and
 * that falls out of searching in order. */
typedef std::vector<std::pair<std::string, std::string> > param_list;

/* What the element needs from the browser around it. The document container
 * implements this, the same way it implements psctl::host for form controls. */
struct host {
    virtual ~host() {}

    /* Resolves code/codebase against the page and asks for the class. Returns
     * the texture once the applet has painted, or nothing while it is in
     * flight - which is the same contract ps_image_get offers, so layout does
     * not have to special-case applets. */
    virtual bool applet_frame(const std::string &code,
                              const std::string &codebase,
                              const std::string &archive,
                              int w, int h,
                              ps_texture *tex, float *u1, float *v1) = 0;

    /* Hands the element's <param> children to the applet, along with the URL
     * of the page they came from.
     *
     * Called just after applet_frame rather than before it: the frame request
     * is what creates the applet's slot, and until it exists there is nowhere
     * to put a parameter. Repeating it every draw is what the browser does,
     * and the first set is the one that sticks - an applet reads its
     * parameters in init() and cannot be told different ones later. */
    virtual void applet_params(const std::string &code,
                               const std::string &codebase,
                               const param_list &params) = 0;

    /* Why there is no frame, for the placeholder. */
    virtual std::string applet_why(const std::string &code,
                                   const std::string &codebase) = 0;

    /* Paints the applet as geometry, straight into the page. True when it
     * did; false means it needs the texture path. */
    virtual bool applet_paint(const std::string &code,
                              const std::string &codebase,
                              int x, int y, int w, int h) = 0;

    /* A press, release or move inside the applet's box, in its own
     * coordinates. Returns true if the applet took it. */
    virtual bool applet_input(const std::string &code,
                              const std::string &codebase,
                              int x, int y, int down, int dragging) = 0;

    /* Which applet the pointer is inside, in its own coordinates, or an empty
     * code for none. Separate from applet_input because hovering is not
     * clicking: this runs every frame and must not deliver anything, it only
     * says where the pointer is so the crossing can be worked out. */
    virtual void applet_hover(const std::string &code,
                              const std::string &codebase,
                              int x, int y) = 0;
    virtual void applet_hover_none() = 0;

    /* Where the applet sits on screen this frame, so the browser can tell
     * whether it is worth running. */
    virtual void applet_screen_box(int y, int h) = 0;

    virtual ps_paint      *paint() = 0;
    virtual ps_text_cache *text()  = 0;
};

class el_applet : public litehtml::html_tag {
public:
    el_applet(const std::shared_ptr<litehtml::document> &doc, host *h)
        : litehtml::html_tag(doc), m_host(h)
    {
    }

    bool is_replaced() const override { return true; }

    /* Without a replaced render item litehtml lays this out as a generic block
     * and never consults the intrinsic size, so the applet collapses to
     * nothing. render_item_image is the generic replaced-content renderer; it
     * is not image specific. Same note as ps_controls.h, same trap. */
    std::shared_ptr<litehtml::render_item> create_render_item(
        const std::shared_ptr<litehtml::render_item> &parent_ri) override
    {
        auto ret = std::make_shared<litehtml::render_item_image>(
            shared_from_this());
        ret->parent(parent_ri);
        return ret;
    }

    void parse_attributes() override
    {
        collect_params();

        m_code     = get_attr("code", "");
        m_codebase = get_attr("codebase", "");
        m_archive  = get_attr("archive", "");

        /* <object> spells the class differently. classid="java:Foo.class" is
         * the form the authoring tools emitted. */
        if(m_code.empty()) {
            std::string cid = get_attr("classid", "");

            if(!cid.compare(0, 5, "java:"))
                m_code = cid.substr(5);
            else if(!cid.empty())
                m_code = cid;
        }
        if(m_code.empty())
            m_code = param("code");
        if(m_codebase.empty())
            m_codebase = param("codebase");
        if(m_archive.empty())
            m_archive = param("archive");

        /* The .class suffix is optional in the attribute and required in the
         * URL; a page may write either. */
        if(!m_code.empty() && m_code.size() > 6 &&
           m_code.compare(m_code.size() - 6, 6, ".class") != 0)
            m_code += ".class";
        else if(!m_code.empty() && m_code.size() <= 6)
            m_code += ".class";

        m_w = attr_px("width",  200);
        m_h = attr_px("height", 200);

        litehtml::html_tag::parse_attributes();
    }

    void get_content_size(litehtml::size &sz, litehtml::pixel_t) override
    {
        sz.width  = (litehtml::pixel_t)m_w;
        sz.height = (litehtml::pixel_t)m_h;
    }

    void draw(litehtml::uint_ptr hdc, litehtml::pixel_t x, litehtml::pixel_t y,
              const litehtml::position *clip,
              const std::shared_ptr<litehtml::render_item> &ri) override
    {
        litehtml::html_tag::draw(hdc, x, y, clip, ri);

        if(!m_host)
            return;

        litehtml::position pos = ri->pos();
        int bx = (int)(x + pos.x), by = (int)(y + pos.y);
        int bw = (int)pos.width,   bh = (int)pos.height;

        if(bw <= 0 || bh <= 0)
            return;

        /* Remembered for hit testing. litehtml gives an element its position
         * during paint and nowhere else, so this is where it can be caught. */
        m_bx = bx; m_by = by; m_bw = bw; m_bh = bh;
        m_host->applet_screen_box(by, bh);

        ps_texture tex = PS_TEXTURE_NONE;
        float      u1 = 1.0f, v1 = 1.0f;

        /* The frame request is also what creates the applet, so the page's
         * <param> values follow immediately: on the very first draw there is
         * nothing to give them to until this has run, and the class file is
         * still in flight, so they are in place well before init() reads
         * them. */
        bool framed = m_host->applet_frame(m_code, m_codebase, m_archive,
                                           bw, bh, &tex, &u1, &v1);

        m_host->applet_params(m_code, m_codebase, m_params);

        /* Geometry first. The applet's paint() runs here, inside the page's
         * own draw, which is where a browser has always run it - and its
         * output lands in the same display list as everything around it,
         * ordered against the page by construction. */
        if(framed && m_host->applet_paint(m_code, m_codebase, bx, by, bw, bh))
            return;

        if(tex != PS_TEXTURE_NONE) {
            ps_rect dst = { (int16_t)bx, (int16_t)by,
                            (int16_t)(bx + bw), (int16_t)(by + bh) };

            ps_paint_image(m_host->paint(), tex, &dst, 0.0f, 0.0f, u1, v1,
                           PS_ARGB(255, 255, 255, 255));
            return;
        }

        draw_placeholder(bx, by, bw, bh);
    }

    /* The applet's box in page coordinates, so the shell can decide whether a
     * click belongs to it. Written by draw, which is the only place the
     * layout position is available. */
    bool box(int *x, int *y, int *w, int *h) const
    {
        if(m_bw <= 0)
            return false;
        *x = m_bx; *y = m_by; *w = m_bw; *h = m_bh;
        return true;
    }

    const std::string &code() const     { return m_code; }
    const std::string &codebase() const { return m_codebase; }

private:
    /* A framed box naming what went wrong.
     *
     * A blank space would be indistinguishable from a page that simply has a
     * gap there, and an applet that fails is the normal case on a browser
     * whose JVM is this young. Saying which class and why turns a mystery into
     * a bug report. */
    void draw_placeholder(int x, int y, int w, int h)
    {
        ps_paint *p = m_host->paint();
        ps_rect   r = { (int16_t)x, (int16_t)y,
                        (int16_t)(x + w), (int16_t)(y + h) };

        ps_paint_rect(p, &r, PS_ARGB(255, 30, 32, 40));

        {
            ps_rect e = r;
            e.y1 = (int16_t)(y + PS_STROKE);
            ps_paint_rect(p, &e, PS_C_ACCENT_DIM);
            e = r; e.y0 = (int16_t)(y + h - PS_STROKE);
            ps_paint_rect(p, &e, PS_C_ACCENT_DIM);
            e = r; e.x1 = (int16_t)(x + PS_STROKE);
            ps_paint_rect(p, &e, PS_C_ACCENT_DIM);
            e = r; e.x0 = (int16_t)(x + w - PS_STROKE);
            ps_paint_rect(p, &e, PS_C_ACCENT_DIM);
        }

        ps_font *f = ps_text_font(m_host->text(), PS_FONT_UI);
        if(!f || w < 60 || h < 30)
            return;

        std::string why = m_host->applet_why(m_code, m_codebase);
        std::string l1  = m_code.empty() ? "applet" : m_code;

        ps_paint_push_clip(p, &r);
        ps_font_draw(p, f, x + PS_PAD, y + 24, l1.c_str(), l1.size(),
                     PS_C_TEXT);
        if(!why.empty() && h >= 52)
            ps_font_draw(p, f, x + PS_PAD, y + 48, why.c_str(), why.size(),
                         PS_C_TEXT_DIM);
        ps_paint_pop_clip(p);
    }

    /* Every <param name= value=> child, kept.
     *
     * This used to answer three questions - code, codebase, archive - and drop
     * the rest on the floor, which meant an applet configured entirely through
     * <param> ran with none of its configuration and drew something plausible
     * and wrong. They are all kept now and handed to the applet, and the three
     * the element itself cares about are read back out of the same list.
     *
     * Bounded because a document is untrusted: the count and both strings are
     * capped at what the applet cache will store anyway, so a page declaring
     * ten thousand parameters costs nothing beyond the first sixteen. */
    void collect_params()
    {
        m_params.clear();

        for(const auto &ch : m_children) {
            if(ch->tag() != litehtml::_param_)
                continue;
            if(m_params.size() >= (size_t)PS_APPLET_PARAMS)
                break;

            std::string n = ch->get_attr("name", "");
            std::string v = ch->get_attr("value", "");

            if(n.empty())
                continue;

            /* One short of the cache's field, which is where the terminator
             * goes. Truncating to the same length at both ends is what keeps
             * the element's idea of a parameter and the applet's identical. */
            if(n.size() >= PS_APPLET_PARAM_NAME)
                n.resize(PS_APPLET_PARAM_NAME - 1);
            if(v.size() >= PS_APPLET_PARAM_VALUE)
                v.resize(PS_APPLET_PARAM_VALUE - 1);

            m_params.push_back(std::make_pair(n, v));
        }
    }

    /* One parameter by name, which is where <object> keeps what <applet> puts
     * in attributes. Case-insensitive, as the applet side is - a page is free
     * to write CODEBASE. */
    std::string param(const char *want) const
    {
        for(const auto &kv : m_params) {
            if(!strcasecmp(kv.first.c_str(), want))
                return kv.second;
        }
        return "";
    }

    int attr_px(const char *name, int dflt) const
    {
        const char *v = get_attr(name, "");
        int         n;

        if(!v || !*v)
            return dflt;
        n = atoi(v);
        return n > 0 ? n : dflt;
    }

    host       *m_host = nullptr;
    std::string m_code, m_codebase, m_archive;
    param_list  m_params;
    int         m_w = 200, m_h = 200;
    int         m_bx = 0, m_by = 0, m_bw = 0, m_bh = 0;

public:
    /* Offers a page-space point to this applet. Returns true when it was
     * inside and the applet consumed it, which is what stops the click also
     * being delivered to the page underneath. */
    bool offer_input(int px, int py, int down, int dragging)
    {
        if(!m_host || m_bw <= 0)
            return false;
        if(px < m_bx || px >= m_bx + m_bw || py < m_by || py >= m_by + m_bh)
            return false;

        return m_host->applet_input(m_code, m_codebase, px - m_bx, py - m_by,
                                    down, dragging);
    }

    /* Same box test, no delivery. True when the point was inside, in which
     * case the host has been told where. */
    bool offer_hover(int px, int py)
    {
        if(!m_host || m_bw <= 0)
            return false;
        if(px < m_bx || px >= m_bx + m_bw || py < m_by || py >= m_by + m_bh)
            return false;

        m_host->applet_hover(m_code, m_codebase, px - m_bx, py - m_by);
        return true;
    }
};

/* Whether an <object>/<embed> is carrying Java rather than Flash or a movie. */
inline bool is_java_object(const litehtml::string_map &attrs)
{
    auto get = [&](const char *k) -> std::string {
        auto it = attrs.find(k);
        return it == attrs.end() ? std::string() : it->second;
    };

    std::string type = get("type"), cid = get("classid"), code = get("code");

    for(auto &c : type) c = (char)tolower((unsigned char)c);
    for(auto &c : cid)  c = (char)tolower((unsigned char)c);

    if(type.find("java") != std::string::npos)
        return true;
    if(cid.find("java") != std::string::npos)
        return true;
    /* An <object> with a code= attribute is an applet however it is typed. */
    return !code.empty();
}

} /* namespace psapp */

#endif /* PS_APPLETEL_H */
