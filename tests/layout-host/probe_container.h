/* A document_container that measures and draws nothing.
 *
 * litehtml ships a headless test container, but it drags in a bitmap class and
 * a whole software canvas to satisfy the drawing half of the interface - none
 * of which a layout question needs, and which do not compile cleanly outside
 * their own build's include order. Layout only ever calls back for metrics, so
 * everything else can be an empty body.
 *
 * The metrics are deliberately synthetic and blunt: every glyph is half the
 * font size wide, and the vertical metrics are fixed fractions of it. That
 * makes results exactly reproducible and independent of any font on the
 * machine, at the cost of saying nothing about real text. Good for "did this
 * box get the height its CSS asked for", useless for "does this line wrap in
 * the right place" - which is the right trade for structural layout bugs.
 */
#pragma once

#include <litehtml.h>
#include <cstring>
#include <string>

class probe_container : public litehtml::document_container
{
  public:
    int vw, vh;

    probe_container(int w, int h) : vw(w), vh(h) {}

    /* The handle is the pixel size itself, so text_width needs no table. */
    litehtml::uint_ptr create_font(const litehtml::font_description& descr, const litehtml::document*,
                                   litehtml::font_metrics* fm) override
    {
        int size = (int)descr.size;
        if(size <= 0)
            size = 16;
        if(fm) {
            fm->font_size   = (litehtml::pixel_t)size;
            fm->height      = (litehtml::pixel_t)size;
            fm->ascent      = (litehtml::pixel_t)(size * 4 / 5);
            fm->descent     = (litehtml::pixel_t)(size / 5);
            fm->x_height    = (litehtml::pixel_t)(size / 2);
            fm->ch_width    = (litehtml::pixel_t)(size / 2);
            fm->draw_spaces = false;
        }
        return (litehtml::uint_ptr)size;
    }
    void            delete_font(litehtml::uint_ptr) override {}
    litehtml::pixel_t text_width(const char* text, litehtml::uint_ptr hFont) override
    {
        int size = (int)hFont ? (int)hFont : 16;
        return (litehtml::pixel_t)((int)strlen(text ? text : "") * (size / 2));
    }
    void draw_text(litehtml::uint_ptr, const char*, litehtml::uint_ptr, litehtml::web_color,
                   const litehtml::position&) override {}

    litehtml::pixel_t pt_to_px(float pt) const override { return (litehtml::pixel_t)(pt * 4 / 3); }
    litehtml::pixel_t get_default_font_size() const override { return 16; }
    const char*       get_default_font_name() const override { return "sans"; }

    void draw_list_marker(litehtml::uint_ptr, const litehtml::list_marker&) override {}
    void load_image(const char*, const char*, bool) override {}
    /* Zero-sized: an image whose size is unknown must not perturb the boxes
     * being measured. Tests that care about image sizing should state one. */
    void get_image_size(const char*, const char*, litehtml::size& sz) override
    {
        sz.width  = 0;
        sz.height = 0;
    }

    void draw_image(litehtml::uint_ptr, const litehtml::background_layer&, const std::string&,
                    const std::string&) override {}
    void draw_solid_fill(litehtml::uint_ptr, const litehtml::background_layer&,
                         const litehtml::web_color&) override {}
    void draw_linear_gradient(litehtml::uint_ptr, const litehtml::background_layer&,
                              const litehtml::background_layer::linear_gradient&) override {}
    void draw_radial_gradient(litehtml::uint_ptr, const litehtml::background_layer&,
                              const litehtml::background_layer::radial_gradient&) override {}
    void draw_conic_gradient(litehtml::uint_ptr, const litehtml::background_layer&,
                             const litehtml::background_layer::conic_gradient&) override {}
    void draw_borders(litehtml::uint_ptr, const litehtml::borders&, const litehtml::position&,
                      bool) override {}

    void set_caption(const char*) override {}
    void set_base_url(const char*) override {}
    void link(const std::shared_ptr<litehtml::document>&, const litehtml::element::ptr&) override {}
    void on_anchor_click(const char*, const litehtml::element::ptr&) override {}
    void on_mouse_event(const litehtml::element::ptr&, litehtml::mouse_event) override {}
    void set_cursor(const char*) override {}
    void transform_text(std::string&, litehtml::text_transform) override {}
    void import_css(std::string&, const std::string&, std::string&) override {}
    void set_clip(const litehtml::position&, const litehtml::border_radiuses&) override {}
    void del_clip() override {}

    void get_viewport(litehtml::position& viewport) const override
    {
        viewport.x      = 0;
        viewport.y      = 0;
        viewport.width  = (litehtml::pixel_t)vw;
        viewport.height = (litehtml::pixel_t)vh;
    }
    litehtml::element::ptr create_element(const char*, const litehtml::string_map&,
                                          const std::shared_ptr<litehtml::document>&) override
    {
        return nullptr;
    }

    void get_media_features(litehtml::media_features& media) const override
    {
        media.type          = litehtml::media_type_screen;
        media.width         = (litehtml::pixel_t)vw;
        media.height        = (litehtml::pixel_t)vh;
        media.device_width  = (litehtml::pixel_t)vw;
        media.device_height = (litehtml::pixel_t)vh;
        media.color         = 8;
        media.monochrome    = 0;
        media.color_index   = 256;
        media.resolution    = 96;
    }
    void get_language(std::string& language, std::string& culture) const override
    {
        language = "en";
        culture  = "";
    }
};
