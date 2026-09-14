#pragma once

#include <string>

/// The parts of ```vegalite handling that are pure JSON-in/JSON-out, with
/// no renderer attached: reading a spec's inline caption, and applying this
/// plugin's layout treatment (theme defaults, title/subtitle wrapping,
/// legend placement) to it.
///
/// Split out of chart_render_vegalite.cpp so it survives
/// -DENABLE_VLCONVERT=OFF. That build compiles the stub instead of the real
/// renderer, but still needs both of these: it renders figures by POSTing
/// the spec to Kroki (see DiagramRender::renderVegaLiteWeb), and a spec
/// must get the same treatment before being sent as it would before being
/// handed to a local vl-convert -- otherwise the two editions lay the same
/// figure out differently.
namespace VegaLiteSpec {

/// The figure caption carried inline in a spec's "_caption" key -- the
/// methodology/caveats/source line that belongs UNDER the image, distinct
/// from the spec's own "title", which is a headline drawn INSIDE it.
/// reportgen.py already promotes this to a visible caption paragraph in
/// the PDF (via the image's alt text); returning it here lets the
/// on-screen preview show the same text, instead of a reader seeing bare
/// numbered markers whose key exists but is invisible. Empty when the
/// spec has none, or when it doesn't parse.
std::string captionOf(const std::string &vegaLiteJson);

/// Applies the layout treatment and returns the spec to actually render:
/// strips the underscore-prefixed provenance keys, fills theme defaults in
/// UNDERNEATH whatever the spec already sets, wraps an over-long title or
/// subtitle, and places the legends.
///
/// darkMode picks the default background; bodyFontFamily the default font
/// -- both only for a spec that didn't set its own, so a light-authored
/// figure set stays exactly as its author drew it.
///
/// Returns empty on malformed JSON, which every caller treats the same way
/// a render failure is treated: leave the fenced block as plain text.
std::string normalizeSpec(const std::string &vegaLiteJson, bool darkMode,
                          const std::string &bodyFontFamily);

} // namespace VegaLiteSpec
