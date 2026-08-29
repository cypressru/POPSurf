/* litehtml document container and POPSurf element extensions. */
#include "ps_document.h"
#include "../net/ps_url.h"

#include <litehtml.h>

#include "ps_controls.h"
#include "ps_legacy.h"
#include "ps_imagemap.h"
#include "ps_frames.h"
#include "ps_appletel.h"
#include "ps_swfel.h"
#include "../java/ps_applet.h"

#include <cstdio>
#include <cstring>
#include <cctype>
#include <string>
#include <memory>
#include <vector>

/* Defined with the tree scan that is its main caller; declared here because
 * create_element needs the same answer several hundred lines earlier. */
static bool url_is_swf(const char *u);

namespace {

ps_color to_ps_color(const litehtml::web_color &c)
{
    return PS_ARGB(c.alpha, c.red, c.green, c.blue);
}

/* Minimum readable size at 480i. */
const int PS_MIN_FONT_PX = 14;

/* Ceiling on tiles for one background layer. */
const long PS_BG_MAX_TILES = 4096;

class ps_container : public litehtml::document_container, public psctl::host,
                     public psapp::host {
public:
    /* --- psctl::host --------------------------------------------------- */

    ps_paint      *paint() override { return m_paint; }
    ps_text_cache *text() override  { return m_text; }

    void focus_control(litehtml::element *el) override { m_focus = el; }

    long anim_ms() const override { return m_anim_ms; }
    int  click_x() const override { return m_click_x; }
    int  click_y() const override { return m_click_y; }

    void navigate(const char *href) override
    {
        std::string abs;

        if(m_nav && resolve(href, nullptr, abs))
            m_nav(m_nav_user, abs.c_str(), nullptr);
    }

    /* --- psapp::host ----------------------------------------------------
     *
     * An applet's code= is relative to codebase=, which is itself relative to
     * the page - two levels of resolution rather than one, which is why this
     * does not just call resolve() on the raw attribute. */
    bool applet_frame(const std::string &code, const std::string &codebase,
                      const std::string &archive, int w, int h,
                      ps_texture *tex, float *u1, float *v1) override
    {
        std::string url, jar;

        if(!m_applets || !applet_url(code, codebase, url))
            return false;

        /* archive= is a comma-separated list in the spec; the first entry is
         * the one that carries the applet in every real page. */
        if(!archive.empty()) {
            std::string first = archive.substr(0, archive.find(','));

            applet_url(first, codebase, jar);
        }

        const ps_applet_view *v = ps_applet_get(m_applets, url.c_str(),
                                                jar.c_str(), w, h);

        /* Where it sits in the document. Whether that is on screen is worked
         * out per frame from the scroll position, because a page is now drawn
         * once and replayed at every scroll offset. */
        ps_applet_set_box(m_applets, url.c_str(), m_vis_y, m_vis_h);

        if(!v || !v->ready)
            return false;

        *tex = v->tex;
        *u1  = v->u1;
        *v1  = v->v1;
        return true;
    }

    /* The element's <param> children, and the page's own URL for
     * getDocumentBase. The applet cache keeps the first set it is given, so
     * calling this on every draw costs one pass over a short list. */
    void applet_params(const std::string &code, const std::string &codebase,
                       const psapp::param_list &params) override
    {
        std::string url;
        const char *names[PS_APPLET_PARAMS], *values[PS_APPLET_PARAMS];
        char        page[PS_URL_MAX];
        int         n = 0;

        if(!m_applets || !applet_url(code, codebase, url))
            return;

        /* getDocumentBase is the document's URL, filename and all. m_base is
         * the parsed form of exactly that, so it is formatted back rather
         * than kept twice. */
        if(!m_have_base || ps_url_format(&m_base, page, sizeof page) < 0)
            page[0] = '\0';

        for(const auto &kv : params) {
            if(n >= PS_APPLET_PARAMS)
                break;
            names[n]  = kv.first.c_str();
            values[n] = kv.second.c_str();
            n++;
        }

        ps_applet_set_params(m_applets, url.c_str(), page, names, values, n);
    }

    /* Set by the element just before it asks for a frame, because that is the
     * only moment its position on screen is known. */
    void applet_screen_box(int y, int h) { m_vis_y = y; m_vis_h = h; }

    /* The two shapes ps_jgfx reduces everything to, forwarded into the paint
     * layer. Static because the vtable behind them is C. */
    struct vec_ctx { ps_paint *p; ps_text_cache *t; };

    static void vec_rect(void *user, int x0, int y0, int x1, int y1,
                         uint32_t argb)
    {
        vec_ctx *c = (vec_ctx *)user;
        ps_rect  r = { (int16_t)x0, (int16_t)y0, (int16_t)x1, (int16_t)y1 };

        ps_paint_rect(c->p, &r, argb);
    }

    static void vec_text(void *user, int x, int baseline, const char *utf8,
                         size_t len, uint32_t argb, int px_size)
    {
        vec_ctx *c = (vec_ctx *)user;
        ps_font *f = ps_text_font(c->t, px_size);

        /* The page's own glyph atlas: applet text costs what the page's text
         * costs, which is textured quads and no rasterising at all. */
        if(f)
            ps_font_draw(c->p, f, x, baseline, utf8, len, argb);
    }

    static int vec_measure(void *user, const char *utf8, size_t len,
                           int px_size)
    {
        vec_ctx *c = (vec_ctx *)user;
        ps_font *f = ps_text_font(c->t, px_size);

        return f ? ps_font_measure(f, utf8, len) : 0;
    }

    bool applet_paint(const std::string &code, const std::string &codebase,
                      int x, int y, int w, int h) override
    {
        static const ps_jvec_ops ops = { vec_rect, vec_text, vec_measure };
        std::string url;
        vec_ctx     ctx;
        bool        ok;

        if(!m_applets || !applet_url(code, codebase, url))
            return false;

        ctx.p = m_paint;
        ctx.t = m_text;

        /* Clipped here as well as inside the applet. Its own clip already
         * contains it; a page scrolling it half off the screen is this side's
         * business. */
        {
            ps_rect box = { (int16_t)x, (int16_t)y,
                            (int16_t)(x + w), (int16_t)(y + h) };

            ps_paint_push_clip(m_paint, &box);
            ok = ps_applet_draw(m_applets, url.c_str(), &ops, &ctx,
                                x, y, w, h) != 0;
            ps_paint_pop_clip(m_paint);
        }
        return ok;
    }

    bool applet_input(const std::string &code, const std::string &codebase,
                      int x, int y, int down, int dragging) override
    {
        std::string url;

        if(!m_applets || !applet_url(code, codebase, url))
            return false;

        return ps_applet_mouse(m_applets, url.c_str(), x, y, down,
                               dragging) != 0;
    }

    void applet_hover(const std::string &code, const std::string &codebase,
                      int x, int y) override
    {
        std::string url;

        if(!m_applets)
            return;
        if(!applet_url(code, codebase, url)) {
            applet_hover_none();
            return;
        }
        ps_applet_set_hover(m_applets, url.c_str(), x, y);
    }

    void applet_hover_none() override
    {
        if(m_applets)
            ps_applet_set_hover(m_applets, nullptr, 0, 0);
    }

    std::string applet_why(const std::string &code,
                           const std::string &codebase) override
    {
        std::string url;

        if(code.empty())
            return "no code attribute";
        if(!m_applets)
            return "no applet support in this build";
        if(!applet_url(code, codebase, url))
            return "cannot resolve " + code;

        return ps_applet_status(m_applets, url.c_str());
    }

    void set_applets(ps_applet_cache *a) { m_applets = a; }
    ps_applet_cache *applets() const { return m_applets; }

    /* The applets on this page, in creation order.
     *
     * Kept because -fno-rtti rules out finding them again with a dynamic_cast
     * over the element tree, and because the container is the one thing that
     * knows it made them. Held as shared_ptr so a stale entry cannot dangle;
     * the list is dropped when a new document is parsed. */
    std::shared_ptr<psapp::el_applet> remember(
        const std::shared_ptr<psapp::el_applet> &el)
    {
        m_applet_els.push_back(el);
        return el;
    }

    void forget_applets() { m_applet_els.clear(); }

    /* Last created first: a later applet in the source paints over an earlier
     * one if they overlap, so it should also be the one that takes the
     * click. */
    bool applet_input_at(int x, int y, int down, int dragging)
    {
        for(auto it = m_applet_els.rbegin(); it != m_applet_els.rend(); ++it) {
            if(*it && (*it)->offer_input(x, y, down, dragging))
                return true;
        }
        return false;
    }

    /* Topmost first, same order and for the same reason. Somebody has to be
     * told even when the answer is nobody, or an applet the pointer has just
     * left never hears that it was left. */
    void applet_hover_at(int x, int y)
    {
        for(auto it = m_applet_els.rbegin(); it != m_applet_els.rend(); ++it) {
            if(*it && (*it)->offer_hover(x, y))
                return;
        }
        applet_hover_none();
    }

    void set_click(int x, int y) { m_click_x = x; m_click_y = y; }
    void tick(int dt_ms) { m_anim_ms += dt_ms; }
    void clear_focus() { m_focus = nullptr; }

    int focused_editable() const
    {
        auto *c = psctl::control_of(m_focus);
        return c && c->editable() ? 1 : 0;
    }

    const char *focused_value() const
    {
        auto *c = psctl::control_of(m_focus);
        if(!c)
            return "";
        m_scratch = c->ctl_value();
        return m_scratch.c_str();
    }

    const char *focused_label() const
    {
        auto *c = psctl::control_of(m_focus);
        if(!c)
            return "";

        /* The control's name is the only label we reliably have without
         * resolving <label for>, and it is usually descriptive enough. */
        m_scratch2 = c->ctl_name().empty() ? std::string("Text")
                                           : c->ctl_name();
        return m_scratch2.c_str();
    }

    bool set_focused_value(const char *text)
    {
        auto *c = psctl::control_of(m_focus);

        if(!c || !c->editable())
            return false;
        c->set_value(text);
        return true;
    }

    bool is_focused(const litehtml::element *el) const override
    {
        return m_focus == el;
    }

    /* Submits successful controls to the owning form, or resets them. */
    void submit(litehtml::element *from, bool reset) override
    {
        litehtml::element::ptr form;

        for(litehtml::element::ptr p = from->parent(); p; p = p->parent()) {
            if(p->tag() == litehtml::_form_) {
                form = p;
                break;
            }
        }
        if(!form)
            return;

        if(reset) {
            reset_controls(form.get());
            return;
        }

        const char *action = form->get_attr("action", "");
        const char *method = form->get_attr("method", "get");
        std::string query;
        bool        post = method && (method[0] == 'p' || method[0] == 'P');

        gather(form.get(), query);

        /* The submit button that was actually pressed is successful if it has
         * a name. Guestbooks and other PHP forms routinely gate on
         * isset($_POST['submit']), so omitting it silently does nothing. */
        {
            auto *btn = psctl::control_of(from);

            if(btn && !btn->ctl_name().empty()) {
                if(!query.empty())
                    query += '&';
                form_encode(btn->ctl_name(), query);
                query += '=';
                form_encode(btn->ctl_value(), query);
            }
        }

        std::string target = (action && *action) ? action : "";
        if(target.empty())
            target = m_have_base ? m_base.path : "/";

        /* A form action may already carry a query string; for GET the form
         * data replaces it. */
        if(!post) {
            size_t q = target.find('?');
            if(q != std::string::npos)
                target.erase(q);
            if(!query.empty())
                target += "?" + query;
        }

        std::string abs;
        if(m_nav && resolve(target.c_str(), nullptr, abs))
            m_nav(m_nav_user, abs.c_str(), post ? query.c_str() : nullptr);
    }


    ps_container(ps_paint *paint, ps_text_cache *text, ps_image_cache *images,
                 int width, int height)
        : m_paint(paint), m_text(text), m_images(images), m_width(width),
          m_height(height)
    {
    }

    /* codebase is a directory and must end in a slash before code is
     * resolved against it, or "classes" + "Foo.class" resolves as a sibling
     * rather than a child - which silently fetches the wrong URL. */
    bool applet_url(const std::string &code, const std::string &codebase,
                    std::string &out)
    {
        if(code.empty())
            return false;

        std::string rel = code;

        if(!codebase.empty()) {
            std::string base = codebase;

            if(base.back() != '/')
                base += '/';
            rel = base + code;
        }
        return resolve(rel.c_str(), nullptr, out);
    }

    void set_base(const char *base_url)
    {
        if(base_url && ps_url_parse(&m_base, base_url) == 0)
            m_have_base = true;
    }

    /* The layout viewport, which is what vh units and media queries are
     * measured against. It changes when chrome takes a band off an edge. */
    void set_view_size(int width, int height)
    {
        m_width  = width;
        m_height = height;
    }

    litehtml::uint_ptr create_font(const litehtml::font_description &descr,
                                   const litehtml::document *,
                                   litehtml::font_metrics *fm) override
    {
        int px = (int)descr.size;

        if(px < PS_MIN_FONT_PX)
            px = PS_MIN_FONT_PX;

        /* Family and weight are ignored: one face is baked, and synthesising
         * bold would change metrics without matching any real font. */
        ps_font *f = ps_text_font(m_text, px);

        /* The font table is small and fixed. When it is full, fall back to the
         * default size rather than hand back a null the caller will not
         * expect. */
        if(!f)
            f = ps_text_font(m_text, 16);

        if(fm) {
            ps_font_metrics m;
            memset(&m, 0, sizeof m);
            if(f)
                ps_font_get_metrics(f, &m);

            fm->font_size   = (litehtml::pixel_t)px;
            fm->height      = (litehtml::pixel_t)m.height;
            fm->ascent      = (litehtml::pixel_t)m.ascent;
            fm->descent     = (litehtml::pixel_t)m.descent;
            fm->x_height    = (litehtml::pixel_t)m.x_height;
            fm->ch_width    =
                (litehtml::pixel_t)(f ? ps_font_measure(f, "0", 1) : 0);
            fm->draw_spaces = false;
            fm->sub_shift   = (litehtml::pixel_t)(m.descent / 2);
            fm->super_shift = (litehtml::pixel_t)(m.ascent / 3);
        }

        return (litehtml::uint_ptr)f;
    }

    void delete_font(litehtml::uint_ptr) override
    {
        /* Fonts belong to the text cache and are shared by every element of
         * the same size, so one element releasing a handle means nothing. */
    }

    litehtml::pixel_t text_width(const char *text,
                                 litehtml::uint_ptr hFont) override
    {
        const ps_font *f = (const ps_font *)hFont;

        if(!f || !text)
            return 0;
        return (litehtml::pixel_t)ps_font_measure(f, text, strlen(text));
    }

    void draw_text(litehtml::uint_ptr, const char *text,
                   litehtml::uint_ptr hFont, litehtml::web_color color,
                   const litehtml::position &pos) override
    {
        const ps_font  *f = (const ps_font *)hFont;
        ps_font_metrics m;

        if(!f || !text || !*text)
            return;

        memset(&m, 0, sizeof m);
        ps_font_get_metrics(f, &m);

        /* litehtml gives the text box; the glyph origin is its baseline. */
        ps_font_draw(m_paint, f, round_px((float)pos.x),
                     round_px((float)pos.y) + m.ascent,
                     text, strlen(text), to_ps_color(color));
    }

    litehtml::pixel_t pt_to_px(float pt) const override
    {
        /* 96 CSS px per inch, 72 pt per inch. */
        return (litehtml::pixel_t)(int)(pt * 96.0f / 72.0f + 0.5f);
    }

    litehtml::pixel_t get_default_font_size() const override
    {
        return (litehtml::pixel_t)16;
    }

    const char *get_default_font_name() const override
    {
        return "sans-serif";
    }

    void draw_list_marker(litehtml::uint_ptr,
                          const litehtml::list_marker &marker) override
    {
        /* A filled box for every marker type. Discs and glyph markers need a
         * shape rasterizer, and at 480i a real disc barely resolves anyway. */
        ps_rect r = to_rect(marker.pos);
        ps_paint_rect(m_paint, &r, to_ps_color(marker.color));
    }

    void load_image(const char *src, const char *baseurl, bool) override
    {
        /* Only queues the fetch. The bytes arrive later and the shell re-runs
         * layout, which is what keeps a load from blocking the frame. */
        std::string url;

        if(m_images && resolve(src, baseurl, url))
            ps_image_get(m_images, url.c_str());
    }

    void get_image_size(const char *src, const char *baseurl,
                        litehtml::size &sz) override
    {
        std::string     url;
        const ps_image *img = NULL;

        if(m_images && resolve(src, baseurl, url))
            img = ps_image_get(m_images, url.c_str());

        /* Zero size lays the element out as an empty inline box instead of
         * reserving space for something that never arrives. */
        sz.width  = img ? (litehtml::pixel_t)img->w : (litehtml::pixel_t)0;
        sz.height = img ? (litehtml::pixel_t)img->h : (litehtml::pixel_t)0;
    }

    void draw_image(litehtml::uint_ptr, const litehtml::background_layer &layer,
                    const std::string &url_in,
                    const std::string &base_url) override
    {
        std::string     url;
        const ps_image *img;

        if(!m_images || !resolve(url_in.c_str(), base_url.c_str(), url))
            return;

        img = ps_image_peek(m_images, url.c_str());
        if(!img || img->nframes < 1)
            return;

        const ps_image_frame *fr = &img->frames[img->cur_frame];
        ps_rect               r  = to_rect(layer.border_box);

#ifdef PS_DEBUG_IMAGES
        printf("img %s intrinsic=%dx%d border=(%d,%d %dx%d) "
               "origin=(%d,%d %dx%d) clip=(%d,%d %dx%d) uv=%.4f,%.4f\n",
               url.c_str(), img->w, img->h,
               (int)layer.border_box.x, (int)layer.border_box.y,
               (int)layer.border_box.width, (int)layer.border_box.height,
               (int)layer.origin_box.x, (int)layer.origin_box.y,
               (int)layer.origin_box.width, (int)layer.origin_box.height,
               (int)layer.clip_box.x, (int)layer.clip_box.y,
               (int)layer.clip_box.width, (int)layer.clip_box.height,
               (double)fr->u1, (double)fr->v1);
#endif

        (void)r;
        draw_bg_layer(layer, img, fr);
    }

    /* Tiled background painting.
     *
     * origin_box is where the first tile lands, clip_box bounds the whole
     * thing, and repeat says which axes continue. Ignoring repeat was fine
     * while every image was an <img>, but a tiled background is the single
     * most characteristic thing about a page of this era. */
    void draw_bg_layer(const litehtml::background_layer &layer,
                       const ps_image *img, const ps_image_frame *fr)
    {
        const ps_color white = PS_ARGB(255, 255, 255, 255);

        ps_rect clip   = to_rect(layer.clip_box);
        ps_rect origin = to_rect(layer.origin_box);
        int     iw     = img->w;
        int     ih     = img->h;

        bool rep_x = layer.repeat == litehtml::background_repeat_repeat ||
                     layer.repeat == litehtml::background_repeat_repeat_x;
        bool rep_y = layer.repeat == litehtml::background_repeat_repeat ||
                     layer.repeat == litehtml::background_repeat_repeat_y;

        if(iw <= 0 || ih <= 0 || ps_rect_empty(&clip))
            return;

        if(!rep_x && !rep_y) {
            ps_rect r = origin;

            r.x1 = (int16_t)(origin.x0 + iw);
            r.y1 = (int16_t)(origin.y0 + ih);
            ps_paint_push_clip(m_paint, &clip);
            ps_paint_image(m_paint, fr->tex, &r, 0.0f, 0.0f, fr->u1, fr->v1,
                           white);
            ps_paint_pop_clip(m_paint);
            return;
        }

        /* A one-pixel strip repeated along an axis is the classic gradient
         * background, and tiling it literally would be one quad per pixel.
         * Stretching is pixel-identical there and costs a single quad.
         *
         * An axis that does *not* repeat keeps the image's natural size:
         * repeat-y on a 1px-wide strip is a hairline running down the box, not
         * a strip stretched across it. */
        bool span_x = rep_x && iw == 1;
        bool span_y = rep_y && ih == 1;

        int step_x = span_x ? (clip.x1 - clip.x0) : iw;
        int step_y = span_y ? (clip.y1 - clip.y0) : ih;

        if(step_x <= 0 || step_y <= 0)
            return;

        int sx = span_x ? clip.x0
                        : (rep_x ? tile_start(origin.x0, clip.x0, iw)
                                 : origin.x0);
        int sy = span_y ? clip.y0
                        : (rep_y ? tile_start(origin.y0, clip.y0, ih)
                                 : origin.y0);

        /* Hard cap. A tiny tile over a tall page is otherwise tens of
         * thousands of quads, which is a denial of service dressed as a
         * background. */
        long cols = ((long)clip.x1 - sx + step_x - 1) / step_x;
        long rows = ((long)clip.y1 - sy + step_y - 1) / step_y;

        if(cols < 1) cols = 1;
        if(rows < 1) rows = 1;
        if(cols * rows > PS_BG_MAX_TILES)
            return;

        ps_paint_push_clip(m_paint, &clip);

        for(int y = sy; y < clip.y1; y += step_y) {
            for(int x = sx; x < clip.x1; x += step_x) {
                ps_rect r;

                r.x0 = (int16_t)x;
                r.y0 = (int16_t)y;
                r.x1 = (int16_t)(x + step_x);
                r.y1 = (int16_t)(y + step_y);

                ps_paint_image(m_paint, fr->tex, &r, 0.0f, 0.0f, fr->u1,
                               fr->v1, white);

                if(!rep_x)
                    break;
            }
            if(!rep_y)
                break;
        }

        ps_paint_pop_clip(m_paint);
    }

    /* Largest position <= lo that is congruent to origin modulo step, so tiles
     * stay aligned to the origin however far the clip extends past it. */
    static int tile_start(int origin, int lo, int step)
    {
        int d = lo - origin;
        int m = step ? d % step : 0;

        if(m < 0)
            m += step;
        return lo - m;
    }

    void draw_solid_fill(litehtml::uint_ptr,
                         const litehtml::background_layer &layer,
                         const litehtml::web_color &color) override
    {
        ps_rect r = to_rect(layer.border_box);
        ps_paint_rect(m_paint, &r, to_ps_color(color));
    }

    /* Gradients collapse to their first stop. Banding one across quads is
     * cheap to add later; a readable page matters more now. */
    void draw_linear_gradient(
        litehtml::uint_ptr, const litehtml::background_layer &layer,
        const litehtml::background_layer::linear_gradient &g) override
    {
        flat_gradient(layer, g.color_points);
    }

    void draw_radial_gradient(
        litehtml::uint_ptr, const litehtml::background_layer &layer,
        const litehtml::background_layer::radial_gradient &g) override
    {
        flat_gradient(layer, g.color_points);
    }

    void draw_conic_gradient(
        litehtml::uint_ptr, const litehtml::background_layer &layer,
        const litehtml::background_layer::conic_gradient &g) override
    {
        flat_gradient(layer, g.color_points);
    }

    void draw_borders(litehtml::uint_ptr, const litehtml::borders &borders,
                      const litehtml::position &draw_pos, bool) override
    {
        ps_rect box = to_rect(draw_pos);
        ps_rect r;

        /* Four rects, with the corners owned by the horizontal edges. Mitred
         * joins only show on thick multi-colour borders, which pages rarely
         * use and 480i would not resolve. Radii are ignored. */
        int top    = (int)borders.top.width;
        int bottom = (int)borders.bottom.width;
        int left   = (int)borders.left.width;
        int right  = (int)borders.right.width;

        if(top > 0 && borders.top.style > litehtml::border_style_hidden) {
            r = box;
            r.y1 = (int16_t)(box.y0 + top);
            ps_paint_rect(m_paint, &r, to_ps_color(borders.top.color));
        }
        if(bottom > 0 && borders.bottom.style > litehtml::border_style_hidden) {
            r = box;
            r.y0 = (int16_t)(box.y1 - bottom);
            ps_paint_rect(m_paint, &r, to_ps_color(borders.bottom.color));
        }
        if(left > 0 && borders.left.style > litehtml::border_style_hidden) {
            r = box;
            r.x1 = (int16_t)(box.x0 + left);
            r.y0 = (int16_t)(box.y0 + top);
            r.y1 = (int16_t)(box.y1 - bottom);
            ps_paint_rect(m_paint, &r, to_ps_color(borders.left.color));
        }
        if(right > 0 && borders.right.style > litehtml::border_style_hidden) {
            r = box;
            r.x0 = (int16_t)(box.x1 - right);
            r.y0 = (int16_t)(box.y0 + top);
            r.y1 = (int16_t)(box.y1 - bottom);
            ps_paint_rect(m_paint, &r, to_ps_color(borders.right.color));
        }
    }

    /* litehtml asks us first and falls back to its own element on null. This
     * is how the form controls get in: litehtml has no implementation for any
     * of these tags beyond styling them. */
    litehtml::element::ptr create_element(
        const char *tag_name, const litehtml::string_map &attributes,
        const std::shared_ptr<litehtml::document> &doc) override
    {
        using namespace psctl;

        if(!tag_name)
            return nullptr;

        if(!strcmp(tag_name, "input")) {
            auto it = attributes.find("type");
            std::string t = (it == attributes.end()) ? "text" : it->second;

            for(auto &c : t)
                c = (char)tolower((unsigned char)c);

            ctl_type ct = CTL_TEXT;
            if(t == "password")      ct = CTL_PASSWORD;
            else if(t == "checkbox") ct = CTL_CHECKBOX;
            else if(t == "radio")    ct = CTL_RADIO;
            else if(t == "submit")   ct = CTL_SUBMIT;
            else if(t == "reset")    ct = CTL_RESET;
            else if(t == "button")   ct = CTL_BUTTON;
            else if(t == "hidden")   ct = CTL_HIDDEN;
            else if(t == "file")     ct = CTL_FILE;
            else if(t == "image")    ct = CTL_SUBMIT;

            return std::make_shared<el_control>(doc, this, ct);
        }

        /* Anything that moves by itself rules out replaying the page. */
        if(!strcmp(tag_name, "marquee") || !strcmp(tag_name, "blink") ||
           !strcmp(tag_name, "input")   || !strcmp(tag_name, "textarea"))
            m_animated = true;

        if(!strcmp(tag_name, "applet"))
            return remember(std::make_shared<psapp::el_applet>(doc, this));

        /* <object> and <embed> carried applets too, and <object> is what the
         * later authoring tools emitted - a browser that only knows <applet>
         * misses a good share of what survives. Only claim them when they
         * actually name Java, or this would swallow every Flash movie on the
         * page. */
        if((!strcmp(tag_name, "object") || !strcmp(tag_name, "embed")) &&
           psapp::is_java_object(attributes))
            return remember(std::make_shared<psapp::el_applet>(doc, this));

        /* And the Flash half of the same two tags. A box only - see
         * ps_swfel.h - and claimed only when an attribute actually names a
         * movie, which is why the attributes are read here rather than the
         * element deciding for itself later. */
        if(!strcmp(tag_name, "object") || !strcmp(tag_name, "embed")) {
            auto s = attributes.find("src");
            auto d = attributes.find("data");

            if((s != attributes.end() && url_is_swf(s->second.c_str())) ||
               (d != attributes.end() && url_is_swf(d->second.c_str())))
                return std::make_shared<psswf::el_movie>(doc);
        }

        if(!strcmp(tag_name, "textarea"))
            return std::make_shared<el_control>(doc, this, CTL_TEXTAREA);
        if(!strcmp(tag_name, "select"))
            return std::make_shared<el_control>(doc, this, CTL_SELECT);
        if(!strcmp(tag_name, "button"))
            return std::make_shared<el_control>(doc, this, CTL_BUTTON);

        /* Legacy presentational elements litehtml does not implement. */
        /* usemap is inert in litehtml, and slicing one picture into a
         * navigation bar is a defining idiom of this era. */
        if(!strcmp(tag_name, "img"))
            return std::make_shared<psmap::el_image_map>(doc, this);

        if(!strcmp(tag_name, "marquee"))
            return std::make_shared<psleg::el_marquee>(doc, this);
        if(!strcmp(tag_name, "blink"))
            return std::make_shared<psleg::el_blink>(doc, this);

        /* Null means "use litehtml's own element", which is right everywhere
         * else. */
        return nullptr;
    }

    void set_caption(const char *caption) override
    {
        m_caption = caption ? caption : "";
    }

    void set_base_url(const char *base_url) override
    {
        m_base_url = base_url ? base_url : "";
    }

    void link(const std::shared_ptr<litehtml::document> &,
              const litehtml::element::ptr &) override
    {
    }

    void set_navigate_cb(ps_navigate_fn cb, void *user)
    {
        m_nav      = cb;
        m_nav_user = user;
    }

    void on_anchor_click(const char *url, const litehtml::element::ptr &) override
    {
        std::string abs;

        if(!m_nav || !resolve(url, nullptr, abs))
            return;

        /* Only hands the URL up. Loading here would destroy the document
         * litehtml is still walking. */
        m_nav(m_nav_user, abs.c_str(), nullptr);
    }

    void on_mouse_event(const litehtml::element::ptr &,
                        litehtml::mouse_event) override
    {
    }

    /* litehtml reports the computed CSS cursor for whatever is under the last
     * hit test. Keeping the whole keyword rather than just a clickable flag
     * lets the cursor art follow the page: a text field gets an I-beam, a
     * resize handle gets the right arrow, and so on. */
    void set_cursor(const char *cursor) override
    {
        std::string next = cursor ? cursor : "";

#ifdef PS_APPLET_PROFILE
        /* The hover ring, and the pointer art, both hang on this string being
         * "pointer" over a link. Two attempts at the ring have now failed and
         * neither established whether it ever is. */
        if(next != m_cursor_css)
            printf("popsurf: cursor '%s'\n", next.c_str());
#endif

        m_cursor_css     = next;
        m_cursor_is_link = m_cursor_css == "pointer";
    }

    bool cursor_is_link() const { return m_cursor_is_link; }
    const std::string &cursor_css() const { return m_cursor_css; }

    void transform_text(std::string &text,
                        litehtml::text_transform tt) override
    {
        /* ASCII only: applying this to UTF-8 continuation bytes would corrupt
         * them, so anything above 0x7f is left alone. */
        switch(tt) {
        case litehtml::text_transform_uppercase:
            for(char &c : text)
                if((unsigned char)c < 0x80)
                    c = (char)toupper((unsigned char)c);
            break;
        case litehtml::text_transform_lowercase:
            for(char &c : text)
                if((unsigned char)c < 0x80)
                    c = (char)tolower((unsigned char)c);
            break;
        case litehtml::text_transform_capitalize:
            if(!text.empty() && (unsigned char)text[0] < 0x80)
                text[0] = (char)toupper((unsigned char)text[0]);
            break;
        default:
            break;
        }
    }

    void import_css(std::string &text, const std::string &,
                    std::string &) override
    {
        /* External stylesheets need the network layer. Empty makes the @import
         * a no-op rather than a stall. */
        text.clear();
    }

    void set_clip(const litehtml::position &pos,
                  const litehtml::border_radiuses &) override
    {
        ps_rect r = to_rect(pos);
        ps_paint_push_clip(m_paint, &r);
    }

    void del_clip() override { ps_paint_pop_clip(m_paint); }

    void get_viewport(litehtml::position &viewport) const override
    {
        viewport.x      = 0;
        viewport.y      = 0;
        viewport.width  = (litehtml::pixel_t)m_width;
        viewport.height = (litehtml::pixel_t)m_height;
    }

    void get_media_features(litehtml::media_features &media) const override
    {
        litehtml::position client;
        get_viewport(client);

        media.type          = litehtml::media_type_screen;
        media.width         = client.width;
        media.height        = client.height;
        media.device_width  = (litehtml::pixel_t)m_width;
        media.device_height = (litehtml::pixel_t)m_height;
        media.color         = 8;
        media.monochrome    = 0;
        media.color_index   = 0;
        media.resolution    = 96;
    }

    void get_language(std::string &language, std::string &culture) const override
    {
        language = "en";
        culture  = "";
    }

    const std::string &caption() const { return m_caption; }

private:
    /* Rounds each edge from its absolute coordinate rather than adding a
     * truncated origin to a truncated size.
     *
     * litehtml lays out in floats. A box at x=100.6 of width 40.6 truncates to
     * 100..140, while the box that begins exactly where it ends, at 141.2,
     * truncates to 141 - and the page background shows through the 1px seam
     * between them. Deriving both edges from the same absolute number makes
     * adjacent boxes land on the same integer and meet exactly. */
    static int round_px(float v)
    {
        /* Explicit rather than lrintf: this must round half away from zero on
         * every target regardless of the FPU rounding mode, or two consoles
         * disagree on where a box edge lands. */
        return (int)(v < 0.0f ? v - 0.5f : v + 0.5f);
    }

    ps_rect to_rect(const litehtml::position &p) const
    {
        float   x0 = p.x, y0 = p.y;
        ps_rect r;

        r.x0 = (int16_t)round_px(x0);
        r.y0 = (int16_t)round_px(y0);
        r.x1 = (int16_t)round_px(x0 + (float)p.width);
        r.y1 = (int16_t)round_px(y0 + (float)p.height);
        return r;
    }

    void flat_gradient(
        const litehtml::background_layer &layer,
        const std::vector<litehtml::background_layer::color_point> &stops)
    {
        if(stops.empty())
            return;

        ps_rect r = to_rect(layer.border_box);
        ps_paint_rect(m_paint, &r, to_ps_color(stops[0].color));
    }

    /* application/x-www-form-urlencoded. Space becomes '+', and everything
     * outside the unreserved set is percent-encoded; anything less and a form
     * with punctuation in it silently submits the wrong thing. */
    static void form_encode(const std::string &in, std::string &out)
    {
        static const char hex[] = "0123456789ABCDEF";

        for(unsigned char c : in) {
            if((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
               c == '~') {
                out += (char)c;
            }
            else if(c == ' ') {
                out += '+';
            }
            else {
                out += '%';
                out += hex[c >> 4];
                out += hex[c & 0x0f];
            }
        }
    }

    void gather(litehtml::element *el, std::string &query)
    {
        for(const auto &child : el->children()) {
            auto *c = psctl::control_of(child.get());

            if(c && c->successful()) {
                if(!query.empty())
                    query += '&';
                form_encode(c->ctl_name(), query);
                query += '=';
                form_encode(c->ctl_value(), query);
            }

            gather(child.get(), query);
        }
    }

    void reset_controls(litehtml::element *el)
    {
        for(const auto &child : el->children()) {
            auto *c = psctl::control_of(child.get());

            if(c)
                c->reset_to_default();
            reset_controls(child.get());
        }
    }

public:
    bool resolve_public(const char *src, std::string &out) const
    {
        return resolve(src, nullptr, out);
    }

private:
    /* Turns a possibly-relative src into an absolute URL. Falls back to the
     * document base when litehtml has no baseurl for the element. */
    bool resolve(const char *src, const char *baseurl, std::string &out) const
    {
        ps_url ref, base;
        char   buf[PS_URL_MAX];

        if(!src || !*src)
            return false;

        if(baseurl && *baseurl && ps_url_parse(&base, baseurl) == 0) {
            /* fall through with this base */
        }
        else if(m_have_base) {
            base = m_base;
        }
        else {
            return false;
        }

        if(ps_url_resolve(&ref, &base, src) != 0)
            return false;
        if(ps_url_format(&ref, buf, sizeof buf) < 0)
            return false;

        out = buf;
        return true;
    }

    ps_paint        *m_paint;
    ps_text_cache   *m_text;
    ps_image_cache  *m_images;
    ps_applet_cache *m_applets = nullptr;
    bool             m_animated = false;

public:
    bool animated() const { return m_animated; }
    void clear_animated() { m_animated = false; }

private:
    std::vector<std::shared_ptr<psapp::el_applet>> m_applet_els;
    int m_vis_y = 0, m_vis_h = 0;
    int             m_width, m_height;
    std::string     m_caption;
    std::string     m_base_url;
    ps_url          m_base {};
    bool            m_have_base = false;
    bool            m_cursor_is_link = false;
    std::string     m_cursor_css;
    ps_navigate_fn  m_nav = nullptr;
    void           *m_nav_user = nullptr;

    /* Focused form control. Raw pointer: the document owns the element tree
     * and outlives this, and clearing on load keeps it from dangling. */
    litehtml::element *m_focus = nullptr;
    long               m_anim_ms = 0;
    int                m_click_x = 0, m_click_y = 0;

    /* Backing store for the C string accessors, which must outlive the call
     * but cannot hand out a pointer into a temporary. */
    mutable std::string m_scratch, m_scratch2;
};

} /* namespace */

/* The C handle owns both halves so callers never see a litehtml type. */
/* One independently scrolling, independently loaded document.
 *
 * A frameset page is several of these side by side. Each needs its own
 * container because the container carries the base URL and the viewport size,
 * and both differ per frame. */
struct ps_frame {
    std::unique_ptr<ps_container> container;
    litehtml::document::ptr       doc;

    ps_rect     rect {};
    std::string src;         /* as authored, resolved against the page base */
    std::string url;         /* absolute, once known */
    std::string name;
    int         scroll_y = 0;
    int         height   = 0;
    int         scrolling = 1;
    bool        requested = false;
    bool        loaded    = false;
};

struct ps_document {
    ps_container            container;
    litehtml::document::ptr doc;
    int                     view_w, view_h;

    /* Non-empty when the page is a frameset; the primary document is then only
     * the parse result and is never painted. */
    std::vector<ps_frame> frames;
    int                   focus_frame = 0;

    ps_paint       *paint  = nullptr;
    ps_text_cache  *text   = nullptr;
    ps_image_cache *images = nullptr;

    /* Hovered element, in document coordinates. */
    ps_rect hover {};
    bool    has_hover = false;



    ps_document(ps_paint *p, ps_text_cache *t, ps_image_cache *im,
                int w, int h)
        : container(p, t, im, w, h), view_w(w), view_h(h), paint(p), text(t),
          images(im)
    {
    }

    bool is_frameset() const { return !frames.empty(); }

    /* Background music for the page, absolute. Empty when silent. */
    std::string bgsound;
    int         bgsound_loop = 1;

    /* A page may ship its own instrument set, which replaces the session
     * default for as long as that page is open. Empty means use the default. */
    std::string soundbank;

    /* Where <meta http-equiv="refresh"> wants to go, and how long it wants to
     * wait. The address is empty when the page asked to reload itself, which
     * only the shell can resolve - it knows the current address after
     * redirects and this does not. refresh_ms is negative when the page asked
     * for nothing at all, so zero can keep meaning "go now". */
    std::string refresh_url;
    int         refresh_ms = -1;

    /* The first Flash movie the page embeds, and the element that carries it.
     * The element is kept rather than a box, because the box is only known
     * after layout and layout runs again whenever a subresource changes what
     * the page measures to - a box copied at scan time would be stale by the
     * first repaint. */
    std::string           swf_url;
    litehtml::element::ptr swf_el;

    int frame_at(int x, int y) const
    {
        for(size_t i = 0; i < frames.size(); i++) {
            const ps_rect &r = frames[i].rect;

            if(x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1)
                return (int)i;
        }
        return -1;
    }
};

/* <bgsound> is an IE extension and <embed> the Netscape one; pages of this era
 * use both, often together, to play a MIDI on load. Neither means anything to
 * litehtml, so the tree is walked for them after parsing. */
static void scan_soundbank_el(ps_container &c, const litehtml::element::ptr &el,
                              std::string &out)
{
    if(!el || !out.empty())
        return;

    /* A POPSurf extension, since HTML has no way to say this. Either form
     * works, whichever an author finds natural:
     *
     *   <meta name="soundbank" content="chip.psb">
     *   <link rel="soundbank" href="chip.psb">
     *
     * A site with its own chiptune instruments is a real thing to want on
     * this hardware, and the alternative is every page sounding identical. */
    litehtml::string_id t = el->tag();

    if(t == litehtml::_meta_ || t == litehtml::_link_) {
        const char *key = el->get_attr(t == litehtml::_meta_ ? "name" : "rel");
        const char *val = el->get_attr(t == litehtml::_meta_ ? "content"
                                                             : "href");

        if(key && val && *val &&
           (!strcasecmp(key, "soundbank") || !strcasecmp(key, "ps-soundbank"))) {
            std::string abs;

            if(c.resolve_public(val, abs))
                out = abs;
        }
    }

    for(const auto &ch : el->children())
        scan_soundbank_el(c, ch, out);
}

/* <meta http-equiv="refresh" content="8;url=next.html">
 *
 * The splash-then-move-on page is a defining shape of this era, and without
 * this it is a dead end: a picture, and nothing ever happens, with no link to
 * press. Sonic Adventure's page is exactly that.
 *
 * The content attribute is a number, then optionally a semicolon, the literal
 * "url", an equals sign and an address - with whitespace anywhere, the keyword
 * in any case, and the address sometimes quoted. A bare number with no address
 * means reload this page. */
static void scan_refresh_el(ps_container &c, const litehtml::element::ptr &el,
                            std::string &out, int &delay_ms, bool &found)
{
    if(!el || found)
        return;

    if(el->tag() == litehtml::_meta_) {
        const char *eq  = el->get_attr("http-equiv");
        const char *val = el->get_attr("content");

        if(eq && val && !strcasecmp(eq, "refresh")) {
            const char *p = val;
            long        secs;
            char       *end = nullptr;

            while(*p == ' ' || *p == '\t')
                p++;

            secs = strtol(p, &end, 10);
            if(end != p) {
                found = true;

                /* Negative or absurd delays are treated as immediate and
                 * absent respectively; the clamp lives in the shell, which is
                 * the only place that knows how long a frame takes. */
                delay_ms = (secs < 0) ? 0 : (int)(secs * 1000);

                p = end;
                while(*p == ' ' || *p == '\t' || *p == ';' || *p == ',')
                    p++;

                if(!strncasecmp(p, "url", 3)) {
                    p += 3;
                    while(*p == ' ' || *p == '\t' || *p == '=')
                        p++;

                    /* Strip a quote pair if present, and stop at the closing
                     * quote rather than running to the end of the value. */
                    char quote = (*p == '"' || *p == '\'') ? *p : 0;
                    if(quote)
                        p++;

                    std::string raw;
                    while(*p && *p != quote &&
                          !(quote == 0 && (*p == ' ' || *p == '\t')))
                        raw += *p++;

                    std::string abs;
                    if(!raw.empty() && c.resolve_public(raw.c_str(), abs))
                        out = abs;
                }
                /* No url: the page is asking to reload itself. Left empty, and
                 * the shell substitutes the current address - it knows what
                 * that is after redirects and we do not. */
            }
        }
    }

    for(const auto &ch : el->children())
        scan_refresh_el(c, ch, out, delay_ms, found);
}

/* A Flash movie is embedded three ways and pages of the era usually used all
 * three at once, nested, so that whichever plugin the visitor had would find
 * one: <embed src>, <object data>, and <param name="movie"> inside that object.
 * Any of them naming a .swf is the same movie, which is why the first match
 * wins and the rest of the tree is not searched.
 *
 * Recognised by extension and not by type attribute, because the type is
 * absent as often as it is wrong. */
static bool url_is_swf(const char *u)
{
    size_t n;

    if(!u)
        return false;
    n = strlen(u);
    /* Past a query string as well: <embed src="movie.swf?id=3"> is period
     * markup, not a modern invention. */
    for(size_t i = 0; i + 4 <= n; i++)
        if(!strncasecmp(u + i, ".swf", 4) &&
           (i + 4 == n || u[i + 4] == '?' || u[i + 4] == '#'))
            return true;
    return false;
}

static void scan_swf_el(ps_container &c, const litehtml::element::ptr &el,
                        std::string &out, litehtml::element::ptr &box)
{
    if(!el || !out.empty())
        return;

    {
        const char *tn  = el->get_tagName();
        const char *src = nullptr;
        litehtml::element::ptr owner = el;

        if(el->tag() == litehtml::_embed_)
            src = el->get_attr("src");
        else if(tn && !strcasecmp(tn, "object"))
            src = el->get_attr("data");
        else if(tn && !strcasecmp(tn, "param")) {
            const char *nm = el->get_attr("name");

            if(nm && (!strcasecmp(nm, "movie") || !strcasecmp(nm, "src"))) {
                src = el->get_attr("value");
                /* A param has no box of its own - it is metadata inside the
                 * object that does. */
                if(el->parent())
                    owner = el->parent();
            }
        }

        if(url_is_swf(src)) {
            std::string abs;

            if(c.resolve_public(src, abs)) {
                out = abs;
                box = owner;
                return;
            }
        }
    }

    for(const auto &ch : el->children())
        scan_swf_el(c, ch, out, box);
}

static void scan_bgsound_el(ps_container &c, const litehtml::element::ptr &el,
                            std::string &out, int &loop)
{
    if(!el || !out.empty())
        return;

    /* bgsound is an IE extension with no entry in litehtml's tag table, so it
     * is matched by name; embed does have one. */
    const char *tn = el->get_tagName();
    bool is_snd = (el->tag() == litehtml::_embed_) ||
                  (tn && !strcasecmp(tn, "bgsound"));

    if(is_snd) {
        const char *src = el->get_attr("src");
        const char *as  = el->get_attr("autostart");

        /* AUTOSTART=false asks for a player the visitor starts by hand. There
         * is no player - an <embed> of audio draws nothing here, and the
         * pages that carry this are WIDTH=0 HEIGHT=0 anyway - so the only
         * honest reading is silence. Playing regardless would be the one
         * behaviour the page explicitly ruled out. */
        bool wants_start = !(as && (!strcasecmp(as, "false") ||
                                    !strcasecmp(as, "no") ||
                                    !strcasecmp(as, "0")));

        /* An <embed> of a movie is not a tune. Without this the Flash file is
         * handed to the MIDI player, which refuses it, and the page is silent
         * for a reason that has nothing to do with sound. */
        if(url_is_swf(src))
            src = nullptr;

        if(src && *src && wants_start) {
            std::string abs;

            if(c.resolve_public(src, abs)) {
                out = abs;

                const char *lp = el->get_attr("loop");

                /* loop="infinite" and loop="-1" both mean forever, and a
                 * count is close enough to forever for background music. */
                loop = 1;
                if(lp && (!strcasecmp(lp, "0") || !strcasecmp(lp, "false") ||
                          !strcasecmp(lp, "no")))
                    loop = 0;
            }
        }
    }

    for(const auto &ch : el->children())
        scan_bgsound_el(c, ch, out, loop);
}

static void scan_bgsound(ps_document *doc)
{
    doc->bgsound.clear();
    doc->bgsound_loop = 1;

    doc->soundbank.clear();

    doc->refresh_url.clear();
    doc->refresh_ms = -1;

    doc->swf_url.clear();
    doc->swf_el.reset();

    if(doc->doc) {
        bool found = false;

        scan_swf_el(doc->container, doc->doc->root(), doc->swf_url,
                    doc->swf_el);
        scan_bgsound_el(doc->container, doc->doc->root(), doc->bgsound,
                        doc->bgsound_loop);
        scan_soundbank_el(doc->container, doc->doc->root(), doc->soundbank);
        scan_refresh_el(doc->container, doc->doc->root(), doc->refresh_url,
                        doc->refresh_ms, found);
    }
}

extern "C" {

int ps_document_refresh_ms(const ps_document *doc)
{
    return doc ? doc->refresh_ms : -1;
}

const char *ps_document_refresh_url(const ps_document *doc)
{
    return doc ? doc->refresh_url.c_str() : "";
}

const char *ps_document_bgsound(const ps_document *doc)
{
    return doc ? doc->bgsound.c_str() : "";
}

int ps_document_bgsound_loop(const ps_document *doc)
{
    return doc ? doc->bgsound_loop : 0;
}

const char *ps_document_soundbank(const ps_document *doc)
{
    return doc ? doc->soundbank.c_str() : "";
}

const char *ps_document_swf(const ps_document *doc)
{
    return doc ? doc->swf_url.c_str() : "";
}

int ps_document_swf_rect(const ps_document *doc, ps_rect *out)
{
    if(!doc || !out || !doc->swf_el || doc->swf_url.empty())
        return 0;

    litehtml::position p = doc->swf_el->get_placement();

    if((int)p.width <= 0 || (int)p.height <= 0)
        return 0;

    /* Document coordinates, which is what the page is now recorded in - the
     * shell subtracts the scroll itself. Same space ps_document_applet_input
     * works in, and for the same reason its comment gives. */
    out->x0 = (int16_t)(int)p.x;
    out->y0 = (int16_t)(int)p.y;
    out->x1 = (int16_t)((int)p.x + (int)p.width);
    out->y1 = (int16_t)((int)p.y + (int)p.height);
    return 1;
}


ps_document *ps_document_create(ps_paint *paint, ps_text_cache *text,
                                ps_image_cache *images,
                                int view_w, int view_h)
{
    if(!paint || !text)
        return NULL;

    return new(std::nothrow) ps_document(paint, text, images, view_w, view_h);
}

void ps_document_set_base(ps_document *doc, const char *base_url)
{
    if(doc)
        doc->container.set_base(base_url);
}

int ps_document_applet_input(ps_document *doc, int x, int y, int scroll_y,
                             int down, int dragging)
{
    if(!doc)
        return 0;

    /* Document space, because that is what the element now records.
     *
     * This has been both ways and each was right at the time. When every frame
     * drew the page at -scroll_y the box was screen space and adding the
     * scroll counted it twice. Now the page is drawn once, unscrolled, and
     * recorded - so the box is document space and the scroll has to go back
     * on. Third coordinate-space bug in this file from the same cause: two
     * paths producing the same value in different spaces. */
    if(doc->container.applet_input_at(x, y + scroll_y, down, dragging))
        return 1;

    /* A press that reached the page is a press outside every applet, and that
     * is what gives the keyboard back. Without it the d-pad would belong to
     * the first applet clicked for the rest of the page's life, and there is
     * nothing else on a controller to take it back with. */
    if(down > 0 && doc->container.applets())
        ps_applet_set_focus(doc->container.applets(), nullptr);

    return 0;
}

int ps_document_applet_focused(const ps_document *doc)
{
    if(!doc || !doc->container.applets())
        return 0;

    return ps_applet_focus(doc->container.applets()) ? 1 : 0;
}

void ps_document_applet_hover(ps_document *doc, int x, int y, int scroll_y)
{
    if(doc)
        doc->container.applet_hover_at(x, y + scroll_y);
}

int ps_document_applet_key(ps_document *doc, int key, int down)
{
    const char *url;

    if(!doc || !doc->container.applets())
        return 0;

    /* Keys go to the focused applet wherever the pointer has wandered to
     * since - that is what focus means, and a game played with the stick
     * would otherwise lose its keys the moment the cursor drifted off. */
    url = ps_applet_focus(doc->container.applets());
    if(!url)
        return 0;

    return ps_applet_key(doc->container.applets(), url, key, down) ? 1 : 0;
}

void ps_document_set_applets(ps_document *doc, struct ps_applet_cache *a)
{
    if(!doc)
        return;

    doc->container.set_applets(a);

    /* Frames are separate documents with separate containers, and an applet
     * inside a frameset is not unusual - the period's habit was a nav frame
     * and a content frame with the applet in the content one. */
    for(auto &f : doc->frames) {
        if(f.container)
            f.container->set_applets(a);
    }
}

void ps_document_destroy(ps_document *doc)
{
    delete doc;
}

int ps_document_load_memory(ps_document *doc, const char *html, size_t len)
{
    if(!doc || !html)
        return -1;

    std::string src(html, len);

    /* The old tree is about to be released, and the focused control lived in
     * it. */
    doc->container.clear_focus();
    doc->container.forget_applets();
    doc->container.clear_animated();
    doc->has_hover = false;

    doc->frames.clear();
    doc->focus_frame = 0;

    doc->doc = litehtml::document::createFromString(src.c_str(),
                                                    &doc->container);
    if(!doc->doc)
        return -1;

    /* A frameset replaces body entirely, so the primary document is only a
     * parse result: it is never laid out or painted. Each frame becomes its
     * own document with its own container, base URL and scroll. */
    {
        auto fs = psfrm::find_frameset(doc->doc->root());

        if(fs) {
            std::vector<psfrm::frame_def> defs;

            psfrm::build(fs, doc->view_w, doc->view_h, defs);

            for(const auto &d : defs) {
                ps_frame f;
                int      fw = d.rect.x1 - d.rect.x0;
                int      fh = d.rect.y1 - d.rect.y0;

                if(fw <= 0 || fh <= 0)
                    continue;

                f.container = std::unique_ptr<ps_container>(
                    new ps_container(doc->paint, doc->text, doc->images, fw,
                                     fh));
                f.rect      = d.rect;
                f.src       = d.src;
                f.name      = d.name;
                f.scrolling = d.scrolling;
                doc->frames.push_back(std::move(f));
            }

            if(!doc->frames.empty())
                return 0;
        }
    }

    doc->doc->render((litehtml::pixel_t)doc->view_w);
    scan_bgsound(doc);
    return 0;
}

/* --- frames ---------------------------------------------------------- */

int ps_document_is_frameset(const ps_document *doc)
{
    return doc && doc->is_frameset() ? 1 : 0;
}

int ps_document_frame_count(const ps_document *doc)
{
    return doc ? (int)doc->frames.size() : 0;
}

static bool frame_valid(const ps_document *doc, int i)
{
    return doc && i >= 0 && i < (int)doc->frames.size();
}

int ps_document_frame_pending(const ps_document *doc, int i)
{
    if(!frame_valid(doc, i))
        return 0;
    const ps_frame &f = doc->frames[i];
    return (!f.requested && !f.src.empty()) ? 1 : 0;
}

const char *ps_document_frame_src(const ps_document *doc, int i)
{
    return frame_valid(doc, i) ? doc->frames[i].src.c_str() : "";
}

void ps_document_frame_mark_requested(ps_document *doc, int i,
                                      const char *abs_url)
{
    if(!frame_valid(doc, i))
        return;
    doc->frames[i].requested = true;
    if(abs_url)
        doc->frames[i].url = abs_url;
}

int ps_document_frame_load(ps_document *doc, int i, const char *html,
                           size_t len, const char *base)
{
    if(!frame_valid(doc, i) || !html)
        return -1;

    ps_frame &f = doc->frames[i];
    std::string src(html, len);

    f.container->clear_focus();
    if(base)
        f.container->set_base(base);

    f.doc = litehtml::document::createFromString(src.c_str(),
                                                 f.container.get());
    if(!f.doc)
        return -1;

    f.doc->render((litehtml::pixel_t)(f.rect.x1 - f.rect.x0));
    f.height = (int)f.doc->height();
    f.loaded = true;
    return 0;
}

void ps_document_frame_rect(const ps_document *doc, int i, ps_rect *out)
{
    if(frame_valid(doc, i) && out)
        *out = doc->frames[i].rect;
}

int ps_document_frame_at(const ps_document *doc, int x, int y)
{
    return doc ? doc->frame_at(x, y) : -1;
}

void ps_document_frame_scroll(ps_document *doc, int i, int dy)
{
    if(!frame_valid(doc, i))
        return;

    ps_frame &f  = doc->frames[i];
    int       vh = f.rect.y1 - f.rect.y0;
    int       max = f.height - vh;

    if(!f.scrolling)
        return;
    if(max < 0)
        max = 0;

    f.scroll_y += dy;
    if(f.scroll_y > max)
        f.scroll_y = max;
    if(f.scroll_y < 0)
        f.scroll_y = 0;
}

void ps_document_render(ps_document *doc, int width)
{
    if(!doc || !doc->doc)
        return;

    doc->view_w = width;
    doc->doc->render((litehtml::pixel_t)width);
}

void ps_document_set_view_h(ps_document *doc, int height)
{
    int old_h;

    if(!doc || height <= 0 || height == doc->view_h)
        return;

    old_h        = doc->view_h;
    doc->view_h  = height;
    doc->container.set_view_size(doc->view_w, height);

    /* Frame rectangles were carved out of the old viewport at load time, and
     * psfrm::build is only reachable from there. Scaling them is exactly right
     * for percentage and wildcard rows, which is how nearly every frameset in
     * the wild is written, and close enough for pixel rows that nobody will
     * find the few lines it moves them by. The alternative - refetching every
     * frame to rebuild the layout - would cost a full page load to show or
     * hide a toolbar. */
    for(auto &f : doc->frames) {
        f.rect.y0 = (int16_t)((int)f.rect.y0 * height / old_h);
        f.rect.y1 = (int16_t)((int)f.rect.y1 * height / old_h);

        if(f.container)
            f.container->set_view_size(f.rect.x1 - f.rect.x0,
                                       f.rect.y1 - f.rect.y0);
        if(f.doc)
            f.doc->render((litehtml::pixel_t)(f.rect.x1 - f.rect.x0));

        f.height = f.doc ? (int)f.doc->height() : 0;

        /* A frame scrolled to its old bottom is past the new one. */
        {
            int max = f.height - (f.rect.y1 - f.rect.y0);

            if(max < 0)
                max = 0;
            if(f.scroll_y > max)
                f.scroll_y = max;
        }
    }

    ps_document_relayout(doc);
}

int ps_document_tick(ps_document *doc, int dt_ms)
{
    if(!doc)
        return 0;

    doc->container.tick(dt_ms);
    for(auto &f : doc->frames) {
        if(f.container)
            f.container->tick(dt_ms);
    }

    return doc->container.animated() ? 1 : 0;
}

void ps_document_relayout(ps_document *doc)
{
    if(!doc || !doc->doc)
        return;

    /* The hover rect was measured against the old layout, so it is stale the
     * moment boxes move. The next mouse move re-establishes it. */
    doc->has_hover = false;
    doc->doc->render((litehtml::pixel_t)doc->view_w);
}

int ps_document_height(const ps_document *doc)
{
    if(!doc)
        return 0;

    /* A frameset never scrolls as a whole; each frame scrolls itself. */
    if(doc->is_frameset())
        return doc->view_h;

    if(!doc->doc)
        return 0;
    return (int)doc->doc->height();
}

const char *ps_document_title(const ps_document *doc)
{
    return doc ? doc->container.caption().c_str() : "";
}

void ps_document_set_navigate_cb(ps_document *doc, ps_navigate_fn cb,
                                 void *user)
{
    if(doc)
        doc->container.set_navigate_cb(cb, user);
}

/* litehtml wants document coordinates and viewport coordinates separately, and
 * reports which boxes need repainting. We always repaint the whole frame, so
 * for clicks the boxes are discarded and only the "did anything change" flag
 * matters. */
#define PS_MOUSE_BODY(call)                                                   \
    if(!doc)                                                                  \
        return 0;                                                             \
    if(doc->is_frameset()) {                                                  \
        /* Coordinates are viewport-relative; a frame's document knows        \
         * nothing of where its rectangle sits, so translate into it. */      \
        int fi = doc->frame_at(x, y);                                         \
        if(fi < 0 || !doc->frames[fi].doc)                                    \
            return 0;                                                         \
        ps_frame &fr = doc->frames[fi];                                       \
        doc->focus_frame = fi;                                                \
        int lx = x - fr.rect.x0;                                              \
        int ly = y - fr.rect.y0;                                              \
        return fr.doc->call((litehtml::pixel_t)lx,                            \
                            (litehtml::pixel_t)(ly + fr.scroll_y),            \
                            (litehtml::pixel_t)lx, (litehtml::pixel_t)ly,     \
                            [](const litehtml::position &) {}) ? 1 : 0;       \
    }                                                                         \
    if(!doc->doc)                                                             \
        return 0;                                                             \
    return doc->doc->call((litehtml::pixel_t)x,                               \
                          (litehtml::pixel_t)(y + scroll_y),                  \
                          (litehtml::pixel_t)x, (litehtml::pixel_t)y,         \
                          [](const litehtml::position &) {})                  \
               ? 1                                                            \
               : 0;

/* Innermost anchor containing a document-space point.
 *
 * Only walked when the hit test has already said a link is there and litehtml
 * declined to name it, so this runs on a hover change rather than per frame.
 * Depth first with the deepest match winning: an anchor wrapping an image
 * wrapping text should ring the anchor, and taking the first hit from the top
 * would ring the body. */
/* Bounding box of everything an element renders, itself and its descendants.
 *
 * An anchor's own placement is not the answer: text is laid out a run at a
 * time, so each word is its own render item and the anchor may carry none.
 * Ringing what is under the cursor gives one word of the link; ringing the
 * union gives the link. A wrapped anchor gets a box covering both lines
 * including the gap between them, which is not what a desktop browser draws
 * but is the right shape for something aimed at from a sofa. */
static void union_boxes(const litehtml::element::ptr &el,
                        litehtml::position &acc, bool &any)
{
    if(!el)
        return;

    {
        litehtml::position p  = el->get_placement();
        int                pw = (int)p.width, ph = (int)p.height;

        if(pw > 0 && ph > 0) {
            int px = (int)p.x, py = (int)p.y;

            if(!any) {
                acc = p;
                any = true;
            }
            else {
                int ax = (int)acc.x, ay = (int)acc.y;
                int ar = ax + (int)acc.width, ab = ay + (int)acc.height;

                if(px < ax) { acc.width  = (litehtml::pixel_t)(ar - px);
                              acc.x = p.x; }
                else if(px + pw > ar)
                              acc.width  = (litehtml::pixel_t)(px + pw - ax);

                if(py < ay) { acc.height = (litehtml::pixel_t)(ab - py);
                              acc.y = p.y; }
                else if(py + ph > ab)
                              acc.height = (litehtml::pixel_t)(py + ph - ay);
            }
        }
    }

    for(const auto &ch : el->children())
        union_boxes(ch, acc, any);
}

static bool link_box_at(const litehtml::element::ptr &el, int x, int y,
                        litehtml::position &out,
                        const litehtml::element::ptr &anchor)
{
    litehtml::element::ptr a = anchor;

    if(!el)
        return false;

    if(!a && el->tag() == litehtml::_a_) {
        const char *href = el->get_attr("href", "");

        if(href && *href)
            a = el;
    }

    /* Deepest first. An anchor is often only a style carrier: the text inside
     * it owns the render items, so the anchor's own placement can be empty
     * while its child's is exactly the box wanted. Descending before testing
     * means whichever of the two actually has geometry is the one used, and
     * the answer is right without knowing in advance which it will be. */
    for(const auto &ch : el->children()) {
        if(link_box_at(ch, x, y, out, a))
            return true;
    }

    if(!a)
        return false;

    {
        /* pixel_t is a wrapper type in this litehtml, so the comparisons are
         * done in int rather than relying on an implicit conversion it does
         * not offer unambiguously. */
        litehtml::position p  = el->get_placement();
        int                px = (int)p.x,     py = (int)p.y;
        int                pw = (int)p.width, ph = (int)p.height;

        if(pw <= 0 || ph <= 0)
            return false;
        if(x < px || x >= px + pw || y < py || y >= py + ph)
            return false;

        /* The point landed inside the link. What gets ringed is the whole
         * anchor, not the word that happened to be under the cursor. */
        {
            litehtml::position acc;
            bool               any = false;

            union_boxes(a, acc, any);
            out = any ? acc : p;
        }
        return true;
    }
}

int ps_document_mouse_move(ps_document *doc, int x, int y, int scroll_y)
{
    if(!doc)
        return 0;

    if(doc->is_frameset()) {
        int fi = doc->frame_at(x, y);

        if(fi < 0 || !doc->frames[fi].doc)
            return 0;

        ps_frame &fr = doc->frames[fi];
        int lx = x - fr.rect.x0;
        int ly = y - fr.rect.y0;

        doc->focus_frame = fi;
        return fr.doc->on_mouse_over((litehtml::pixel_t)lx,
                                     (litehtml::pixel_t)(ly + fr.scroll_y),
                                     (litehtml::pixel_t)lx,
                                     (litehtml::pixel_t)ly,
                                     [](const litehtml::position &) {})
                   ? 1 : 0;
    }

    if(!doc->doc)
        return 0;

    const int doc_y = y + scroll_y;
    bool      found = false;
    ps_rect   best {};
    long      best_area = 0;

    /* On a hover change litehtml reports every box that needs repainting,
     * which is the element being left as well as the one being entered. The
     * one under the cursor is the one we want, so filter by containment; if
     * several nest, the smallest is the innermost and the real target. */
    bool changed = doc->doc->on_mouse_over(
        (litehtml::pixel_t)x, (litehtml::pixel_t)doc_y,
        (litehtml::pixel_t)x, (litehtml::pixel_t)y,
        [&](const litehtml::position &p) {
            int px = (int)p.x, py = (int)p.y;
            int pw = (int)p.width, ph = (int)p.height;

            if(x < px || x >= px + pw || doc_y < py || doc_y >= py + ph)
                return;

            long area = (long)pw * ph;
            if(found && area >= best_area)
                return;

            best.x0   = (int16_t)px;
            best.y0   = (int16_t)py;
            best.x1   = (int16_t)(px + pw);
            best.y1   = (int16_t)(py + ph);
            best_area = area;
            found     = true;
        });

    /* litehtml reports boxes that need *repainting*, and hovering a link only
     * needs a repaint if some rule restyles it. These pages have no :hover
     * rules - almost nothing from this era does - so nothing was ever
     * reported, has_hover stayed false, and the ring this browser draws round
     * a link has never once appeared. It was written against an assumption
     * about what on_mouse_over reports rather than against what it does.
     *
     * So when the hit test says a link is under the cursor and litehtml
     * offered no box, go and find it. */
    if(changed) {
        doc->hover     = best;
        doc->has_hover = found;
    }

    if(!doc->has_hover && doc->container.cursor_is_link()) {
        litehtml::position p;
        bool               got = link_box_at(doc->doc->root(), x, doc_y, p,
                                            nullptr);

#ifdef PS_APPLET_PROFILE
        /* Third attempt at this ring, so it reports rather than assumes. The
         * hit test says a link is under the cursor; the question is only
         * whether the anchor can be found again and whether an inline element
         * has a placement worth drawing. */
        {
            static int last;

            if(got != last) {
                last = got;
                if(got)
                    printf("popsurf: link box %d,%d %dx%d\n", (int)p.x,
                           (int)p.y, (int)p.width, (int)p.height);
                else
                    printf("popsurf: link under cursor, no box found\n");
            }
        }
#endif

        if(got) {
            doc->hover.x0  = (int16_t)(int)p.x;
            doc->hover.y0  = (int16_t)(int)p.y;
            doc->hover.x1  = (int16_t)((int)p.x + (int)p.width);
            doc->hover.y1  = (int16_t)((int)p.y + (int)p.height);
            doc->has_hover = true;
        }
    }
    else if(doc->has_hover && !doc->container.cursor_is_link()) {
        doc->has_hover = false;
    }

    return changed ? 1 : 0;
}

int ps_document_hover_rect(const ps_document *doc, int scroll_y, ps_rect *out)
{
    if(!doc || !out || !doc->has_hover)
        return 0;

    out->x0 = doc->hover.x0;
    out->y0 = (int16_t)(doc->hover.y0 - scroll_y);
    out->x1 = doc->hover.x1;
    out->y1 = (int16_t)(doc->hover.y1 - scroll_y);
    return 1;
}

int ps_document_mouse_down(ps_document *doc, int x, int y, int scroll_y)
{
    if(doc)
        doc->container.set_click(x, y + scroll_y);
    PS_MOUSE_BODY(on_lbutton_down)
}

int ps_document_mouse_up(ps_document *doc, int x, int y, int scroll_y)
{
    /* Recorded before the event runs: on_click fires inside it and an image
     * map needs the position. */
    if(doc)
        doc->container.set_click(x, y + scroll_y);
    PS_MOUSE_BODY(on_lbutton_up)
}

static const ps_container *active_container(const ps_document *doc)
{
    if(!doc)
        return nullptr;
    if(doc->is_frameset()) {
        int i = doc->focus_frame;
        if(i >= 0 && i < (int)doc->frames.size() && doc->frames[i].container)
            return doc->frames[i].container.get();
        return nullptr;
    }
    return &doc->container;
}

int ps_document_cursor_is_link(const ps_document *doc)
{
    const ps_container *c = active_container(doc);
    return c && c->cursor_is_link() ? 1 : 0;
}

const char *ps_document_cursor_css(const ps_document *doc)
{
    const ps_container *c = active_container(doc);
    return c ? c->cursor_css().c_str() : "";
}

int ps_document_focused_editable(const ps_document *doc)
{
    return doc ? doc->container.focused_editable() : 0;
}

const char *ps_document_focused_value(const ps_document *doc)
{
    return doc ? doc->container.focused_value() : "";
}

const char *ps_document_focused_label(const ps_document *doc)
{
    return doc ? doc->container.focused_label() : "";
}

void ps_document_set_focused_value(ps_document *doc, const char *text)
{
    if(!doc || !text)
        return;

    /* The value change can widen a text field, so the page has to be measured
     * again before it is painted. */
    if(doc->container.set_focused_value(text))
        ps_document_relayout(doc);
}

void ps_document_draw(ps_document *doc, int scroll_y)
{
    if(!doc)
        return;

    if(doc->is_frameset()) {
        for(auto &f : doc->frames) {
            int fw = f.rect.x1 - f.rect.x0;
            int fh = f.rect.y1 - f.rect.y0;

            /* Each frame paints into its own rectangle, clipped, with its own
             * scroll. Without the clip a tall frame would paint straight over
             * its neighbours. */
            ps_paint_push_clip(doc->paint, &f.rect);

            if(f.doc) {
                /* litehtml tests this clip against positions that already
                 * include the x/y offset below, so it is in screen space, not
                 * document space. Passing document coordinates here shrank
                 * every frame by its own x offset. */
                litehtml::position clip((litehtml::pixel_t)f.rect.x0,
                                        (litehtml::pixel_t)f.rect.y0,
                                        (litehtml::pixel_t)fw,
                                        (litehtml::pixel_t)fh);

                f.doc->draw((litehtml::uint_ptr)0,
                            (litehtml::pixel_t)f.rect.x0,
                            (litehtml::pixel_t)(f.rect.y0 - f.scroll_y),
                            &clip);
            }
            ps_paint_pop_clip(doc->paint);
        }
        return;
    }

    if(!doc->doc)
        return;

    /* Screen space, same reasoning as the frame case: content is drawn at
     * document position minus scroll, so the visible band is always 0..view_h
     * regardless of how far down the page is. */
    litehtml::position clip(0, 0, (litehtml::pixel_t)doc->view_w,
                            (litehtml::pixel_t)doc->view_h);

    doc->doc->draw((litehtml::uint_ptr)0, 0, -scroll_y, &clip);
}

void ps_document_draw_all(ps_document *doc, int height)
{
    if(!doc || !doc->doc)
        return;

    /* A frameset is several documents with their own scroll positions, so
     * there is no single offset a replay could apply. Those keep redrawing. */
    if(doc->is_frameset()) {
        ps_document_draw(doc, 0);
        return;
    }

    {
        litehtml::position clip(0, 0, (litehtml::pixel_t)doc->view_w,
                                (litehtml::pixel_t)height);

        doc->doc->draw((litehtml::uint_ptr)0, 0, 0, &clip);
    }
}

} /* extern "C" */
