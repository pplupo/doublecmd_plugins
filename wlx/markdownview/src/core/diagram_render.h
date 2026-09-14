#pragma once

#include <string>
#include <vector>
#include <cstdint>

/// Toolkit-neutral helpers for rendering Mermaid/PlantUML/Vega-Lite fenced
/// code blocks: fetch a rendered SVG from a web renderer, patch it up, and
/// rasterize to PNG bytes via librsvg+Cairo. No Qt, no GTK -- used from
/// markdown_engine.cpp regardless of which UI target links it.
///
/// Each notation goes to its own service: Mermaid to mermaid.ink,
/// PlantUML to plantuml.com, and Vega-Lite to Kroki, which is the only one
/// of the three with no comparable single-purpose service. Routing all
/// three through Kroki was tried and reverted -- see the measurements on
/// the service-base globals in diagram_render.cpp. Every base URL is
/// settable, so a self-hosted instance of any of them works the same way,
/// which is what keeps diagram sources inside your own network if that
/// matters.
namespace DiagramRender {

/// Base URLs of the three services. Each defaults to the public instance
/// ("https://mermaid.ink", "http://www.plantuml.com/plantuml",
/// "https://kroki.io") and is overridable from the plugin's ini. A
/// trailing slash is tolerated; an empty string is ignored, keeping
/// whatever was set before.
void setMermaidBaseUrl(const std::string &url);
void setPlantUmlBaseUrl(const std::string &url);
void setKrokiBaseUrl(const std::string &url);

/// Renders Mermaid/PlantUML source to SVG, locally when the Rust
/// renderers are compiled in (see diagram_render_local.h) and via the web
/// service otherwise. This is what markdown_engine.cpp calls; the theme
/// block / skinparams are applied here either way, so both paths style a
/// diagram identically. Returns empty on failure.
///
/// The choice is made on LocalDiagram::isCompiledIn(), not on whether a
/// local render succeeded: a build that ships the offline renderers must
/// never fall back to the network, so a per-diagram failure degrades to
/// plain text instead.
std::string renderMermaid(const std::string &code, bool darkMode);
std::string renderPlantUml(const std::string &code, bool darkMode);

/// The web renderers specifically, bypassing the local ones -- for
/// comparing the two, and for the service harness. Prefer the dispatching
/// pair above in the plugin itself.
///
/// Note the default PlantUML endpoint is plain HTTP, so the diagram source
/// is readable in transit; a self-hosted instance over HTTPS avoids that.
std::string renderMermaidWeb(const std::string &code, bool darkMode);
std::string renderPlantUmlWeb(const std::string &code, bool darkMode);

/// Fetches a rendered SVG for a Vega-Lite spec, by POSTing it to the Kroki
/// base. The spec gets the same VegaLiteSpec::normalizeSpec() treatment the
/// local vl-convert path applies before rendering -- which also strips the
/// underscore-prefixed provenance keys, so that metadata never reaches the
/// endpoint. Returns empty on failure, including on a spec that doesn't
/// parse.
std::string renderVegaLiteWeb(const std::string &vegaLiteJson, bool darkMode);

/// mermaid.js emits <tspan> y/dy in `em` units relative to the parent
/// <text>, which most SVG renderers (including librsvg) don't resolve the
/// way browsers do -- combines them into an absolute pixel y on the <text>
/// element itself, and replaces <foreignObject> HTML-label workarounds
/// with plain <text> elements librsvg can render directly.
std::string fixMermaidSvgText(const std::string &svg, bool darkMode);

/// Recolors PlantUML's default dark strokes/lines for a dark background.
std::string fixPlantUmlSvgDark(const std::string &svg);

/// Rasterizes SVG bytes to PNG bytes via librsvg + Cairo, at `scale`x the
/// SVG's intrinsic size (for a sharper embedded image).
std::vector<uint8_t> svgToHighDpiPng(const std::string &svg, float scale, bool darkMode,
                                      int &logicalWidth, int &logicalHeight);

} // namespace DiagramRender
