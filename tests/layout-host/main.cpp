/* Host-side layout probe: lay out a document and print the box tree.
 *
 * litehtml is portable C++, so layout questions do not need a Dreamcast to
 * answer. Running it here turns a twenty-minute upload-and-squint cycle into a
 * sub-second one, and prints exact numbers instead of leaving us reading a
 * 640x480 screen across the room trying to judge whether a cell is 16 or 64
 * pixels tall.
 *
 * The container's font metrics are synthetic and deterministic - every glyph
 * half the font size wide, no system fonts involved - so a number here is
 * reproducible and means the same thing on any machine. That makes it good for
 * structural questions (did this box get the height its CSS asked for?) and
 * useless for typographic ones (does this text wrap where it should?), which is
 * the right trade for layout bugs.
 *
 * Usage:
 *   ./layouttest <file.html> [viewport_width]
 *   ./layouttest <file.html> [viewport_width] --compare
 */
#include <litehtml.h>

#include "litehtml/render_item.h"
#include "probe_container.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

using namespace litehtml;

namespace litehtml
{
    extern bool ps_fit_to_viewport;
}

/* Prints one box per line, indented by depth, and tracks the widest right edge
 * any box reached.
 *
 * That widest edge is the number the fit-to-viewport policy exists to control:
 * anything past the viewport is content the console physically cannot scroll
 * to, so it says plainly whether clamping bought anything on a given page -
 * and, just as importantly, whether it cost anything on pages that already
 * fitted. */
static void dump(const std::shared_ptr<render_item>& ri, int depth, int max_depth,
                 double* widest)
{
    if(!ri)
        return;

    const auto& el  = ri->src_el();
    const char* tag = el ? el->get_tagName() : "?";
    position    p   = ri->pos();

    if(widest && (double)(p.x + p.width) > *widest)
        *widest = (double)(p.x + p.width);

    if(depth <= max_depth)
        std::printf("%*s%-12s x=%-6.0f y=%-6.0f w=%-6.0f h=%-6.0f\n",
                    depth * 2, "", tag ? tag : "?",
                    (double)p.x, (double)p.y, (double)p.width, (double)p.height);

    for(const auto& child : ri->children())
        dump(child, depth + 1, max_depth, widest);
}

/* Lays out once under the given policy. max_depth < 0 prints nothing and only
 * measures, which is what --compare wants for its first pass. */
static double layout_once(const std::string& html, int width, bool fit,
                          int max_depth, double* out_height)
{
    ps_fit_to_viewport = fit;

    probe_container container(width, 480);
    auto            doc = document::createFromString(html.c_str(), &container);

    if(!doc)
        return -1;
    doc->render(width);

    double widest = 0;
    if(out_height)
        *out_height = (double)doc->height();
    dump(doc->root_render(), 0, max_depth, &widest);
    return widest;
}

int main(int argc, char** argv)
{
    if(argc < 2) {
        std::fprintf(stderr,
                     "usage: %s <file.html> [viewport_width] [--compare]\n"
                     "  --compare  lay the page out both with and without the\n"
                     "             fit-to-viewport clamp, and report how far\n"
                     "             past the screen each one runs\n",
                     argv[0]);
        return 2;
    }

    int  width   = (argc > 2 && argv[2][0] != '-') ? std::atoi(argv[2]) : 640;
    bool compare = false;

    for(int i = 2; i < argc; i++)
        if(!std::strcmp(argv[i], "--compare"))
            compare = true;

    std::ifstream in(argv[1]);
    if(!in) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    std::string html = ss.str();

    if(compare) {
        double h_off = 0, h_on = 0;
        double off = layout_once(html, width, false, -1, &h_off);
        double on  = layout_once(html, width, true, -1, &h_on);

        if(off < 0 || on < 0) {
            std::fprintf(stderr, "parse failed\n");
            return 1;
        }

        std::printf("viewport %d\n", width);
        std::printf("  spec       widest right edge %6.0f  height %6.0f%s\n",
                    off, h_off, off > width ? "   OVERFLOWS" : "");
        std::printf("  fitted     widest right edge %6.0f  height %6.0f%s\n",
                    on, h_on, on > width ? "   OVERFLOWS" : "");

        if(on == off)
            std::printf("  -> no difference on this page\n");
        else
            std::printf("  -> clamp pulled %.0f px back on screen\n", off - on);
        return 0;
    }

    double h = 0;
    double widest = layout_once(html, width, true, 12, &h);

    if(widest < 0) {
        std::fprintf(stderr, "parse failed\n");
        return 1;
    }
    std::printf("\ndocument width=%d height=%.0f widest right edge=%.0f%s\n",
                width, h, widest, widest > width ? "  OVERFLOWS" : "");
    return 0;
}
