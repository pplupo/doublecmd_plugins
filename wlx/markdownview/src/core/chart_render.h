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

/// Renders a chart spec (raw JSON text, as captured from the fenced code
/// block) to PNG bytes.
///
/// Supported spec["type"]: "line", "bar", "scatter" (unlike charts.py,
/// "pie" is not supported). Common fields: title, xlabel, ylabel, figsize
/// ([width, height] in inches). x is either all-numeric (a real numeric
/// axis) or all-string (categorical positions with x as tick labels).
/// Either a single unlabeled series via top-level y, or series: [{y,
/// label, marker}, ...] for one or more labeled series sharing the same x.
/// type:"bar" with multiple series draws grouped (side-by-side) bars by
/// default; set stacked:true to stack them instead, or stacked:"percent"
/// for a 100% stacked chart (each category rescaled to sum to 100, so
/// every bar reaches the same height and shows each series' share) --
/// all additions specific to this plugin, not part of charts.py's spec
/// shape.
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
