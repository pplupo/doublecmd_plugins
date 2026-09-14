#include "vegalite_spec.h"

#include "../../3rdparty/nlohmann_json/json.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace {

using nlohmann::json;

// =============================================================================
// TITLE / SUBTITLE WRAPPING
//
// Vega-Lite never wraps title or subtitle text: a title line is laid out
// at its full natural width, and if that exceeds the plot, the exported
// image simply grows to fit it. A figure with a compact plot and a long
// explanatory subtitle therefore renders with the plot pinned left and a
// wide band of empty background to its right -- confirmed live across
// several report figures, and not something the spec's own "width" can
// fix, since width sizes the PLOT rather than the canvas.
//
// Splitting the text into explicit lines is the only lever available:
// Vega-Lite accepts an ARRAY of strings for both title and subtitle and
// stacks them. Doing that here, from the plot width the spec already
// declares, keeps it automatic -- no per-figure hand-tuning, and a spec
// that already supplies its own array (an author who wrapped
// deliberately) is left exactly as written.
// =============================================================================

// Greedy wrap on word boundaries. The character budget is derived from the
// plot width and font size rather than measured: an over-long line costs
// some empty canvas (the very thing being fixed) and an over-short one
// costs an extra line of height, so an approximation is acceptable in
// both directions.
json wrapToLines(const std::string &text, double widthPx, double fontSizePx) {
    const double avgCharPx = fontSizePx * 0.52;
    size_t maxChars = (size_t)std::max(24.0, widthPx / std::max(1.0, avgCharPx));
    if (text.size() <= maxChars) return json(text); // already fits -- leave it a plain string

    json lines = json::array();
    std::istringstream words(text);
    std::string word, line;
    while (words >> word) {
        if (!line.empty() && line.size() + 1 + word.size() > maxChars) {
            lines.push_back(line);
            line = word;
        } else {
            line = line.empty() ? word : line + " " + word;
        }
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
}

// =============================================================================
// LEGEND COMPACTION
//
// A bottom-oriented legend lays its entries out in a horizontal run, and
// several legends side by side run wider still. When that run is wider
// than the plot, the exported canvas grows to fit IT -- the same failure
// as an unwrapped subtitle (see above), and the dominant cause in
// practice: on a real report figure the legends held the canvas at 932px
// wide while the plot needed ~500, and wrapping the subtitle alone
// changed nothing because the legends, not the text, were binding.
//
// Vega-Lite has no "stack these legends" control, but each legend can
// stack its OWN entries with direction:"vertical", which turns a long
// horizontal run into a narrow column and lets the canvas shrink to the
// plot. Applied only when the estimated run actually exceeds the plot
// width, so a compact legend that already fits keeps its normal
// horizontal layout.
// =============================================================================

// Collects every encoding that carries a legend, from the top-level
// encoding and from each layer's.
void forEachLegendEncoding(json &spec, const std::function<void(json &)> &fn) {
    // Every channel that PRODUCES a legend, not only those that spell one
    // out: an encoding legends by default, so requiring an explicit
    // "legend" object here skipped exactly the common case and left those
    // legends to Vega-Lite's own left-aligned bottom flow. A legend
    // switched off with "legend": null is respected and left alone.
    static const char *kLegendChannels[] = {"color", "size", "shape", "opacity", "strokeDash", "fill", "stroke"};
    auto visit = [&](json &enc) {
        if (!enc.is_object()) return;
        for (const char *channel : kLegendChannels) {
            if (!enc.contains(channel)) continue;
            json &e = enc[channel];
            if (!e.is_object() || !e.contains("field")) continue;
            if (e.contains("legend") && e["legend"].is_null()) continue; // deliberately hidden
            if (!e.contains("legend") || !e["legend"].is_object()) e["legend"] = json::object();
            fn(e);
        }
    };
    if (spec.contains("encoding")) visit(spec["encoding"]);
    if (spec.contains("layer") && spec["layer"].is_array())
        for (auto &layer : spec["layer"]) if (layer.contains("encoding")) visit(layer["encoding"]);
}

// Label metrics for one legend: how many entries, the total label length
// (which sets a horizontal row's width) and the longest single label
// (which sets a vertical column's width). Labels come from the scale
// domain when it is a literal list of strings; a legend built by a
// labelExpr has no readable labels here, so those fall back to nominal
// figures rather than being ignored.
struct LegendMetrics { size_t entries = 0; double totalChars = 0, longestChars = 0; };

LegendMetrics legendMetrics(const json &e) {
    LegendMetrics m;
    const json &scale = e.value("scale", json::object());
    if (scale.contains("domain") && scale["domain"].is_array()) {
        for (const auto &v : scale["domain"]) {
            ++m.entries;
            double n = v.is_string() ? (double)v.get<std::string>().size() : 8.0;
            m.totalChars += n;
            m.longestChars = std::max(m.longestChars, n);
        }
    }
    const json &legend = e.at("legend");
    if (m.entries == 0 && legend.contains("values") && legend["values"].is_array())
        m.entries = legend["values"].size();
    if (m.entries == 0) m.entries = 3;
    if (m.totalChars == 0) { m.totalChars = (double)m.entries * 14.0; m.longestChars = 14.0; }
    if (legend.contains("title") && legend["title"].is_string())
        m.longestChars = std::max(m.longestChars, (double)legend["title"].get<std::string>().size());
    return m;
}

// Places every legend on its own row beneath the plot, centred on the
// plot's width.
//
// Vega-Lite's own bottom placement left-aligns legends to the plot's left
// edge and flows several of them along one line -- which strands white
// space on the left and, measured on a real report figure, let one legend
// row push the canvas to 1130px around a 420px plot. There is no property
// for "one legend per row, centred", so they are positioned explicitly
// (orient "none"), which is the only way to control either.
//
// Widths have to be estimated, since the real laid-out size isn't knowable
// before rendering. A horizontal row's width is the SUM of its labels and
// estimates poorly; a vertical column's is just its longest label and
// estimates well. So a legend that would need a wide row is stacked into a
// column first, which both narrows it and makes its centring dependable.
void layoutLegends(json &spec) {
    double width = spec.contains("width") && spec["width"].is_number()
                       ? spec["width"].get<double>() : 480.0;
    double height = spec.contains("height") && spec["height"].is_number()
                        ? spec["height"].get<double>() : 300.0;
    double labelSize = spec.value("config", json::object())
                           .value("legend", json::object()).value("labelFontSize", 11.0);

    std::vector<json *> legends;
    forEachLegendEncoding(spec, [&](json &e) { legends.push_back(&e); });
    if (legends.empty()) return;

    // Legends share ONE left edge rather than each being centred on its
    // own width. Centring them individually lines their left edges up
    // differently -- a narrow legend above a wide one reads as indented,
    // which looks like a mistake rather than a layout. Centring the
    // widest and left-aligning the rest to it keeps the block centred
    // while the legends stay flush with each other.
    struct Placement { json *legend; bool stack; size_t entries; double w; };
    std::vector<Placement> placements;
    double widest = 0.0;
    for (json *e : legends) {
        LegendMetrics m = legendMetrics(*e);
        double rowWidth = m.totalChars * labelSize * 0.55 + (double)m.entries * 26.0 + 30.0;
        double colWidth = m.longestChars * labelSize * 0.55 + 46.0;
        // Compared against the CANVAS, not the plot: the plot is only the
        // data rectangle, while the image is wider by the y-axis labels,
        // the axis title and the padding, so a legend row that overruns
        // "width" can still sit comfortably in the image. Measured across
        // these figures the canvas runs roughly 1.8x the plot width.
        bool stack = rowWidth > width * 1.8;
        double w = stack ? colWidth : rowWidth;
        widest = std::max(widest, w);
        placements.push_back({&(*e)["legend"], stack, m.entries, w});
    }

    // Cleared beneath the plot and its x axis. The axis chrome's real
    // depth isn't knowable here; 78px left a visibly dead band between
    // the axis title and the legend, so this is roughly what an axis
    // label plus title actually occupies.
    double rowY = height + 52.0;
    double sharedX = std::max(0.0, (width - widest) / 2.0);
    for (const Placement &p : placements) {
        json &legend = *p.legend;
        legend["orient"] = "none";
        legend["direction"] = p.stack ? "vertical" : "horizontal";
        // "columns" outranks "direction" in Vega-Lite's legend layout, so
        // a spec that pins it keeps its multi-column block no matter what
        // direction says -- confirmed live. Pin to one column when
        // stacking, and leave it untouched otherwise.
        if (p.stack) legend["columns"] = 1;
        legend["legendX"] = sharedX;
        legend["legendY"] = rowY;
        legend["titleAnchor"] = "start";
        // A stacked legend occupies as many lines as it has entries, so
        // the next legend clears all of them rather than a fixed row.
        rowY += p.stack ? ((double)p.entries * (labelSize + 7.0) + 30.0) : 44.0;
    }
}

void wrapSpecTitle(json &spec) {
    if (!spec.contains("title")) return;
    // Only the plot width is knowable here; the axis and legend chrome
    // around it is not, so this deliberately under-estimates the usable
    // width rather than over-estimate it and leave the blank band behind.
    double width = spec.contains("width") && spec["width"].is_number()
                       ? spec["width"].get<double>() : 480.0;
    json cfgTitle = spec.value("config", json::object()).value("title", json::object());
    double titleSize = cfgTitle.value("fontSize", 15.0);
    double subtitleSize = cfgTitle.value("subtitleFontSize", 11.0);

    json &title = spec["title"];
    if (title.is_string()) {
        title = json{{"text", wrapToLines(title.get<std::string>(), width, titleSize)}};
        return;
    }
    if (!title.is_object()) return;
    if (title.contains("text") && title["text"].is_string())
        title["text"] = wrapToLines(title["text"].get<std::string>(), width, titleSize);
    if (title.contains("subtitle") && title["subtitle"].is_string())
        title["subtitle"] = wrapToLines(title["subtitle"].get<std::string>(), width, subtitleSize);
}

} // namespace

namespace VegaLiteSpec {

std::string captionOf(const std::string &vegaLiteJson) {
    try {
        json spec = json::parse(vegaLiteJson);
        if (spec.is_object() && spec.contains("_caption") && spec["_caption"].is_string())
            return spec["_caption"].get<std::string>();
    } catch (const std::exception &) {
        // A malformed spec has no caption to show; the render path
        // reports the failure, this stays quiet.
    }
    return {};
}

std::string normalizeSpec(const std::string &vegaLiteJson, bool darkMode,
                          const std::string &bodyFontFamily) {
    json spec;
    try {
        spec = json::parse(vegaLiteJson);
    } catch (const std::exception &) {
        return {}; // malformed JSON -- caller leaves the fenced block as plain text
    }
    if (!spec.is_object()) return {};

    // Drop underscore-prefixed top-level keys before handing the spec
    // over. Report figures carry provenance metadata inline (_caption,
    // _note, _src, _calc) so it travels with the spec and needs no
    // sidecar file; Vega-Lite ignores unrecognised fields inside DATA
    // rows, but a top-level property outside its schema is not something
    // to rely on it tolerating. Stripping them also keeps that metadata
    // off the wire when the spec is being POSTed to a Kroki endpoint.
    for (auto it = spec.begin(); it != spec.end(); ) {
        if (!it.key().empty() && it.key()[0] == '_') it = spec.erase(it);
        else ++it;
    }

    // Theme defaults are written UNDERNEATH the spec's own config, never
    // over it: a spec that ships a config block has already made these
    // choices deliberately, and silently repainting it would be the
    // renderer overruling its author. Only keys the spec left unset get
    // filled in, so a light-authored figure set keeps rendering light.
    json &config = spec["config"];
    if (!config.is_object()) config = json::object();
    if (!config.contains("background")) config["background"] = darkMode ? "#14171c" : "#fcfcfb";
    if (!config.contains("font") && !bodyFontFamily.empty()) config["font"] = bodyFontFamily;
    // Vega-Lite truncates legend labels at ~160px by default, which cuts
    // exactly the categories that need their full name to be worth
    // distinguishing (two same-metric series separated only by their
    // source, say). Filled in per-key rather than per-block, so a spec
    // that sets its own limit keeps it.
    if (!config["legend"].is_object()) config["legend"] = json::object();
    if (!config["legend"].contains("labelLimit")) config["legend"]["labelLimit"] = 260;
    // Title centred over the figure. This deliberately OVERRIDES a spec's
    // own anchor rather than filling in underneath it like the defaults
    // above: a left-anchored title over a centred figure reads as
    // belonging to the page rather than to the chart, and the alignment
    // of title, legend and plot needs to be decided in one place to stay
    // consistent across a whole figure set.
    if (!config["title"].is_object()) config["title"] = json::object();
    config["title"]["anchor"] = "middle";

    wrapSpecTitle(spec);
    layoutLegends(spec);

    return spec.dump();
}

} // namespace VegaLiteSpec
