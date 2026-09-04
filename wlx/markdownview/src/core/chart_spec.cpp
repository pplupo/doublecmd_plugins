#include "chart_spec.h"

namespace ChartSpec {

const std::set<std::string> &knownTypes() {
    static const std::set<std::string> kKnownTypes = {
        "line", "bar", "barh", "scatter", "area", "step", "stem", "errorbar",
        "histogram", "boxplot", "violin", "heatmap", "pie"
    };
    return kKnownTypes;
}

std::vector<double> numArray(const json &arr) {
    std::vector<double> r;
    if (arr.is_array()) for (const auto &v : arr) if (v.is_number()) r.push_back(v.get<double>());
    return r;
}

namespace {

// stacked:"percent" (a plugin-specific addition beyond charts.py's own bar
// spec, which only has a bool) rescales each category's series to sum to
// 100 -- done once here, in place on the layer's own JSON, so every
// renderer's extent/draw code never needs to know percent vs plain
// stacking, only the resulting bool "stacked". Returns true if this layer
// was percent-mode (so the caller can set the panel's yIsPercent flag for
// the "%" y-tick suffix).
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
        if (!knownTypes().count(t)) return false;
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
                !(an["x"].is_number() || an["x"].is_string()) || !(an["y"].is_number() || an["y"].is_string())) continue;
            Annotation a;
            a.x = an["x"];
            a.y = an["y"];
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

} // namespace

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

} // namespace ChartSpec
