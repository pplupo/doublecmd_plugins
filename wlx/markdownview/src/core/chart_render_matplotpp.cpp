#include "chart_render_matplotpp.h"
#include <cstdio>

#include <matplot/matplot.h>

#include <sys/stat.h>
#include <unistd.h>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <thread>
#include <limits>
#include <map>

// Embedded by CMakeLists.txt's embed_binary_resource() -- see
// 3rdparty/gnuplot/README.md for exactly how this binary was built.
extern const unsigned char gnuplot_binary[];
extern const size_t gnuplot_binary_size;

namespace {

using ChartSpec::Layer;
using ChartSpec::PanelSpec;
using ChartSpec::TopSpec;
using nlohmann::json;

bool fileExists(const std::string &path) { struct stat st; return stat(path.c_str(), &st) == 0; }

// figure_type::save() (see figure_type.cpp) writes gnuplot commands to its
// pipe and returns as soon as they're SENT, not once gnuplot has actually
// finished processing them and writing the output file -- confirmed live:
// save() reported success every time, but the file didn't exist yet when
// immediately reopened for reading right after. Poll for the file to
// appear and its size to stop changing (a plain existence check alone
// could still catch it mid-write) instead of assuming save() is
// synchronous.
bool waitForStableFile(const std::string &path, int timeoutMs) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    off_t lastSize = -1;
    int stableCount = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        struct stat st;
        if (stat(path.c_str(), &st) == 0 && st.st_size > 0) {
            if (st.st_size == lastSize) {
                if (++stableCount >= 2) return true; // unchanged across 2 consecutive checks
            } else {
                stableCount = 0;
            }
            lastSize = st.st_size;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    return fileExists(path); // last resort: at least confirm it exists
}

std::string homeDir() {
    const char *h = std::getenv("HOME");
    return h ? h : "";
}

void mkdirParents(const std::string &dir) {
    std::string partial;
    for (size_t i = 1; i <= dir.size(); ++i) {
        if (i == dir.size() || dir[i] == '/') {
            partial = dir.substr(0, i);
            if (!partial.empty()) mkdir(partial.c_str(), 0755); // ignore EEXIST and other errors
        }
    }
}

std::string gnuplotSeedDir() { return homeDir() + "/.config/doublecmd/markdownview_gnuplot"; }

// Writes the embedded gnuplot binary out once (mirrors markdown_engine.cpp's
// font self-seeding) and prepends its directory to PATH so Matplot++'s
// hardcoded `popen("gnuplot")` (a plain shell `/bin/sh -c "gnuplot ..."`
// invocation -- see matplot/util/popen.cpp) finds OUR copy first, rather
// than depending on whatever gnuplot version (or none) the user's system
// happens to have.
bool seedGnuplotBinary() {
    std::string dir = gnuplotSeedDir();
    std::string binPath = dir + "/gnuplot";
    if (fileExists(binPath)) return true; // already seeded, from this run or a previous one
    mkdirParents(dir);
    std::ofstream f(binPath, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char *>(gnuplot_binary), (std::streamsize)gnuplot_binary_size);
    f.close();
    if (!f) return false;
    chmod(binPath.c_str(), 0755);
    return true;
}

struct Rgba { double r, g, b, a = 1.0; };

// CSS-style named colors a report spec's literal "color" field actually
// uses (pin markers, ref_line/ref_band styles, the empty-lane placeholder
// bar) -- previously unrecognized here, so a spec's explicit "black"/
// "gray"/"white" silently failed this parse and fell through to the
// group/auto-cycle palette instead, e.g. every timeline pin that was
// supposed to be a uniform black/gray triangle came out as a different
// palette hue per point. Confirmed live against the real report specs.
// Deliberately just the handful actually used, not a full CSS color table
// -- matches this file's existing scope discipline elsewhere.
bool parseNamedColor(const std::string &s, Rgba &out) {
    if (s == "black") { out = {0, 0, 0, 1.0}; return true; }
    if (s == "white") { out = {1, 1, 1, 1.0}; return true; }
    if (s == "gray" || s == "grey") { out = {0.5, 0.5, 0.5, 1.0}; return true; }
    return false;
}

bool parseHexColor(const std::string &s, Rgba &out) {
    if (parseNamedColor(s, out)) return true;
    if (s.size() != 7 || s[0] != '#') return false;
    try {
        out = {std::stoi(s.substr(1, 2), nullptr, 16) / 255.0,
               std::stoi(s.substr(3, 2), nullptr, 16) / 255.0,
               std::stoi(s.substr(5, 2), nullptr, 16) / 255.0, 1.0};
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

Rgba hexRgba(unsigned int hex) { return {((hex >> 16) & 0xFF) / 255.0, ((hex >> 8) & 0xFF) / 255.0, (hex & 0xFF) / 255.0, 1.0}; }

// Design tokens ported from ~/repos/reports/charts.py's own _apply_theme()
// (the matplotlib renderer these reports were originally designed for) --
// a soft desaturated 8-color categorical palette instead of matplotlib's
// harsher default "tab10" -- see chart_render.cpp's kPaletteLight/Dark for
// the fuller writeup; identical values here so a chart looks the same
// regardless of which backend actually drew it. Selected once per render
// (see renderChartMatplotPng) based on darkMode via g_activePalette, same
// render-scoped-global pattern as chart_render.cpp's.
const Rgba kPaletteLight[] = {
    hexRgba(0x2a78d6), hexRgba(0xeb6834), hexRgba(0x1baf7a), hexRgba(0xeda100),
    hexRgba(0xe87ba4), hexRgba(0x008300), hexRgba(0x4a3aa7), hexRgba(0xe34948),
};
const Rgba kPaletteDark[] = {
    hexRgba(0x5b9ee8), hexRgba(0xf0885c), hexRgba(0x3ecb96), hexRgba(0xf0b840),
    hexRgba(0xee9ac0), hexRgba(0x5abf5a), hexRgba(0x7d6bd0), hexRgba(0xea6f6e),
};
const Rgba *g_activePalette = kPaletteLight;
int g_activePaletteSize = (int)(sizeof(kPaletteLight) / sizeof(kPaletteLight[0]));

// The figure is rendered at this many times the logical (display) pixel
// size -- see renderChartMatplotPng's comment. Every hand-picked
// point-based size in this file (font_size, line_width, marker_size)
// needs multiplying by this to actually read as the same relative size on
// the oversampled canvas, since gnuplot has no single transform we can
// apply once the way Cairo's cairo_scale() covers every drawing call
// automatically.
// Bumped from 2.0: still visibly aliased on circular markers/gridlines in
// a real render at 2x (screenshot-confirmed by the user). Not re-verified
// at 3x in this sandbox (no working build here -- see this file's other
// comments on the CMake cache mismatch); if 3x still isn't enough once
// you rebuild, the next thing to check is whether the <img> tag's
// destination actually gets smooth/interpolated downscaling from whatever
// widget renders markdown_engine.cpp's HTML output, not just this
// oversample factor -- a nearest-neighbor scale-down would look aliased
// at any oversample ratio.
constexpr double kOversample = 3.0;

// A "#rrggbb" hex string -- the form matplot::color_array (matplotColorArray()
// below) needs as input, and also what a handful of setters that DO have a
// working std::string_view overload (marker_face_color(), filled_area's
// lack of one aside) accept directly.
std::string matplotColor(const Rgba &c) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x",
                   (int)std::lround(c.r * 255), (int)std::lround(c.g * 255), (int)std::lround(c.b * 255));
    return buf;
}

// Root-caused live (captured Matplot++'s actual generated gnuplot script
// via a logging wrapper around the seeded binary): line_spec::color(
// std::string_view) and ::marker_color(std::string_view) both go through
// string_to_color(), which only recognizes a fixed set of NAMED colors
// ("red", "blue", ...) and silently falls back to black for anything else
// -- including a "#rrggbb" hex string, which is all this file ever passes.
// Every ->color(hex)/->marker_color(hex) call in this file was therefore
// silently rendering black instead of the requested color the whole time
// (explains the Gantt bars, ref_band boundary lines, and every marker
// OUTLINE all coming out black, while marker FACES -- set via
// marker_face_color(), which forwards through the differently-implemented
// and correctly-hex-aware to_array(std::string_view) -- were always fine).
// to_array(std::string_view) is the one hex parser in this vendored
// version that actually works; route through it explicitly rather than
// the broken string_to_color() path color()/marker_color() take by
// default.
matplot::color_array matplotColorArray(const Rgba &c) {
    return matplot::to_array(matplotColor(c));
}

// Mirrors charts.py's _resolve_color(item, group_colors) -- see
// chart_render.cpp's resolveItemColor for the full writeup (identical
// logic, just Rgba/g_activePalette here instead of Rgb). An explicit
// "color" hex always wins; otherwise "group" claims/reuses a slot in
// group_colors (shared across the WHOLE render call, every layer/panel);
// neither given advances the plain per-draw-call colorIdx cursor instead.
Rgba resolveItemColor(const json &item, double &colorIdx, std::map<std::string, Rgba> &groupColors) {
    std::string colorHex = item.value("color", std::string());
    Rgba c;
    if (!colorHex.empty() && parseHexColor(colorHex, c)) return c;
    if (item.contains("group") && item["group"].is_string()) {
        std::string group = item["group"].get<std::string>();
        auto it = groupColors.find(group);
        if (it != groupColors.end()) return it->second;
        Rgba slot = g_activePalette[groupColors.size() % g_activePaletteSize];
        groupColors.emplace(group, slot);
        return slot;
    }
    Rgba slot = g_activePalette[(int)colorIdx % g_activePaletteSize];
    colorIdx += 1;
    return slot;
}

// One axis's categorical-value -> numeric-position map, shared across
// every layer in a panel (built by buildCategoryRegistries below, before
// any layer is drawn) -- a dumbbell-style chart plots the SAME category
// (e.g. a row label) from more than one layer (a "bars" connector plus a
// "points" scatter at each end), and those need to land on the identical
// position, not each layer inventing its own 0..n-1 numbering.
struct AxisCategoryRegistry {
    std::vector<std::string> labels;
    std::map<std::string, double> pos;
    double resolve(const std::string &s) {
        auto it = pos.find(s);
        if (it != pos.end()) return it->second;
        double p = (double)labels.size();
        labels.push_back(s);
        pos.emplace(s, p);
        return p;
    }
};

std::string jsonToLabel(const json &v) { return v.is_string() ? v.get<std::string>() : v.dump(); }

// gnuplot's pngcairo terminal always runs with "enhanced" text mode when
// the terminal supports it (Matplot++ turns it on unconditionally, see
// figure_type.cpp -- no setting here disables it), which gives "~", "^",
// "_", "{", "}" special overstrike/super/subscript meaning instead of
// their literal characters. Confirmed live on two real report charts: a
// tick label's "(~10M)" rendered as "(0M)" with a stray glyph, and an
// annotation's "~12-24 months" rendered as "224 months" -- both are
// exactly gnuplot enhanced-mode syntax silently eating the literal tildes.
// A SINGLE backslash does NOT fix this -- confirmed live via a raw
// hand-written gnuplot script: gnuplot's own double-quoted-string parser
// consumes one level of backslash before enhanced-mode text ever sees it
// (same reason C string literals need "\\n" to mean a literal backslash-n
// pair), so what reaches the enhanced-mode renderer from a single-escaped
// "\~" is still just a bare "~", identically broken. Two backslashes
// survive the string parse as one literal backslash, which enhanced mode
// then honors as "print this character literally". Applied to every
// user-supplied text string this file hands to Matplot++ (tick labels,
// titles, axis labels, annotations, legend entries) -- not just the ones
// already confirmed broken, since the same misinterpretation risk applies
// to any of them.
std::string escapeGnuplotEnhancedText(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '~' || c == '^' || c == '_' || c == '{' || c == '}') out += "\\\\";
        out += c;
    }
    return out;
}

std::vector<std::string> escapeGnuplotEnhancedText(const std::vector<std::string> &v) {
    std::vector<std::string> out;
    out.reserve(v.size());
    for (const auto &s : v) out.push_back(escapeGnuplotEnhancedText(s));
    return out;
}

// "Nice" tick generation -- matplotlib picks round tick values (clean
// decades on a log axis, 1/2/5-times-a-power-of-ten steps on a linear one)
// via its own locator classes; gnuplot's autotick does neither once an
// axis has an explicit xlim()/ylim() (needed here for the reasons in
// applyLogScale/drawRefLines' comments -- see needsExplicitLimits below),
// instead just dividing the range into a handful of raw equal steps.
// Confirmed live: a log axis from ~2 to ~7e8 came back labelled "2.04,
// 54.46, 1452.28, 38729.83, ..." (a constant ratio, but not decade-aligned)
// and a linear year axis came back "2021.85, 2022.73, 2023.62, ...".
// These two functions replace that with matplotlib-equivalent tick
// selection, applied explicitly via tick_values()/ticklabels() below
// instead of leaving it to gnuplot.
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

// Classic Heckbert "nice number" (Graphics Gems, 1990): snaps to
// 1/2/5/10 x 10^n, the same step family matplotlib's own MaxNLocator
// draws from. round=true snaps to the CLOSEST such step (used for the
// data range itself); round=false snaps UP (used for the tick spacing, so
// the spacing is never finer than what the target tick count asked for).
double niceNum(double range, bool round) {
    if (!(range > 0)) return 1.0;
    double exponent = std::floor(std::log10(range));
    double fraction = range / std::pow(10.0, exponent);
    double niceFraction;
    if (round) {
        if (fraction < 1.5) niceFraction = 1;
        else if (fraction < 3) niceFraction = 2;
        else if (fraction < 7) niceFraction = 5;
        else niceFraction = 10;
    } else {
        if (fraction <= 1) niceFraction = 1;
        else if (fraction <= 2) niceFraction = 2;
        else if (fraction <= 5) niceFraction = 5;
        else niceFraction = 10;
    }
    return niceFraction * std::pow(10.0, exponent);
}

// Round tick positions covering at least [lo, hi], spaced at a "nice"
// step, targeting ~targetTicks of them (matplotlib's own default locator
// targets a similar handful). Returned ticks are the ones to actually
// pass to tick_values()/ticklabels() -- callers do NOT need to further
// clip to [lo, hi] since a tick just outside the padded data range is
// normal (matplotlib does the same).
std::vector<double> niceLinearTicks(double lo, double hi, int targetTicks = 6) {
    std::vector<double> out;
    if (!(hi > lo)) { out.push_back(lo); return out; }
    double range = niceNum(hi - lo, false);
    double step = niceNum(range / std::max(1, targetTicks - 1), true);
    if (!(step > 0)) { out.push_back(lo); out.push_back(hi); return out; }
    double niceMin = std::floor(lo / step) * step;
    double niceMax = std::ceil(hi / step) * step;
    for (double v = niceMin; v <= niceMax + step * 0.5; v += step) out.push_back(v);
    return out;
}

// One tick per decade covering [lo, hi] -- matplotlib's default LogLocator
// behaviour for a range spanning more than a couple of decades (which is
// the common case for this plugin's cost-exchange/output charts). Label
// text uses gnuplot enhanced mode's native superscript syntax ("^{n}"),
// so it must NOT go through escapeGnuplotEnhancedText (that escapes '^'
// specifically to stop it being read as superscript -- here that's
// exactly what's wanted).
struct LogTicks { std::vector<double> values; std::vector<std::string> labels; };
LogTicks logDecadeTicks(double lo, double hi) {
    LogTicks t;
    if (!(lo > 0) || !(hi > lo)) { t.values = {std::max(lo, 1e-300), hi}; t.labels = {formatTick(lo), formatTick(hi)}; return t; }
    int loDecade = (int)std::floor(std::log10(lo));
    int hiDecade = (int)std::ceil(std::log10(hi));
    for (int d = loDecade; d <= hiDecade; ++d) {
        t.values.push_back(std::pow(10.0, d));
        t.labels.push_back("10^{" + std::to_string(d) + "}");
    }
    return t;
}

// A single coordinate value: numeric passes through unchanged, a string
// resolves against (and grows) the registry.
double resolveScalar(const json &v, AxisCategoryRegistry &reg) {
    if (v.is_number()) return v.get<double>();
    return reg.resolve(jsonToLabel(v));
}

// x is either all-numeric (returned as-is) or all-string (categorical --
// resolved through the shared registry so multiple layers agree on
// position).
std::vector<double> resolveAxis(const json &arr, AxisCategoryRegistry *reg) {
    std::vector<double> out;
    if (!arr.is_array()) return out;
    for (const auto &v : arr) out.push_back(reg ? resolveScalar(v, *reg) : (v.is_number() ? v.get<double>() : 0.0));
    return out;
}

// Pre-scans every layer's coordinate fields so categorical axes get a
// stable, shared numbering before any layer actually draws -- run once
// per panel, ahead of drawPanelLayers.
void buildCategoryRegistries(const PanelSpec &panel, AxisCategoryRegistry &xReg, AxisCategoryRegistry &yReg) {
    auto scanArr = [](const json &arr, AxisCategoryRegistry &reg) {
        if (!arr.is_array()) return;
        for (const auto &v : arr) if (!v.is_number()) reg.resolve(jsonToLabel(v));
    };
    for (const auto &L : panel.layers) {
        const json &j = L.j;
        if (L.type == "line" || L.type == "area" || L.type == "step" || L.type == "stem" || L.type == "errorbar") {
            scanArr(j.value("x", json::array()), xReg);
        } else if (L.type == "bar" || L.type == "barh") {
            scanArr(j.value("x", json::array()), xReg);
            if (j.contains("bars") && j["bars"].is_array()) {
                for (const auto &b : j["bars"]) {
                    if (!b.is_object()) continue;
                    if (b.contains("y") && !b["y"].is_number()) yReg.resolve(jsonToLabel(b["y"]));
                    if (b.contains("x") && !b["x"].is_number()) xReg.resolve(jsonToLabel(b["x"]));
                }
            }
        } else if (L.type == "scatter") {
            if (j.contains("points") && j["points"].is_array()) {
                for (const auto &p : j["points"]) {
                    if (!p.is_object()) continue;
                    if (p.contains("x") && !p["x"].is_number()) xReg.resolve(jsonToLabel(p["x"]));
                    if (p.contains("y") && !p["y"].is_number()) yReg.resolve(jsonToLabel(p["y"]));
                }
            } else {
                scanArr(j.value("x", json::array()), xReg);
            }
        }
    }
}

// Whether this whole chart uses anything the Matplot++ path doesn't (yet)
// implement natively -- checked up front so a chart either renders
// entirely via Matplot++ or entirely via Cairo, never a mix of both
// styles across panels/layers within the same image. Matplot++'s `bars`
// object has no stacking support at all (only grouped, via multi-series
// Y) as of this vendored version -- everything else (ref_lines/bands,
// Gantt-style "bars" lists, categorical scatter/errorbar axes) is now
// handled natively; see buildCategoryRegistries/drawRefLines.
bool hasUnsupportedFeature(const TopSpec &top) {
    for (const auto &panel : top.panels) {
        for (const auto &L : panel.layers) {
            if (L.type == "bar" && L.j.value("stacked", false)) return true;
            // ax->area() only fills to a single scalar baseline -- no
            // matplotlib-style fill_between(x, y1, y2) with a per-point
            // second curve, which an array "baseline" (a widening/varying
            // band, not a fixed floor) needs.
            if (L.type == "area" && L.j.contains("baseline") && L.j["baseline"].is_array()) return true;
        }
    }
    return false;
}

// A simplified Gaussian KDE (Silverman's rule-of-thumb bandwidth) -- same
// approach and same reasoning as chart_render.cpp's Cairo violin (no
// native violin support in this Matplot++ version either), drawn here as
// a filled polygon via ax->fill().
void drawViolin(matplot::axes_handle ax, const json &L, double &nextColorIdx) {
    if (!L.contains("data") || !L["data"].is_array()) return;
    bool showMedians = L.value("showmedians", true);
    Rgba color = g_activePalette[(int)nextColorIdx % g_activePaletteSize];
    nextColorIdx += 1;
    size_t idx = 0;
    constexpr int kSamples = 40;
    for (const auto &arrJson : L["data"]) {
        std::vector<double> v = ChartSpec::numArray(arrJson);
        ++idx;
        if (v.size() < 2) continue;
        double mean = 0; for (double x : v) mean += x; mean /= v.size();
        double var = 0; for (double x : v) var += (x - mean) * (x - mean); var /= (v.size() - 1);
        double sigma = std::sqrt(std::max(var, 1e-9));
        double bandwidth = std::max(1.06 * sigma * std::pow((double)v.size(), -0.2), 1e-6);
        double vmin = *std::min_element(v.begin(), v.end()), vmax = *std::max_element(v.begin(), v.end());
        double pad = (vmax - vmin) * 0.15 + bandwidth;
        double cx = (double)idx;
        std::vector<double> polyX, polyY;
        double maxDensity = 1e-9;
        std::vector<double> density(kSamples + 1);
        for (int s = 0; s <= kSamples; ++s) {
            double y = vmin - pad + (vmax - vmin + 2 * pad) * s / kSamples;
            double d = 0;
            for (double x : v) { double z = (y - x) / bandwidth; d += std::exp(-0.5 * z * z); }
            d /= (v.size() * bandwidth * std::sqrt(2 * M_PI));
            density[s] = d;
            maxDensity = std::max(maxDensity, d);
        }
        double halfW = 0.35;
        for (int s = 0; s <= kSamples; ++s) {
            double y = vmin - pad + (vmax - vmin + 2 * pad) * s / kSamples;
            polyX.push_back(cx + (density[s] / maxDensity) * halfW);
            polyY.push_back(y);
        }
        for (int s = kSamples; s >= 0; --s) {
            double y = vmin - pad + (vmax - vmin + 2 * pad) * s / kSamples;
            polyX.push_back(cx - (density[s] / maxDensity) * halfW);
            polyY.push_back(y);
        }
        // Not ax->area() -- this KDE outline is a closed polygon, not a
        // simple curve-vs-baseline shape area() can express. ax->fill()
        // is confirmed to render solid black instead of any requested
        // color (see drawRefLines' comment for the full story: "with
        // filledcurves", which this goes through, never gets gnuplot's
        // fill style enabled), so this violin's fill color is a known,
        // currently-unfixed gap -- lower priority since no chart in
        // either real report document uses type:"violin" yet.
        auto h = ax->fill(polyX, polyY);
        h->color(matplotColorArray(color));
        h->line_width(0.5 * kOversample);
        if (showMedians) {
            std::vector<double> sorted = v;
            std::sort(sorted.begin(), sorted.end());
            double med = sorted[sorted.size() / 2];
            ax->plot(std::vector<double>{cx - halfW * 0.5, cx + halfW * 0.5}, std::vector<double>{med, med})
                ->color("black");
        }
    }
}

void applyLogScale(matplot::axes_handle ax, bool logX, bool logY) {
    if (logX) ax->x_axis().scale(matplot::axis_type::axis_scale::log);
    if (logY) ax->y_axis().scale(matplot::axis_type::axis_scale::log);
}

// ref_lines/ref_bands are drawn as an ordinary line/fill spanning
// explicit, caller-computed limits -- the classic axhline/axhspan
// "infinite line" trick. This deliberately does NOT read back
// ax->x_axis().limits()/y_axis().limits(): those are only ever what an
// explicit xlim()/ylim() call set, never back-filled from data actually
// plotted (confirmed live: after drawing a 2-point errorbar layer,
// limits() still read the class's uninitialized default), because
// gnuplot's own autoscale only happens once the whole script runs at
// save() time, long after this function returns. So the caller
// (renderChartMatplotPng) computes its own extent from the panel's data
// (extentForPanel below) and calls ax->xlim()/ylim() explicitly BEFORE
// this runs -- exactly what the Cairo renderer already does -- which
// both gives this trick real limits to clip against AND pins the axes to
// manual mode so gnuplot's autoscale can't re-expand them afterward.
void drawRefLines(matplot::axes_handle ax, const PanelSpec &panel, const std::array<double, 2> &xlim, const std::array<double, 2> &ylim, int axesPxHeight) {
    for (const auto &rl : panel.refLines) {
        Rgba c{0, 0, 0};
        bool hasColor = !rl.colorHex.empty() && parseHexColor(rl.colorHex, c);
        auto h = (rl.axis == "y")
            ? ax->plot(std::vector<double>{xlim[0], xlim[1]}, std::vector<double>{rl.value, rl.value})
            : ax->plot(std::vector<double>{rl.value, rl.value}, std::vector<double>{ylim[0], ylim[1]});
        h->line_width(1.2 * kOversample);
        if (hasColor) h->color(matplotColorArray(c));
        if (!rl.label.empty()) h->display_name(escapeGnuplotEnhancedText(rl.label));
    }
    for (const auto &rb : panel.refBands) {
        Rgba c{0.5, 0.5, 0.5};
        bool hasColor = !rb.colorHex.empty() && parseHexColor(rb.colorHex, c);
        Rgba bandColor = hasColor ? c : Rgba{0.5, 0.5, 0.5};
        if (rb.axis == "y") {
            // NOT ax->area() -- a `filled_area` sends its fill as its own
            // separate inline '-' data block, and confirmed live, that
            // block comes out invisible when generated through Matplot++'s
            // always-multiplot-wrapped pipeline (gnuplot's own
            // "Reading from '-' inside a multiplot not supported" warning,
            // seen on every render, is the real cause here -- not the
            // harmless red herring it looked like earlier in this file's
            // history). A single thick line has only one data block and
            // sidesteps it entirely -- draw the band as a heavy stroke
            // through its own centerline. line_width is gnuplot's own
            // "points" unit, calibrated against this axes' actual pixel
            // height so the band's apparent thickness matches its real
            // data-unit span regardless of figsize/axis range.
            constexpr double kPxPerLineWidthUnit = 2.2;
            double axesRangeUnits = std::max(ylim[1] - ylim[0], 1e-9);
            double bandLineWidth = std::max(1.0, ((rb.high - rb.low) / axesRangeUnits) * axesPxHeight / kPxPerLineWidthUnit);
            auto h = ax->plot(std::vector<double>{xlim[0], xlim[1]}, std::vector<double>{(rb.low + rb.high) / 2.0, (rb.low + rb.high) / 2.0});
            h->line_width(bandLineWidth);
            h->color(matplotColorArray(bandColor));
        } else {
            // Vertical band: area() only fills vertically (in y, as a
            // function of x), so it can't express this directly -- draw
            // the two boundary lines instead of a fill. Rarer in
            // practice than the horizontal case above (neither real
            // report document used one at the time this was written).
            auto h1 = ax->plot(std::vector<double>{rb.low, rb.low}, std::vector<double>{ylim[0], ylim[1]});
            h1->color(matplotColorArray(bandColor)); h1->line_width(1.0 * kOversample);
            auto h2 = ax->plot(std::vector<double>{rb.high, rb.high}, std::vector<double>{ylim[0], ylim[1]});
            h2->color(matplotColorArray(bandColor)); h2->line_width(1.0 * kOversample);
        }
    }
}

void drawAnnotations(matplot::axes_handle ax, const PanelSpec &panel, AxisCategoryRegistry &xReg, AxisCategoryRegistry &yReg, const Rgba &fg,
                      double xRangeDataUnits, double yRangeDataUnits, double axesPxWidth, double axesPxHeight) {
    for (const auto &a : panel.annotations) {
        double ax_ = resolveScalar(a.x, xReg), ay_ = resolveScalar(a.y, yReg);
        // a.dx/a.dy are matplotlib's "offset points" (textcoords="offset
        // points" in charts.py) -- a screen-space nudge away from the
        // anchor, not a data-unit one. Previously ignored entirely here
        // (text landed exactly ON the anchor point), which looked
        // survivable when markers were small; once the 2x oversample fix
        // (kOversample) made markers/pins noticeably bigger, un-offset
        // annotation text started rendering fully UNDER a pin instead of
        // beside it, confirmed live on a real Gantt timeline chart.
        // Converting via the axes' known pixel size is an approximation
        // (assumes ~1 point ~= 1 css px, and doesn't account for log
        // scales), but a close, visible nudge beats none at all.
        double dxData = axesPxWidth > 0 ? (a.dx * kOversample / axesPxWidth) * xRangeDataUnits : 0.0;
        double dyData = axesPxHeight > 0 ? (a.dy * kOversample / axesPxHeight) * yRangeDataUnits : 0.0;
        auto t = ax->text(ax_ + dxData, ay_ + dyData, escapeGnuplotEnhancedText(a.text));
        t->font_size(a.fontsize * kOversample);
        // labels::color(T) forwards to to_array(), the hex parser that
        // actually works (unlike line_spec's color()/marker_color(), see
        // matplotColorArray's comment) -- a plain hex string is fine here.
        t->color(matplotColor(fg));
    }
}

// Tracks the numeric extent of everything drawPanelLayers actually plots,
// so ref_lines/ref_bands (and eventually axis padding) have real data
// bounds to work with -- see drawRefLines' comment for why this can't
// just be read back from the axes afterward.
struct Extent {
    double xmin = std::numeric_limits<double>::infinity(), xmax = -std::numeric_limits<double>::infinity();
    double ymin = std::numeric_limits<double>::infinity(), ymax = -std::numeric_limits<double>::infinity();
    void addX(double v) { if (std::isfinite(v)) { xmin = std::min(xmin, v); xmax = std::max(xmax, v); } }
    void addY(double v) { if (std::isfinite(v)) { ymin = std::min(ymin, v); ymax = std::max(ymax, v); } }
    bool hasX() const { return xmin <= xmax; }
    bool hasY() const { return ymin <= ymax; }
};

// Draws every layer of one panel onto ax, returns true if the panel used
// anything worth a legend. xReg/yReg must already be fully populated (see
// buildCategoryRegistries) so every layer agrees on the same category ->
// position mapping; ext accumulates the real data range as a side effect,
// for ref_lines/ref_bands and axis padding.
// legendNames collects the ACTUAL label text at every display_name() call
// (not just a bool) -- matplot::legend(ax) with no names auto-includes
// EVERY plotted series regardless of whether it was ever given a label
// (confirmed live: an unlabeled Gantt connector bar and 6 unlabeled scatter
// points all showed up as "data1".."data10" placeholder entries alongside
// the 2 real ones). Passing this vector to matplot::legend(ax, names)
// instead restricts the legend to exactly the series that were actually
// labeled.
bool drawPanelLayers(matplot::axes_handle ax, const PanelSpec &panel, double &colorIdx,
                      AxisCategoryRegistry &xReg, AxisCategoryRegistry &yReg, Extent &ext, int axesPxHeight,
                      std::map<std::string, Rgba> &groupColors,
                      std::vector<std::pair<matplot::axes_object_handle, std::string>> &legendEntries) {
    bool anyLegend = false;
    matplot::hold(ax, true);
    for (const auto &L : panel.layers) {
        const json &j = L.j;
        if (L.type == "line" || L.type == "area" || L.type == "step") {
            std::vector<double> x = resolveAxis(j.value("x", json::array()), &xReg);
            for (double v : x) ext.addX(v);
            auto drawSeries = [&](const std::vector<double> &y, const std::string &label, const std::string &marker) {
                for (double v : y) ext.addY(v);
                matplot::line_handle h;
                if (L.type == "step") h = ax->stairs(x, y);
                else h = ax->plot(x, y);
                Rgba c = g_activePalette[(int)colorIdx % g_activePaletteSize]; colorIdx += 1;
                h->color(matplotColorArray(c));
                h->line_width(1.6 * kOversample);
                if (L.type == "line" && marker != "none") h->marker(matplot::line_spec::marker_style::circle);
                if (L.type == "area") { auto fh = ax->area(x, y); fh->color(matplotColorArray(c)); }
                if (!label.empty()) { h->display_name(escapeGnuplotEnhancedText(label)); anyLegend = true; legendEntries.push_back({h, label}); }
            };
            if (j.contains("series") && j["series"].is_array()) {
                for (const auto &s : j["series"])
                    if (s.is_object() && s.contains("y"))
                        drawSeries(ChartSpec::numArray(s["y"]), s.value("label", std::string()), s.value("marker", std::string()));
            } else if (j.contains("y")) {
                drawSeries(ChartSpec::numArray(j["y"]), j.value("label", std::string()), j.value("marker", std::string()));
            }
        } else if (L.type == "bar" || L.type == "barh") {
            bool isBarh = L.type == "barh";
            if (j.contains("bars") && j["bars"].is_array()) {
                // Gantt-style form: each entry is its own span (start
                // "left", size "width") at its own row/column position
                // ("y" for barh, "x" for bar) -- drawn as a thin filled
                // rectangle rather than through matplot's own bars object,
                // which only understands one shared category axis across
                // all bars, not an independent span per entry.
                for (const auto &b : j["bars"]) {
                    if (!b.is_object()) continue;
                    double left = b.value("left", 0.0), width = b.value("width", 0.0);
                    double half = b.value("height", 0.6) / 2.0;
                    double pos = isBarh ? resolveScalar(b.value("y", json(0.0)), yReg)
                                        : resolveScalar(b.value("x", json(0.0)), xReg);
                    Rgba c = resolveItemColor(b, colorIdx, groupColors);
                    // NOT ax->area()/ax->fill() -- both are a
                    // `filled_area`/`line` object that sends its fill as
                    // its own separate inline '-' data block, and
                    // gnuplot's own diagnostic ("Reading from '-' inside
                    // a multiplot not supported; use a datablock
                    // instead", seen on every render since Matplot++
                    // always wraps a figure in `set multiplot` even for
                    // one panel) turned out not to be the harmless red
                    // herring it looked like earlier in this file's
                    // history: confirmed live via a raw hand-written
                    // gnuplot script that the exact same "with
                    // filledcurve" command DOES fill solid outside a
                    // multiplot, but produced a fully invisible fill
                    // (color-checked pixel-by-pixel) when generated
                    // through Matplot++'s actual multiplot-wrapped
                    // pipeline. A single thick line has only one data
                    // block and doesn't hit this at all -- draw the bar
                    // as a heavy stroke through its own centerline
                    // instead of a true filled rectangle. line_width is
                    // gnuplot's own "points" unit, calibrated against
                    // this axes' actual pixel height so a bar's apparent
                    // thickness roughly matches its "height" fraction of
                    // the row spacing regardless of figsize.
                    constexpr double kPxPerLineWidthUnit = 2.2;
                    double axesRangeUnits = (isBarh ? yReg.labels.size() : xReg.labels.size()) + 1.2; // matches paddedLimits' +-0.6 categorical headroom
                    double barLineWidth = std::max(1.0, (2 * half / std::max(axesRangeUnits, 1.0)) * axesPxHeight / kPxPerLineWidthUnit);
                    // "left" defaulting to (or explicitly) 0 is a valid
                    // start point on a LINEAR span axis (the origin) but is
                    // literally log(0) = -infinity on a log-scale one --
                    // confirmed live: an entire barh chart ("Five different
                    // quantities", log_x:true, no bar specifying "left" at
                    // all so every one defaulted to 0) rendered completely
                    // blank, every bar's line silently dropped because one
                    // endpoint was unplottable. Use a small positive
                    // stand-in for the DRAWING only, scaled off the bar's
                    // own width so it reads as "starts near the left edge"
                    // regardless of this chart's value scale; the real
                    // (invalid) left is excluded from ext below so it
                    // doesn't blow the axis range out toward zero.
                    bool logSpanAxis = isBarh ? panel.logX : panel.logY;
                    bool leftIsLogInvalid = logSpanAxis && left <= 0;
                    double leftDraw = leftIsLogInvalid ? std::max(width, 1e-9) * 1e-3 : left;
                    matplot::line_handle h;
                    if (isBarh) {
                        h = ax->plot(std::vector<double>{leftDraw, left + width}, std::vector<double>{pos, pos});
                        if (!leftIsLogInvalid) ext.addX(left);
                        ext.addX(left + width); ext.addY(pos - half); ext.addY(pos + half);
                    } else {
                        h = ax->plot(std::vector<double>{pos, pos}, std::vector<double>{leftDraw, left + width});
                        if (!leftIsLogInvalid) ext.addY(left);
                        ext.addY(left + width); ext.addX(pos - half); ext.addX(pos + half);
                    }
                    h->color(matplotColorArray(c));
                    h->line_width(barLineWidth);
                }
                continue;
            }
            std::vector<double> x = resolveAxis(j.value("x", json::array()), isBarh ? &yReg : &xReg);
            for (double v : x) isBarh ? ext.addY(v) : ext.addX(v);
            std::vector<std::vector<double>> ys;
            std::vector<std::string> labels;
            if (j.contains("series") && j["series"].is_array()) {
                for (const auto &s : j["series"]) if (s.is_object() && s.contains("y")) {
                    ys.push_back(ChartSpec::numArray(s["y"]));
                    labels.push_back(s.value("label", std::string()));
                }
            } else if (j.contains("y")) {
                ys.push_back(ChartSpec::numArray(j["y"]));
                labels.push_back(j.value("label", std::string()));
            }
            if (ys.empty()) continue;
            for (const auto &y : ys) for (double v : y) isBarh ? ext.addX(v) : ext.addY(v);
            auto bh = ax->bar(x, ys);
            bh->vertical_orientation(!isBarh);
            if (ys.size() == 1) {
                Rgba c = g_activePalette[(int)colorIdx % g_activePaletteSize]; colorIdx += 1;
                bh->face_color(matplotColor(c));
                if (!labels[0].empty()) { bh->display_name(escapeGnuplotEnhancedText(labels[0])); anyLegend = true; legendEntries.push_back({bh, labels[0]}); }
            } else {
                std::vector<matplot::color_array> faceColors;
                for (size_t i = 0; i < ys.size(); ++i) {
                    Rgba c = g_activePalette[(int)colorIdx % g_activePaletteSize]; colorIdx += 1;
                    // color_array's first slot is alpha in matplot::to_string's
                    // own convention (util/colors.cpp:253) -- 0 means OPAQUE
                    // (every named-color shortcut there has c[0]==0), the
                    // opposite of the usual 0=transparent/1=opaque alpha
                    // convention Rgba::a follows. Passing c.a (1.0 here,
                    // "fully opaque" in Rgba's own sense) through unchanged
                    // produced alpha byte 0xFF -- fully TRANSPARENT bars.
                    // Hardcode 0.0f instead of forwarding c.a.
                    faceColors.push_back({0.0f, (float)c.r, (float)c.g, (float)c.b});
                }
                bh->face_colors(faceColors);
                anyLegend = anyLegend || std::any_of(labels.begin(), labels.end(), [](auto &s) { return !s.empty(); });
            }
        } else if (L.type == "scatter") {
            if (j.contains("points") && j["points"].is_array()) {
                for (const auto &p : j["points"]) {
                    if (!p.is_object() || !p.contains("x") || !p.contains("y")) continue;
                    double px = resolveScalar(p["x"], xReg), py = resolveScalar(p["y"], yReg);
                    ext.addX(px); ext.addY(py);
                    double size = std::max(1.0, p.value("size", 36.0));
                    bool filled = p.value("filled", true);
                    Rgba c = resolveItemColor(p, colorIdx, groupColors);
                    auto sh = ax->scatter(std::vector<double>{px}, std::vector<double>{py});
                    // line_spec::marker(std::string_view) (via the marker()
                    // template forwarding to marker_style()) parses
                    // matplotlib-style single-char marker codes directly --
                    // "o"/"x"/"^"/"v"/"D"/"+"/"*"/"s" all map correctly.
                    // Previously never read at all, so every point rendered
                    // as a plain circle regardless of what the spec asked
                    // for -- confirmed live against a real report chart
                    // that deliberately used different marker shapes ("x"
                    // for reversals, "^" for builds, "o" for context
                    // events) to distinguish event types sharing one row.
                    sh->marker(p.value("marker", std::string("o")));
                    sh->marker_size(std::sqrt(size) * 3.0 * kOversample);
                    sh->marker_color(matplotColorArray(c));
                    // Marker outline width is independent of marker_size, but
                    // left at the object's default it reads as a
                    // disproportionately thick ring on a large bubble --
                    // pin it to a small constant regardless of size.
                    sh->line_width(1.0 * kOversample);
                    sh->marker_face(filled);
                    if (filled) sh->marker_face_color(matplotColor(c));
                    std::string label = p.value("label", std::string());
                    if (!label.empty()) { sh->display_name(escapeGnuplotEnhancedText(label)); anyLegend = true; legendEntries.push_back({sh, label}); }
                    if (p.value("annotate", false)) {
                        std::string txt = p.value("annotate_text", label);
                        // Exactly at the marker's own center, not offset by a
                        // fixed data-unit epsilon -- that epsilon was tuned
                        // against one chart's axis range and drifted visibly
                        // off-center on any chart with a different range
                        // (confirmed live). alignment(center) does the
                        // horizontal centering; gnuplot's own label anchor
                        // already sits close enough vertically for a number
                        // inside a bubble.
                        auto t = ax->text(px, py, escapeGnuplotEnhancedText(txt));
                        t->font_size(9 * kOversample);
                        t->alignment(matplot::labels::alignment::center);
                    }
                }
            } else if (j.contains("x")) {
                std::vector<double> x = resolveAxis(j["x"], &xReg);
                for (double v : x) ext.addX(v);
                auto drawSeries = [&](const std::vector<double> &y, const std::string &label) {
                    for (double v : y) ext.addY(v);
                    auto sh = ax->scatter(x, y);
                    Rgba c = g_activePalette[(int)colorIdx % g_activePaletteSize]; colorIdx += 1;
                    sh->marker_color(matplotColorArray(c));
                    sh->marker_face(true);
                    sh->marker_face_color(matplotColor(c));
                    if (!label.empty()) { sh->display_name(escapeGnuplotEnhancedText(label)); anyLegend = true; legendEntries.push_back({sh, label}); }
                };
                if (j.contains("series") && j["series"].is_array()) {
                    for (const auto &s : j["series"]) if (s.is_object() && s.contains("y"))
                        drawSeries(ChartSpec::numArray(s["y"]), s.value("label", std::string()));
                } else if (j.contains("y")) {
                    drawSeries(ChartSpec::numArray(j["y"]), j.value("label", std::string()));
                }
            }
        } else if (L.type == "stem") {
            std::vector<double> x = resolveAxis(j.value("x", json::array()), &xReg);
            std::vector<double> y = ChartSpec::numArray(j.value("y", json::array()));
            for (double v : x) ext.addX(v);
            for (double v : y) ext.addY(v);
            ax->stem(x, y);
        } else if (L.type == "errorbar") {
            std::vector<double> x = resolveAxis(j.value("x", json::array()), &xReg);
            std::vector<double> y = ChartSpec::numArray(j.value("y", json::array()));
            std::vector<double> yerr = j.contains("yerr") ? ChartSpec::numArray(j["yerr"]) : std::vector<double>();
            // matplotlib's asymmetric [[lo...],[hi...]] form: average the
            // two into one symmetric magnitude -- Matplot++'s errorbar()
            // only takes one magnitude per point, not separate +/-.
            if (j.contains("yerr") && j["yerr"].is_array() && !j["yerr"].empty() && j["yerr"][0].is_array()) {
                std::vector<double> lo = ChartSpec::numArray(j["yerr"][0]), hi = ChartSpec::numArray(j["yerr"][1]);
                yerr.clear();
                for (size_t i = 0; i < lo.size() && i < hi.size(); ++i) yerr.push_back((lo[i] + hi[i]) / 2.0);
            }
            for (double v : x) ext.addX(v);
            for (size_t i = 0; i < y.size(); ++i) {
                ext.addY(y[i]);
                if (i < yerr.size()) { ext.addY(y[i] - yerr[i]); ext.addY(y[i] + yerr[i]); }
            }
            auto eh = ax->errorbar(x, y, yerr);
            // charts.py's "fmt": "o" convention means markers only, no
            // connecting line between points -- errorbar() draws one by
            // default regardless.
            if (j.value("fmt", std::string()).find('-') == std::string::npos) {
                // error_bar::plot_string() unconditionally appends its
                // base `line`'s own plot command (an ordinary line
                // through the same points) -- line_width(0) alone didn't
                // suppress it live (gnuplot clamps to a visible minimum
                // width rather than truly hiding it); line_style("none")
                // does, and is safe here (unlike filled_area's
                // plot_base_line, error_bar's extra command is always
                // included regardless of style, so there's no matching
                // data block to desync).
                eh->line_style("none");
                eh->marker(matplot::line_spec::marker_style::circle);
            }
        } else if (L.type == "histogram") {
            std::vector<double> values = j.contains("values") ? ChartSpec::numArray(j["values"]) : std::vector<double>();
            if (j.contains("bins") && j["bins"].is_number_integer())
                ax->hist(values, (size_t)j["bins"].get<int>());
            else
                ax->hist(values);
        } else if (L.type == "boxplot") {
            if (j.contains("data") && j["data"].is_array()) {
                std::vector<std::vector<double>> data;
                for (const auto &arr : j["data"]) data.push_back(ChartSpec::numArray(arr));
                ax->boxplot(data);
                if (j.contains("labels") && j["labels"].is_array()) {
                    std::vector<std::string> labels;
                    for (const auto &v : j["labels"]) labels.push_back(v.is_string() ? v.get<std::string>() : v.dump());
                    ax->x_axis().ticklabels(escapeGnuplotEnhancedText(labels));
                }
            }
        } else if (L.type == "violin") {
            drawViolin(ax, j, colorIdx);
        }
    }
    auto categoryPositions = [](size_t n) { std::vector<double> p(n); for (size_t i = 0; i < n; ++i) p[i] = (double)i; return p; };
    if (!xReg.labels.empty()) { ax->x_axis().tick_values(categoryPositions(xReg.labels.size())); ax->x_axis().ticklabels(escapeGnuplotEnhancedText(xReg.labels)); }
    if (!yReg.labels.empty()) { ax->y_axis().tick_values(categoryPositions(yReg.labels.size())); ax->y_axis().ticklabels(escapeGnuplotEnhancedText(yReg.labels)); }
    return anyLegend;
}

// A little headroom around the real data range, in whatever units that
// axis is in: a fixed half-category width for a categorical axis (so the
// first/last bar or point isn't flush against the frame), a multiplicative
// factor for a log axis (additive padding would be meaningless once
// exponentiated back), plain 5% of the range otherwise. lo==hi (a single
// data point, or no data at all) gets a fixed fallback span so the axes
// aren't degenerate.
std::array<double, 2> paddedLimits(double lo, double hi, bool isLog, bool isCategorical) {
    if (isCategorical) return {lo - 0.6, hi + 0.6};
    if (!(lo <= hi)) return isLog ? std::array<double, 2>{0.1, 10.0} : std::array<double, 2>{0.0, 1.0};
    if (isLog) {
        double loP = std::max(lo, 1e-300) / 1.15, hiP = std::max(hi, 1e-300) * 1.15;
        return {loP, hiP};
    }
    double range = hi - lo;
    double pad = range > 0 ? range * 0.05 : std::max(std::abs(hi), 1.0) * 0.1;
    return {lo - pad, hi + pad};
}

void renderHeatmapPanel(matplot::axes_handle ax, const json &j) {
    if (!j.contains("matrix") || !j["matrix"].is_array()) return;
    std::vector<std::vector<double>> matrix;
    for (const auto &row : j["matrix"]) matrix.push_back(ChartSpec::numArray(row));
    ax->heatmap(matrix);
    if (j.contains("xticklabels") && j["xticklabels"].is_array()) {
        std::vector<std::string> labels;
        for (const auto &v : j["xticklabels"]) labels.push_back(v.is_string() ? v.get<std::string>() : v.dump());
        ax->x_axis().ticklabels(escapeGnuplotEnhancedText(labels));
    }
    if (j.contains("yticklabels") && j["yticklabels"].is_array()) {
        std::vector<std::string> labels;
        for (const auto &v : j["yticklabels"]) labels.push_back(v.is_string() ? v.get<std::string>() : v.dump());
        ax->y_axis().ticklabels(escapeGnuplotEnhancedText(labels));
    }
}

void renderPiePanel(matplot::axes_handle ax, const json &j) {
    std::vector<double> values = j.contains("values") ? ChartSpec::numArray(j["values"]) : std::vector<double>();
    if (values.empty()) return;
    std::vector<std::string> labels;
    if (j.contains("labels") && j["labels"].is_array())
        for (const auto &v : j["labels"]) labels.push_back(v.is_string() ? v.get<std::string>() : v.dump());
    ax->pie(values, labels);
}

} // namespace

namespace MatplotPP {

bool isAvailable() {
    static bool checked = false;
    static bool available = false;
    if (checked) return available;
    checked = true;

    if (seedGnuplotBinary()) {
        std::string dir = gnuplotSeedDir();
        std::string binPath = dir + "/gnuplot";
        if (access(binPath.c_str(), X_OK) == 0) {
            const char *existingPath = std::getenv("PATH");
            std::string newPath = dir + (existingPath ? std::string(":") + existingPath : std::string());
            setenv("PATH", newPath.c_str(), 1);
            available = true;
        }
    }
    return available;
}

std::vector<uint8_t> renderChartMatplotPng(const TopSpec &top, bool darkMode,
                                            const std::string &bodyFontFamily, const std::string &titleFontFamily, bool titleBold,
                                            int &logicalWidth, int &logicalHeight)
{
    if (hasUnsupportedFeature(top)) return {};

    try {
        constexpr double kPxPerInch = 100.0;
        int logicalW = std::max(200, (int)std::lround(top.figW * kPxPerInch));
        int logicalH = std::max(150, (int)std::lround(top.figH * kPxPerInch));
        logicalWidth = logicalW;
        logicalHeight = logicalH;

        // Render at 2x pixel dimensions and let the <img> tag's
        // width/height attributes (set by the caller from
        // logicalWidth/logicalHeight above) scale it back down for
        // display -- same oversample-for-crispness technique
        // chart_render.cpp's Cairo renderer already uses via
        // cairo_scale(cr, 2.0, 2.0). Confirmed live as a real, visible
        // quality gap: unlike Cairo's uniform scale transform (which
        // scales every subsequent logical-unit drawing call, fonts
        // included, automatically), gnuplot has no equivalent hook we can
        // reach from here, so every point-based size (font_size,
        // line_width, marker_size -- see kOversample near the top of this
        // file) needs multiplying by hand to actually land bigger on the
        // doubled canvas rather than just occupying a smaller fraction of
        // it.
        int w = (int)std::lround(logicalW * kOversample);
        int h = (int)std::lround(logicalH * kOversample);

        auto f = matplot::figure(true);
        f->quiet_mode(true);
        f->size(w, h);

        // charts.py's group_colors: one map for this whole render call,
        // shared across every panel below (see resolveItemColor).
        std::map<std::string, Rgba> groupColors;

        // Same charts.py-derived tokens as chart_render.cpp's Cairo
        // renderer (see its kPaletteLight/Dark comment for the full
        // writeup) -- kept in sync by hand since the two renderers don't
        // share a color-token header.
        Rgba bg = darkMode ? hexRgba(0x14171c) : hexRgba(0xfcfcfb);
        Rgba fg = darkMode ? hexRgba(0xc9d1d9) : hexRgba(0x52514e);
        Rgba headingColor = darkMode ? hexRgba(0xdbe4f0) : hexRgba(0x1a2b3c);
        g_activePalette = darkMode ? kPaletteDark : kPaletteLight;
        g_activePaletteSize = darkMode ? (int)(sizeof(kPaletteDark) / sizeof(kPaletteDark[0])) : (int)(sizeof(kPaletteLight) / sizeof(kPaletteLight[0]));
        f->font_size(15 * kOversample); // figure_type's own font_size (default 10) drives the whole-figure suptitle only; matches chart_render.cpp's Cairo renderer's own top-title size (15)

        // figure_type::run_window_color_command() -- the ONLY code path that
        // fills the outer canvas (the margin outside every axes box, where a
        // title/legend can sit) -- only runs when backend_->output() is
        // EMPTY (figure_type.cpp:105-107: "if (backend_->output().empty())
        // run_window_color_command()"). We always save to a real file, so
        // output() is never empty, so that fill NEVER fires -- confirmed
        // live via a captured gnuplot script: no "set object 1 rectangle"
        // command ever appears, regardless of dark/light mode. This is why
        // every chart shows a plain white margin around the (correctly
        // dark-filled) axes box in dark mode. run_command() itself is
        // protected on figure_type, so it can't be called directly to patch
        // this in; instead, add a dummy axes spanning the WHOLE canvas
        // (position 0,0 to 1,1), created FIRST so every real panel draws on
        // top of it, and give IT the background color -- axes_type::color()
        // already correctly emits the same "set object N rectangle from
        // graph 0,0 to graph 1,1 behind fillcolor ..." pattern (confirmed
        // working for each real panel's own background), so this reproduces
        // the missing canvas-level fill using a mechanism that's already
        // proven to work, without needing the inaccessible protected API.
        //
        // Deliberately do NOT also call f->color(bg): axes_type::run_
        // background_command() skips emitting its rectangle entirely when
        // "color_ == parent()->color()" (axes_type.cpp:812) -- an
        // optimization that assumes the parent figure already painted that
        // color itself. Since figure_type's own fill is dead code (above),
        // giving the figure the SAME bg color as bgAxes made Matplot++
        // silently skip bgAxes's rectangle too -- confirmed live: the
        // captured script had no "set object" for bgAxes at all, and
        // matched "unset object 1" for a rectangle that was never set.
        // Leaving the figure at its default color (white) makes it differ
        // from bgAxes's bg, so the skip condition is false and the
        // rectangle actually gets emitted.
        matplot::axes_handle bgAxes = f->add_axes(std::array<float, 4>{0.0f, 0.0f, 1.0f, 1.0f});
        bgAxes->color(matplotColorArray(bg));
        bgAxes->box(false);
        bgAxes->x_axis().visible(false);
        bgAxes->y_axis().visible(false);
        bgAxes->grid(false);

        std::string bodyFont = bodyFontFamily.empty() ? "sans-serif" : bodyFontFamily;
        std::string titleFont = titleFontFamily.empty() ? bodyFont : titleFontFamily;

        size_t nPanels = top.panels.size();
        for (size_t i = 0; i < nPanels; ++i) {
            const PanelSpec &panel = top.panels[i];
            matplot::axes_handle ax = (nPanels > 1) ? f->add_subplot(nPanels, 1, i) : f->add_axes();
            // axes_type::run_background_command() only emits the "fill
            // this axes rectangle" gnuplot command when color_ != the
            // PARENT figure's color -- and separately,
            // figure_type::run_window_color_command() (the outer canvas
            // fill) only fires when the figure has no title set at all.
            // Every one of our charts has a title, and axes/figure bg
            // were both set to the exact same value, so BOTH background
            // fills were getting silently skipped -- confirmed live: a
            // "dark mode" render came back with a plain white background,
            // identical to light mode. Nudge the axes color by one part
            // in 255 (imperceptible) so it reads as different from the
            // figure color and the axes-level fill actually fires; the
            // thin margin outside the axes box (title/label area) still
            // won't pick up the figure-level fill, a smaller residual gap
            // than "no background color at all".
            Rgba axBg = bg;
            axBg.r = bg.r < 0.5 ? std::min(1.0, bg.r + 1.0 / 255.0) : std::max(0.0, bg.r - 1.0 / 255.0);
            ax->color(matplotColorArray(axBg));
            // NOT matplotColor(fg) (a hex string) -- axis_type::color(
            // std::string_view) goes through the same broken
            // string_to_color() as line_spec's (see matplotColorArray's
            // comment); in dark mode specifically this was rendering the
            // axis spines/ticks solid black on a dark background instead
            // of the intended light ink, since only light mode's
            // near-black fg happened to be close enough to the black
            // fallback to look right by accident.
            ax->x_axis().color(matplotColorArray(fg));
            ax->y_axis().color(matplotColorArray(fg));
            ax->font_size(11 * kOversample);
            // gnuplot's own default has no gridlines at all -- the Cairo
            // renderer always draws subtle ones, so a chart's look
            // depended entirely on which backend happened to draw it.
            ax->grid(true);

            // axes_type::default_axes_position() reserves no top margin at
            // all for a title -- the title text ends up drawn right at the
            // very top of the figure canvas with no breathing room above
            // it (confirmed against a real render: the title's ascenders
            // sat almost flush against the image edge). Same technique as
            // the left-margin widening below (shrink this axes' own box,
            // don't touch sibling panels' positions): reduce height,
            // holding y_origin (the BOTTOM edge, per
            // figure_type's t_margin = 1 - y_origin - height) fixed, so
            // the freed space opens up strictly above this axes.
            if (!panel.title.empty()) {
                std::array<float, 4> pos = ax->position();
                pos[3] *= 0.90f; // 10% of this axes' own height reserved for its title
                ax->position(pos);
            }

            bool soleHeatmap = panel.layers.size() == 1 && panel.layers[0].type == "heatmap";
            bool solePie = panel.layers.size() == 1 && panel.layers[0].type == "pie";
            double colorIdx = 0;
            bool anyLegend = false;
            std::vector<std::pair<matplot::axes_object_handle, std::string>> legendEntries;
            if (soleHeatmap) {
                renderHeatmapPanel(ax, panel.layers[0].j);
            } else if (solePie) {
                renderPiePanel(ax, panel.layers[0].j);
            } else {
                applyLogScale(ax, panel.logX, panel.logY);
                AxisCategoryRegistry xReg, yReg;
                buildCategoryRegistries(panel, xReg, yReg);
                // charts.py's _render_barh defaults invert_yaxis=true (first-
                // listed row at the TOP) -- see chart_render.cpp's identical
                // fix for the full writeup; same logic here, just against
                // AxisCategoryRegistry's double-valued pos map instead of
                // CategoryRegistry's int-valued indexOf.
                bool wantsInvertY = false;
                for (const auto &L : panel.layers)
                    if (L.type == "barh" && L.j.value("invert_yaxis", true)) wantsInvertY = true;
                if (wantsInvertY && !yReg.labels.empty()) {
                    std::reverse(yReg.labels.begin(), yReg.labels.end());
                    yReg.pos.clear();
                    for (size_t i = 0; i < yReg.labels.size(); ++i) yReg.pos[yReg.labels[i]] = (double)i;
                }
                // axes_type::default_axes_position reserves a FIXED 13% of
                // figure width for the left margin, completely oblivious
                // to how wide the actual y-tick label text is -- confirmed
                // live: a real category label ("Iron Beam shot / Tamir
                // interceptor") got clipped mid-word against that fixed
                // margin. Cairo's own renderer measures the actual label
                // text and sizes marginLeft to fit (see its
                // renderCartesianPanel); do the equivalent here by
                // widening the axes' left edge when the longest y
                // category label wouldn't otherwise fit.
                if (!yReg.labels.empty()) {
                    size_t maxLen = 0;
                    for (const auto &lbl : yReg.labels) maxLen = std::max(maxLen, lbl.size());
                    double tickFontPx = 11 * kOversample;
                    // 0.55 * font size wasn't wide enough -- confirmed live,
                    // still clipped a few characters off real category
                    // labels. 0.68 plus more end padding errs toward
                    // reserving too much room (wasted whitespace) rather
                    // than too little (a clipped, broken-looking label).
                    double labelPx = maxLen * tickFontPx * 0.68 + 30 * kOversample;
                    double neededLeftFrac = std::min(0.62, labelPx / w);
                    std::array<float, 4> pos = ax->position();
                    if (neededLeftFrac > pos[0]) {
                        float rightEdge = pos[0] + pos[2];
                        pos[0] = (float)neededLeftFrac;
                        pos[2] = std::max(0.1f, rightEdge - pos[0]);
                        ax->position(pos);
                    }
                }

                Extent ext;
                int axesPxHeight = (int)(h / nPanels); // rough per-panel share, ignoring title/margins -- fine for a line-width approximation
                anyLegend = drawPanelLayers(ax, panel, colorIdx, xReg, yReg, ext, axesPxHeight, groupColors, legendEntries);
                // Pin a categorical axis to an explicit range unconditionally
                // -- confirmed live on a Gantt-style chart with no
                // ref_lines/bands at all: left on gnuplot's own autoscale, a
                // categorical axis gets none of the half-category headroom
                // ax->bar()'s own category axis normally provides, so the
                // first/last row ends up crammed right against the frame
                // edge (row height + zero margin) instead of centered in
                // its own band. ref_lines/ref_bands need the same explicit
                // range for the separate reason in drawRefLines' comment
                // (gnuplot's real autoscale isn't readable back from C++
                // before save()), so both cases share this one block.
                bool needsExplicitLimits = !panel.refLines.empty() || !panel.refBands.empty() ||
                                            !xReg.labels.empty() || !yReg.labels.empty();
                // Computed unconditionally (not just when needsExplicitLimits)
                // so drawAnnotations below always has a real axis range to
                // convert its points-based xytext offset against, whether or
                // not this panel also happens to need ref_lines/bands.
                std::array<double, 2> xlim = ext.hasX() ? paddedLimits(ext.xmin, ext.xmax, panel.logX, !xReg.labels.empty())
                                                         : std::array<double, 2>{0.0, 1.0};
                std::array<double, 2> ylim = ext.hasY() ? paddedLimits(ext.ymin, ext.ymax, panel.logY, !yReg.labels.empty())
                                                         : std::array<double, 2>{0.0, 1.0};
                if (needsExplicitLimits) {
                    ax->x_axis().limits(xlim);
                    ax->y_axis().limits(ylim);
                    // Explicit "nice" ticks (see niceLinearTicks/logDecadeTicks'
                    // comment) -- only for a NUMERIC axis; a categorical one
                    // already got its tick_values()/ticklabels() set from
                    // xReg/yReg.labels at the end of drawPanelLayers, and
                    // overwriting that here would replace real category names
                    // with numbers.
                    if (xReg.labels.empty()) {
                        if (panel.logX) {
                            LogTicks t = logDecadeTicks(xlim[0], xlim[1]);
                            ax->x_axis().tick_values(t.values);
                            ax->x_axis().ticklabels(t.labels);
                        } else {
                            std::vector<double> ticks = niceLinearTicks(xlim[0], xlim[1]);
                            std::vector<std::string> labels;
                            for (double v : ticks) labels.push_back(formatTick(v));
                            ax->x_axis().tick_values(ticks);
                            ax->x_axis().ticklabels(labels);
                        }
                    }
                    if (yReg.labels.empty()) {
                        if (panel.logY) {
                            LogTicks t = logDecadeTicks(ylim[0], ylim[1]);
                            ax->y_axis().tick_values(t.values);
                            ax->y_axis().ticklabels(t.labels);
                        } else {
                            std::vector<double> ticks = niceLinearTicks(ylim[0], ylim[1]);
                            std::vector<std::string> labels;
                            for (double v : ticks) labels.push_back(formatTick(v));
                            ax->y_axis().tick_values(ticks);
                            ax->y_axis().ticklabels(labels);
                        }
                    }
                    drawRefLines(ax, panel, xlim, ylim, axesPxHeight);
                }
                double axesPxWidth = w * (double)ax->position()[2];
                drawAnnotations(ax, panel, xReg, yReg, fg, xlim[1] - xlim[0], ylim[1] - ylim[0], axesPxWidth, axesPxHeight);
            }

            // The invisibility half of this was the color_array
            // alpha-convention bug (fixed above, see title_color's
            // comment) -- confirmed live, the title text now renders.
            //
            // The FONT-SWAP half is still known not to work, and can't be
            // fixed by reordering these calls: axes_type has no separate
            // title-font storage. run_title_command() (axes_type.cpp:75)
            // reads font() -- the SAME single font_ member that tick
            // labels/xlabel/ylabel also read -- live, at save() time
            // (quiet_mode defers all command generation that far), not at
            // the time title()/font() were called here. So whichever
            // ax->font(...) call happens to be LAST before save() wins for
            // every text element on this axes, title included -- there is
            // no way to give the title a different font family than the
            // rest of the axes through this API. Confirmed live via a
            // captured gnuplot script: emitted title command always
            // carried bodyFont ("IBM Plex Sans"), never titleFont ("IBM
            // Plex Serif"), regardless of swap order. A real fix would
            // need to stop using ax->title() entirely and draw the title
            // as a manually-positioned ax->text() object instead (labels
            // have their own independent font()/font_size(), unlike
            // titles) -- deferred, not done here.
            if (!panel.title.empty()) {
                ax->font(titleFont);
                ax->title_font_weight(titleBold ? "bold" : "normal");
                ax->title(escapeGnuplotEnhancedText(panel.title));
                ax->title_color({0.0f, (float)headingColor.r, (float)headingColor.g, (float)headingColor.b}); // alpha=0 is OPAQUE here, not headingColor.a=1.0 -- see the face_colors fix above for the full writeup; this exact bug is why the title never rendered.
                ax->font(bodyFont);
            }
            if (!panel.xlabel.empty()) ax->xlabel(escapeGnuplotEnhancedText(panel.xlabel));
            if (!panel.ylabel.empty()) ax->ylabel(escapeGnuplotEnhancedText(panel.ylabel));
            // ax->legend() with no arguments resolves to axes_type's OWN
            // const getter overload (legend() const -- see axes_type.h),
            // not a setter; it reads back whatever legend_handle already
            // exists (null, since one was never created) and does nothing.
            // matplot::legend(ax) (axes_functions.h) is the real setter --
            // confirmed live via a captured gnuplot script: with the plain
            // getter, every chart came back with a bare "set key off". But
            // matplot::legend(ax) with no names ALSO auto-includes every
            // plotted series regardless of whether it ever got a
            // display_name -- confirmed live again: an unlabeled Gantt
            // connector bar and unlabeled scatter points all showed up as
            // "data1".."data10" placeholder entries. The plain
            // matplot::legend(ax, names) overload isn't right either --
            // tried it, confirmed live: it doesn't pair a name with the
            // handle that was actually given that display_name, so each
            // real label showed up TWICE (once as a generic line swatch,
            // once as the correct point swatch). legend(objs, names) is
            // the one that pairs each name with the EXACT handle
            // display_name() was called on -- collected as legendEntries
            // in drawPanelLayers for exactly this.
            if (anyLegend) {
                std::vector<matplot::axes_object_handle> objs;
                std::vector<std::string> names;
                for (auto &entry : legendEntries) { objs.push_back(entry.first); names.push_back(entry.second); }
                matplot::legend(objs, names);
            }
        }

        // Same font-swap idea as each panel's title, for the whole-figure
        // suptitle -- figure_type has its own separate font_/title_ from
        // axes_type's, untouched by resetting ax->font(bodyFont) on the
        // last panel above. Traced (not run live) through
        // figure_type::run_multiplot_command(), the actual command this
        // renders as ("set multiplot title ... font '<font_>,<size>'") --
        // unlike axes_type::run_title_command(), figure_type::title()
        // itself only stores the string (touch()), so it's the value of
        // font_ at whenever run_multiplot_command() executes during
        // save() that lands, not at this title() call -- fine here since
        // nothing else reads figure_type's font_ before then. One real
        // gap: run_multiplot_command() hardcodes "{/:Bold ...}"
        // unconditionally, so a non-bold top.title isn't expressible
        // through this API short of patching Matplot++ itself -- every
        // real report spec's title is bold anyway, so left as-is rather
        // than a speculative fix for a case that doesn't occur yet.
        if (!top.title.empty()) {
            f->font(titleFont);
            f->title(escapeGnuplotEnhancedText(top.title));
            f->title_color({0.0f, (float)headingColor.r, (float)headingColor.g, (float)headingColor.b}); // same alpha-convention fix as ax->title_color above.
        }

        // save() only writes to a real file (no in-memory buffer API), so
        // round-trip through a temp file, same shape as any other
        // subprocess-based renderer would need.
        std::string tmpPath = "/tmp/markdownview_chart_" + std::to_string((unsigned long)f.get()) + "_" +
                               std::to_string((unsigned long)::getpid()) + ".png";
        bool ok = f->save(tmpPath);
        if (!ok) return {};
        waitForStableFile(tmpPath, 3000);

        std::ifstream in(tmpPath, std::ios::binary);
        if (!in) return {};
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        std::remove(tmpPath.c_str());
        if (bytes.empty()) return {};
        return bytes;
    } catch (const std::exception &) {
        return {};
    }
}

} // namespace MatplotPP
