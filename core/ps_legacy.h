/* Legacy presentational elements: <marquee> and <blink>.
 *
 * C++ internal header, included only by ps_document.cpp.
 *
 * litehtml implements neither, and both are a large part of why a page of this
 * era feels like one. The Dreamcast browsers rendered them, so the
 * compatibility bar (Docs/dc-compat.md) says we do too.
 *
 * Both keep their content as a real subtree: nested markup lays out and paints
 * normally, so <marquee><b>bold</b> <img src=...></marquee> works. The hook is
 * render_item::draw_children, which is virtual and receives the offset applied
 * to every descendant. A custom render item can therefore translate or
 * suppress an entire subtree at paint time without touching layout, which is
 * what makes this cheap: a marquee costs one clip and an added coordinate per
 * frame, not a restyle and a 100-300ms re-layout.
 */
#ifndef PS_LEGACY_H
#define PS_LEGACY_H

#include <litehtml.h>
#include <render_item.h>
#include <render_block.h>
#include <render_inline.h>
#include <render_inline_context.h>

#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <string>

#include "ps_types.h"
#include "ps_theme.h"
#include "ps_paint.h"
#include "ps_text.h"
#include "ps_controls.h"   /* psctl::host */

namespace psleg {

/* Defaults from the HTML rendering spec: 6px a step, one step every 85ms. */
#define PS_MARQUEE_AMOUNT  6
#define PS_MARQUEE_DELAY   85

#define PS_BLINK_PERIOD_MS 1000

enum marquee_dir {
    MQ_LEFT = 0,
    MQ_RIGHT,
    MQ_UP,
    MQ_DOWN
};

struct marquee_cfg {
    marquee_dir dir    = MQ_LEFT;
    int         amount = PS_MARQUEE_AMOUNT;
    int         delay  = PS_MARQUEE_DELAY;
};

/* ------------------------------------------------------------- marquee ri */

/* render_item_block::init() does not initialise the object - it *replaces* it,
 * returning a fresh render_item_block_context or render_item_inline_context
 * built from the same element. Any subclass of render_item_block is therefore
 * silently discarded during layout, which is exactly what happened here: the
 * custom item was constructed, then thrown away before it ever painted.
 *
 * Deriving from the inline-context form we would have been converted into
 * anyway, and overriding init() to keep ourselves, is what survives. The base
 * render_item::init() is harmless by comparison - it registers and recurses -
 * so that is what this reproduces.
 *
 * The one thing skipped is render_item_block::init()'s inline-splitting pass,
 * which exists to break an inline element that contains block children. Inside
 * a marquee that would be pathological markup, and the cost of getting it
 * wrong is a mis-wrapped decoration rather than a broken page.
 */
template <class Base>
class ri_keep_self : public Base {
public:
    using Base::Base;

    std::shared_ptr<litehtml::render_item> init() override
    {
        this->src_el()->add_render(this->shared_from_this());

        for(auto &el : this->children())
            el = el->init();

        return this->shared_from_this();
    }
};

class ri_marquee : public ri_keep_self<litehtml::render_item_inline_context> {
public:
    ri_marquee(std::shared_ptr<litehtml::element> el, psctl::host *h,
               const marquee_cfg *cfg)
        : ri_keep_self<litehtml::render_item_inline_context>(std::move(el)),
          m_host(h), m_cfg(cfg)
    {
    }

    void draw_children(litehtml::uint_ptr hdc, litehtml::pixel_t x,
                       litehtml::pixel_t y, const litehtml::position *clip,
                       litehtml::draw_flag flag, int zindex) override
    {
        int bx = (int)pos().x + (int)x;
        int by = (int)pos().y + (int)y;
        int bw = (int)pos().width;
        int bh = (int)pos().height;

        if(bw <= 0 || bh <= 0)
            return;

        int cw = content_w();
        int ch = content_h();

        /* Pixels per second from the authored step and delay, so a page asking
         * for a slow crawl gets one. Time based rather than per frame, so it
         * runs at the authored speed under PAL 50Hz too. */
        int  speed = (m_cfg->delay > 0)
                         ? (m_cfg->amount * 1000) / m_cfg->delay
                         : 70;
        long t     = m_host->anim_ms();

        int dx = 0, dy = 0;

        if(m_cfg->dir == MQ_LEFT || m_cfg->dir == MQ_RIGHT) {
            /* Travel is content plus box, so it exits completely before it
             * re-enters instead of jumping. */
            long span = (long)cw + bw;
            long off  = span > 0 ? (t * speed / 1000) % span : 0;

            dx = (m_cfg->dir == MQ_LEFT) ? (int)(bw - off) : (int)(off - cw);
        }
        else {
            long span = (long)ch + bh;
            long off  = span > 0 ? (t * speed / 1000) % span : 0;

            dy = (m_cfg->dir == MQ_UP) ? (int)(bh - off) : (int)(off - ch);
        }

#ifdef PS_DEBUG_MARQUEE
        {
            static int n = 0;
            if((n++ % 120) == 0)
                printf("marquee t=%ld bw=%d cw=%d dx=%d flag=%d\n", t, bw, cw,
                       dx, (int)flag);
        }
#endif

        ps_paint *p = m_host->paint();
        ps_rect   box = { (int16_t)bx, (int16_t)by, (int16_t)(bx + bw),
                          (int16_t)(by + bh) };

        /* Content that has scrolled past the edge must not paint over the rest
         * of the page. */
        ps_paint_push_clip(p, &box);
        ri_keep_self<litehtml::render_item_inline_context>::draw_children(
            hdc, x + (litehtml::pixel_t)dx, y + (litehtml::pixel_t)dy, clip,
            flag, zindex);
        ps_paint_pop_clip(p);
    }

private:
    /* Extent of the laid-out subtree, which is what the animation wraps
     * around. Children can be wider than the box - that is the entire point of
     * a marquee - so the box's own width will not do. */
    int content_w() const
    {
        int w = 0;

        for(const auto &c : const_cast<ri_marquee *>(this)->children()) {
            int r = (int)c->pos().x + (int)c->pos().width;
            if(r > w)
                w = r;
        }
        return w;
    }

    int content_h() const
    {
        int h = 0;

        for(const auto &c : const_cast<ri_marquee *>(this)->children()) {
            int b = (int)c->pos().y + (int)c->pos().height;
            if(b > h)
                h = b;
        }
        return h;
    }

    psctl::host       *m_host;
    const marquee_cfg *m_cfg;
};

/* --------------------------------------------------------------- marquee */

class el_marquee : public litehtml::html_tag {
public:
    el_marquee(const litehtml::document::ptr &doc, psctl::host *h)
        : litehtml::html_tag(doc), m_host(h)
    {
    }

    const char *get_tagName() const override { return "marquee"; }

    std::shared_ptr<litehtml::render_item> create_render_item(
        const std::shared_ptr<litehtml::render_item> &parent_ri) override
    {
        auto ret = std::make_shared<ri_marquee>(shared_from_this(), m_host,
                                                &m_cfg);
        ret->parent(parent_ri);
        add_children_to(ret);
        return ret;
    }

    void compute_styles(bool recursive = true) override
    {
        litehtml::html_tag::compute_styles(recursive);

        /* A block that fills its container, whatever the master stylesheet
         * made of an unknown tag, and one line: a marquee that wrapped would
         * scroll a paragraph sideways. */
        css_w().set_display(litehtml::display_block);
        css_w().set_white_space(litehtml::white_space_nowrap);
    }

    void parse_attributes() override
    {
        const char *s = get_attr("direction");

        if(s) {
            if(!strcmp(s, "right"))     m_cfg.dir = MQ_RIGHT;
            else if(!strcmp(s, "up"))   m_cfg.dir = MQ_UP;
            else if(!strcmp(s, "down")) m_cfg.dir = MQ_DOWN;
        }

        s = get_attr("scrollamount");
        if(s && atoi(s) > 0)
            m_cfg.amount = atoi(s);

        s = get_attr("scrolldelay");
        if(s && atoi(s) > 0)
            m_cfg.delay = atoi(s);

        litehtml::html_tag::parse_attributes();
    }

private:
    /* element::create_render_item builds the child render items itself; doing
     * the same here keeps the subtree intact when we substitute our own. */
    void add_children_to(const std::shared_ptr<litehtml::render_item> &ri)
    {
        for(const auto &el : m_children) {
            if(el->css().get_display() == litehtml::display_none)
                continue;
            auto child = el->create_render_item(ri);
            if(child)
                ri->add_child(child);
        }
    }

    psctl::host *m_host;
    marquee_cfg  m_cfg;
};

/* --------------------------------------------------------------- blink ri */

/* Blink is inline, so it gets an inline render item; suppressing the subtree
 * for half of each period leaves layout untouched, which is what keeps the
 * surrounding text from reflowing as it flashes. */
template <class Base>
class ri_blink : public Base {
public:
    ri_blink(std::shared_ptr<litehtml::element> el, psctl::host *h)
        : Base(std::move(el)), m_host(h)
    {
    }

    void draw_children(litehtml::uint_ptr hdc, litehtml::pixel_t x,
                       litehtml::pixel_t y, const litehtml::position *clip,
                       litehtml::draw_flag flag, int zindex) override
    {
        if((m_host->anim_ms() % PS_BLINK_PERIOD_MS) >= PS_BLINK_PERIOD_MS / 2)
            return;
        Base::draw_children(hdc, x, y, clip, flag, zindex);
    }

private:
    psctl::host *m_host;
};

class el_blink : public litehtml::html_tag {
public:
    el_blink(const litehtml::document::ptr &doc, psctl::host *h)
        : litehtml::html_tag(doc), m_host(h)
    {
    }

    const char *get_tagName() const override { return "blink"; }

    std::shared_ptr<litehtml::render_item> create_render_item(
        const std::shared_ptr<litehtml::render_item> &parent_ri) override
    {
        std::shared_ptr<litehtml::render_item> ret;

        /* Match whatever display the cascade settled on, so a blink styled as
         * a block still lays out as one. */
        if(css().get_display() == litehtml::display_inline) {
            /* render_item_inline does not override init(), so the base is used
             * and the object survives layout as-is. */
            ret = std::make_shared<ri_blink<litehtml::render_item_inline>>(
                shared_from_this(), m_host);
        }
        else {
            ret = std::make_shared<
                ri_blink<ri_keep_self<litehtml::render_item_inline_context>>>(
                shared_from_this(), m_host);
        }

        ret->parent(parent_ri);

        for(const auto &el : m_children) {
            if(el->css().get_display() == litehtml::display_none)
                continue;
            auto child = el->create_render_item(ret);
            if(child)
                ret->add_child(child);
        }
        return ret;
    }

private:
    psctl::host *m_host;
};

} /* namespace psleg */

#endif /* PS_LEGACY_H */
