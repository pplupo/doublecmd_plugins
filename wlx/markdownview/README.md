# MarkdownView Lister Plugin for Double Commander

A Lister (WLX) plugin for [Double Commander](https://doublecmd.sourceforge.io/) that provides rich, interactive previews of Markdown files (`.md`, `.markdown`, `.mdown`, `.mkd`).

This plugin ships as **two independent native builds** — one for DC's **GTK3** build, one for its **Qt6** build. Both share the same Markdown/LaTeX/diagram/chart rendering core (`src/core/`), the same CSS theming system, and the same context menu, but each links against its host toolkit directly and has some real behavioral differences (see [Feature Differences](#feature-differences-gtk3-vs-qt6) below). Install whichever one matches your Double Commander build — they cannot be mixed.

---

## Features & Capabilities (both variants)

* **High-Fidelity Markdown Rendering**: Full support for CommonMark and GitHub-Flavored Markdown (tables, task lists, strikethrough, blockquotes, code blocks) powered by `md4c`.
* **LaTeX Math Equations**: Renders inline (`$E=mc^2$`) and block (`$$\int_0^\infty f(x) dx$$`) mathematical equations locally using **MicroTeX**, with a choice of **8 embedded OpenType math fonts** (see [Math Font Selection](#math-font-selection) below) — no system font installation required, and no network access.
* **Chart Rendering (` ```chart ` blocks)**: Renders a JSON chart spec into a crisp embedded image, entirely locally via Cairo — no Python, no matplotlib, no network. Supports all **13 mark types** (line, bar, barh, scatter, area, step, stem, errorbar, histogram, boxplot, violin, heatmap, pie), layering multiple marks onto one panel, multi-panel figures, and log-scale/reference-line/reference-band/annotation overlays. See [Chart Rendering](#chart-rendering) below.
* **Mermaid & PlantUML Diagrams**: Fetches and renders Mermaid and PlantUML diagrams into crisp high-DPI images, with a consistent accent color and automatic dark/light mode adaptation.
* **Live Auto-Reload**: Watches opened files for changes and automatically re-renders the document when saved in an external editor.
* **Theme Modes**: **System**, **Dark**, and **Light** rendering modes, switchable from the context menu. Chart/math-title text automatically matches the active CSS's own body/heading fonts.
* **Interactive Navigation**:
  * **Zooming**: `Ctrl` + Mouse Wheel to zoom in/out, with a persistable "Save Zoom" level; LaTeX/diagram/chart images scale along with the surrounding text.
  * **Text Selection & Copy**: Native text selection with context menu and `Ctrl+C` copy support.
  * **Ctrl+Q**: Closes Quick View even when the plugin has input focus.
* **Context Menu**: Copy Text, Select All, Find in Document, Print, Save/Reset Zoom, Reload Document, Auto-Reload on Save (toggle), Theme Mode (System/Dark/Light), Math Font (Default + every available embedded/custom font).

---

## Math Font Selection

LaTeX math is rendered locally by MicroTeX using real OpenType MATH-table fonts (not the old glyph-resource-pack approach). Eight fonts are embedded directly into the compiled `.wlx` binary and self-seeded on first use to:

```path
~/.config/doublecmd/markdownview_fonts/
```

Available fonts: **Latin Modern Math**, **IBM Plex Math**, **STIX Two Math**, **Libertinus Math**, **Fira Math**, **DejaVu Math TeX Gyre**, **TeX Gyre Pagella Math**, **Euler Math**. Pick one from the context menu's **Math Font** submenu — the choice persists to `markdownview.ini`'s `math_font` key and survives restarts.

**Using your own font**: drop a matching `.otf`/`.clm1` pair into `markdownview_fonts/` (the `.clm1` is MicroTeX's own font-metrics format, generated from an `.otf` via its `otf2clm.py` conversion script) and it shows up in the Math Font menu automatically, no rebuild needed. A selector that fails to resolve to a valid font (missing file, wrong path, not a valid math font) silently falls back to the default font rather than failing the render.

---

## Chart Rendering

A ` ```chart ` fenced code block contains a JSON spec describing one or more charts, rendered locally with Cairo. The spec shape mirrors the `reports` repo's own `charts.py` chart engine (matplotlib-backed there, native-Cairo here), so specs are portable between the two.

**Simple example:**

````markdown
```chart
{"type": "bar", "x": ["Q1", "Q2", "Q3", "Q4"], "y": [3, 7, 4, 9], "title": "Quarterly Sales"}
```
````

**Supported `type` values**: `line`, `bar`, `barh`, `scatter`, `area`, `step`, `stem`, `errorbar`, `histogram`, `boxplot`, `violin`, `heatmap`, `pie`.

**Common fields**: `title`, `xlabel`, `ylabel`, `figsize` (`[width, height]` in inches, default `[6, 4]`). `x` is either all-numeric (a real numeric axis) or all-string (categorical positions, used as tick labels). A single unlabeled series is `y: [...]`; multiple labeled series sharing the same `x` are `series: [{y, label, marker}, ...]` (adds a legend automatically).

**`bar`-specific**: multiple series draw grouped (side-by-side) by default; `"stacked": true` stacks them, `"stacked": "percent"` makes a 100%-stacked chart (every bar the same height, showing each series' share) — the `"percent"` mode is a plugin-specific addition beyond `charts.py`'s own plain boolean.

**Layering** — combine several mark types on one panel's shared axes:

````markdown
```chart
{"layers": [
  {"type": "barh", "bars": [{"y": 0, "width": 4, "left": 1}]},
  {"type": "scatter", "points": [{"x": 1, "y": 0, "label": "Start"}, {"x": 5, "y": 0, "label": "End"}]}
], "title": "Dumbbell"}
```
````

**Multi-panel** — stack several panels vertically in one figure via top-level `"panels": [...]` (each element is its own single-type or `"layers"` panel spec), with `"shared_x": true|false` (default `true`, hides x tick labels on all but the last panel).

**Cross-cutting fields** (any panel): `log_x`/`log_y` (bool), `ref_lines` (`[{axis, value, label, style: {color}}]`), `ref_bands` (`[{axis, low, high, label, style: {color}}]`), `annotations` (`[{x, y, text, xytext, ha, fontsize}]`).

Malformed JSON or an unsupported type falls back to the block's plain text rather than breaking the render.

---

## Configuration (`markdownview.ini`)

Both variants store their settings the same way and in the same location — the directory Double Commander itself hands the plugin at load time (its `DefaultIniName`), typically:

```path
~/.config/doublecmd/plugins/wlx/markdownview.ini
```

```ini
[markdownview]
# Path to a custom CSS stylesheet file. Takes precedence over default plugin CSS.
theme_file_path=

# Theme rendering mode: system | dark | light (default: system)
mode=system

# Live auto-reload on file save: true | false (default: true)
auto_reload=true

# Persisted "Save Zoom" font-size multiplier (default: 1.0)
zoom_multiplier=1.0

# Selected LaTeX math font, as a .clm1 file path (see Math Font Selection
# below) -- empty uses the default font (Latin Modern Math).
math_font=
```

## CSS Styling & Customization

A single stylesheet covers both light and dark mode via `body.theme-light` / `body.theme-dark` class selectors on the rendered `<body>` — there is no separate `-dark`-suffixed file. This is deliberate: WebKitGTK (the GTK3 variant's renderer) supports `@media (prefers-color-scheme)`, but Qt's `QTextBrowser` (the Qt6 variant's renderer, a rich-text document view rather than a browser engine) does not support media queries at all, so a plain class selector is the one theming mechanism both toolkits honor identically.

### CSS Lookup Order

1. **`theme_file_path`** setting in `markdownview.ini`, if set and the file exists.
2. **`markdownview.css`** next to the ini file (same `DefaultIniName` directory), e.g. `~/.config/doublecmd/plugins/wlx/markdownview.css`.
3. **`~/.config/markdownpart.css`** — a lower-precedence fallback, useful for a theme shared across other plugins/tools.
4. **Built-in default**, compiled into the binary. If nothing above is found, this default is also written out to `markdownview.css` (step 2's location) so there's always a real, editable file going forward.

---

## Feature Differences (GTK3 vs Qt6)

| | GTK3 | Qt6 |
|---|---|---|
| Rendering engine | WebKitGTK (`webkit2gtk-4.1`) — a full browser engine | `QTextBrowser` (`QTextDocument`) — a rich-text document view, not a browser engine |
| CSS support | Full CSS as implemented by WebKit, including `@media` queries (unused by this plugin, see above) | A limited CSS2.1-ish subset; no `@media` support, and class-selector inheritance through elements is not as reliable as a real browser's — some rules need higher specificity or `!important` to consistently win |
| In-document search (`ListSearchText`) | **Not implemented** — DC's native in-viewer search does nothing in this plugin | Implemented |
| LaTeX rendering backend | MicroTeX's Cairo/Pango backend (`src/gtk3/latex_render_cairo.cpp`) | MicroTeX's Qt backend (`src/qt6/latex_render_qt.cpp`) — same underlying MicroTeX layout engine, different rasterizer |
| Zoom | `webkit_web_view_set/get_zoom_level()`, driven by `GDK_SCROLL_SMOOTH` delta events | `QTextBrowser::zoomIn/zoomOut`, driven by `QWheelEvent` |
| Link/runtime dependency | `libwebkit2gtk-4.1`, `cairomm-1.0`, `pangomm-1.4`, `fontconfig`, `freetype2` | Qt6 Core/Gui/Widgets only |

---

## Building and Installation

### Prerequisites (both variants)
* CMake 3.16+
* C++17 compiler (`g++` or `clang++`)
* `cairo`, `librsvg-2.0` (used by the shared toolkit-neutral core for diagram rasterization and chart rendering, regardless of which plugin target you build)

No system font files are required for LaTeX math rendering — MicroTeX (vendored from upstream's `openmath` branch, `3rdparty/MicroTeX/`) uses real OpenType MATH-table fonts, and all 8 supported fonts are embedded directly into the compiled binary (see [Math Font Selection](#math-font-selection)). The chart JSON spec is parsed with a vendored `nlohmann/json` single header (`3rdparty/nlohmann_json/`) — no separate install needed.

### GTK3 variant

Additional prerequisites: GTK3 (`gtk+-3.0`), `webkit2gtk-4.1`, `cairomm-1.0`, `pangomm-1.4`, `fontconfig`, `freetype2` development packages.

```bash
cd wlx/markdownview
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DENABLE_QT6=OFF -DENABLE_GTK3=ON ..
make -j$(nproc)
```

Output: `markdownview_gtk3.wlx`

### Qt6 variant

Additional prerequisites: Qt6 Development Libraries (`Qt6Core`, `Qt6Gui`, `Qt6Widgets`).

```bash
cd wlx/markdownview
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DENABLE_QT6=ON -DENABLE_GTK3=OFF ..
make -j$(nproc)
```

Output: `markdownview_qt6.wlx`

Both targets are enabled by default (`ENABLE_QT6=ON ENABLE_GTK3=ON`); a plain `cmake .. && make` builds whichever toolkits are actually found on the system, skipping the other with a CMake warning if its dependencies are missing.

### Installation

Copy the binary matching your Double Commander build to its plugin directory:

```bash
mkdir -p ~/.config/doublecmd/plugins/wlx
cp markdownview_gtk3.wlx ~/.config/doublecmd/plugins/wlx/   # GTK3 build of DC
# or
cp markdownview_qt6.wlx ~/.config/doublecmd/plugins/wlx/    # Qt6 build of DC
```

Register the plugin in Double Commander under **Options -> Plugins -> WLX (Lister Plugins)**, pointing to whichever `.wlx` file matches your DC build.

---

## License

MIT License.
