// LaTeX math rendering for the GTK3 plugin: MicroTeX's Cairo Graphics2D
// backend (cairomm/pangomm). Only compiled into markdownview_gtk3, which
// has zero Qt dependency -- this keeps it that way. See
// src/qt6/latex_render_qt.cpp for the Qt6 target's equivalent.
//
// Vendored MicroTeX is upstream's "openmath" branch, not master -- see the
// comment on markdown_engine.cpp's init() for the full story of why.

#include "core/latex_render.h"

#include "microtex.h"
#include "graphic_cairo.h"

#include <cairomm/context.h>
#include <cairomm/surface.h>
#include <cairo.h>
#include <pangomm/init.h>
#include <algorithm>
#include <cstdio>

// microtex::MicroTeX::parse() below (via graphic_cairo.cpp's
// TextLayout_cairo) constructs Pango::Layout -- a pangomm C++ wrapper
// around the plain-C PangoLayout. pangomm keeps its own internal table
// mapping GObject types to wrapper constructors, populated only by
// Pango::init(); nothing in this plugin used pangomm before this file, and
// plain gtk_init() does not populate that table (it's a pure-C API,
// unrelated to the C++ bindings). Reproduced with a debug build + GDB: the
// FIRST call into TextLayout_cairo's constructor asserted
// "Glib::wrap_create_new_wrapper(): wrap_func_table != nullptr" and then
// segfaulted in Pango::Layout::set_text() with a null/garbage vtable --
// deterministic on every markdown file containing LaTeX ($$...$$ or
// $...$), which is very likely THE field crash behind "markdownview simply
// crashes": the sample repro file (wlx_samples/testmd.md) has both a $$
// display formula and inline $...$ text and crashed on the very first
// ListLoad. MicroTeX's own gtkmm sample program already calls Pango::init()
// before rendering anything -- this file just never carried that call over
// when the cairo LaTeX backend was wired into a plain-C GTK3 host instead
// of a gtkmm one.
void ensurePangommInitialized()
{
    static bool initialized = false;
    if (initialized) return;
    Pango::init();
    initialized = true;
}

void initLatexFonts(std::vector<LatexFontEntry> &fonts)
{
    static bool inited = false;
    if (inited || fonts.empty()) return;
    inited = true;

    microtex::PlatformFactory::registerFactory("cairo", std::make_unique<microtex::PlatformFactory_cairo>());
    microtex::PlatformFactory::activate("cairo");

    microtex::FontSrcFile first(fonts.front().clmPath, fonts.front().otfPath);
    fonts.front().canonicalName = microtex::MicroTeX::init(first).name;
    for (size_t i = 1; i < fonts.size(); ++i) {
        microtex::FontSrcFile src(fonts[i].clmPath, fonts[i].otfPath);
        fonts[i].canonicalName = microtex::MicroTeX::addFont(src).name;
    }
}

std::string addLatexFont(const std::string &clmPath, const std::string &otfPath)
{
    if (!microtex::MicroTeX::isInited()) return "";
    try {
        microtex::FontSrcFile src(clmPath, otfPath);
        return microtex::MicroTeX::addFont(src).name;
    } catch (const std::exception &) {
        // Same validation MicroTeX applies to the embedded fonts at
        // startup (rejected the real newpxmath CTAN package's incomplete
        // MATH table with ex_invalid_param, for instance) can just as well
        // reject an arbitrary user-supplied ini path -- that must fall
        // back to the default font, not crash or propagate.
        return "";
    }
}

std::vector<uint8_t> renderLatexToPng(const std::string &tex, bool darkMode,
                                       const std::string &mathFontName,
                                       int &logicalWidth, int &logicalHeight)
{
    ensurePangommInitialized();
    if (!microtex::MicroTeX::isInited()) return {};

    constexpr int oversample = 8;
    constexpr float baseTextSize = 20.0f;
    constexpr float renderTextSize = baseTextSize * oversample;
    unsigned int fgColor = darkMode ? 0xfff0f6fc : 0xff000000;

    microtex::Render *render = nullptr;
    try {
        render = microtex::MicroTeX::parse(
            tex, 0, renderTextSize, renderTextSize / 3.f, fgColor,
            true, {false, microtex::TexStyle::text}, mathFontName
        );
    } catch (const std::exception &) {
        return {};
    }
    if (!render || render->getWidth() <= 0 || render->getHeight() <= 0) {
        delete render;
        return {};
    }

    int padding = 4 * oversample;
    int physicalWidth = std::max(1, render->getWidth() + padding * 2);
    int physicalHeight = std::max(1, render->getHeight() + render->getDepth() + padding * 2);

    Cairo::RefPtr<Cairo::ImageSurface> surface =
        Cairo::ImageSurface::create(Cairo::FORMAT_ARGB32, physicalWidth, physicalHeight);
    Cairo::RefPtr<Cairo::Context> cr = Cairo::Context::create(surface);

    // Transparent background: ARGB32 surfaces are already zero-initialized
    // (fully transparent), so skip the paint() fill entirely instead of
    // matting onto an opaque page color.
    microtex::Graphics2D_cairo g2(cr->cobj());
    render->draw(g2, padding, padding);
    delete render;

    surface->flush();

    logicalWidth = physicalWidth / oversample;
    logicalHeight = physicalHeight / oversample;

    // Downscale to logical size via a second surface, same "oversample then
    // shrink for quality" approach as the Qt path's QImage::scaled().
    Cairo::RefPtr<Cairo::ImageSurface> scaledSurface =
        Cairo::ImageSurface::create(Cairo::FORMAT_ARGB32, logicalWidth, logicalHeight);
    Cairo::RefPtr<Cairo::Context> scaledCr = Cairo::Context::create(scaledSurface);
    scaledCr->scale((double)logicalWidth / physicalWidth, (double)logicalHeight / physicalHeight);
    scaledCr->set_source(surface, 0, 0);
    scaledCr->paint();
    scaledSurface->flush();

    std::vector<uint8_t> png;
    auto writeCb = [](void *closure, const unsigned char *data, unsigned int length) -> cairo_status_t {
        auto *out = static_cast<std::vector<uint8_t> *>(closure);
        out->insert(out->end(), data, data + length);
        return CAIRO_STATUS_SUCCESS;
    };
    cairo_surface_write_to_png_stream(scaledSurface->cobj(), writeCb, &png);

    return png;
}
