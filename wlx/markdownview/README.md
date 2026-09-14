# MarkdownView Lister Plugin for Double Commander

A Lister (WLX) plugin for [Double Commander](https://doublecmd.sourceforge.io/) that provides rich, interactive previews of Markdown files (`.md`, `.markdown`, `.mdown`, `.mkd`).

The plugin varies along **two independent axes**:

* **Toolkit variant** — a **GTK3** build and a **Qt6** build. Both share the same Markdown/LaTeX/diagram rendering core (`src/core/`), the same CSS theming system and the same context menu, but each links against its host toolkit directly and has some real behavioural differences (see [Feature Differences](#feature-differences-gtk3-vs-qt6)). Install whichever matches your Double Commander build — they cannot be mixed.
* **Edition** — **light**, **vlcharts** or **offline**, differing in how much rendering happens on your own machine (see [Editions](#editions)).

So a download is one of six combinations, e.g. the vlcharts edition's Qt6 variant.

---

## Editions

All editions are built from identical sources; they differ only in two CMake options, `ENABLE_VLCONVERT` and `ENABLE_LOCAL_DIAGRAMS`.

| | `markdownview` (light) | `markdownview-vlcharts` | `markdownview-offline` |
|---|---|---|---|
| Approx. size | **~10 MB** | **~94 MB** | **~103 MB** |
| Markdown, LaTeX math, math fonts, theming, zoom, footnotes, find, print | Local | Local | Local |
| ` ```vegalite ` figures | via `kroki_url` | **Local** | **Local** |
| ` ```mermaid ` | via `mermaid_url` | via `mermaid_url` | **Local** |
| ` ```plantuml ` | via `plantuml_url` | via `plantuml_url` | **Local** |
| Needs network for diagrams | Yes | Yes | **No** |

The size gaps are the two vendored renderers: `vl-convert` (~84 MB, which carries its own JavaScript runtime to run the real Vega-Lite layout engine) and the supramark Mermaid/PlantUML archives (~14 MB linked). Every edition applies the *same* layout treatment to a Vega-Lite spec and the same theming to a diagram — the difference is only *where* the rendering happens, and so whether your content leaves the machine.

The **offline** edition is the one to pick if diagram sources are sensitive or you work disconnected: nothing it renders reaches the network at all. It renders Mermaid with a reimplementation rather than mermaid.js, so read [known Mermaid fidelity gaps](#offline-edition-known-mermaid-fidelity-gaps) before choosing it — sequence diagrams in particular.

The editions are **alternatives, not co-installable**: the `.wlx` files are deliberately named identically in all of them, so switching means replacing the file. Settings carry over — they read the same ini and the same self-seeded CSS, so your theme, zoom and math-font choices survive the swap.

---

## Features & Capabilities (all editions)

* **High-Fidelity Markdown Rendering**: Full support for CommonMark and GitHub-Flavored Markdown (tables, task lists, strikethrough, blockquotes, code blocks) powered by `md4c`.
* **LaTeX Math Equations**: Renders inline (`$E=mc^2$`) and block (`$$\int_0^\infty f(x) dx$$`) mathematical equations locally using **MicroTeX**, with a choice of **8 embedded OpenType math fonts** (see [Math Font Selection](#math-font-selection) below) — no system font installation required, and no network access.
* **Figure Rendering (` ```vegalite ` blocks)**: Renders a literal **Vega-Lite v5** spec into a crisp embedded image — no Python involved. The vlcharts edition renders it locally with a vendored `vl-convert`; the light edition posts it to `kroki_url`. See [Figure Rendering](#figure-rendering) below.
* **Mermaid & PlantUML Diagrams** — *requires network access*: Renders Mermaid and PlantUML diagrams into crisp high-DPI images, with a consistent accent color and automatic dark/light mode adaptation. **These are never rendered locally in either edition** — the diagram source is sent to `mermaid_url` / `plantuml_url`, which default to public services. See [Diagram Rendering and Privacy](#diagram-rendering-and-privacy).
* **Live Auto-Reload**: Watches opened files for changes and automatically re-renders the document when saved in an external editor.
* **Theme Modes**: **System**, **Dark**, and **Light** rendering modes, switchable from the context menu.
* **Interactive Navigation**:
  * **Zooming**: `Ctrl` + Mouse Wheel to zoom in/out, with a persistable "Save Zoom" level; LaTeX, diagram and figure images scale along with the surrounding text.
  * **Text Selection & Copy**: Native text selection with context menu and `Ctrl+C` copy support.
  * **Ctrl+Q**: Closes Quick View even when the plugin has input focus.
* **Footnotes**: Reference-style footnotes render as proper linked definitions, including definitions split across multiple blocks.
* **Printing**: Prints the document as displayed, honouring the active theme.
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

## Figure Rendering

A ` ```vegalite ` fenced block contains a literal **Vega-Lite v5** spec.

In the **light edition** the spec is posted to `kroki_url` — see [Diagram Rendering and Privacy](#diagram-rendering-and-privacy). In the **vlcharts edition** it is rendered locally by a vendored [`vl-convert`](https://github.com/vega/vl-convert) binary embedded in the plugin and self-seeded on first use to:

```path
~/.config/doublecmd/markdownview_vlconvert/
```

That binary runs the real Vega-Lite/Vega layout engine, so tick locators, scales, legends, faceting and stacking behave exactly as they would in any other Vega-Lite renderer. Nothing is reimplemented and no network access is involved.

**Example:**

````markdown
```vegalite
{"$schema": "https://vega.github.io/schema/vega-lite/v5.json",
 "data": {"values": [{"q": "Q1", "v": 3}, {"q": "Q2", "v": 7}]},
 "mark": "bar",
 "encoding": {"x": {"field": "q", "type": "nominal"},
              "y": {"field": "v", "type": "quantitative"}}}
```
````

Specs are rendered **as written** — the full Vega-Lite vocabulary is available, including explicit scale domains (the only way to make an intentionally *empty* category render), per-datum opacity, `labelExpr` tick relabelling, temporal axes and faceting.

### What the plugin adds around the spec

Vega-Lite leaves some layout decisions to the caller. These are applied at render time, identically in every edition — the light edition applies them to the spec *before* posting it — so the same figure lays out the same way whichever renderer draws it. A spec that already sets any of them keeps its own value.

* **Title and subtitle wrapping.** Vega-Lite never wraps title text, so a long subtitle stretches the canvas and strands the plot to its left. Both are wrapped to the plot width.
* **Legend layout.** Bottom legends are left-aligned to the plot and flow along one line, which can push the canvas far wider than the plot needs. Legends are placed one per row, sharing a single left edge; a legend too wide for a row stacks its entries into a column instead.
* **Centred title**, over the figure rather than the page. This one deliberately overrides a spec's own `anchor`.
* **HiDPI output.** Figures are rendered at the display's device pixel ratio and downscaled with a proper filter, so they map 1:1 onto device pixels instead of being stretched by the toolkit. *(vlcharts edition; the light edition rasterizes Kroki's SVG at a fixed 2×.)*
* **Theme defaults** for background and font are filled in *underneath* a spec's own `config`, never over it — a light-themed figure set keeps rendering light.

### Captions and inline metadata

Top-level keys beginning with `_` are stripped before the spec reaches Vega-Lite, so a figure can carry its own provenance inline rather than in a sidecar file. `_caption` is rendered as a caption beneath the figure, wrapped to the image's width:

````markdown
```vegalite
{"_caption": "Reported figures, 2022-2026. Log scale.",
 "_src": "Where this number came from",
 "mark": "bar", "...": "..."}
```
````

Malformed JSON, an unsupported construction, a renderer that isn't available, or a network failure falls back to the block's plain text rather than breaking the document — as does `chart_renderer=off`.

---

## Diagram Rendering and Privacy

Markdown and LaTeX math are always rendered locally, in-process. Diagram blocks are rendered either locally or by sending the block's source to a web service and rasterizing the returned SVG locally (librsvg + Cairo) — which one depends on the edition. Each notation has its own service, and **every endpoint is configurable**:

| Block | light | vlcharts | offline | Endpoint setting |
|---|---|---|---|---|
| ` ```mermaid ` | `mermaid.ink` | `mermaid.ink` | **local** | `mermaid_url` |
| ` ```plantuml ` / ` ```puml ` | `plantuml.com` — **HTTP, not encrypted** | same | **local** | `plantuml_url` |
| ` ```vegalite ` | [Kroki](https://kroki.io) | **local** | **local** | `kroki_url` |
| `$…$` / `$$…$$` math | local | local | local | — |

**The offline edition sends nothing at all** — the endpoint settings are still read, but never used, because an edition that ships a local renderer never falls back to the network. Verified at syscall level: with every endpoint pointed at an unresolvable host, all eight test diagrams still render and the plugin spawns no `curl` and opens no socket. The rest of this section applies to the other two.

> **One exception, in every edition.** Vega-Lite specs can name their own data source. A spec whose `data` is `{"url": "https://…"}` makes the *renderer* fetch that URL — confirmed live, an offline-edition build connects out to load it. That is Vega-Lite's data model, not a fallback the plugin controls. Specs with inline `data.values` (what this plugin's own figures use) are fully local.

Kroki is used for Vega-Lite only, because it is the one notation here with no comparable single-purpose service. Routing Mermaid and PlantUML through it as well was tried and reverted — see [HANDOVER.md](HANDOVER.md) for the measurements.

Two consequences worth being deliberate about:

* **Confidentiality.** The text of the diagram — which may name internal systems, people or architecture — is transmitted to a third party you do not control. The default PlantUML endpoint is plain HTTP, so it is also readable in transit. Underscore-prefixed provenance keys (`_caption`, `_src`, …) are stripped from a Vega-Lite spec before it is sent.
* **Availability.** These blocks do not render offline, and are subject to those services' uptime and rate limits. A failure of any kind — no network, a rejected diagram, a timeout — leaves the block as its plain text rather than breaking the document.

Each is **individually toggleable from the context menu**, and disabling one leaves its blocks as plain text. If you preview documents containing anything sensitive, use the offline edition, turn the notations off, or point the endpoints at your own servers, which all three settings exist for:

```ini
mermaid_url=http://mermaid.internal
plantuml_url=https://plantuml.internal/plantuml
kroki_url=http://localhost:8000
```

A self-hosted [Kroki](https://kroki.io) container in fact speaks all three notations, so one container can back all three settings if you prefer a single service — note that its Mermaid companion was observed returning HTTP 500 on the *public* instance (2026-09-08), which is part of why Mermaid is not routed through it by default.

### Offline edition: known Mermaid fidelity gaps

The offline edition renders Mermaid with [`mermaid-little`](https://github.com/Actrium/supramark), a Rust reimplementation rather than the real mermaid.js. It is close, but **not** a drop-in replacement for every diagram type. Upstream measures it at ~90.4% byte-exact against mermaid.js reference output (1200/1328 tests, 22 of 25 diagram types exact), with two types well below that:

| Diagram type | Byte-exact vs mermaid.js | What that means for you |
|---|---|---|
| **Sequence diagrams** | **51/150** | The most-used type with the largest gap. They render, but spacing and label placement can differ visibly from what `mermaid.ink` produces. |
| **Mindmaps** | **7/25** | Rarely used here, but expect noticeable layout differences. |

Also **not implemented at all** — these are explicit upstream non-goals, so a diagram relying on one either renders without that feature or fails and shows as plain text:

* KaTeX/LaTeX formulas inside Mermaid diagrams
* Hand-drawn (`look: handDrawn`) styling
* ELK layout engine
* Architecture diagrams

Byte-exactness is a stricter bar than "looks right" — a one-pixel difference counts as a failure — so these numbers understate how usable the output is in practice. But if a sequence diagram matters and looks wrong, that is the known cause: switch to the light or vlcharts edition (which use `mermaid.ink`) for that document, or turn `enable_mermaid` off and read the source.

**PlantUML has no such caveat.** `plantuml-little` is byte-exact with Java PlantUML v1.2026.2 across all 337 reference tests and 29 diagram types, omitting only DITAA and JCCKIT.

---

## Configuration (`markdownview.ini`)

All editions and toolkit variants store their settings the same way and in the same location — the directory Double Commander itself hands the plugin at load time (its `DefaultIniName`), typically:

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

# Figure rendering: "off" leaves ```vegalite blocks as plain text, any
# other value renders them. Rendered locally in the vlcharts edition and
# via kroki_url in the light edition. Also settable from the context menu.
#
# The key name is legacy -- it once chose between chart BACKENDS, so an
# existing ini holding "cairo" or "auto" still reads as "render". There is
# only one figure renderer now, so this is effectively on/off.
chart_renderer=on

# Per-notation rendering, all also settable from the context menu.
enable_mermaid=true
enable_plantuml=true
enable_latex=true

# Where each notation is rendered. Point any of these at a self-hosted
# instance to keep diagram sources inside your own network -- see "Diagram
# Rendering and Privacy" above. kroki_url is unused in the vlcharts
# edition, whose figures never leave the machine.
mermaid_url=https://mermaid.ink
plantuml_url=http://www.plantuml.com/plantuml
kroki_url=https://kroki.io
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

### Prerequisites (all variants)
* CMake 3.16+
* C++17 compiler (`g++` or `clang++`)
* `cairo`, `librsvg-2.0` (used by the shared toolkit-neutral core for diagram rasterization and image downscaling, regardless of which plugin target you build)

No system font files are required for LaTeX math rendering — MicroTeX (vendored from upstream's `openmath` branch, `3rdparty/MicroTeX/`) uses real OpenType MATH-table fonts, and all 8 supported fonts are embedded directly into the compiled binary (see [Math Font Selection](#math-font-selection)). Vega-Lite specs are parsed with a vendored `nlohmann/json` single header (`3rdparty/nlohmann_json/`) — no separate install needed.

### Choosing an edition

Two independent options select the edition, and both apply to both toolkit variants:

```bash
cmake -DENABLE_VLCONVERT=ON  -DENABLE_LOCAL_DIAGRAMS=OFF ..  # vlcharts (~94 MB)
cmake -DENABLE_VLCONVERT=OFF -DENABLE_LOCAL_DIAGRAMS=OFF ..  # light    (~10 MB)
cmake -DENABLE_VLCONVERT=ON  -DENABLE_LOCAL_DIAGRAMS=ON  ..  # offline  (~103 MB)
```

* **`ENABLE_VLCONVERT`** (default `ON`) embeds `3rdparty/vl-convert/` for local figure rendering. With `OFF`, a stub is compiled in its place and ` ```vegalite ` blocks are posted to `kroki_url` instead.
* **`ENABLE_LOCAL_DIAGRAMS`** (default `OFF`) statically links `3rdparty/supramark/libmarkdownview_diagrams.a` so Mermaid and PlantUML render in-process rather than at `mermaid_url` / `plantuml_url`. See [that directory's README](3rdparty/supramark/README.md) for how the archive is built — it is not produced by this CMake build.

The choice is made on whether the local renderer was **compiled in**, never on whether a given render succeeded: an edition that ships a local renderer will show a block as plain text rather than quietly falling back to the network. Every other feature is unaffected by either option.

`build.sh` builds all three editions into separate release directories, producing one download each. The offline one is skipped with a message if `3rdparty/supramark/libmarkdownview_diagrams.a` is not present, since this build cannot produce it.

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

The two editions use the same `.wlx` filenames, so switching between them is a matter of replacing the file and restarting Double Commander — no re-registration, and settings are preserved.

---

## License

MIT License.
