#pragma once

#include <string>

/// Offline Mermaid and PlantUML rendering, via supramark's `mermaid-little`
/// and `plantuml-little` statically linked as Rust archives (see
/// 3rdparty/supramark/README.md). Source in, SVG out -- the same contract
/// DiagramRender's web renderers have, so the two are interchangeable at
/// the call site.
///
/// A soft dependency, exactly like vl-convert: built with
/// -DENABLE_LOCAL_DIAGRAMS=OFF, diagram_render_local_stub.cpp is compiled
/// instead of the real implementation, isCompiledIn() returns false, and
/// the caller uses the web renderers.
namespace LocalDiagram {

/// Whether the Rust renderers were compiled in at all -- true in the real
/// implementation, false in the stub.
///
/// The caller picks local over web on THIS, so a build that ships the
/// renderers never falls back to the network: rendering that reaches a
/// third party is the property the offline edition exists to not have, and
/// a per-render failure must degrade to plain text rather than to a
/// silent web request.
bool isCompiledIn();

/// Renders Mermaid source (already carrying this plugin's `%%{init}%%`
/// theme block) to SVG. Empty on any failure.
///
/// The result is mermaid.js-shaped -- `width="100%"`, no height, a viewBox
/// -- so it still needs DiagramRender::fixMermaidSvgText() and
/// svgToHighDpiPng()'s viewBox fallback, just like mermaid.ink's output.
std::string renderMermaid(const std::string &themedSource);

/// Renders PlantUML source (already carrying this plugin's skinparams) to
/// SVG. Empty on any failure.
std::string renderPlantUml(const std::string &themedSource);

} // namespace LocalDiagram
