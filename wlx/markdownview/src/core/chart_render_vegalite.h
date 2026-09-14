#pragma once

#include "../../3rdparty/nlohmann_json/json.hpp"

#include <string>
#include <vector>
#include <cstdint>

/// The ```vegalite renderer: hands a literal Vega-Lite v5 spec to a
/// self-seeded, vendored `vl-convert` binary (see
/// 3rdparty/vl-convert/README.md), which runs the real Vega-Lite/Vega
/// layout engine and rasterizes it to PNG.
///
/// Figures are authored AS Vega-Lite and rendered verbatim -- there is no
/// intermediate spec vocabulary and nothing translates between one shape
/// and another. Earlier revisions of this plugin did carry such a layer
/// (a matplotlib-derived ```chart spec, rendered first through
/// Matplot++/gnuplot and later translated into Vega-Lite); both are gone.
/// See git history if that path ever needs revisiting.
///
/// A soft dependency: nothing here is required for the plugin to work,
/// only for figures to render. Two ways it can be absent --
///   - built with -DENABLE_VLCONVERT=OFF (see CMakeLists.txt), in which
///     case chart_render_vegalite_stub.cpp is compiled instead of this
///     file's real implementation and isAvailable() always returns false;
///   - built with vl-convert embedded, but self-seeding fails at runtime
///     (read-only filesystem, unsupported architecture, ...).
/// Either way rendering returns empty and the caller's existing
/// empty-result contract leaves the fenced block showing as plain text.
namespace VegaLite {

/// Self-seeds the embedded vl-convert binary to
/// ~/.config/doublecmd/markdownview_vlconvert/ on first call (mirrors the
/// embedded math fonts' own self-seeding) and reports whether a working
/// copy is actually usable. Cheap to call repeatedly; the real work
/// happens once.
bool isAvailable();

/// Whether the real renderer was compiled in at all -- true here, false in
/// chart_render_vegalite_stub.cpp.
///
/// Distinct from isAvailable(), and the distinction matters: isAvailable()
/// is also false in THIS build when self-seeding fails at runtime. The
/// caller picks the local renderer over the Kroki one on isCompiledIn(),
/// so a build that ships vl-convert never silently starts sending specs
/// over the network because seeding happened to fail -- it renders
/// nothing, and the fenced block stays plain text.
bool isCompiledIn();

/// Physical pixels per logical pixel of the display the output is headed
/// for (a HiDPI screen is typically 2.0). Rendered PNGs are produced at
/// this multiple of their logical size while still being DECLARED at
/// logical size in the HTML, so the image maps 1:1 onto device pixels
/// instead of being stretched by the toolkit -- which is what makes a
/// figure look soft or aliased on a HiDPI display even though the PNG
/// itself is clean. Defaults to 1.0; the Qt plugin sets it from the
/// widget's own devicePixelRatioF().
void setDisplayScale(double scale);

/// Renders a Vega-Lite spec to PNG bytes.
///
/// darkMode: the spec's OWN config wins wherever it sets something; this
/// only fills a background/font default in underneath for a spec that
/// didn't specify one, so a light-authored figure set stays exactly as
/// its author drew it.
///
/// Returns empty on any failure (malformed JSON, a vl-convert error, a
/// timeout) -- the caller leaves the fenced block as plain text.
std::vector<uint8_t> renderVegaLiteSpecPng(const std::string &vegaLiteJson, bool darkMode,
                                            const std::string &bodyFontFamily,
                                            int &logicalWidth, int &logicalHeight);

} // namespace VegaLite
