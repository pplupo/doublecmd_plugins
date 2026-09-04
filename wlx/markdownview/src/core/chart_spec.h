#pragma once

#include "../../3rdparty/nlohmann_json/json.hpp"

#include <string>
#include <vector>
#include <set>

// The parsed ```chart JSON spec model, shared between both chart renderers
// (chart_render_matplotpp.cpp, the preferred path when the self-seeded
// gnuplot binary is usable; chart_render.cpp's own native-Cairo renderer,
// the fallback) -- parsing the spec is identical either way, only how a
// parsed PanelSpec gets turned into pixels differs. See chart_render.h for
// the overall dispatch and fallback contract.
namespace ChartSpec {

using nlohmann::json;

// One mark within a panel -- either the panel's own top-level fields (no
// "layers" key: the whole panel spec IS the one layer) or one element of
// its "layers" array.
struct Layer {
    json j;
    std::string type;
};

struct RefLine { std::string axis = "y"; double value = 0; std::string label; std::string colorHex; };
struct RefBand { std::string axis = "y"; double low = 0, high = 0; std::string label; std::string colorHex; };
// x/y kept as raw JSON (number or string) rather than resolved doubles --
// resolving a string against a category registry can only happen once a
// panel's registries exist, at render time, not at parse time. Parsing
// used to reject any annotation whose x/y wasn't already numeric, which
// silently dropped every annotation naming a category (e.g. a timeline's
// date pins labelled by the same row name as their barh) -- confirmed
// live against a real report spec.
struct Annotation { json x, y; std::string text; double dx = 0, dy = 10; std::string ha = "center"; double fontsize = 9; };

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

const std::set<std::string> &knownTypes();

std::vector<double> numArray(const json &arr);

// Parses the full spec (raw JSON text, as captured from the fenced code
// block) into a TopSpec. Returns false on anything malformed or missing
// required fields -- callers must treat that as total rendering failure
// (fall back to the fenced block's plain text), same contract every other
// renderer in this plugin follows.
bool parseTopSpec(const std::string &specJson, TopSpec &out);

} // namespace ChartSpec
