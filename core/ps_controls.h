/* HTML form controls implemented through litehtml's create_element hook. */
#ifndef PS_CONTROLS_H
#define PS_CONTROLS_H

#include <litehtml.h>
/* litehtml.h forward-declares render_item; draw() calls ri->pos() on it. */
#include <render_item.h>
#include <render_image.h>

#include <cstdlib>
#include <cctype>
#include <string>
#include <vector>

#include "ps_types.h"
#include "ps_theme.h"
#include "ps_paint.h"
#include "ps_text.h"

namespace psctl {

enum ctl_type {
    CTL_TEXT = 0,
    CTL_PASSWORD,
    CTL_CHECKBOX,
    CTL_RADIO,
    CTL_SUBMIT,
    CTL_RESET,
    CTL_BUTTON,
    CTL_HIDDEN,
    CTL_TEXTAREA,
    CTL_SELECT,
    CTL_FILE          /* rendered, but never functional: no filesystem to pick from */
};

/* What a control needs from the browser. Keeps the controls independent of the
 * container class, which is defined later in the same file. */
struct host {
    virtual ~host() = default;
    virtual ps_paint      *paint()  = 0;
    virtual ps_text_cache *text()   = 0;
    /* `from` is the control that triggered it, which matters: an activated
     * submit button is itself successful and must appear in the data. */
    virtual void submit(litehtml::element *from, bool reset) = 0;
    virtual void focus_control(litehtml::element *el)        = 0;

    /* Monotonic milliseconds since load, for animated legacy elements. */
    virtual long anim_ms() const = 0;

    /* Last click position in document coordinates. on_click carries none, and
     * an image map needs to know where inside the image it landed. */
    virtual int  click_x() const = 0;
    virtual int  click_y() const = 0;

    /* Resolved against the document base and deferred, exactly like an anchor
     * click: navigating from inside litehtml's traversal would destroy the
     * tree it is walking. */
    virtual void navigate(const char *href) = 0;
    virtual bool is_focused(const litehtml::element *el) const = 0;
};

/* ------------------------------------------------------------------ paint */

inline void fill(ps_paint *p, int x, int y, int w, int h, ps_color c)
{
    ps_rect r;

    if(w <= 0 || h <= 0)
        return;

    r.x0 = (int16_t)x;
    r.y0 = (int16_t)y;
    r.x1 = (int16_t)(x + w);
    r.y1 = (int16_t)(y + h);
    ps_paint_rect(p, &r, c);
}

/* Two-tone bevel. Flat panels read as dead space on a TV; a bevel says
 * "control" at a glance, and it costs four rects. */
inline void bevel(ps_paint *p, int x, int y, int w, int h,
                  ps_color hi, ps_color lo)
{
    fill(p, x, y, w, PS_STROKE, hi);                 /* top    */
    fill(p, x, y, PS_STROKE, h, hi);                 /* left   */
    fill(p, x, y + h - PS_STROKE, w, PS_STROKE, lo); /* bottom */
    fill(p, x + w - PS_STROKE, y, PS_STROKE, h, lo); /* right  */
}

inline void outline(ps_paint *p, int x, int y, int w, int h, ps_color c)
{
    fill(p, x, y, w, PS_STROKE, c);
    fill(p, x, y + h - PS_STROKE, w, PS_STROKE, c);
    fill(p, x, y, PS_STROKE, h, c);
    fill(p, x + w - PS_STROKE, y, PS_STROKE, h, c);
}

/* Focus ring sits *outside* the control so it never eats into the widget's own
 * pixels, which matters when the widget is only 20px across. */
inline void focus_ring(ps_paint *p, int x, int y, int w, int h)
{
    outline(p, x - 3, y - 3, w + 6, h + 6, PS_C_OUTLINE);
    outline(p, x - 2, y - 2, w + 4, h + 4, PS_C_FOCUS);
}

/* Half-widths of a 20px circle, per row. A radio button has to be visibly
 * round or it reads as a checkbox, and at this size a rasterised circle is
 * cheaper and crisper as a table than as maths. */
static const unsigned char circle20[PS_CTL_BOX] = {
    6, 8, 9, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 9, 8, 6, 0
};

inline void disc(ps_paint *p, int cx, int y, int d, ps_color c, int inset)
{
    int i;

    for(i = inset; i < d - inset; i++) {
        int hw = circle20[i] - inset;

        if(hw > 0)
            fill(p, cx - hw, y + i, hw * 2, 1, c);
    }
}

/* ---------------------------------------------------------------- element */

class el_control : public litehtml::html_tag {
public:
    el_control(const litehtml::document::ptr &doc, host *h, ctl_type type)
        : litehtml::html_tag(doc), m_host(h), m_type(type)
    {
    }

    bool is_replaced() const override { return m_type != CTL_HIDDEN; }

    /* is_replaced() and get_content_size() alone are not enough: without a
     * replaced render item litehtml lays the element out as a generic block
     * and the intrinsic size is never consulted, so every control collapses to
     * nothing. render_item_image is the generic replaced-content renderer -
     * it is not image specific, it just honours get_content_size. */
    std::shared_ptr<litehtml::render_item> create_render_item(
        const std::shared_ptr<litehtml::render_item> &parent_ri) override
    {
        if(m_type == CTL_HIDDEN)
            return litehtml::html_tag::create_render_item(parent_ri);

        auto ret = std::make_shared<litehtml::render_item_image>(
            shared_from_this());
        ret->parent(parent_ri);
        return ret;
    }

    void parse_attributes() override
    {
        m_name  = get_attr("name", "");
        m_value = get_attr("value", "");

        /* checked and disabled are boolean attributes: presence is the value,
         * so checked="false" still means checked. */
        m_checked  = get_attr("checked") != nullptr;
        m_disabled = get_attr("disabled") != nullptr;

        m_cols = atoi_default(get_attr("size"), atoi_default(get_attr("cols"),
                                                             PS_CTL_COLS));
        m_rows = atoi_default(get_attr("rows"), PS_CTL_ROWS);

        if(m_type == CTL_SELECT)
            collect_options();

        /* A textarea's value is its child text, not a value attribute. */
        if(m_type == CTL_TEXTAREA) {
            m_value.clear();
            get_text(m_value);
            trim_leading_newline(m_value);
            m_default = m_value;
        }

        /* Buttons with no value still need a legible face. */
        if(m_value.empty()) {
            if(m_type == CTL_SUBMIT)
                m_value = "Submit";
            else if(m_type == CTL_RESET)
                m_value = "Reset";
        }

        litehtml::html_tag::parse_attributes();
    }

    void get_content_size(litehtml::size &sz, litehtml::pixel_t) override
    {
        const litehtml::font_metrics &fm = css().get_font_metrics();
        int ch   = (int)fm.ch_width  > 0 ? (int)fm.ch_width  : 8;
        int line = (int)fm.height    > 0 ? (int)fm.height    : 16;

        switch(m_type) {
        case CTL_CHECKBOX:
        case CTL_RADIO:
            sz.width  = (litehtml::pixel_t)PS_CTL_BOX;
            sz.height = (litehtml::pixel_t)PS_CTL_BOX;
            break;

        case CTL_SUBMIT:
        case CTL_RESET:
        case CTL_BUTTON:
            sz.width  = (litehtml::pixel_t)(label_width() + PS_CTL_PAD_X * 4);
            sz.height = (litehtml::pixel_t)(line + PS_CTL_PAD_Y * 2 +
                                            PS_STROKE * 2);
            break;

        case CTL_TEXTAREA:
            sz.width  = (litehtml::pixel_t)(m_cols * ch + PS_CTL_PAD_X * 2);
            sz.height = (litehtml::pixel_t)(m_rows * line + PS_CTL_PAD_Y * 2);
            break;

        case CTL_SELECT:
            /* Room for the widest option plus the drop arrow. */
            sz.width  = (litehtml::pixel_t)(m_widest * ch + PS_CTL_PAD_X * 2 +
                                            PS_CTL_BOX);
            sz.height = (litehtml::pixel_t)(line + PS_CTL_PAD_Y * 2);
            break;

        case CTL_HIDDEN:
            sz.width = sz.height = (litehtml::pixel_t)0;
            break;

        default:
            sz.width  = (litehtml::pixel_t)(m_cols * ch + PS_CTL_PAD_X * 2);
            sz.height = (litehtml::pixel_t)(line + PS_CTL_PAD_Y * 2);
            break;
        }
    }

    void draw(litehtml::uint_ptr hdc, litehtml::pixel_t x, litehtml::pixel_t y,
              const litehtml::position *clip,
              const std::shared_ptr<litehtml::render_item> &ri) override
    {
        litehtml::html_tag::draw(hdc, x, y, clip, ri);

        if(m_type == CTL_HIDDEN)
            return;

        litehtml::position pos = ri->pos();
        pos.x += x;
        pos.y += y;
        pos.round();

        if(!pos.does_intersect(clip))
            return;

        ps_paint *p = m_host->paint();
        int px = (int)pos.x, py = (int)pos.y;
        int pw = (int)pos.width, ph = (int)pos.height;
        bool foc = m_host->is_focused(this);

        switch(m_type) {
        case CTL_CHECKBOX: draw_checkbox(p, px, py, pw, ph); break;
        case CTL_RADIO:    draw_radio(p, px, py, pw, ph);    break;

        case CTL_SUBMIT:
        case CTL_RESET:
        case CTL_BUTTON:   draw_button(p, px, py, pw, ph);   break;

        case CTL_SELECT:   draw_select(p, px, py, pw, ph);   break;

        default:           draw_field(p, px, py, pw, ph);    break;
        }

        if(foc)
            focus_ring(p, px, py, pw, ph);
    }

    /* litehtml routes clicks here through on_lbutton_up. */
    void on_click() override
    {
        if(m_disabled)
            return;

        switch(m_type) {
        case CTL_CHECKBOX:
            m_checked = !m_checked;
            break;

        case CTL_RADIO:
            /* Radios are exclusive across a name within one form, so the group
             * has to be cleared before this one is set. */
            clear_radio_group();
            m_checked = true;
            break;

        case CTL_SELECT:
            /* Advances to the next option and wraps.
             *
             * A popup option list is the eventual design (it is in the UI
             * inventory), but cycling in place is genuinely good console UX
             * for the short selects this era's pages use, and it needs no
             * overlay surface inside the page. Long lists will want the
             * popup. */
            if(!m_options.empty()) {
                m_sel = (m_sel + 1) % (int)m_options.size();
                sync_selection();
            }
            break;

        case CTL_SUBMIT:
            m_host->submit(this, false);
            return;

        case CTL_RESET:
            m_host->submit(this, true);
            return;

        default:
            break;
        }

        m_host->focus_control(this);
    }

    /* --- form serialisation ------------------------------------------- */

    bool successful() const
    {
        /* "Successful" in the HTML sense: what actually gets submitted. An
         * unnamed or disabled control contributes nothing, and an unchecked
         * box is absent rather than empty. */
        if(m_disabled || m_name.empty())
            return false;
        if(m_type == CTL_CHECKBOX || m_type == CTL_RADIO)
            return m_checked;

        /* Buttons are not successful by being present. The one that was
         * actually activated is, and the container adds it separately, since
         * only the container knows which that was. */
        if(m_type == CTL_SUBMIT || m_type == CTL_RESET || m_type == CTL_BUTTON)
            return false;
        return true;
    }

    const std::string &ctl_name() const { return m_name; }

    std::string ctl_value() const
    {
        if(m_type == CTL_CHECKBOX || m_type == CTL_RADIO)
            return m_value.empty() ? std::string("on") : m_value;
        return m_value;
    }

    void set_value(const std::string &v) { m_value = v; }
    ctl_type type() const { return m_type; }
    bool     editable() const
    {
        return m_type == CTL_TEXT || m_type == CTL_PASSWORD ||
               m_type == CTL_TEXTAREA;
    }

    void reset_to_default()
    {
        m_checked = get_attr("checked") != nullptr;

        if(m_type == CTL_TEXTAREA) {
            m_value = m_default;
        }
        else if(m_type == CTL_SELECT) {
            m_sel = m_default_sel;
            sync_selection();
        }
        else {
            m_value = get_attr("value", "");
        }
    }

private:
    static int atoi_default(const char *s, int dflt)
    {
        if(!s || !*s)
            return dflt;
        int v = atoi(s);
        return v > 0 ? v : dflt;
    }

    int label_width() const
    {
        const ps_font *f = (const ps_font *)css().get_font();

        if(!f || m_value.empty())
            return 40;
        return ps_font_measure(f, m_value.c_str(), m_value.size());
    }

    static void trim_leading_newline(std::string &s)
    {
        /* HTML drops a newline immediately after the <textarea> open tag. */
        if(!s.empty() && s[0] == '\n')
            s.erase(0, 1);
        else if(s.size() > 1 && s[0] == '\r' && s[1] == '\n')
            s.erase(0, 2);
    }

    struct option {
        std::string text;
        std::string value;
    };

    void collect_options()
    {
        /* Without an explicit selection the first option wins, which is what
         * browsers do. */
        m_widest = 1;
        m_options.clear();
        m_sel = 0;

        for(const auto &child : m_children) {
            if(child->tag() != litehtml::_option_)
                continue;

            option o;
            child->get_text(o.text);

            const char *v = child->get_attr("value");
            o.value = v ? v : o.text;

            if((int)o.text.size() > m_widest)
                m_widest = (int)o.text.size();

            if(child->get_attr("selected"))
                m_sel = (int)m_options.size();

            m_options.push_back(o);
        }

        m_default_sel = m_sel;
        sync_selection();
    }

    void sync_selection()
    {
        if(m_options.empty()) {
            m_display.clear();
            m_value.clear();
            return;
        }
        if(m_sel < 0 || m_sel >= (int)m_options.size())
            m_sel = 0;

        m_display = m_options[m_sel].text;
        m_value   = m_options[m_sel].value;
    }

    void clear_radio_group()
    {
        litehtml::element::ptr f = owning_form();

        if(!f)
            return;
        clear_radios_in(f.get());
    }

    void clear_radios_in(litehtml::element *el);

    litehtml::element::ptr owning_form() const
    {
        for(litehtml::element::ptr p = parent(); p; p = p->parent()) {
            if(p->tag() == litehtml::_form_)
                return p;
        }
        return nullptr;
    }

    /* --- widget painting ------------------------------------------------ */

    void draw_text_in(ps_paint *p, int x, int baseline, const std::string &s,
                      ps_color col) const
    {
        const ps_font *f = (const ps_font *)css().get_font();

        if(f && !s.empty())
            ps_font_draw(p, f, x, baseline, s.c_str(), s.size(), col);
    }

    void draw_field(ps_paint *p, int x, int y, int w, int h)
    {
        const litehtml::font_metrics &fm = css().get_font_metrics();
        int line = (int)fm.height > 0 ? (int)fm.height : 16;

        fill(p, x, y, w, h, PS_C_FIELD_BG);
        /* Sunken: dark on top-left is what makes a field look recessed. */
        bevel(p, x, y, w, h, PS_C_FIELD_SHADOW, PS_C_FIELD_EDGE);

        std::string shown = m_value;
        if(m_type == CTL_PASSWORD)
            shown.assign(m_value.size(), '*');

        /* Clip so long content cannot spill out of the field. */
        ps_rect cr = { (int16_t)(x + PS_STROKE), (int16_t)(y + PS_STROKE),
                       (int16_t)(x + w - PS_STROKE),
                       (int16_t)(y + h - PS_STROKE) };
        ps_paint_push_clip(p, &cr);

        /* A textarea wraps on its own newlines. Soft wrapping on width needs
         * the line breaker and is not here yet. */
        size_t start = 0;
        int    row   = 0;
        int    tail_w = 0;

        for(;;) {
            size_t nl = shown.find('\n', start);
            std::string seg = shown.substr(start, nl == std::string::npos
                                                      ? std::string::npos
                                                      : nl - start);

            draw_text_in(p, x + PS_CTL_PAD_X,
                         y + PS_CTL_PAD_Y + row * line + (int)fm.ascent, seg,
                         PS_C_FIELD_TEXT);

            const ps_font *f = (const ps_font *)css().get_font();
            tail_w = f ? ps_font_measure(f, seg.c_str(), seg.size()) : 0;

            if(nl == std::string::npos || m_type != CTL_TEXTAREA)
                break;
            start = nl + 1;
            row++;
        }

        if(m_host->is_focused(this)) {
            int cx = x + PS_CTL_PAD_X + tail_w;

            fill(p, cx + 1, y + PS_CTL_PAD_Y + row * line, PS_STROKE, line,
                 PS_C_FIELD_TEXT);
        }
        ps_paint_pop_clip(p);
    }

    void draw_button(ps_paint *p, int x, int y, int w, int h)
    {
        const litehtml::font_metrics &fm = css().get_font_metrics();
        const ps_font *f  = (const ps_font *)css().get_font();
        int tw = f ? ps_font_measure(f, m_value.c_str(), m_value.size()) : 0;
        int baseline = y + (h - (int)fm.height) / 2 + (int)fm.ascent;

        fill(p, x, y, w, h, PS_C_BTN_FACE);
        /* Raised: the inverse of the field bevel. */
        bevel(p, x, y, w, h, PS_C_BTN_HI, PS_C_BTN_LO);
        outline(p, x, y, w, h, PS_C_FIELD_EDGE);

        draw_text_in(p, x + (w - tw) / 2, baseline, m_value,
                     m_disabled ? PS_C_FIELD_SHADOW : PS_C_BTN_TEXT);
    }

    void draw_checkbox(ps_paint *p, int x, int y, int w, int h)
    {
        fill(p, x, y, w, h, PS_C_FIELD_BG);
        bevel(p, x, y, w, h, PS_C_FIELD_SHADOW, PS_C_FIELD_EDGE);

        /* A filled block, not a tick: at 480i a two-pixel check mark breaks up
         * into unreadable fragments, and a solid block never does. */
        if(m_checked)
            fill(p, x + 5, y + 5, w - 10, h - 10, PS_C_FIELD_TEXT);
    }

    void draw_radio(ps_paint *p, int x, int y, int w, int h)
    {
        int cx = x + w / 2;

        (void)h;
        disc(p, cx, y, PS_CTL_BOX, PS_C_FIELD_EDGE, 0);
        disc(p, cx, y, PS_CTL_BOX, PS_C_FIELD_BG, 2);

        if(m_checked)
            disc(p, cx, y, PS_CTL_BOX, PS_C_FIELD_TEXT, 6);
    }

    void draw_select(ps_paint *p, int x, int y, int w, int h)
    {
        const litehtml::font_metrics &fm = css().get_font_metrics();
        int baseline = y + PS_CTL_PAD_Y + (int)fm.ascent;
        int ax = x + w - PS_CTL_BOX;
        int i;

        fill(p, x, y, w, h, PS_C_FIELD_BG);
        bevel(p, x, y, w, h, PS_C_FIELD_SHADOW, PS_C_FIELD_EDGE);

        ps_rect cr = { (int16_t)(x + PS_STROKE), (int16_t)(y + PS_STROKE),
                       (int16_t)(ax), (int16_t)(y + h - PS_STROKE) };
        ps_paint_push_clip(p, &cr);
        draw_text_in(p, x + PS_CTL_PAD_X, baseline, m_display, PS_C_FIELD_TEXT);
        ps_paint_pop_clip(p);

        /* Drop arrow: a stack of shrinking rows, which stays crisp at 480i
         * where a diagonal edge would crawl. */
        fill(p, ax, y + PS_STROKE, PS_CTL_BOX - PS_STROKE, h - PS_STROKE * 2,
             PS_C_BTN_FACE);
        for(i = 0; i < 6; i++)
            fill(p, ax + 4 + i, y + h / 2 - 3 + i, (6 - i) * 2, 1,
                 PS_C_FIELD_TEXT);
    }

    host       *m_host;
    ctl_type    m_type;
    std::string m_name;
    std::string m_value;
    std::string m_display;   /* select: text of the chosen option */
    std::string m_default;   /* textarea: original child text, for reset */
    std::vector<option> m_options;
    int         m_sel         = 0;
    int         m_default_sel = 0;
    bool        m_checked  = false;
    bool        m_disabled = false;
    int         m_cols     = PS_CTL_COLS;
    int         m_rows     = PS_CTL_ROWS;
    int         m_widest   = 1;

public:
    /* Marks this subclass without RTTI. */
    static const int PS_CONTROL_MAGIC = 0x54435350; /* 'PSCT' */
    int magic = PS_CONTROL_MAGIC;
};

/* -fno-rtti rules out dynamic_cast, so the downcast is guarded two ways: the
 * tag must be one we create elements for, and the object must carry our magic.
 * Same approach as the is_html_tag() patch in the litehtml fork. */
inline el_control *control_of(litehtml::element *el)
{
    if(!el)
        return nullptr;

    litehtml::string_id t = el->tag();
    if(t != litehtml::_input_ && t != litehtml::_select_ &&
       t != litehtml::_textarea_ && t != litehtml::_button_)
        return nullptr;

    auto *c = static_cast<el_control *>(el);
    return c->magic == el_control::PS_CONTROL_MAGIC ? c : nullptr;
}

inline void el_control::clear_radios_in(litehtml::element *el)
{
    for(const auto &child : el->children()) {
        auto *c = control_of(child.get());

        if(c && c->m_type == CTL_RADIO && c->m_name == m_name)
            c->m_checked = false;

        clear_radios_in(child.get());
    }
}

} /* namespace psctl */

#endif /* PS_CONTROLS_H */
