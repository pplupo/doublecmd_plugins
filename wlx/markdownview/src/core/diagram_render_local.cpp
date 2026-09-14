#include "diagram_render_local.h"

#include <cairo.h>

#include <cstddef>
#include <cstdint>
#include <string>

// One vendored archive (3rdparty/supramark/libmarkdownview_diagrams.a),
// linked in by CMakeLists.txt's ENABLE_LOCAL_DIAGRAMS option, wrapping both
// renderers -- supramark's own per-renderer archives cannot both be linked
// into one binary, since each bundles its own Rust std. See that
// directory's README.md and the wrapper crate's Cargo.toml.
//
// Declared here rather than including a generated header: the surface is
// four functions and this way the file needs no include path into 3rdparty.
extern "C" {
int markdownview_diagrams_render_mermaid(const uint8_t *input, size_t input_len,
                                         uint8_t **out_buf, size_t *out_len);
int markdownview_diagrams_render_plantuml(const uint8_t *input, size_t input_len,
                                          uint8_t **out_buf, size_t *out_len);
void markdownview_diagrams_free(uint8_t *buf, size_t len);

typedef void (*markdownview_measure_text_fn)(const char *family, size_t family_len,
                                             const char *text, size_t text_len,
                                             double size, uint8_t bold, uint8_t italic,
                                             double *out_width, double *out_ascent,
                                             double *out_descent);
void markdownview_diagrams_install_metrics_callback(markdownview_measure_text_fn cb);
}

namespace {

// One process-wide scratch context for measurement. Cairo's "toy" text API
// is enough for advance widths and ascent/descent, and it resolves family
// names through fontconfig.
cairo_t *measureContext()
{
    static cairo_t *cr = [] {
        cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
        cairo_t *c = cairo_create(surface);
        cairo_surface_destroy(surface); // the context keeps its own reference
        return c;
    }();
    return cr;
}

// Both crates are built with supramark's `metrics-ffi-callback` feature,
// which means they do NOT measure text themselves -- the host has to
// supply this, and every layout decision (node widths, lifeline spacing,
// label boxes) is computed from what it returns.
//
// Cairo rather than the sibling `metrics-ttf-parser` feature, which would
// embed a ~130KB DejaVu subset and measure against that: librsvg draws the
// returned SVG with the local fontconfig/FreeType stack, so measuring
// through that same stack is what keeps the laid-out geometry and the
// drawn glyphs agreeing. Measuring against a font that isn't the one drawn
// is how labels end up overflowing the boxes sized for them.
void measureText(const char *family, size_t familyLen, const char *text, size_t textLen,
                 double size, uint8_t bold, uint8_t italic,
                 double *outWidth, double *outAscent, double *outDescent)
{
    if (outWidth) *outWidth = 0;
    if (outAscent) *outAscent = 0;
    if (outDescent) *outDescent = 0;

    cairo_t *cr = measureContext();
    if (!cr) return;

    std::string fam = (family && familyLen) ? std::string(family, familyLen) : std::string("sans-serif");
    cairo_select_font_face(cr, fam.c_str(),
                           italic ? CAIRO_FONT_SLANT_ITALIC : CAIRO_FONT_SLANT_NORMAL,
                           bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, size > 0 ? size : 12.0);

    std::string s = (text && textLen) ? std::string(text, textLen) : std::string();
    cairo_text_extents_t textExtents;
    cairo_text_extents(cr, s.c_str(), &textExtents);
    cairo_font_extents_t fontExtents;
    cairo_font_extents(cr, &fontExtents);

    // x_advance, not width: width is the inked extent, which for a string
    // ending in a space (or any zero-ink glyph) is narrower than the space
    // the text actually occupies.
    if (outWidth) *outWidth = textExtents.x_advance;
    if (outAscent) *outAscent = fontExtents.ascent;
    if (outDescent) *outDescent = fontExtents.descent;
}

// Both renderers share one metrics registration, so installing it once
// covers either. Must happen before the first render call.
void ensureMetricsInstalled()
{
    static bool installed = [] {
        markdownview_diagrams_install_metrics_callback(measureText);
        return true;
    }();
    (void)installed;
}

} // namespace

namespace LocalDiagram {

bool isCompiledIn() { return true; }

std::string renderMermaid(const std::string &themedSource)
{
    if (themedSource.empty()) return {};
    ensureMetricsInstalled();

    uint8_t *out = nullptr;
    size_t outLen = 0;
    int rc = markdownview_diagrams_render_mermaid(
        reinterpret_cast<const uint8_t *>(themedSource.data()), themedSource.size(), &out, &outLen);
    if (rc != 0 || !out || outLen == 0) {
        if (out) markdownview_diagrams_free(out, outLen);
        return {};
    }
    std::string svg(reinterpret_cast<const char *>(out), outLen);
    markdownview_diagrams_free(out, outLen);
    return svg;
}

std::string renderPlantUml(const std::string &themedSource)
{
    if (themedSource.empty()) return {};
    ensureMetricsInstalled();

    uint8_t *out = nullptr;
    size_t outLen = 0;
    int rc = markdownview_diagrams_render_plantuml(
        reinterpret_cast<const uint8_t *>(themedSource.data()), themedSource.size(), &out, &outLen);
    if (rc != 0 || !out || outLen == 0) {
        if (out) markdownview_diagrams_free(out, outLen);
        return {};
    }
    std::string svg(reinterpret_cast<const char *>(out), outLen);
    markdownview_diagrams_free(out, outLen);
    return svg;
}

} // namespace LocalDiagram
