#pragma once

#include "chart_spec.h"

#include <string>
#include <vector>
#include <cstdint>

/// The preferred ```chart renderer: Matplot++ driving a self-seeded,
/// vendored, minimal gnuplot binary (2D-only build; see
/// 3rdparty/gnuplot/README.md) -- real matplotlib-grade rendering (proper
/// tick locators, exact colormaps, box/violin statistics) that this
/// plugin's own hand-rolled Cairo renderer (chart_render.cpp's fallback
/// path) can only approximate. A soft dependency throughout: nothing here
/// is ever required for the plugin to work, only for charts to look their
/// best.
namespace MatplotPP {

/// Self-seeds the embedded gnuplot binary to
/// ~/.config/doublecmd/markdownview_gnuplot/ on first call (mirrors the
/// embedded math fonts' self-seeding in markdown_engine.cpp) and reports
/// whether a working gnuplot -- the seeded copy, or a system one found on
/// PATH if seeding somehow fails -- is actually usable. Cheap to call
/// repeatedly; the real work happens once. false means chart_render.cpp's
/// Cairo renderer must be used instead -- this is not an error condition.
bool isAvailable();

/// Renders an already-parsed chart spec via Matplot++. Returns empty on
/// any failure (an unsupported combination, a gnuplot subprocess error, a
/// timeout) -- the caller falls back to the native Cairo renderer, same
/// contract as every other renderer failure in this plugin.
std::vector<uint8_t> renderChartMatplotPng(const ChartSpec::TopSpec &top, bool darkMode,
                                            const std::string &bodyFontFamily, const std::string &titleFontFamily, bool titleBold,
                                            int &logicalWidth, int &logicalHeight);

} // namespace MatplotPP
