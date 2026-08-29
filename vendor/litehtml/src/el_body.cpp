#include "el_body.h"
#include "document.h"

litehtml::el_body::el_body(const std::shared_ptr<litehtml::document>& doc) :
    litehtml::html_tag(doc)
{
}

bool litehtml::el_body::is_body() const
{
    return true;
}

// KOSWeb: https://html.spec.whatwg.org/multipage/rendering.html#the-page
// bgcolor and text on <body> are still how the retro web sets page colours.
void litehtml::el_body::parse_attributes()
{
    const char* str = get_attr("bgcolor");
    if(str)
    {
        m_style.add_property(_background_color_, str, "", false,
                             get_document()->container());
    }

    // Tiled page backgrounds are set this way on essentially every page of
    // the era this browser targets.
    str = get_attr("background");
    if(str)
    {
        std::string url = "url('";
        url += str;
        url += "')";
        m_style.add_property(_background_image_, url);
    }

    str = get_attr("text");
    if(str)
    {
        m_style.add_property(_color_, str, "", false, get_document()->container());
    }

    // KOSWeb: link, vlink and alink. The spec maps these onto a:link,
    // a:visited and a:active, which cannot be done by setting a property on
    // body - master_css already colours anchors and would win. Injecting a
    // stylesheet is how el_style does it, and it lands at the same point in
    // parsing.
    {
        std::string sheet;
        const char* v;

        if((v = get_attr("link")))
            sheet += std::string("a:link{color:") + v + "}";
        if((v = get_attr("vlink")))
            sheet += std::string("a:visited{color:") + v + "}";
        if((v = get_attr("alink")))
            sheet += std::string("a:active{color:") + v + "}";

        if(!sheet.empty())
            get_document()->add_stylesheet(sheet.c_str(), nullptr, nullptr);
    }

    html_tag::parse_attributes();
}
