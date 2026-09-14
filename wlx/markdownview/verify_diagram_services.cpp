// Exercises the three web render paths -- Mermaid via mermaid.ink,
// PlantUML via plantuml.com, Vega-Lite via Kroki -- plus the failure
// contract, and times each so the routing choice stays evidence-backed.
//
// Links only markdownview_core: no toolkit, and no QGuiApplication needed,
// since nothing here touches the LaTeX path. Build:
//
//   g++ -std=c++17 -O1 -o verify_diagram_services verify_diagram_services.cpp \
//     -Isrc/core $(pkg-config --cflags cairo librsvg-2.0) \
//     build_online/libmarkdownview_core.a $(pkg-config --libs cairo librsvg-2.0)
#include "diagram_render.h"
#include "vegalite_spec.h"

#include <chrono>
#include <cstdio>
#include <string>

static int failures = 0;

static void check(const char *what, bool ok, const std::string &detail = {})
{
    printf("%-56s %s%s%s\n", what, ok ? "PASS" : "FAIL",
           detail.empty() ? "" : "  ", detail.c_str());
    if (!ok) ++failures;
}

// Runs fn, reports how long it took alongside the SVG size.
template <typename Fn>
static std::string timed(const char *what, Fn fn)
{
    auto t0 = std::chrono::steady_clock::now();
    std::string svg = fn();
    double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
    char detail[128];
    snprintf(detail, sizeof(detail), "%6.0f ms, %zu bytes", ms, svg.size());
    check(what, !svg.empty() && svg.find("<svg") != std::string::npos, detail);
    return svg;
}

// A real report figure's structure -- layers, a legend with an explicit
// scale domain, a long title and subtitle that must wrap, and the
// underscore-prefixed provenance keys that must NOT reach the endpoint.
static const char *kSpec = R"JSON({
  "$schema": "https://vega.github.io/schema/vega-lite/v5.json",
  "_caption": "Synthetic data. Nothing real.",
  "_src": "verify_diagram_services.cpp",
  "width": 420, "height": 260,
  "title": {
    "text": "A deliberately long figure title that has to wrap onto several lines",
    "subtitle": "And a subtitle that is longer still, so the wrapping path is exercised on both of them at once."
  },
  "data": {"values": [
    {"q": "Q1", "v": 12, "series": "Northern region"},
    {"q": "Q2", "v": 19, "series": "Northern region"},
    {"q": "Q3", "v": 7,  "series": "Northern region"},
    {"q": "Q1", "v": 9,  "series": "Southern region"},
    {"q": "Q2", "v": 14, "series": "Southern region"},
    {"q": "Q3", "v": 22, "series": "Southern region"}
  ]},
  "layer": [
    {"mark": "line", "encoding": {
      "x": {"field": "q", "type": "ordinal"},
      "y": {"field": "v", "type": "quantitative"},
      "color": {"field": "series", "type": "nominal",
                "scale": {"domain": ["Northern region", "Southern region"]}}
    }},
    {"mark": {"type": "point", "filled": true}, "encoding": {
      "x": {"field": "q", "type": "ordinal"},
      "y": {"field": "v", "type": "quantitative"},
      "color": {"field": "series", "type": "nominal"}
    }}
  ]
})JSON";

static const char *kMermaid = "sequenceDiagram\n  Alice->>Bob: Hello\n  Bob-->>Alice: Hi\n";
static const char *kPlantUml = "@startuml\nAlice -> Bob: Hello\nBob --> Alice: Hi\n@enduml\n";

static bool rasterizes(const std::string &svg)
{
    int w = 0, h = 0;
    return !DiagramRender::svgToHighDpiPng(svg, 2.0f, false, w, h).empty() && w > 0 && h > 0;
}

int main(int argc, char **argv)
{
    // Optional overrides: mermaid base, plantuml base, kroki base.
    if (argc > 1) DiagramRender::setMermaidBaseUrl(argv[1]);
    if (argc > 2) DiagramRender::setPlantUmlBaseUrl(argv[2]);
    if (argc > 3) DiagramRender::setKrokiBaseUrl(argv[3]);

    printf("-- spec normalisation (pure, nothing leaves the machine yet)\n");
    std::string normalized = VegaLiteSpec::normalizeSpec(kSpec, false, "sans-serif");
    check("normalizeSpec returns a spec", !normalized.empty());
    check("  provenance keys stripped (_caption)", normalized.find("_caption") == std::string::npos);
    check("  provenance keys stripped (_src)", normalized.find("_src") == std::string::npos);
    check("  caption still readable from the ORIGINAL",
          VegaLiteSpec::captionOf(kSpec) == "Synthetic data. Nothing real.");
    check("  legend positioned explicitly", normalized.find("\"legendX\"") != std::string::npos);
    check("  title wrapped into lines", normalized.find("\"text\":[") != std::string::npos);
    check("  font default filled in", normalized.find("\"font\":\"sans-serif\"") != std::string::npos);
    check("normalizeSpec rejects malformed JSON",
          VegaLiteSpec::normalizeSpec("{not json", false, "sans-serif").empty());

    printf("\n-- live round trips, each to its own service\n");
    std::string mm = timed("mermaid.ink", [] { return DiagramRender::renderMermaidWeb(kMermaid, false); });
    check("  rasterizes after fixMermaidSvgText",
          !mm.empty() && rasterizes(DiagramRender::fixMermaidSvgText(mm, false)));

    std::string pu = timed("plantuml.com", [] { return DiagramRender::renderPlantUmlWeb(kPlantUml, false); });
    check("  rasterizes after fixPlantUmlSvgDark",
          !pu.empty() && rasterizes(DiagramRender::fixPlantUmlSvgDark(pu)));

    std::string vl = timed("kroki (vegalite)", [] { return DiagramRender::renderVegaLiteWeb(kSpec, false); });
    check("  rasterizes through librsvg", !vl.empty() && rasterizes(vl));

    printf("\n-- failure contract: every one must come back empty, so the\n");
    printf("   caller leaves the fenced block as its plain text\n");
    check("malformed vegalite spec -> empty",
          DiagramRender::renderVegaLiteWeb("{nope", false).empty());

    DiagramRender::setMermaidBaseUrl("https://mermaid.invalid.example");
    DiagramRender::setPlantUmlBaseUrl("https://plantuml.invalid.example");
    DiagramRender::setKrokiBaseUrl("https://kroki.invalid.example/");
    check("unreachable mermaid host -> empty",
          DiagramRender::renderMermaidWeb(kMermaid, false).empty());
    check("unreachable plantuml host -> empty",
          DiagramRender::renderPlantUmlWeb(kPlantUml, false).empty());
    check("unreachable kroki host -> empty",
          DiagramRender::renderVegaLiteWeb(kSpec, false).empty());

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASS", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
