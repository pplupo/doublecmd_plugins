# Vendored vl-convert (Vega-Lite -> PNG)

`vl_convert_linux_x86_64` is the unmodified upstream `vl-convert` v1.9.0
Linux x86-64 release binary (https://github.com/vega/vl-convert), which
renders a Vega-Lite JSON chart spec to a PNG by running the actual
Vega-Lite/Vega JS libraries in an embedded Deno/V8 runtime, then
rasterizing the resulting SVG via `resvg`. This is what makes chart
rendering (legends, log-axis tick locators, floating bars, category
ordering, KDE density transforms, ...) actually correct without this
plugin reimplementing any of Vega-Lite's layout engine by hand -- the
approach the previous Matplot++/gnuplot renderer took, and the whole
reason it kept accumulating structural, unfixable-through-the-public-API
bugs (see git history on the `markdownview-matplotpp` branch / the
`matplot` tag for the full account: broken multi-series legends, notched
"fake filled rectangle" bars, an invisible-title alpha convention bug,
etc).

Downloaded directly from the release, not built from source: it's Rust
(via `cargo`/Deno), not C++, so there's no "trimmed 2D-only build" step
like gnuplot's -- the release binary already only depends on
`libz.so.1`/`libgcc_s.so.1`/`libm.so.6`/`libc.so.6` (confirmed via `ldd`),
all of which are guaranteed present on any Linux DC runs on, so it needs
no bundled runtime libraries of its own the way the old gnuplot vendor did.

Self-seeded to `~/.config/doublecmd/markdownview_vlconvert/vl-convert` on
first chart render that needs it (same pattern the embedded math fonts and
the old gnuplot binary used) -- embedded into the compiled `.wlx` via
`ld -r -b binary` (see CMakeLists.txt's `ENABLE_VLCONVERT` option), NOT
CMake's own hex-array `embed_binary_resource()` helper used for the small
font/gnuplot binaries: that helper's `string(REGEX REPLACE ...)` step
measured at under a second for gnuplot's ~1.5MB, but didn't finish inside
60 seconds on this binary's ~84MB (CMake's string ops don't scale to that
size). `ld -r -b binary` embeds the raw bytes directly as an object file
in under 100ms, at any size, with no text intermediate.

`ENABLE_VLCONVERT` (CMakeLists.txt, default ON) controls whether this
binary gets embedded at all -- when OFF, the plugin builds without it and
every chart render always uses the native Cairo fallback (chart_render.cpp)
instead. Vega-Lite rendering is a soft dependency either way, exactly like
gnuplot was: `VegaLite::isAvailable()` self-seeds and reports whether a
working copy is actually usable, and `renderChartVegaLitePng()` can still
fail per-chart -- either case falls through to Cairo, chart rendering
never hard-fails because this wasn't available.

License: Apache-2.0 (see `LICENSE` in this directory, copied from the
release archive) -- unmodified upstream binary, redistributed as-is.

Source: https://github.com/vega/vl-convert/releases/tag/v1.9.0
