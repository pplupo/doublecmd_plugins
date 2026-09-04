#pragma once

#include <string>
#include <vector>
#include <cstdint>

/// Toolkit-neutral rendering of ```chart fenced code blocks: a JSON spec
/// (same shape as ~/repos/reports' charts.py/preprocess_md.py use, so a
/// document written for that pipeline renders unchanged here) drawn
/// directly with Cairo -- no external process, no network, no Python --
/// and rasterized to PNG bytes. Cairo is already a shared dependency of
/// markdownview_core on both toolkits (see diagram_render.h's
/// svgToHighDpiPng), so this needs no per-toolkit backend split the way
/// LaTeX rendering does.
namespace ChartRender {

/// Which backend renderChartToPng() below uses. "Auto" (the default) tries
/// Matplot++/gnuplot first and falls back to the native Cairo renderer per
/// chart on any failure or unsupported feature -- unchanged from this
/// plugin's original soft-dependency behavior. "CairoOnly" skips the
/// Matplot++ attempt entirely (e.g. for a user who prefers the lighter,
/// dependency-free renderer even where Matplot++ would work). "Off" skips
/// chart rendering altogether -- renderChartToPng() returns empty
/// unconditionally, so the caller's existing empty-result fallback leaves
/// the fenced ```chart block showing as plain text.
enum class RendererMode { Auto, CairoOnly, Off };

/// Set once (e.g. from the plugin's ini-backed settings) before any
/// renderChartToPng() call; persists across calls until changed again.
void setRendererMode(RendererMode mode);

/// Renders a chart spec (raw JSON text, as captured from the fenced code
/// block) to PNG bytes. Mirrors ~/repos/reports' charts.py's full spec
/// shape: all 13 mark types, "layers" (several marks sharing one panel's
/// axes, drawn in order), "panels" (a multi-panel figure, stacked
/// vertically), and the cross-cutting log_x/log_y/ref_lines/ref_bands/
/// annotations fields -- see charts.py's own module docstring for the
/// full field-by-field spec, which this implementation follows.
///
/// Supported types: line, bar, barh, scatter, area, step, stem, errorbar,
/// histogram, boxplot, violin, heatmap, pie. heatmap/pie have no shared
/// x/y coordinate system (a pixel grid; a radial layout) and are only
/// meaningful as a panel's sole layer -- see chart_render.cpp's top
/// comment for the exact scoping.
///
/// type:"bar" additionally accepts stacked:"percent" (beyond charts.py's
/// own plain stacked:true/false) for a 100% stacked chart -- a
/// plugin-specific addition.
///
/// Returns empty on any failure (malformed JSON, missing required fields,
/// unsupported/missing type, empty data) -- same contract as
/// renderDiagramImgTag()'s other renderers, so the caller falls back to
/// the fenced block's plain text.
///
/// bodyFontFamily/titleFontFamily: bare Cairo/fontconfig family names (not
/// a CSS-style comma fallback stack) to use for axis/tick/legend text and
/// the chart title respectively -- markdown_engine.cpp derives these from
/// the document's own active CSS (body/heading font-family), so charts
/// visually match the surrounding document. Empty falls back to
/// "sans-serif". titleBold requests a bold weight for the title only,
/// mirroring a heading's font-weight.
std::vector<uint8_t> renderChartToPng(const std::string &specJson, bool darkMode,
                                       const std::string &bodyFontFamily, const std::string &titleFontFamily, bool titleBold,
                                       int &logicalWidth, int &logicalHeight);

} // namespace ChartRender
