#include "chart_render.h"

#include "../../3rdparty/nlohmann_json/json.hpp"

#include <cairo.h>
#include <cmath>
#include <algorithm>
#include <sstream>

namespace {

using nlohmann::json;

struct SeriesData {
    std::vector<double> y;
    std::string label;
    std::string marker; // only "none" is checked for -- see the line-type
                         // drawing loop below; any other value just means
                         // "draw the default filled-circle marker", not a
                         // real per-marker-shape implementation.
};

struct ChartSpec {
    std::string type = "line";
    std::vector<double> xNumeric;      // real values for a numeric axis, or 0..n-1 positions for a categorical one
    std::vector<std::string> xLabels;  // tick labels when categorical
    bool categorical = false;
    std::vector<SeriesData> series;
    std::string title, xlabel, ylabel;
    double figW = 6.0, figH = 4.0; // inches, same field/units as charts.py's spec["figsize"]
    // type:"bar" only; not part of charts.py's spec shape (no stacking
    // support there) -- an addition specific to this plugin. Multiple
    // series draw as grouped (side-by-side) bars per category unless this
    // is set, in which case they stack bottom-to-top instead.
    bool stacked = false;
    // stacked:"percent" (instead of stacked:true) -- each category's
    // series are rescaled to sum to 100 before stacking, so every bar
    // reaches the same total height and shows each series' *share* of the
    // category rather than its absolute value ("100% stacked bar chart").
    bool stackedPercent = false;
};

// Same JSON shape ~/repos/reports' charts.py/preprocess_md.py already use,
// so a ```chart block written for that pipeline renders unchanged here
// (minus type:"pie", not supported). Returns false on anything malformed
// or missing required fields -- the caller (renderChartToPng) treats that
// identically to any other rendering failure: fall back to the fenced
// block's plain text.
bool parseSpec(const std::string &specJson, ChartSpec &out) {
    json j;
    try {
        j = json::parse(specJson);
    } catch (const std::exception &) {
        return false;
    }
    if (!j.is_object()) return false;

    out.type = j.value("type", std::string("line"));
    if (out.type != "line" && out.type != "bar" && out.type != "scatter") return false;

    out.title = j.value("title", std::string());
    out.xlabel = j.value("xlabel", std::string());
    out.ylabel = j.value("ylabel", std::string());
    if (j.contains("stacked")) {
        if (j["stacked"].is_boolean()) {
            out.stacked = j["stacked"].get<bool>();
        } else if (j["stacked"].is_string() && j["stacked"].get<std::string>() == "percent") {
            out.stacked = true;
            out.stackedPercent = true;
        }
    }

    if (j.contains("figsize") && j["figsize"].is_array() && j["figsize"].size() == 2 &&
        j["figsize"][0].is_number() && j["figsize"][1].is_number()) {
        out.figW = j["figsize"][0].get<double>();
        out.figH = j["figsize"][1].get<double>();
    }

    if (!j.contains("x") || !j["x"].is_array() || j["x"].empty()) return false;
    const json &xArr = j["x"];
    bool allNumeric = true;
    for (const auto &v : xArr) { if (!v.is_number()) { allNumeric = false; break; } }
    if (allNumeric) {
        for (const auto &v : xArr) out.xNumeric.push_back(v.get<double>());
    } else {
        out.categorical = true;
        for (const auto &v : xArr) out.xLabels.push_back(v.is_string() ? v.get<std::string>() : v.dump());
        for (size_t i = 0; i < out.xLabels.size(); ++i) out.xNumeric.push_back((double)i);
    }
    size_t n = out.xNumeric.size();

    auto readSeries = [&](const json &yArr, const std::string &label, const std::string &marker, SeriesData &sd) {
        for (const auto &v : yArr) {
            if (!v.is_number()) return false;
            sd.y.push_back(v.get<double>());
        }
        if (sd.y.size() != n) return false;
        sd.label = label;
        sd.marker = marker;
        return true;
    };

    if (j.contains("series") && j["series"].is_array() && !j["series"].empty()) {
        for (const auto &s : j["series"]) {
            if (!s.is_object() || !s.contains("y") || !s["y"].is_array()) return false;
            SeriesData sd;
            if (!readSeries(s["y"], s.value("label", std::string()), s.value("marker", std::string()), sd)) return false;
            out.series.push_back(std::move(sd));
        }
    } else if (j.contains("y") && j["y"].is_array()) {
        SeriesData sd;
        if (!readSeries(j["y"], std::string(), std::string(), sd)) return false;
        out.series.push_back(std::move(sd));
    } else {
        return false;
    }
    return !out.series.empty();
}

struct Rgb { double r, g, b; };

// A short prefix of matplotlib's default "tab10" color cycle -- enough for
// any realistic number of series in a report chart, cycled if exceeded.
const Rgb kPalette[] = {
    {0x1f / 255.0, 0x77 / 255.0, 0xb4 / 255.0},
    {0xff / 255.0, 0x7f / 255.0, 0x0e / 255.0},
    {0x2c / 255.0, 0xa0 / 255.0, 0x2c / 255.0},
    {0xd6 / 255.0, 0x27 / 255.0, 0x28 / 255.0},
    {0x94 / 255.0, 0x67 / 255.0, 0xbd / 255.0},
    {0x8c / 255.0, 0x56 / 255.0, 0x4b / 255.0},
};
constexpr int kPaletteSize = sizeof(kPalette) / sizeof(kPalette[0]);

cairo_status_t writeToString(void *closure, const unsigned char *data, unsigned int length) {
    static_cast<std::string *>(closure)->append(reinterpret_cast<const char *>(data), length);
    return CAIRO_STATUS_SUCCESS;
}

// Trims to a clean, short tick label (integers with no decimal point,
// otherwise 2 decimal places) -- not attempting matplotlib's full
// auto-precision tick formatter, just enough to look sane on a report chart.
std::string formatTick(double v) {
    std::ostringstream ss;
    if (std::abs(v - std::round(v)) < 1e-9) {
        ss << (long long)std::llround(v);
    } else {
        ss.precision(2);
        ss << std::fixed << v;
    }
    return ss.str();
}

} // namespace

namespace ChartRender {

std::vector<uint8_t> renderChartToPng(const std::string &specJson, bool darkMode,
                                       const std::string &bodyFontFamily, const std::string &titleFontFamily, bool titleBold,
                                       int &logicalWidth, int &logicalHeight)
{
    const char *bodyFont = bodyFontFamily.empty() ? "sans-serif" : bodyFontFamily.c_str();
    const char *titleFont = titleFontFamily.empty() ? bodyFont : titleFontFamily.c_str();

    ChartSpec spec;
    if (!parseSpec(specJson, spec)) return {};

    // 100% stacked: rescale each category's series to shares of that
    // category's total (assumed non-negative -- a percentage breakdown
    // doesn't have a meaningful reading for mixed-sign data) before the
    // normal stacking logic below runs unmodified on the rescaled values.
    if (spec.type == "bar" && spec.stacked && spec.stackedPercent && spec.series.size() > 1) {
        for (size_t i = 0; i < spec.xNumeric.size(); ++i) {
            double total = 0;
            for (const auto &s : spec.series) total += s.y[i];
            if (total > 0) for (auto &s : spec.series) s.y[i] = s.y[i] / total * 100.0;
        }
    }

    constexpr double kPxPerInch = 100.0;
    int w = std::max(200, (int)std::lround(spec.figW * kPxPerInch));
    int h = std::max(150, (int)std::lround(spec.figH * kPxPerInch));
    logicalWidth = w;
    logicalHeight = h;

    // Render at 2x and let the <img> width/height attributes (set by the
    // caller from logicalWidth/logicalHeight) scale it back down for
    // display -- same oversample-for-crispness technique as
    // DiagramRender::svgToHighDpiPng.
    constexpr double scale = 2.0;
    int pixelWidth = (int)std::lround(w * scale);
    int pixelHeight = (int)std::lround(h * scale);

    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pixelWidth, pixelHeight);
    cairo_t *cr = cairo_create(surface);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_GOOD);
    cairo_scale(cr, scale, scale); // everything below is drawn in logical (w x h) units

    // Same dark/light background pair used for every other rendered-image
    // type in the document (LaTeX math, diagrams), for visual consistency.
    Rgb bg = darkMode ? Rgb{0x0d / 255.0, 0x11 / 255.0, 0x17 / 255.0} : Rgb{0xFA / 255.0, 0xFA / 255.0, 0xFA / 255.0};
    Rgb fg = darkMode ? Rgb{0xf0 / 255.0, 0xf6 / 255.0, 0xfc / 255.0} : Rgb{0x1a / 255.0, 0x1a / 255.0, 0x1a / 255.0};
    Rgb gridColor = darkMode ? Rgb{0x30 / 255.0, 0x36 / 255.0, 0x3d / 255.0} : Rgb{0xdd / 255.0, 0xdd / 255.0, 0xdd / 255.0};

    cairo_set_source_rgb(cr, bg.r, bg.g, bg.b);
    cairo_paint(cr);

    // Cairo's built-in "toy" text API (backed by fontconfig/freetype) --
    // no Pango needed here, unlike MicroTeX's Cairo backend, since we're
    // only ever drawing plain single-line labels, never full text layout.
    // bodyFont is the default for everything; the title switches to
    // titleFont (see below, right before it's drawn) and switches back.
    cairo_select_font_face(cr, bodyFont, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);

    double marginLeft = 55, marginRight = 20, marginTop = spec.title.empty() ? 15 : 35, marginBottom = 45;
    bool hasLegend = spec.series.size() > 1 || (spec.series.size() == 1 && !spec.series[0].label.empty());
    if (hasLegend) marginTop += 20;
    if (!spec.xlabel.empty()) marginBottom += 18;
    if (!spec.ylabel.empty()) marginLeft += 15;

    double plotX0 = marginLeft, plotY0 = marginTop;
    double plotX1 = w - marginRight, plotY1 = h - marginBottom;
    double plotW = plotX1 - plotX0, plotH = plotY1 - plotY0;
    if (plotW <= 10 || plotH <= 10) { cairo_destroy(cr); cairo_surface_destroy(surface); return {}; }

    double xMin = spec.xNumeric.front(), xMax = spec.xNumeric.front();
    for (double v : spec.xNumeric) { xMin = std::min(xMin, v); xMax = std::max(xMax, v); }
    bool stacked = spec.type == "bar" && spec.stacked && spec.series.size() > 1;
    double yMin, yMax;
    if (stacked) {
        // Stacked bars grow from cumulative sums per category, not from any
        // single series' own min/max -- each category's positive segments
        // stack on top of each other (and negative ones below zero), so the
        // range has to reflect the tallest/deepest *stack*, not the tallest
        // individual series value.
        yMin = 0; yMax = 0;
        for (size_t i = 0; i < spec.xNumeric.size(); ++i) {
            double posSum = 0, negSum = 0;
            for (const auto &s : spec.series) { double v = s.y[i]; if (v >= 0) posSum += v; else negSum += v; }
            yMax = std::max(yMax, posSum);
            yMin = std::min(yMin, negSum);
        }
    } else {
        yMin = spec.series[0].y[0]; yMax = spec.series[0].y[0];
        for (const auto &s : spec.series) for (double v : s.y) { yMin = std::min(yMin, v); yMax = std::max(yMax, v); }
    }
    if (spec.type == "bar" && yMin > 0) yMin = 0; // bars read from a visible baseline, like matplotlib's default
    if (yMax == yMin) { yMax += 1.0; yMin -= 1.0; } // flat data -- avoid a zero-height range
    double yPad = (yMax - yMin) * 0.08;
    yMax += yPad;
    if (!(spec.type == "bar" && yMin == 0.0)) yMin -= yPad; // keep a bar chart's zero baseline exact
    if (xMax == xMin) { xMax += 1.0; xMin -= 1.0; }
    // Bar charts always need a full half-slot of padding on each side --
    // the bar-width math below (slot = plotW / n) treats the plotted range
    // as exactly n evenly-spaced slots, and assumes half a slot of margin
    // past the first/last category center. A numeric x (as opposed to
    // categorical string labels, which already got a tuned 0.6-unit pad)
    // previously fell through to the generic 3%-of-range padding instead --
    // confirmed live that with closely-spaced x values that's nowhere near
    // enough: bars spilled past the y-axis on the left and past the canvas
    // edge entirely on the right.
    double xPad;
    if (spec.type == "bar") {
        double avgSpacing = (spec.xNumeric.size() > 1) ? (xMax - xMin) / (double)(spec.xNumeric.size() - 1) : 1.0;
        xPad = avgSpacing * 0.6;
    } else {
        xPad = spec.categorical ? 0.6 : (xMax - xMin) * 0.03;
    }

    auto xToPx = [&](double x) { return plotX0 + (x - (xMin - xPad)) / ((xMax + xPad) - (xMin - xPad)) * plotW; };
    auto yToPx = [&](double y) { return plotY1 - (y - yMin) / (yMax - yMin) * plotH; };

    constexpr int kYTicks = 5;
    cairo_set_line_width(cr, 1.0);
    cairo_set_font_size(cr, 10.5);
    for (int i = 0; i <= kYTicks; ++i) {
        double yVal = yMin + (yMax - yMin) * i / kYTicks;
        double py = yToPx(yVal);
        cairo_set_source_rgb(cr, gridColor.r, gridColor.g, gridColor.b);
        cairo_move_to(cr, plotX0, py);
        cairo_line_to(cr, plotX1, py);
        cairo_stroke(cr);

        std::string label = formatTick(yVal) + (spec.stackedPercent ? "%" : "");
        cairo_text_extents_t ext;
        cairo_text_extents(cr, label.c_str(), &ext);
        cairo_set_source_rgb(cr, fg.r, fg.g, fg.b);
        cairo_move_to(cr, plotX0 - ext.width - ext.x_bearing - 8, py - (ext.height / 2 + ext.y_bearing));
        cairo_show_text(cr, label.c_str());
    }

    size_t n = spec.xNumeric.size();
    size_t xTickStride = std::max<size_t>(1, n / 10); // thin out labels for long series
    for (size_t i = 0; i < n; i += xTickStride) {
        double px = xToPx(spec.xNumeric[i]);
        std::string label = spec.categorical ? spec.xLabels[i] : formatTick(spec.xNumeric[i]);
        cairo_text_extents_t ext;
        cairo_text_extents(cr, label.c_str(), &ext);
        cairo_set_source_rgb(cr, fg.r, fg.g, fg.b);
        cairo_move_to(cr, px - (ext.width / 2 + ext.x_bearing), plotY1 + ext.height + 8);
        cairo_show_text(cr, label.c_str());
    }

    cairo_set_source_rgb(cr, fg.r, fg.g, fg.b);
    cairo_set_line_width(cr, 1.2);
    cairo_move_to(cr, plotX0, plotY0); cairo_line_to(cr, plotX0, plotY1); cairo_stroke(cr);
    cairo_move_to(cr, plotX0, plotY1); cairo_line_to(cr, plotX1, plotY1); cairo_stroke(cr);

    size_t seriesCount = spec.series.size();
    // Running per-category cumulative offsets for stacked bars -- positive
    // and negative segments stack independently (positives build upward
    // from 0, negatives build downward from 0), matching the yMin/yMax
    // computation above. Unused for grouped bars/line/scatter.
    std::vector<double> stackPos(n, 0.0), stackNeg(n, 0.0);
    for (size_t si = 0; si < seriesCount; ++si) {
        const SeriesData &s = spec.series[si];
        const Rgb &color = kPalette[si % kPaletteSize];
        cairo_set_source_rgb(cr, color.r, color.g, color.b);

        if (spec.type == "bar" && stacked) {
            double slot = plotW / (double)n;
            double barW = slot * 0.7;
            for (size_t i = 0; i < n; ++i) {
                double v = s.y[i];
                double base = (v >= 0) ? stackPos[i] : stackNeg[i];
                double top = base + v;
                double cx = xToPx(spec.xNumeric[i]);
                double bx = cx - barW / 2;
                double byBase = yToPx(base), byTop = yToPx(top);
                cairo_rectangle(cr, bx, std::min(byBase, byTop), barW, std::abs(byBase - byTop));
                cairo_fill(cr);
                (v >= 0 ? stackPos[i] : stackNeg[i]) = top;
            }
        } else if (spec.type == "bar") {
            double slot = plotW / (double)n;
            double groupW = slot * 0.7;
            double barW = groupW / (double)seriesCount;
            double baseVal = (yMin <= 0 && yMax >= 0) ? 0.0 : yMin;
            double baseY = yToPx(baseVal);
            for (size_t i = 0; i < n; ++i) {
                double cx = xToPx(spec.xNumeric[i]);
                double bx = cx - groupW / 2 + barW * si;
                double by = yToPx(s.y[i]);
                cairo_rectangle(cr, bx, std::min(by, baseY), barW * 0.9, std::abs(baseY - by));
                cairo_fill(cr);
            }
        } else if (spec.type == "line") {
            cairo_set_line_width(cr, 1.8);
            for (size_t i = 0; i < n; ++i) {
                double px = xToPx(spec.xNumeric[i]), py = yToPx(s.y[i]);
                if (i == 0) cairo_move_to(cr, px, py); else cairo_line_to(cr, px, py);
            }
            cairo_stroke(cr);
            if (s.marker != "none") {
                for (size_t i = 0; i < n; ++i) {
                    cairo_arc(cr, xToPx(spec.xNumeric[i]), yToPx(s.y[i]), 2.6, 0, 2 * M_PI);
                    cairo_fill(cr);
                }
            }
        } else { // scatter
            for (size_t i = 0; i < n; ++i) {
                cairo_arc(cr, xToPx(spec.xNumeric[i]), yToPx(s.y[i]), 3.2, 0, 2 * M_PI);
                cairo_fill(cr);
            }
        }
    }

    cairo_set_source_rgb(cr, fg.r, fg.g, fg.b);
    if (!spec.title.empty()) {
        // Title uses the CSS heading's own font-family/weight; switched
        // back to bodyFont right after so xlabel/ylabel/legend (all body
        // text, not headings) aren't affected.
        cairo_select_font_face(cr, titleFont, CAIRO_FONT_SLANT_NORMAL,
                                titleBold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 13);
        cairo_text_extents_t ext;
        cairo_text_extents(cr, spec.title.c_str(), &ext);
        cairo_move_to(cr, plotX0 + plotW / 2 - (ext.width / 2 + ext.x_bearing), 20);
        cairo_show_text(cr, spec.title.c_str());
        cairo_select_font_face(cr, bodyFont, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    }
    cairo_set_font_size(cr, 11);
    if (!spec.xlabel.empty()) {
        cairo_text_extents_t ext;
        cairo_text_extents(cr, spec.xlabel.c_str(), &ext);
        cairo_move_to(cr, plotX0 + plotW / 2 - (ext.width / 2 + ext.x_bearing), h - 8);
        cairo_show_text(cr, spec.xlabel.c_str());
    }
    if (!spec.ylabel.empty()) {
        cairo_text_extents_t ext;
        cairo_text_extents(cr, spec.ylabel.c_str(), &ext);
        cairo_save(cr);
        cairo_translate(cr, 14, plotY0 + plotH / 2 + ext.width / 2);
        cairo_rotate(cr, -M_PI / 2);
        cairo_move_to(cr, 0, 0);
        cairo_show_text(cr, spec.ylabel.c_str());
        cairo_restore(cr);
    }

    if (hasLegend) {
        double legendY = spec.title.empty() ? 15 : 32;
        double lx = plotX0;
        cairo_set_font_size(cr, 10.5);
        for (size_t si = 0; si < seriesCount; ++si) {
            const SeriesData &s = spec.series[si];
            std::string label = s.label.empty() ? ("Series " + std::to_string(si + 1)) : s.label;
            const Rgb &color = kPalette[si % kPaletteSize];
            cairo_set_source_rgb(cr, color.r, color.g, color.b);
            cairo_rectangle(cr, lx, legendY - 8, 10, 10);
            cairo_fill(cr);
            cairo_set_source_rgb(cr, fg.r, fg.g, fg.b);
            cairo_move_to(cr, lx + 14, legendY + 1);
            cairo_show_text(cr, label.c_str());
            cairo_text_extents_t ext;
            cairo_text_extents(cr, label.c_str(), &ext);
            lx += 14 + ext.width + 18;
        }
    }

    std::string pngBytes;
    cairo_surface_write_to_png_stream(surface, writeToString, &pngBytes);

    cairo_destroy(cr);
    cairo_surface_destroy(surface);

    return std::vector<uint8_t>(pngBytes.begin(), pngBytes.end());
}

} // namespace ChartRender
