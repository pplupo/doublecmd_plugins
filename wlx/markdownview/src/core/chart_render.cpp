#include "chart_render.h"

#include "../../3rdparty/nlohmann_json/json.hpp"

#include <cairo.h>
#include <cmath>
#include <algorithm>
#include <sstream>
#include <map>
#include <set>

// One extent function + one draw function per mark type, mirroring
// ~/repos/reports' charts.py's _RENDERERS dispatch table (one explicit
// function per type, no generic kwargs pass-through). "layers" lets a
// panel combine several mark types on one shared axis system, exactly like
// charts.py's _render_single(): each layer's extent function extends a
// running (xmin,xmax,ymin,ymax) bounding box (matplotlib's own autoscale
// does the equivalent by unioning each artist's data limits), THEN the
// axis/gridlines/ticks are drawn once from the finalized box, THEN each
// layer's draw function paints onto that same coordinate mapping in order.
//
// heatmap and pie are the two types with no shared x/y coordinate system at
// all (a pixel grid indexed by matrix position; a radial layout) -- these
// are only supported as a panel's sole layer, rendered through their own
// self-contained path instead of the extent/draw dispatch below. Nothing
// stops a spec from putting "type":"heatmap" inside a multi-layer "layers"
// list (matplotlib wouldn't stop you either), but the result is undefined
// here; realistic composites (charts.py's own docstring examples: a
// dumbbell = thin barh + two scatter points; a timeline = barh duration
// bars + scatter pins + annotations) only ever combine the Cartesian types.

namespace {

using nlohmann::json;

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

bool parseHexColor(const std::string &s, Rgb &out) {
    if (s.size() != 7 || s[0] != '#') return false;
    try {
        out = {std::stoi(s.substr(1, 2), nullptr, 16) / 255.0,
               std::stoi(s.substr(3, 2), nullptr, 16) / 255.0,
               std::stoi(s.substr(5, 2), nullptr, 16) / 255.0};
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

std::vector<double> numArray(const json &arr) {
    std::vector<double> r;
    if (arr.is_array()) for (const auto &v : arr) if (v.is_number()) r.push_back(v.get<double>());
    return r;
}

// A JSON array of axis positions: either all-numeric (a real numeric axis)
// or all-string (categorical positions 0..n-1, with the strings as labels).
struct AxisPositions {
    std::vector<double> pos;
    std::vector<std::string> labels;
    bool categorical = false;
};
AxisPositions parseAxisArray(const json &arr) {
    AxisPositions r;
    if (!arr.is_array() || arr.empty()) return r;
    bool allNumeric = true;
    for (const auto &v : arr) if (!v.is_number()) { allNumeric = false; break; }
    if (allNumeric) {
        for (const auto &v : arr) r.pos.push_back(v.get<double>());
    } else {
        r.categorical = true;
        for (const auto &v : arr) r.labels.push_back(v.is_string() ? v.get<std::string>() : v.dump());
        for (size_t i = 0; i < r.labels.size(); ++i) r.pos.push_back((double)i);
    }
    return r;
}

// --- Spec model: one Layer per mark, grouped into panels, grouped into
// the whole figure ("panels" top-level layout). ------------------------

struct Layer {
    json j;
    std::string type;
};

struct RefLine { std::string axis = "y"; double value = 0; std::string label; std::string colorHex; };
struct RefBand { std::string axis = "y"; double low = 0, high = 0; std::string label; std::string colorHex; };
struct Annotation { double x = 0, y = 0; std::string text; double dx = 0, dy = 10; std::string ha = "center"; double fontsize = 9; };

struct PanelSpec {
    std::vector<Layer> layers;
    std::string title, xlabel, ylabel;
    bool logX = false, logY = false;
    bool yIsPercent = false; // any layer was stacked:"percent" -- y ticks get a "%" suffix
    std::vector<RefLine> refLines;
    std::vector<RefBand> refBands;
    std::vector<Annotation> annotations;
};

struct TopSpec {
    std::vector<PanelSpec> panels;
    bool sharedX = true;
    std::string title;
    double figW = 6.0, figH = 4.0; // inches, same field/units as charts.py's spec["figsize"]
};

const std::set<std::string> kKnownTypes = {
    "line", "bar", "barh", "scatter", "area", "step", "stem", "errorbar",
    "histogram", "boxplot", "violin", "heatmap", "pie"
};

// stacked:"percent" (a plugin-specific addition beyond charts.py's own bar
// spec, which only has a bool) rescales each category's series to sum to
// 100 -- done once here, in place on the layer's own JSON, so extent/draw
// functions never need to know percent vs plain stacking, only the
// resulting bool "stacked". Returns true if this layer was percent-mode
// (so the caller can set the panel's yIsPercent flag for the "%" y-tick
// suffix).
bool preprocessBarLayerPercent(json &L) {
    if (!L.contains("stacked") || !L["stacked"].is_string() || L["stacked"].get<std::string>() != "percent")
        return false;
    L["stacked"] = true;
    if (!L.contains("series") || !L["series"].is_array() || L["series"].size() < 2) return false;
    size_t n = L.contains("x") && L["x"].is_array() ? L["x"].size() : 0;
    for (size_t i = 0; i < n; ++i) {
        double total = 0;
        for (auto &s : L["series"])
            if (s.is_object() && s.contains("y") && s["y"].is_array() && i < s["y"].size() && s["y"][i].is_number())
                total += s["y"][i].get<double>();
        if (total > 0) {
            for (auto &s : L["series"])
                if (s.is_object() && s.contains("y") && s["y"].is_array() && i < s["y"].size() && s["y"][i].is_number())
                    s["y"][i] = s["y"][i].get<double>() / total * 100.0;
        }
    }
    return true;
}

bool parsePanelSpec(const json &j, PanelSpec &out) {
    if (!j.is_object()) return false;
    out.title = j.value("title", std::string());
    out.xlabel = j.value("xlabel", std::string());
    out.ylabel = j.value("ylabel", std::string());
    out.logX = j.value("log_x", false);
    out.logY = j.value("log_y", false);

    auto addLayer = [&](json lj) -> bool {
        if (!lj.is_object()) return false;
        std::string t = lj.value("type", std::string("line"));
        if (!kKnownTypes.count(t)) return false;
        if (t == "bar" && preprocessBarLayerPercent(lj)) out.yIsPercent = true;
        out.layers.push_back({std::move(lj), t});
        return true;
    };

    if (j.contains("layers") && j["layers"].is_array() && !j["layers"].empty()) {
        for (const auto &lj : j["layers"]) if (!addLayer(lj)) return false;
    } else {
        if (!addLayer(j)) return false;
    }

    if (j.contains("ref_lines") && j["ref_lines"].is_array()) {
        for (const auto &rl : j["ref_lines"]) {
            if (!rl.is_object() || !rl.contains("value") || !rl["value"].is_number()) continue;
            RefLine r;
            r.axis = rl.value("axis", std::string("y"));
            r.value = rl["value"].get<double>();
            r.label = rl.value("label", std::string());
            if (rl.contains("style") && rl["style"].is_object() && rl["style"].value("color", json()).is_string())
                r.colorHex = rl["style"]["color"].get<std::string>();
            out.refLines.push_back(r);
        }
    }
    if (j.contains("ref_bands") && j["ref_bands"].is_array()) {
        for (const auto &rb : j["ref_bands"]) {
            if (!rb.is_object() || !rb.contains("low") || !rb.contains("high") ||
                !rb["low"].is_number() || !rb["high"].is_number()) continue;
            RefBand r;
            r.axis = rb.value("axis", std::string("y"));
            r.low = rb["low"].get<double>();
            r.high = rb["high"].get<double>();
            r.label = rb.value("label", std::string());
            if (rb.contains("style") && rb["style"].is_object() && rb["style"].value("color", json()).is_string())
                r.colorHex = rb["style"]["color"].get<std::string>();
            out.refBands.push_back(r);
        }
    }
    if (j.contains("annotations") && j["annotations"].is_array()) {
        for (const auto &an : j["annotations"]) {
            if (!an.is_object() || !an.contains("x") || !an.contains("y") || !an.contains("text") ||
                !an["x"].is_number() || !an["y"].is_number()) continue;
            Annotation a;
            a.x = an["x"].get<double>();
            a.y = an["y"].get<double>();
            a.text = an["text"].get<std::string>();
            if (an.contains("xytext") && an["xytext"].is_array() && an["xytext"].size() == 2 &&
                an["xytext"][0].is_number() && an["xytext"][1].is_number()) {
                a.dx = an["xytext"][0].get<double>();
                a.dy = an["xytext"][1].get<double>();
            }
            a.ha = an.value("ha", std::string("center"));
            a.fontsize = an.value("fontsize", 9.0);
            out.annotations.push_back(a);
        }
    }
    return true;
}

bool parseTopSpec(const std::string &specJson, TopSpec &out) {
    json j;
    try {
        j = json::parse(specJson);
    } catch (const std::exception &) {
        return false;
    }
    if (!j.is_object()) return false;

    bool hasFigsize = j.contains("figsize") && j["figsize"].is_array() && j["figsize"].size() == 2 &&
                       j["figsize"][0].is_number() && j["figsize"][1].is_number();
    if (hasFigsize) {
        out.figW = j["figsize"][0].get<double>();
        out.figH = j["figsize"][1].get<double>();
    }

    if (j.contains("panels") && j["panels"].is_array() && !j["panels"].empty()) {
        // "title" is only a separate figure-level suptitle in the
        // multi-panel case (matches charts.py's own `if panels:
        // fig.suptitle(...)`) -- for a single-panel spec, the same "title"
        // field belongs to the panel alone (parsePanelSpec reads it below,
        // via the shared `j`). Reading it here unconditionally duplicated
        // it: confirmed live, a single-panel spec rendered its title twice
        // (once as this figure suptitle, once as the panel's own ax title).
        out.title = j.value("title", std::string());
        out.sharedX = j.value("shared_x", true);
        for (const auto &pj : j["panels"]) {
            PanelSpec ps;
            if (!parsePanelSpec(pj, ps)) return false;
            out.panels.push_back(std::move(ps));
        }
        // charts.py's own multi-panel default: figsize=(6, 3.5 * n).
        if (!hasFigsize) out.figH = 3.5 * out.panels.size();
    } else {
        PanelSpec ps;
        if (!parsePanelSpec(j, ps)) return false;
        out.panels.push_back(std::move(ps));
    }
    return !out.panels.empty();
}

// --- Axis extent accumulation: each layer's extent function extends this
// shared running bounding box, mirroring matplotlib's own per-artist
// autoscale union. -------------------------------------------------------

struct AxisRange {
    double xmin = INFINITY, xmax = -INFINITY, ymin = INFINITY, ymax = -INFINITY;
    bool xCat = false;
    std::map<int, std::string> xCatLabels; // absolute integer position -> label (bar: 0-based, boxplot/violin: 1-based)
    bool yCat = false;
    std::map<int, std::string> yCatLabels; // barh only
};
void extendX(AxisRange &r, double v) { if (std::isfinite(v)) { r.xmin = std::min(r.xmin, v); r.xmax = std::max(r.xmax, v); } }
void extendY(AxisRange &r, double v) { if (std::isfinite(v)) { r.ymin = std::min(r.ymin, v); r.ymax = std::max(r.ymax, v); } }

// line/area/step share this x + (y | series) shape.
void extentXY(const json &L, AxisRange &r) {
    if (!L.contains("x")) return;
    AxisPositions xa = parseAxisArray(L["x"]);
    if (xa.categorical && !r.xCat) { r.xCat = true; for (size_t i = 0; i < xa.labels.size(); ++i) r.xCatLabels[(int)i] = xa.labels[i]; }
    for (double p : xa.pos) extendX(r, p);
    auto extY = [&](const json &yArr) { for (double v : numArray(yArr)) extendY(r, v); };
    if (L.contains("series") && L["series"].is_array()) {
        for (const auto &s : L["series"]) if (s.is_object() && s.contains("y")) extY(s["y"]);
    } else if (L.contains("y")) {
        extY(L["y"]);
    }
}

void extentScatter(const json &L, AxisRange &r) {
    if (L.contains("points") && L["points"].is_array()) {
        for (const auto &p : L["points"]) {
            if (p.is_object() && p.contains("x") && p["x"].is_number()) extendX(r, p["x"].get<double>());
            if (p.is_object() && p.contains("y") && p["y"].is_number()) extendY(r, p["y"].get<double>());
        }
    } else {
        extentXY(L, r);
    }
}

void extentStem(const json &L, AxisRange &r) {
    extentXY(L, r);
    extendY(r, 0.0); // stem always draws from a 0 baseline
}

void extentErrorbar(const json &L, AxisRange &r) {
    if (!L.contains("x") || !L.contains("y")) return;
    std::vector<double> xs = numArray(L["x"]), ys = numArray(L["y"]);
    std::vector<double> yerr = L.contains("yerr") ? numArray(L["yerr"]) : std::vector<double>();
    std::vector<double> xerr = L.contains("xerr") ? numArray(L["xerr"]) : std::vector<double>();
    for (size_t i = 0; i < xs.size() && i < ys.size(); ++i) {
        double ye = i < yerr.size() ? yerr[i] : 0.0, xe = i < xerr.size() ? xerr[i] : 0.0;
        extendX(r, xs[i] - xe); extendX(r, xs[i] + xe);
        extendY(r, ys[i] - ye); extendY(r, ys[i] + ye);
    }
}

void extentBar(const json &L, AxisRange &r) {
    if (!L.contains("x")) return;
    AxisPositions xa = parseAxisArray(L["x"]);
    if (xa.categorical && !r.xCat) { r.xCat = true; for (size_t i = 0; i < xa.labels.size(); ++i) r.xCatLabels[(int)i] = xa.labels[i]; }
    for (double p : xa.pos) extendX(r, p);
    size_t n = xa.pos.size();

    std::vector<std::vector<double>> seriesY;
    if (L.contains("series") && L["series"].is_array()) {
        for (const auto &s : L["series"]) if (s.is_object() && s.contains("y")) seriesY.push_back(numArray(s["y"]));
    } else if (L.contains("y")) {
        seriesY.push_back(numArray(L["y"]));
    }
    bool stacked = L.value("stacked", false) && seriesY.size() > 1;
    if (stacked) {
        for (size_t i = 0; i < n; ++i) {
            double pos = 0, neg = 0;
            for (const auto &sy : seriesY) if (i < sy.size()) { double v = sy[i]; if (v >= 0) pos += v; else neg += v; }
            extendY(r, pos); extendY(r, neg);
        }
    } else {
        for (const auto &sy : seriesY) for (double v : sy) { extendY(r, v); extendY(r, 0.0); }
    }
}

void extentBarh(const json &L, AxisRange &r) {
    if (L.contains("bars") && L["bars"].is_array()) {
        std::vector<std::string> cats;
        for (const auto &b : L["bars"]) {
            if (!b.is_object()) continue;
            std::string cat = b.contains("y") ? (b["y"].is_string() ? b["y"].get<std::string>() : b["y"].dump()) : "";
            if (std::find(cats.begin(), cats.end(), cat) == cats.end()) cats.push_back(cat);
            double width = b.value("width", 0.0), left = b.value("left", 0.0);
            extendX(r, left); extendX(r, left + width);
        }
        if (!r.yCat) { r.yCat = true; for (size_t i = 0; i < cats.size(); ++i) r.yCatLabels[(int)i] = cats[i]; }
        for (size_t i = 0; i < cats.size(); ++i) extendY(r, (double)i);
    } else if (L.contains("x") && L.contains("y")) {
        AxisPositions ya = parseAxisArray(L["y"]);
        if (ya.categorical && !r.yCat) { r.yCat = true; for (size_t i = 0; i < ya.labels.size(); ++i) r.yCatLabels[(int)i] = ya.labels[i]; }
        for (double p : ya.pos) extendY(r, p);
        for (double v : numArray(L["x"])) { extendX(r, v); extendX(r, 0.0); }
    }
}

// Histogram binning: recomputed identically (a pure function of the
// layer's own JSON) in both the extent pass and the draw pass, rather than
// cached across them -- cheap enough, and avoids a second cross-pass state
// structure alongside AxisRange.
struct HistResult { std::vector<double> edges; std::vector<double> counts; };
HistResult computeHistogram(const json &L) {
    HistResult hr;
    std::vector<double> values = L.contains("values") ? numArray(L["values"]) : std::vector<double>();
    if (values.empty()) return hr;
    int nbins = 10;
    std::vector<double> edges;
    if (L.contains("bins")) {
        if (L["bins"].is_number_integer()) nbins = L["bins"].get<int>();
        else if (L["bins"].is_array()) edges = numArray(L["bins"]);
    }
    if (edges.empty()) {
        double lo = *std::min_element(values.begin(), values.end());
        double hi = *std::max_element(values.begin(), values.end());
        if (hi <= lo) { hi = lo + 1; lo -= 1; }
        nbins = std::max(1, nbins);
        edges.resize(nbins + 1);
        for (int i = 0; i <= nbins; ++i) edges[i] = lo + (hi - lo) * i / nbins;
    }
    if (edges.size() < 2) return hr;
    hr.edges = edges;
    hr.counts.assign(edges.size() - 1, 0.0);
    for (double v : values) {
        for (size_t i = 0; i + 1 < edges.size(); ++i) {
            bool lastBin = (i + 2 == edges.size());
            if (v >= edges[i] && (v < edges[i + 1] || (lastBin && v <= edges[i + 1]))) { hr.counts[i] += 1; break; }
        }
    }
    return hr;
}
void extentHistogram(const json &L, AxisRange &r) {
    HistResult hr = computeHistogram(L);
    if (hr.edges.empty()) return;
    extendX(r, hr.edges.front()); extendX(r, hr.edges.back());
    extendY(r, 0.0);
    double maxCount = 0; for (double c : hr.counts) maxCount = std::max(maxCount, c);
    extendY(r, maxCount);
}

void extentBoxplotXCat(const json &L, AxisRange &r) {
    if (!L.contains("data") || !L["data"].is_array()) return;
    size_t n = L["data"].size();
    std::vector<std::string> labels;
    if (L.contains("labels") && L["labels"].is_array())
        for (const auto &v : L["labels"]) labels.push_back(v.is_string() ? v.get<std::string>() : v.dump());
    if (!r.xCat) {
        r.xCat = true;
        for (size_t i = 0; i < labels.size(); ++i) r.xCatLabels[(int)(i + 1)] = labels[i];
    }
    for (size_t i = 1; i <= n; ++i) extendX(r, (double)i);
}

void extentBoxplot(const json &L, AxisRange &r) {
    extentBoxplotXCat(L, r);
    if (!L.contains("data") || !L["data"].is_array()) return;
    for (const auto &arr : L["data"]) for (double v : numArray(arr)) extendY(r, v);
}

// Violin's drawn shape extends beyond the raw data min/max (the KDE curve
// tapers off over vmin-pad..vmax+pad, see drawViolin) -- extending Y by
// only the raw values here left the curve clipped against the plot's top
// edge whenever that pad pushed it past the data's own max. Must mirror
// drawViolin's pad computation exactly, not just the data range.
void extentViolin(const json &L, AxisRange &r) {
    extentBoxplotXCat(L, r);
    if (!L.contains("data") || !L["data"].is_array()) return;
    for (const auto &arrJson : L["data"]) {
        std::vector<double> v = numArray(arrJson);
        if (v.size() < 2) { for (double x : v) extendY(r, x); continue; }
        double mean = 0; for (double x : v) mean += x; mean /= v.size();
        double var = 0; for (double x : v) var += (x - mean) * (x - mean); var /= (v.size() - 1);
        double sigma = std::sqrt(std::max(var, 1e-9));
        double bandwidth = std::max(1.06 * sigma * std::pow((double)v.size(), -0.2), 1e-6);
        double vmin = *std::min_element(v.begin(), v.end()), vmax = *std::max_element(v.begin(), v.end());
        double pad = (vmax - vmin) * 0.15 + bandwidth;
        extendY(r, vmin - pad); extendY(r, vmax + pad);
    }
}

void extentForType(const std::string &type, const json &L, AxisRange &r) {
    if (type == "line" || type == "area" || type == "step") extentXY(L, r);
    else if (type == "scatter") extentScatter(L, r);
    else if (type == "stem") extentStem(L, r);
    else if (type == "errorbar") extentErrorbar(L, r);
    else if (type == "bar") extentBar(L, r);
    else if (type == "barh") extentBarh(L, r);
    else if (type == "histogram") extentHistogram(L, r);
    else if (type == "boxplot") extentBoxplot(L, r);
    else if (type == "violin") extentViolin(L, r);
    // heatmap/pie: handled by their own whole-panel renderer, never reach here.
}

// --- Draw-pass coordinate mapping + shared per-panel state -------------

struct PlotCtx {
    double plotX0, plotY0, plotX1, plotY1, plotW, plotH;
    bool logX = false, logY = false;
    double xmin, xmax, ymin, ymax; // already padded
    cairo_t *cr;
    Rgb fg;
    int nextColor = 0;
    std::vector<std::pair<Rgb, std::string>> legend;

    double xToPx(double x) const {
        double xv = logX ? std::log10(std::max(x, 1e-300)) : x;
        double lo = logX ? std::log10(std::max(xmin, 1e-300)) : xmin;
        double hi = logX ? std::log10(std::max(xmax, 1e-300)) : xmax;
        return plotX0 + (hi > lo ? (xv - lo) / (hi - lo) : 0.5) * plotW;
    }
    double yToPx(double y) const {
        double yv = logY ? std::log10(std::max(y, 1e-300)) : y;
        double lo = logY ? std::log10(std::max(ymin, 1e-300)) : ymin;
        double hi = logY ? std::log10(std::max(ymax, 1e-300)) : ymax;
        return plotY1 - (hi > lo ? (yv - lo) / (hi - lo) : 0.5) * plotH;
    }
    Rgb nextPaletteColor() { return kPalette[(nextColor++) % kPaletteSize]; }
};

void setColor(cairo_t *cr, const Rgb &c, double alpha = 1.0) {
    if (alpha >= 1.0) cairo_set_source_rgb(cr, c.r, c.g, c.b);
    else cairo_set_source_rgba(cr, c.r, c.g, c.b, alpha);
}

// --- Per-type draw functions ---------------------------------------------

void drawLine(PlotCtx &ctx, const json &L) {
    cairo_t *cr = ctx.cr;
    if (!L.contains("x")) return;
    AxisPositions xa = parseAxisArray(L["x"]);
    std::string marker = L.value("marker", std::string());
    auto drawOne = [&](const std::vector<double> &y, const std::string &label) {
        Rgb color = ctx.nextPaletteColor();
        if (!label.empty()) ctx.legend.push_back({color, label});
        setColor(cr, color);
        cairo_set_line_width(cr, 1.8);
        bool first = true;
        for (size_t i = 0; i < xa.pos.size() && i < y.size(); ++i) {
            double px = ctx.xToPx(xa.pos[i]), py = ctx.yToPx(y[i]);
            if (first) { cairo_move_to(cr, px, py); first = false; } else cairo_line_to(cr, px, py);
        }
        cairo_stroke(cr);
        if (marker != "none") for (size_t i = 0; i < xa.pos.size() && i < y.size(); ++i) {
            cairo_arc(cr, ctx.xToPx(xa.pos[i]), ctx.yToPx(y[i]), 2.6, 0, 2 * M_PI);
            cairo_fill(cr);
        }
    };
    if (L.contains("series") && L["series"].is_array()) {
        for (const auto &s : L["series"]) if (s.is_object() && s.contains("y"))
            drawOne(numArray(s["y"]), s.value("label", std::string()));
    } else if (L.contains("y")) {
        drawOne(numArray(L["y"]), L.value("label", std::string()));
    }
}

void drawArea(PlotCtx &ctx, const json &L) {
    cairo_t *cr = ctx.cr;
    if (!L.contains("x")) return;
    AxisPositions xa = parseAxisArray(L["x"]);
    size_t n = xa.pos.size();
    bool stacked = L.value("stacked", false);
    std::vector<double> runningBase(n, 0.0);

    auto fillOne = [&](const std::vector<double> &y, double baseline, double alpha, const std::string &label, bool useRunningBase) {
        Rgb color = ctx.nextPaletteColor();
        if (!label.empty()) ctx.legend.push_back({color, label});
        setColor(cr, color, alpha);
        bool any = false;
        for (size_t i = 0; i < n && i < y.size(); ++i) {
            double top = y[i] + (useRunningBase ? runningBase[i] : 0.0);
            double base = useRunningBase ? runningBase[i] : baseline;
            double pxTop = ctx.xToPx(xa.pos[i]), pyTop = ctx.yToPx(top);
            if (!any) { cairo_move_to(cr, pxTop, pyTop); any = true; } else cairo_line_to(cr, pxTop, pyTop);
            (void)base;
        }
        for (size_t ii = n; ii-- > 0;) {
            if (ii >= y.size()) continue;
            double base = useRunningBase ? runningBase[ii] : baseline;
            cairo_line_to(cr, ctx.xToPx(xa.pos[ii]), ctx.yToPx(base));
        }
        cairo_close_path(cr);
        cairo_fill(cr);
        setColor(cr, color);
        cairo_set_line_width(cr, 1.4);
        bool first = true;
        for (size_t i = 0; i < n && i < y.size(); ++i) {
            double top = y[i] + (useRunningBase ? runningBase[i] : 0.0);
            double px = ctx.xToPx(xa.pos[i]), py = ctx.yToPx(top);
            if (first) { cairo_move_to(cr, px, py); first = false; } else cairo_line_to(cr, px, py);
        }
        cairo_stroke(cr);
        if (useRunningBase) for (size_t i = 0; i < n && i < y.size(); ++i) runningBase[i] += y[i];
    };

    if (L.contains("series") && L["series"].is_array()) {
        for (const auto &s : L["series"]) if (s.is_object() && s.contains("y"))
            fillOne(numArray(s["y"]), 0.0, s.value("alpha", 0.4), s.value("label", std::string()), stacked);
    } else if (L.contains("y")) {
        fillOne(numArray(L["y"]), L.value("baseline", 0.0), L.value("alpha", 0.4), L.value("label", std::string()), false);
    }
}

void drawStep(PlotCtx &ctx, const json &L) {
    cairo_t *cr = ctx.cr;
    if (!L.contains("x")) return;
    AxisPositions xa = parseAxisArray(L["x"]);
    std::string where = L.value("where", std::string("pre"));
    auto drawOne = [&](const std::vector<double> &y, const std::string &label) {
        Rgb color = ctx.nextPaletteColor();
        if (!label.empty()) ctx.legend.push_back({color, label});
        setColor(cr, color);
        cairo_set_line_width(cr, 1.8);
        size_t n = std::min(xa.pos.size(), y.size());
        if (n == 0) return;
        cairo_move_to(cr, ctx.xToPx(xa.pos[0]), ctx.yToPx(y[0]));
        for (size_t i = 1; i < n; ++i) {
            double x0 = xa.pos[i - 1], x1 = xa.pos[i], y0 = y[i - 1], y1 = y[i];
            if (where == "post") {
                cairo_line_to(cr, ctx.xToPx(x1), ctx.yToPx(y0));
                cairo_line_to(cr, ctx.xToPx(x1), ctx.yToPx(y1));
            } else if (where == "mid") {
                double xm = (x0 + x1) / 2;
                cairo_line_to(cr, ctx.xToPx(xm), ctx.yToPx(y0));
                cairo_line_to(cr, ctx.xToPx(xm), ctx.yToPx(y1));
                cairo_line_to(cr, ctx.xToPx(x1), ctx.yToPx(y1));
            } else { // pre
                cairo_line_to(cr, ctx.xToPx(x0), ctx.yToPx(y1));
                cairo_line_to(cr, ctx.xToPx(x1), ctx.yToPx(y1));
            }
        }
        cairo_stroke(cr);
    };
    if (L.contains("series") && L["series"].is_array()) {
        for (const auto &s : L["series"]) if (s.is_object() && s.contains("y"))
            drawOne(numArray(s["y"]), s.value("label", std::string()));
    } else if (L.contains("y")) {
        drawOne(numArray(L["y"]), L.value("label", std::string()));
    }
}

void drawStem(PlotCtx &ctx, const json &L) {
    cairo_t *cr = ctx.cr;
    if (!L.contains("x") || !L.contains("y")) return;
    AxisPositions xa = parseAxisArray(L["x"]);
    std::vector<double> y = numArray(L["y"]);
    Rgb color = ctx.nextPaletteColor();
    if (L.value("label", std::string()) != "") ctx.legend.push_back({color, L.value("label", std::string())});
    setColor(cr, color);
    cairo_set_line_width(cr, 1.4);
    double zeroY = ctx.yToPx(0.0);
    for (size_t i = 0; i < xa.pos.size() && i < y.size(); ++i) {
        double px = ctx.xToPx(xa.pos[i]), py = ctx.yToPx(y[i]);
        cairo_move_to(cr, px, zeroY);
        cairo_line_to(cr, px, py);
        cairo_stroke(cr);
        cairo_arc(cr, px, py, 2.8, 0, 2 * M_PI);
        cairo_fill(cr);
    }
}

void drawErrorbar(PlotCtx &ctx, const json &L) {
    cairo_t *cr = ctx.cr;
    if (!L.contains("x") || !L.contains("y")) return;
    std::vector<double> xs = numArray(L["x"]), ys = numArray(L["y"]);
    std::vector<double> yerr = L.contains("yerr") ? numArray(L["yerr"]) : std::vector<double>();
    std::vector<double> xerr = L.contains("xerr") ? numArray(L["xerr"]) : std::vector<double>();
    Rgb color = ctx.nextPaletteColor();
    std::string label = L.value("label", std::string());
    if (!label.empty()) ctx.legend.push_back({color, label});
    setColor(cr, color);
    cairo_set_line_width(cr, 1.4);
    double capsize = L.value("capsize", 3.0);
    for (size_t i = 0; i < xs.size() && i < ys.size(); ++i) {
        double px = ctx.xToPx(xs[i]), py = ctx.yToPx(ys[i]);
        if (i < yerr.size()) {
            double pyLo = ctx.yToPx(ys[i] - yerr[i]), pyHi = ctx.yToPx(ys[i] + yerr[i]);
            cairo_move_to(cr, px, pyLo); cairo_line_to(cr, px, pyHi); cairo_stroke(cr);
            cairo_move_to(cr, px - capsize, pyLo); cairo_line_to(cr, px + capsize, pyLo); cairo_stroke(cr);
            cairo_move_to(cr, px - capsize, pyHi); cairo_line_to(cr, px + capsize, pyHi); cairo_stroke(cr);
        }
        if (i < xerr.size()) {
            double pxLo = ctx.xToPx(xs[i] - xerr[i]), pxHi = ctx.xToPx(xs[i] + xerr[i]);
            cairo_move_to(cr, pxLo, py); cairo_line_to(cr, pxHi, py); cairo_stroke(cr);
            cairo_move_to(cr, pxLo, py - capsize); cairo_line_to(cr, pxLo, py + capsize); cairo_stroke(cr);
            cairo_move_to(cr, pxHi, py - capsize); cairo_line_to(cr, pxHi, py + capsize); cairo_stroke(cr);
        }
        cairo_arc(cr, px, py, 3.0, 0, 2 * M_PI);
        cairo_fill(cr);
    }
}

void drawBar(PlotCtx &ctx, const json &L) {
    cairo_t *cr = ctx.cr;
    if (!L.contains("x")) return;
    AxisPositions xa = parseAxisArray(L["x"]);
    size_t n = xa.pos.size();
    if (n == 0) return;

    std::vector<std::vector<double>> seriesY;
    std::vector<std::string> labels;
    if (L.contains("series") && L["series"].is_array()) {
        for (const auto &s : L["series"]) if (s.is_object() && s.contains("y")) {
            seriesY.push_back(numArray(s["y"]));
            labels.push_back(s.value("label", std::string()));
        }
    } else if (L.contains("y")) {
        seriesY.push_back(numArray(L["y"]));
        labels.push_back(L.value("label", std::string()));
    }
    if (seriesY.empty()) return;
    size_t seriesCount = seriesY.size();
    bool stacked = L.value("stacked", false) && seriesCount > 1;
    double width = L.value("width", 0.8);
    double slot = ctx.plotW / (double)n;

    if (stacked) {
        std::vector<double> stackPos(n, 0.0), stackNeg(n, 0.0);
        double barW = slot * width * 0.9;
        for (size_t si = 0; si < seriesCount; ++si) {
            Rgb color = ctx.nextPaletteColor();
            if (!labels[si].empty()) ctx.legend.push_back({color, labels[si]});
            setColor(cr, color);
            for (size_t i = 0; i < n && i < seriesY[si].size(); ++i) {
                double v = seriesY[si][i];
                double base = (v >= 0) ? stackPos[i] : stackNeg[i];
                double top = base + v;
                double cx = ctx.xToPx(xa.pos[i]);
                double byBase = ctx.yToPx(base), byTop = ctx.yToPx(top);
                cairo_rectangle(cr, cx - barW / 2, std::min(byBase, byTop), barW, std::abs(byBase - byTop));
                cairo_fill(cr);
                (v >= 0 ? stackPos[i] : stackNeg[i]) = top;
            }
        }
    } else {
        double groupW = slot * width * 0.9;
        double barW = groupW / (double)seriesCount;
        for (size_t si = 0; si < seriesCount; ++si) {
            Rgb color = ctx.nextPaletteColor();
            if (!labels[si].empty()) ctx.legend.push_back({color, labels[si]});
            setColor(cr, color);
            double baseY = ctx.yToPx(0.0);
            for (size_t i = 0; i < n && i < seriesY[si].size(); ++i) {
                double cx = ctx.xToPx(xa.pos[i]);
                double bx = cx - groupW / 2 + barW * si;
                double by = ctx.yToPx(seriesY[si][i]);
                cairo_rectangle(cr, bx, std::min(by, baseY), barW * 0.9, std::abs(baseY - by));
                cairo_fill(cr);
            }
        }
    }
}

void drawBarh(PlotCtx &ctx, const json &L) {
    cairo_t *cr = ctx.cr;
    if (L.contains("bars") && L["bars"].is_array()) {
        std::vector<std::string> cats;
        for (const auto &b : L["bars"]) {
            if (!b.is_object()) continue;
            std::string cat = b.contains("y") ? (b["y"].is_string() ? b["y"].get<std::string>() : b["y"].dump()) : "";
            auto it = std::find(cats.begin(), cats.end(), cat);
            int pos = it == cats.end() ? (int)cats.size() : (int)(it - cats.begin());
            if (it == cats.end()) cats.push_back(cat);
            double width = b.value("width", 0.0), left = b.value("left", 0.0), height = b.value("height", 0.8);
            Rgb color;
            std::string colorHex = b.value("color", std::string());
            if (colorHex.empty() || !parseHexColor(colorHex, color)) color = ctx.nextPaletteColor();
            std::string label = b.value("label", std::string());
            if (!label.empty()) ctx.legend.push_back({color, label});
            setColor(cr, color);
            double py = ctx.yToPx((double)pos);
            double barH = height * (ctx.plotH / std::max<size_t>(1, cats.size())) * 0.9;
            cairo_rectangle(cr, ctx.xToPx(left), py - barH / 2, ctx.xToPx(left + width) - ctx.xToPx(left), barH);
            cairo_fill(cr);
        }
    } else if (L.contains("x") && L.contains("y")) {
        AxisPositions ya = parseAxisArray(L["y"]);
        std::vector<double> xs = numArray(L["x"]);
        Rgb color = ctx.nextPaletteColor();
        setColor(cr, color);
        double barH = (ctx.plotH / std::max<size_t>(1, ya.pos.size())) * 0.72;
        for (size_t i = 0; i < ya.pos.size() && i < xs.size(); ++i) {
            double py = ctx.yToPx(ya.pos[i]);
            double px0 = ctx.xToPx(0.0), px1 = ctx.xToPx(xs[i]);
            cairo_rectangle(cr, std::min(px0, px1), py - barH / 2, std::abs(px1 - px0), barH);
            cairo_fill(cr);
        }
    }
}

void drawScatter(PlotCtx &ctx, const json &L) {
    cairo_t *cr = ctx.cr;
    if (L.contains("points") && L["points"].is_array()) {
        for (const auto &p : L["points"]) {
            if (!p.is_object() || !p.contains("x") || !p.contains("y")) continue;
            if (!p["x"].is_number() || !p["y"].is_number()) continue;
            double px = ctx.xToPx(p["x"].get<double>()), py = ctx.yToPx(p["y"].get<double>());
            double size = std::sqrt(std::max(1.0, p.value("size", 36.0))); // matplotlib's "size" is an area in pt^2
            bool filled = p.value("filled", true);
            Rgb color;
            std::string colorHex = p.value("color", std::string());
            if (colorHex.empty() || !parseHexColor(colorHex, color)) color = ctx.nextPaletteColor();
            std::string label = p.value("label", std::string());
            if (!label.empty()) ctx.legend.push_back({color, label});
            cairo_arc(cr, px, py, size, 0, 2 * M_PI);
            if (filled) { setColor(cr, color); cairo_fill(cr); }
            else { setColor(cr, ctx.fg); cairo_set_line_width(cr, 1.4); cairo_stroke(cr); }
            if (p.value("annotate", false)) {
                cairo_set_font_size(cr, 8);
                setColor(cr, ctx.fg);
                cairo_move_to(cr, px + 4, py - 4);
                cairo_show_text(cr, label.c_str());
            }
        }
    } else if (L.contains("x")) {
        AxisPositions xa = parseAxisArray(L["x"]);
        auto drawOne = [&](const std::vector<double> &y, const std::string &label) {
            Rgb color = ctx.nextPaletteColor();
            if (!label.empty()) ctx.legend.push_back({color, label});
            setColor(cr, color);
            for (size_t i = 0; i < xa.pos.size() && i < y.size(); ++i) {
                cairo_arc(cr, ctx.xToPx(xa.pos[i]), ctx.yToPx(y[i]), 3.2, 0, 2 * M_PI);
                cairo_fill(cr);
            }
        };
        if (L.contains("series") && L["series"].is_array()) {
            for (const auto &s : L["series"]) if (s.is_object() && s.contains("y"))
                drawOne(numArray(s["y"]), s.value("label", std::string()));
        } else if (L.contains("y")) {
            drawOne(numArray(L["y"]), L.value("label", std::string()));
        }
    }
}

void drawHistogram(PlotCtx &ctx, const json &L) {
    cairo_t *cr = ctx.cr;
    HistResult hr = computeHistogram(L);
    if (hr.edges.empty()) return;
    Rgb color = ctx.nextPaletteColor();
    std::string label = L.value("label", std::string());
    if (!label.empty()) ctx.legend.push_back({color, label});
    setColor(cr, color, 0.85);
    double baseY = ctx.yToPx(0.0);
    for (size_t i = 0; i + 1 < hr.edges.size(); ++i) {
        double px0 = ctx.xToPx(hr.edges[i]), px1 = ctx.xToPx(hr.edges[i + 1]);
        double py = ctx.yToPx(hr.counts[i]);
        cairo_rectangle(cr, px0, std::min(py, baseY), px1 - px0, std::abs(baseY - py));
        cairo_fill(cr);
    }
    setColor(cr, ctx.fg);
    cairo_set_line_width(cr, 0.6);
    for (size_t i = 0; i + 1 < hr.edges.size(); ++i) {
        double px0 = ctx.xToPx(hr.edges[i]), px1 = ctx.xToPx(hr.edges[i + 1]);
        double py = ctx.yToPx(hr.counts[i]);
        cairo_rectangle(cr, px0, std::min(py, baseY), px1 - px0, std::abs(baseY - py));
        cairo_stroke(cr);
    }
}

// Five-number summary (min/Q1/median/Q3/max, Tukey whiskers at 1.5*IQR,
// outliers beyond that as individual points) -- the standard boxplot
// statistic, computed the same way matplotlib's own boxplot() does by
// default (no custom whisker method support).
struct BoxStats { double lo, q1, med, q3, hi; std::vector<double> outliers; };
BoxStats computeBoxStats(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    BoxStats b{};
    if (v.empty()) return b;
    auto pct = [&](double p) -> double {
        double idx = p * (v.size() - 1);
        size_t lo = (size_t)std::floor(idx), hi = (size_t)std::ceil(idx);
        if (lo == hi) return v[lo];
        return v[lo] + (v[hi] - v[lo]) * (idx - lo);
    };
    b.q1 = pct(0.25); b.med = pct(0.5); b.q3 = pct(0.75);
    double iqr = b.q3 - b.q1;
    double whiskerLo = b.q1 - 1.5 * iqr, whiskerHi = b.q3 + 1.5 * iqr;
    // b.lo/b.hi must only ever be set from a non-outlier value -- seeding
    // them from v.front()/v.back() (the RAW min/max, outliers included)
    // meant an outlier past the far end left the whisker with nothing
    // smaller/larger to shrink it back down to: confirmed live, a single
    // outlier at 20 (true whisker should stop around 6) instead stretched
    // the whisker line all the way up to overlap the outlier marker itself.
    b.lo = b.q1; b.hi = b.q3; // fallback if every point is an outlier
    bool haveNonOutlier = false;
    for (double x : v) {
        if (x < whiskerLo || x > whiskerHi) { b.outliers.push_back(x); continue; }
        if (!haveNonOutlier) { b.lo = x; b.hi = x; haveNonOutlier = true; }
        else { b.lo = std::min(b.lo, x); b.hi = std::max(b.hi, x); }
    }
    return b;
}

void drawBoxplot(PlotCtx &ctx, const json &L) {
    cairo_t *cr = ctx.cr;
    if (!L.contains("data") || !L["data"].is_array()) return;
    size_t n = L["data"].size();
    double slot = ctx.plotW / (double)n;
    double boxW = slot * 0.5;
    Rgb color = ctx.nextPaletteColor();
    setColor(cr, ctx.fg);
    cairo_set_line_width(cr, 1.2);
    size_t idx = 0;
    for (const auto &arr : L["data"]) {
        BoxStats b = computeBoxStats(numArray(arr));
        double cx = ctx.xToPx((double)(idx + 1));
        double pyLo = ctx.yToPx(b.lo), pyQ1 = ctx.yToPx(b.q1), pyMed = ctx.yToPx(b.med), pyQ3 = ctx.yToPx(b.q3), pyHi = ctx.yToPx(b.hi);
        // whiskers
        cairo_move_to(cr, cx, pyLo); cairo_line_to(cr, cx, pyQ1); cairo_stroke(cr);
        cairo_move_to(cr, cx, pyQ3); cairo_line_to(cr, cx, pyHi); cairo_stroke(cr);
        cairo_move_to(cr, cx - boxW / 4, pyLo); cairo_line_to(cr, cx + boxW / 4, pyLo); cairo_stroke(cr);
        cairo_move_to(cr, cx - boxW / 4, pyHi); cairo_line_to(cr, cx + boxW / 4, pyHi); cairo_stroke(cr);
        // box
        setColor(cr, color, 0.5);
        cairo_rectangle(cr, cx - boxW / 2, std::min(pyQ1, pyQ3), boxW, std::abs(pyQ3 - pyQ1));
        cairo_fill_preserve(cr);
        setColor(cr, ctx.fg);
        cairo_stroke(cr);
        // median
        cairo_move_to(cr, cx - boxW / 2, pyMed); cairo_line_to(cr, cx + boxW / 2, pyMed); cairo_stroke(cr);
        // outliers
        for (double o : b.outliers) { cairo_arc(cr, cx, ctx.yToPx(o), 2.0, 0, 2 * M_PI); cairo_stroke(cr); }
        ++idx;
    }
}

// A simplified Gaussian-KDE violin (Silverman's rule-of-thumb bandwidth,
// sampled at a fixed resolution) -- not matplotlib's exact algorithm, but
// the same fundamental technique (a smoothed density estimate mirrored
// left/right around each category's center), close enough visually.
void drawViolin(PlotCtx &ctx, const json &L) {
    cairo_t *cr = ctx.cr;
    if (!L.contains("data") || !L["data"].is_array()) return;
    size_t n = L["data"].size();
    double slot = ctx.plotW / (double)n;
    double halfW = slot * 0.35;
    Rgb color = ctx.nextPaletteColor();
    bool showMedians = L.value("showmedians", true);
    size_t idx = 0;
    constexpr int kSamples = 40;
    for (const auto &arrJson : L["data"]) {
        std::vector<double> v = numArray(arrJson);
        if (v.size() < 2) { ++idx; continue; }
        double mean = 0; for (double x : v) mean += x; mean /= v.size();
        double var = 0; for (double x : v) var += (x - mean) * (x - mean); var /= (v.size() - 1);
        double sigma = std::sqrt(std::max(var, 1e-9));
        double bandwidth = 1.06 * sigma * std::pow((double)v.size(), -0.2); // Silverman's rule of thumb
        bandwidth = std::max(bandwidth, 1e-6);
        double vmin = *std::min_element(v.begin(), v.end()), vmax = *std::max_element(v.begin(), v.end());
        double pad = (vmax - vmin) * 0.15 + bandwidth;
        double cx = ctx.xToPx((double)(idx + 1));

        std::vector<double> density(kSamples + 1);
        double maxDensity = 1e-9;
        for (int s = 0; s <= kSamples; ++s) {
            double y = vmin - pad + (vmax - vmin + 2 * pad) * s / kSamples;
            double d = 0;
            for (double x : v) { double z = (y - x) / bandwidth; d += std::exp(-0.5 * z * z); }
            d /= (v.size() * bandwidth * std::sqrt(2 * M_PI));
            density[s] = d;
            maxDensity = std::max(maxDensity, d);
        }
        setColor(cr, color, 0.55);
        bool first = true;
        for (int s = 0; s <= kSamples; ++s) {
            double y = vmin - pad + (vmax - vmin + 2 * pad) * s / kSamples;
            double w = (density[s] / maxDensity) * halfW;
            double px = cx + w, py = ctx.yToPx(y);
            if (first) { cairo_move_to(cr, px, py); first = false; } else cairo_line_to(cr, px, py);
        }
        for (int s = kSamples; s >= 0; --s) {
            double y = vmin - pad + (vmax - vmin + 2 * pad) * s / kSamples;
            double w = (density[s] / maxDensity) * halfW;
            cairo_line_to(cr, cx - w, ctx.yToPx(y));
        }
        cairo_close_path(cr);
        cairo_fill_preserve(cr);
        setColor(cr, ctx.fg);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);
        if (showMedians) {
            std::vector<double> sorted = v;
            std::sort(sorted.begin(), sorted.end());
            double med = sorted[sorted.size() / 2];
            double py = ctx.yToPx(med);
            cairo_move_to(cr, cx - halfW * 0.5, py); cairo_line_to(cr, cx + halfW * 0.5, py); cairo_stroke(cr);
        }
        ++idx;
    }
}

void drawForType(PlotCtx &ctx, const std::string &type, const json &L) {
    if (type == "line") drawLine(ctx, L);
    else if (type == "area") drawArea(ctx, L);
    else if (type == "step") drawStep(ctx, L);
    else if (type == "stem") drawStem(ctx, L);
    else if (type == "errorbar") drawErrorbar(ctx, L);
    else if (type == "bar") drawBar(ctx, L);
    else if (type == "barh") drawBarh(ctx, L);
    else if (type == "scatter") drawScatter(ctx, L);
    else if (type == "histogram") drawHistogram(ctx, L);
    else if (type == "boxplot") drawBoxplot(ctx, L);
    else if (type == "violin") drawViolin(ctx, L);
}

// --- Whole-panel special renderers: heatmap, pie -------------------------
// Neither has a shared x/y numeric coordinate system (a pixel grid indexed
// by matrix position; a radial layout) -- both are self-contained, only
// supported as a panel's sole layer (see the file-header comment).

// A compact, hand-picked subset of matplotlib's "viridis" colormap control
// points, linearly interpolated -- not a byte-exact reproduction, but
// recognizably the same perceptually-uniform blue->green->yellow ramp.
Rgb viridisColor(double t) {
    static const Rgb stops[] = {
        {0.267, 0.005, 0.329}, {0.283, 0.141, 0.458}, {0.254, 0.265, 0.530},
        {0.207, 0.372, 0.553}, {0.164, 0.471, 0.558}, {0.128, 0.567, 0.551},
        {0.135, 0.659, 0.518}, {0.267, 0.749, 0.441}, {0.478, 0.821, 0.318},
        {0.741, 0.873, 0.150}, {0.993, 0.906, 0.144},
    };
    constexpr int kStops = sizeof(stops) / sizeof(stops[0]);
    t = std::clamp(t, 0.0, 1.0);
    double pos = t * (kStops - 1);
    int i0 = (int)std::floor(pos), i1 = std::min(i0 + 1, kStops - 1);
    double frac = pos - i0;
    return {stops[i0].r + (stops[i1].r - stops[i0].r) * frac,
            stops[i0].g + (stops[i1].g - stops[i0].g) * frac,
            stops[i0].b + (stops[i1].b - stops[i0].b) * frac};
}

void renderHeatmapPanel(cairo_t *cr, double x0, double y0, double x1, double y1, const json &L, const Rgb &fg) {
    if (!L.contains("matrix") || !L["matrix"].is_array() || L["matrix"].empty()) return;
    std::vector<std::vector<double>> matrix;
    for (const auto &row : L["matrix"]) matrix.push_back(numArray(row));
    size_t rows = matrix.size(), cols = 0;
    for (const auto &row : matrix) cols = std::max(cols, row.size());
    if (rows == 0 || cols == 0) return;

    double vmin = INFINITY, vmax = -INFINITY;
    for (const auto &row : matrix) for (double v : row) { vmin = std::min(vmin, v); vmax = std::max(vmax, v); }
    if (vmax <= vmin) vmax = vmin + 1;

    bool wantColorbar = L.value("colorbar", true);
    double colorbarW = wantColorbar ? 18 : 0, colorbarGap = wantColorbar ? 10 : 0;
    double gridX1 = x1 - colorbarW - colorbarGap;
    double cellW = (gridX1 - x0) / cols, cellH = (y1 - y0) / rows;

    for (size_t i = 0; i < rows; ++i) {
        for (size_t j = 0; j < matrix[i].size(); ++j) {
            double t = (matrix[i][j] - vmin) / (vmax - vmin);
            Rgb c = viridisColor(t);
            cairo_set_source_rgb(cr, c.r, c.g, c.b);
            cairo_rectangle(cr, x0 + j * cellW, y0 + i * cellH, cellW, cellH);
            cairo_fill(cr);
        }
    }
    if (L.value("annotate_values", false)) {
        cairo_set_font_size(cr, 8);
        for (size_t i = 0; i < rows; ++i) {
            for (size_t j = 0; j < matrix[i].size(); ++j) {
                double t = (matrix[i][j] - vmin) / (vmax - vmin);
                setColor(cr, t > 0.6 ? Rgb{0, 0, 0} : Rgb{1, 1, 1});
                std::string s = formatTick(matrix[i][j]);
                cairo_text_extents_t ext; cairo_text_extents(cr, s.c_str(), &ext);
                cairo_move_to(cr, x0 + j * cellW + cellW / 2 - ext.width / 2, y0 + i * cellH + cellH / 2 + ext.height / 2);
                cairo_show_text(cr, s.c_str());
            }
        }
    }
    setColor(cr, fg);
    cairo_set_font_size(cr, 9);
    if (L.contains("xticklabels") && L["xticklabels"].is_array()) {
        cairo_save(cr);
        size_t i = 0;
        for (const auto &v : L["xticklabels"]) {
            if (i >= cols) break;
            std::string s = v.is_string() ? v.get<std::string>() : v.dump();
            cairo_move_to(cr, x0 + i * cellW + cellW / 2 + 3, y1 + 12);
            cairo_save(cr);
            cairo_translate(cr, x0 + i * cellW + cellW / 2, y1 + 12);
            cairo_rotate(cr, -M_PI / 4);
            cairo_move_to(cr, 0, 0);
            cairo_show_text(cr, s.c_str());
            cairo_restore(cr);
            ++i;
        }
        cairo_restore(cr);
    }
    if (L.contains("yticklabels") && L["yticklabels"].is_array()) {
        size_t i = 0;
        for (const auto &v : L["yticklabels"]) {
            if (i >= rows) break;
            std::string s = v.is_string() ? v.get<std::string>() : v.dump();
            cairo_text_extents_t ext; cairo_text_extents(cr, s.c_str(), &ext);
            cairo_move_to(cr, x0 - ext.width - 8, y0 + i * cellH + cellH / 2 + ext.height / 2);
            cairo_show_text(cr, s.c_str());
            ++i;
        }
    }
    if (wantColorbar) {
        double cbX = x1 - colorbarW;
        constexpr int kSteps = 40;
        for (int s = 0; s < kSteps; ++s) {
            double t = 1.0 - (double)s / kSteps;
            Rgb c = viridisColor(t);
            cairo_set_source_rgb(cr, c.r, c.g, c.b);
            cairo_rectangle(cr, cbX, y0 + (y1 - y0) * s / kSteps, colorbarW, (y1 - y0) / kSteps + 0.5);
            cairo_fill(cr);
        }
        setColor(cr, fg);
        cairo_set_font_size(cr, 8);
        std::string hi = formatTick(vmax), lo = formatTick(vmin);
        cairo_move_to(cr, cbX, y0 - 3); cairo_show_text(cr, hi.c_str());
        cairo_move_to(cr, cbX, y1 + 8); cairo_show_text(cr, lo.c_str());
    }
}

void renderPiePanel(cairo_t *cr, double x0, double y0, double x1, double y1, const json &L, const Rgb &fg) {
    std::vector<double> values = L.contains("values") ? numArray(L["values"]) : std::vector<double>();
    if (values.empty()) return;
    std::vector<std::string> labels;
    if (L.contains("labels") && L["labels"].is_array())
        for (const auto &v : L["labels"]) labels.push_back(v.is_string() ? v.get<std::string>() : v.dump());
    double total = 0; for (double v : values) total += v;
    if (total <= 0) return;

    double cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    double radius = std::min(x1 - x0, y1 - y0) / 2 - 10;
    bool donut = L.value("donut", false);
    double innerRadius = donut ? radius * (1.0 - L.value("donut_width", 0.4)) : 0.0;

    double angle = -M_PI / 2;
    cairo_set_font_size(cr, 9);
    for (size_t i = 0; i < values.size(); ++i) {
        double frac = values[i] / total;
        double sweep = frac * 2 * M_PI;
        Rgb color = kPalette[i % kPaletteSize];
        setColor(cr, color);
        cairo_move_to(cr, cx + innerRadius * std::cos(angle), cy + innerRadius * std::sin(angle));
        cairo_arc(cr, cx, cy, radius, angle, angle + sweep);
        if (donut) cairo_arc_negative(cr, cx, cy, innerRadius, angle + sweep, angle);
        cairo_close_path(cr);
        cairo_fill(cr);

        double midAngle = angle + sweep / 2;
        double labelR = radius + 14;
        std::string pct = formatTick(frac * 100) + "%";
        std::string label = i < labels.size() ? labels[i] + " (" + pct + ")" : pct;
        setColor(cr, fg);
        cairo_text_extents_t ext; cairo_text_extents(cr, label.c_str(), &ext);
        double lx = cx + labelR * std::cos(midAngle), ly = cy + labelR * std::sin(midAngle);
        cairo_move_to(cr, lx - (std::cos(midAngle) < 0 ? ext.width : 0), ly + ext.height / 2);
        cairo_show_text(cr, label.c_str());
        angle += sweep;
    }
}

// --- Cartesian panel: shared axis system for all other types -------------

void renderCartesianPanel(cairo_t *cr, double x0, double y0, double x1, double y1, const PanelSpec &panel,
                           const Rgb &fg, const Rgb &gridColor, bool showXTickLabels, const char *bodyFont) {
    double marginLeft = 55, marginRight = 20, marginTop = panel.title.empty() ? 10 : 30, marginBottom = showXTickLabels ? 30 : 10;
    bool hasLegend = false; // determined after the draw pass, but layout needs to reserve space up front
    // Cheap legend-presence check: any layer that could plausibly produce a label.
    for (const auto &L : panel.layers) {
        if (L.j.contains("label") && L.j["label"].is_string() && !L.j["label"].get<std::string>().empty()) hasLegend = true;
        if (L.j.contains("series") && L.j["series"].is_array())
            for (const auto &s : L.j["series"]) if (s.is_object() && s.value("label", std::string()) != "") hasLegend = true;
        if (L.j.contains("bars") && L.j["bars"].is_array())
            for (const auto &b : L.j["bars"]) if (b.is_object() && b.value("label", std::string()) != "") hasLegend = true;
    }
    if (hasLegend) marginTop += 20;
    if (!panel.xlabel.empty() && showXTickLabels) marginBottom += 18;
    if (!panel.ylabel.empty()) marginLeft += 15;

    double px0 = x0 + marginLeft, py0 = y0 + marginTop, px1 = x1 - marginRight, py1 = y1 - marginBottom;
    double pw = px1 - px0, ph = py1 - py0;
    if (pw <= 10 || ph <= 10) return;

    AxisRange r;
    for (const auto &L : panel.layers) extentForType(L.type, L.j, r);
    for (const auto &rl : panel.refLines) { if (rl.axis == "y") extendY(r, rl.value); else extendX(r, rl.value); }
    for (const auto &rb : panel.refBands) {
        if (rb.axis == "y") { extendY(r, rb.low); extendY(r, rb.high); }
        else { extendX(r, rb.low); extendX(r, rb.high); }
    }
    if (!std::isfinite(r.xmin) || !std::isfinite(r.xmax)) { r.xmin = 0; r.xmax = 1; }
    if (!std::isfinite(r.ymin) || !std::isfinite(r.ymax)) { r.ymin = 0; r.ymax = 1; }
    if (r.xmax == r.xmin) { r.xmax += 1; r.xmin -= 1; }
    if (r.ymax == r.ymin) { r.ymax += 1; r.ymin -= 1; }

    PlotCtx ctx;
    ctx.cr = cr; ctx.fg = fg;
    ctx.plotX0 = px0; ctx.plotY0 = py0; ctx.plotX1 = px1; ctx.plotY1 = py1; ctx.plotW = pw; ctx.plotH = ph;
    ctx.logX = panel.logX && r.xmin > 0;
    ctx.logY = panel.logY && r.ymin > 0;

    // Same padding logic as the single-bar-chart case this generalizes:
    // categorical/bar-like axes need a full half-slot of margin (the
    // bar-width math assumes n evenly-spaced slots spanning the range),
    // everything else gets a plain 3% margin -- EXCEPT a log axis, which
    // must pad multiplicatively in log-space instead: a plain linear pad
    // (range * 0.08) can push the padded min below zero, and then
    // clamping that back up to a near-zero epsilon before log10() (see
    // PlotCtx::yToPx) explodes the log-space range to hundreds of decades,
    // collapsing the entire plotted curve into a sliver at one edge --
    // confirmed live on a log_y chart that rendered almost entirely blank.
    bool hasBarLikeLayer = false;
    for (const auto &L : panel.layers) if (L.type == "bar" || L.type == "boxplot" || L.type == "violin") hasBarLikeLayer = true;

    if (ctx.logX) {
        double lo = std::log10(r.xmin), hi = std::log10(r.xmax);
        double pad = (hi - lo) * 0.05;
        ctx.xmin = std::pow(10.0, lo - pad); ctx.xmax = std::pow(10.0, hi + pad);
    } else {
        double xPad = (r.xCat || hasBarLikeLayer) ? 0.6 : (r.xmax - r.xmin) * 0.03;
        ctx.xmin = r.xmin - xPad; ctx.xmax = r.xmax + xPad;
    }
    if (ctx.logY) {
        double lo = std::log10(r.ymin), hi = std::log10(r.ymax);
        double pad = (hi - lo) * 0.08;
        ctx.ymin = std::pow(10.0, lo - pad); ctx.ymax = std::pow(10.0, hi + pad);
    } else {
        double yPad = (r.yCat) ? 0.6 : (r.ymax - r.ymin) * 0.08;
        ctx.ymin = r.ymin - yPad; ctx.ymax = r.ymax + yPad;
    }

    // --- gridlines + ticks ---
    setColor(cr, gridColor);
    cairo_set_line_width(cr, 1.0);
    cairo_set_font_size(cr, 9.5);
    if (r.yCat) {
        for (const auto &[pos, label] : r.yCatLabels) {
            double py = ctx.yToPx((double)pos);
            cairo_move_to(cr, px0, py); cairo_line_to(cr, px1, py); cairo_stroke(cr);
            cairo_text_extents_t ext; cairo_text_extents(cr, label.c_str(), &ext);
            setColor(cr, fg);
            cairo_move_to(cr, px0 - ext.width - 8, py + ext.height / 2);
            cairo_show_text(cr, label.c_str());
            setColor(cr, gridColor);
        }
    } else {
        constexpr int kYTicks = 5;
        for (int i = 0; i <= kYTicks; ++i) {
            double yVal = ctx.logY
                ? std::pow(10.0, std::log10(ctx.ymin) + (std::log10(ctx.ymax) - std::log10(ctx.ymin)) * i / kYTicks)
                : ctx.ymin + (ctx.ymax - ctx.ymin) * i / kYTicks;
            double py = ctx.yToPx(yVal);
            cairo_move_to(cr, px0, py); cairo_line_to(cr, px1, py); cairo_stroke(cr);
            std::string label = formatTick(yVal) + (panel.yIsPercent ? "%" : "");
            cairo_text_extents_t ext; cairo_text_extents(cr, label.c_str(), &ext);
            setColor(cr, fg);
            cairo_move_to(cr, px0 - ext.width - ext.x_bearing - 8, py - (ext.height / 2 + ext.y_bearing));
            cairo_show_text(cr, label.c_str());
            setColor(cr, gridColor);
        }
    }
    if (showXTickLabels) {
        if (r.xCat) {
            size_t stride = std::max<size_t>(1, r.xCatLabels.size() / 12);
            size_t i = 0;
            for (const auto &[pos, label] : r.xCatLabels) {
                if (i++ % stride != 0) continue;
                double px = ctx.xToPx((double)pos);
                cairo_text_extents_t ext; cairo_text_extents(cr, label.c_str(), &ext);
                setColor(cr, fg);
                cairo_move_to(cr, px - (ext.width / 2 + ext.x_bearing), py1 + ext.height + 8);
                cairo_show_text(cr, label.c_str());
            }
        } else {
            constexpr int kXTicks = 6;
            for (int i = 0; i <= kXTicks; ++i) {
                double xVal = ctx.logX
                    ? std::pow(10.0, std::log10(ctx.xmin) + (std::log10(ctx.xmax) - std::log10(ctx.xmin)) * i / kXTicks)
                    : ctx.xmin + (ctx.xmax - ctx.xmin) * i / kXTicks;
                double px = ctx.xToPx(xVal);
                std::string label = formatTick(xVal);
                cairo_text_extents_t ext; cairo_text_extents(cr, label.c_str(), &ext);
                setColor(cr, fg);
                cairo_move_to(cr, px - (ext.width / 2 + ext.x_bearing), py1 + ext.height + 8);
                cairo_show_text(cr, label.c_str());
            }
        }
    }

    setColor(cr, fg);
    cairo_set_line_width(cr, 1.2);
    cairo_move_to(cr, px0, py0); cairo_line_to(cr, px0, py1); cairo_stroke(cr);
    cairo_move_to(cr, px0, py1); cairo_line_to(cr, px1, py1); cairo_stroke(cr);

    // --- ref_bands (drawn under the marks, like matplotlib's axhspan/axvspan) ---
    for (const auto &rb : panel.refBands) {
        Rgb c = fg;
        if (!rb.colorHex.empty()) parseHexColor(rb.colorHex, c);
        setColor(cr, c, 0.15);
        if (rb.axis == "y") cairo_rectangle(cr, px0, ctx.yToPx(rb.high), pw, ctx.yToPx(rb.low) - ctx.yToPx(rb.high));
        else cairo_rectangle(cr, ctx.xToPx(rb.low), py0, ctx.xToPx(rb.high) - ctx.xToPx(rb.low), ph);
        cairo_fill(cr);
    }

    // --- layers ---
    for (const auto &L : panel.layers) drawForType(ctx, L.type, L.j);

    // --- ref_lines (drawn over the marks, like matplotlib's axhline/axvline) ---
    cairo_set_line_width(cr, 1.2);
    for (const auto &rl : panel.refLines) {
        Rgb c = fg;
        if (!rl.colorHex.empty()) parseHexColor(rl.colorHex, c);
        setColor(cr, c);
        if (rl.axis == "y") { double py = ctx.yToPx(rl.value); cairo_move_to(cr, px0, py); cairo_line_to(cr, px1, py); }
        else { double px = ctx.xToPx(rl.value); cairo_move_to(cr, px, py0); cairo_line_to(cr, px, py1); }
        cairo_stroke(cr);
        if (!rl.label.empty()) ctx.legend.push_back({c, rl.label});
    }

    // --- annotations ---
    cairo_set_font_size(cr, 9);
    for (const auto &a : panel.annotations) {
        setColor(cr, fg);
        double px = ctx.xToPx(a.x) + a.dx, py = ctx.yToPx(a.y) - a.dy;
        cairo_text_extents_t ext; cairo_text_extents(cr, a.text.c_str(), &ext);
        double tx = px;
        if (a.ha == "center") tx -= ext.width / 2;
        else if (a.ha == "right") tx -= ext.width;
        cairo_move_to(cr, tx, py);
        cairo_show_text(cr, a.text.c_str());
    }

    // --- title / axis labels ---
    setColor(cr, fg);
    if (!panel.title.empty()) {
        cairo_set_font_size(cr, 12);
        cairo_text_extents_t ext; cairo_text_extents(cr, panel.title.c_str(), &ext);
        cairo_move_to(cr, px0 + pw / 2 - (ext.width / 2 + ext.x_bearing), y0 + 16);
        cairo_show_text(cr, panel.title.c_str());
    }
    cairo_select_font_face(cr, bodyFont, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10);
    if (!panel.xlabel.empty() && showXTickLabels) {
        cairo_text_extents_t ext; cairo_text_extents(cr, panel.xlabel.c_str(), &ext);
        cairo_move_to(cr, px0 + pw / 2 - (ext.width / 2 + ext.x_bearing), y1 - 4);
        cairo_show_text(cr, panel.xlabel.c_str());
    }
    if (!panel.ylabel.empty()) {
        cairo_text_extents_t ext; cairo_text_extents(cr, panel.ylabel.c_str(), &ext);
        cairo_save(cr);
        cairo_translate(cr, x0 + 12, py0 + ph / 2 + ext.width / 2);
        cairo_rotate(cr, -M_PI / 2);
        cairo_move_to(cr, 0, 0);
        cairo_show_text(cr, panel.ylabel.c_str());
        cairo_restore(cr);
    }

    // --- legend ---
    if (!ctx.legend.empty()) {
        double legendY = y0 + (panel.title.empty() ? 12 : 28);
        double lx = px0;
        cairo_set_font_size(cr, 9.5);
        for (const auto &[color, label] : ctx.legend) {
            setColor(cr, color);
            cairo_rectangle(cr, lx, legendY - 7, 9, 9);
            cairo_fill(cr);
            setColor(cr, fg);
            cairo_move_to(cr, lx + 12, legendY + 1);
            cairo_show_text(cr, label.c_str());
            cairo_text_extents_t ext; cairo_text_extents(cr, label.c_str(), &ext);
            lx += 12 + ext.width + 14;
            if (lx > px1 - 40) { lx = px0; legendY += 14; }
        }
    }
}

} // namespace

namespace ChartRender {

std::vector<uint8_t> renderChartToPng(const std::string &specJson, bool darkMode,
                                       const std::string &bodyFontFamily, const std::string &titleFontFamily, bool titleBold,
                                       int &logicalWidth, int &logicalHeight)
{
    const char *bodyFont = bodyFontFamily.empty() ? "sans-serif" : bodyFontFamily.c_str();
    const char *titleFont = titleFontFamily.empty() ? bodyFont : titleFontFamily.c_str();

    TopSpec top;
    if (!parseTopSpec(specJson, top)) return {};

    constexpr double kPxPerInch = 100.0;
    int w = std::max(200, (int)std::lround(top.figW * kPxPerInch));
    int h = std::max(150, (int)std::lround(top.figH * kPxPerInch));
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
    cairo_select_font_face(cr, bodyFont, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);

    double figTitleH = 0;
    if (!top.title.empty()) {
        cairo_select_font_face(cr, titleFont, CAIRO_FONT_SLANT_NORMAL, titleBold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 15);
        cairo_text_extents_t ext; cairo_text_extents(cr, top.title.c_str(), &ext);
        setColor(cr, fg);
        cairo_move_to(cr, w / 2.0 - (ext.width / 2 + ext.x_bearing), 24);
        cairo_show_text(cr, top.title.c_str());
        cairo_select_font_face(cr, bodyFont, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        figTitleH = 30;
    }

    size_t nPanels = top.panels.size();
    double panelH = (h - figTitleH) / (double)nPanels;
    for (size_t i = 0; i < nPanels; ++i) {
        double y0 = figTitleH + panelH * i, y1 = figTitleH + panelH * (i + 1);
        bool showXTickLabels = !top.sharedX || (i == nPanels - 1);
        const PanelSpec &panel = top.panels[i];
        bool soleHeatmap = panel.layers.size() == 1 && panel.layers[0].type == "heatmap";
        bool solePie = panel.layers.size() == 1 && panel.layers[0].type == "pie";
        if (soleHeatmap) {
            const json &hj = panel.layers[0].j;
            // Reserve room for the widest yticklabel -- a fixed left
            // margin clipped labels like "Y1" against the canvas edge
            // whenever they didn't fit in a small default gap.
            double leftMargin = 15;
            if (hj.contains("yticklabels") && hj["yticklabels"].is_array()) {
                cairo_set_font_size(cr, 9);
                for (const auto &v : hj["yticklabels"]) {
                    std::string s = v.is_string() ? v.get<std::string>() : v.dump();
                    cairo_text_extents_t ext; cairo_text_extents(cr, s.c_str(), &ext);
                    leftMargin = std::max(leftMargin, ext.width + 18);
                }
            }
            double px0 = leftMargin, py0 = y0 + (panel.title.empty() ? 10 : 30), px1 = w - 15, py1 = y1 - 40;
            if (!panel.title.empty()) {
                cairo_set_font_size(cr, 12);
                cairo_text_extents_t ext; cairo_text_extents(cr, panel.title.c_str(), &ext);
                setColor(cr, fg);
                cairo_move_to(cr, w / 2.0 - ext.width / 2, y0 + 16);
                cairo_show_text(cr, panel.title.c_str());
            }
            renderHeatmapPanel(cr, px0, py0, px1, py1, panel.layers[0].j, fg);
        } else if (solePie) {
            double px0 = 15, py0 = y0 + (panel.title.empty() ? 10 : 30), px1 = w - 15, py1 = y1 - 15;
            if (!panel.title.empty()) {
                cairo_set_font_size(cr, 12);
                cairo_text_extents_t ext; cairo_text_extents(cr, panel.title.c_str(), &ext);
                setColor(cr, fg);
                cairo_move_to(cr, w / 2.0 - ext.width / 2, y0 + 16);
                cairo_show_text(cr, panel.title.c_str());
            }
            renderPiePanel(cr, px0, py0, px1, py1, panel.layers[0].j, fg);
        } else {
            renderCartesianPanel(cr, 0, y0, (double)w, y1, panel, fg, gridColor, showXTickLabels, bodyFont);
        }
    }

    std::string pngBytes;
    cairo_surface_write_to_png_stream(surface, writeToString, &pngBytes);

    cairo_destroy(cr);
    cairo_surface_destroy(surface);

    return std::vector<uint8_t>(pngBytes.begin(), pngBytes.end());
}

} // namespace ChartRender
