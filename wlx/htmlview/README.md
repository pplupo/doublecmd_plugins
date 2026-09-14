# htmlview

A Double Commander lister plugin that renders HTML with a real browser
engine, locked down so a preview pane cannot execute scripts or reach the
network unless you say so. Two editions from one core: QtWebEngine for Qt
builds of DC, WebKitGTK for GTK builds.

## Why not QTextBrowser

The common approach to an HTML lister is one line of `QTextBrowser`. That
widget renders Qt's rich-text subset — roughly HTML 4 plus a slice of
CSS 2.1. No JavaScript, no flexbox, no grid, no `position`, no web fonts,
no forms, no SVG, limited tables, and remote resources silently absent.
Claiming `EXT="HTML"` with that behind it means every HTML preview in the
file manager becomes a degraded rendering of anything written after about
2005.

A real engine is the honest answer, and the usual objection to the Qt one
— that `QWebEngineView` cannot be constructed from a plugin loaded after
the host's `QApplication` exists — turns out not to hold. Verified on
Qt 6.11 with a harness that reproduces what DC does (`QApplication`,
`dlopen`, `ListLoad`, reparent into a pane): no
`Qt::AA_ShareOpenGLContexts` fatal, correct rendering. The GTK edition was
checked the same way, against a `GtkLayout` parent because that is what
DC's `ResizeWindow` requires.

## What it does

- **Renders modern HTML.** Grid, flexbox, `rowspan` tables, SVG, web
  fonts — whatever the underlying engine supports. Both editions were
  screenshotted side by side on the same test document.
- **Runs nothing by default.** JavaScript is off, along with WebGL,
  developer extras, HTML5 storage, autoplay and clipboard access. The
  browsing context is off-the-record (Qt) / ephemeral (GTK): no cookie
  jar, no cache, no local storage outliving the preview.
- **Blocks the network by default.** Every request passes a gate that
  denies anything off the machine until you allow it, with a notice bar
  telling you how much was blocked and offering the toggle. Qt uses a
  `QWebEngineUrlRequestInterceptor`; GTK uses WebKit's
  `resource-load-started`, which hands over the request pre-dispatch and
  allows it to be repointed at `about:blank`. Verified against a live
  local HTTP server that a blocked request leaves the server log empty.
- **Confines local reads to the document's own directory.** A preview
  pane has no business reading `~/.ssh` through an `<img src>`. The gate
  enforces this; no engine setting in either toolkit does.
- **Gets the encoding right.** The charset is resolved before the engine
  sees the file — BOM, then `<?xml encoding>`, then `<meta charset>` /
  `http-equiv`, then a UTF-8 validity check, then a configurable
  fallback — and the document is transcoded and handed over as text.
  An undeclared legacy page renders as written instead of as mojibake.
- **Declines what it cannot render.** A binary named `.html`, a file with
  no markup, an empty file, or one past the size cap produces a null
  handle, so DC falls through to the next lister or its own viewer. This
  is the fallback path a plugin that always succeeds makes unreachable.
- **Dark mode.** A `color-scheme` hint by default, with an opt-in
  inversion filter for pages that hardcode a white background.
- **Lister integration.** `ListLoadNext` reuses the window when stepping
  through files, `ListSearchText` maps DC's search flags onto the engine's
  find, `ListPrint` prints through the engine, `ListSendCommand` handles
  copy / select-all / focus.

## Configuration

Settings live in `htmlview.ini`, next to `doublecmd.ini`; the file is
seeded on first run so every knob is discoverable without reading source.
Most are also on the context menu.

| Key | Default | Meaning |
|---|---|---|
| `theme` | `system` | `system`, `dark` or `light` |
| `force_dark` | `false` | Inversion filter for pages that hardcode light |
| `allow_scripts` | `false` | Enable JavaScript |
| `allow_remote_content` | `false` | Allow requests off the machine |
| `zoom_factor` | `1.0` | Persisted zoom |
| `max_file_size` | `33554432` | Decline anything larger |
| `fallback_encoding` | `windows-1252` | Used only when the document declares nothing and is not valid UTF-8 |
| `detect_string` | html/htm/xhtml/xht/mht/mhtml | What the plugin claims |

`detect_string` is deliberately narrow and configurable. Installing a
lister should not silently hijack every HTML-ish preview in the file
manager.

## Building

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
```

Produces `build/htmlview_qt6.wlx` (needs Qt 6.2+ with
`Qt6WebEngineWidgets`) and `build/htmlview_gtk3.wlx` (needs `gtk+-3.0` and
`webkit2gtk-4.1`). Each target is skipped with a warning when its toolkit
is missing; `-DENABLE_QT6=OFF` / `-DENABLE_GTK3=OFF` skip one on purpose.
Install the edition matching your DC build — a single self-contained file,
like every other plugin here.

`build/test_html_probe` covers the accept/decline decision and charset
resolution: every case there is one where returning a window handle
unconditionally would put an empty pane in front of the user.

```bash
./build/test_html_probe
```

## Layout

```
src/core/    toolkit-agnostic probe: sniffing, charset, iconv transcode
src/qt6/     QtWebEngine backend and the WLX entry points
src/gtk3/    WebKitGTK backend and the WLX entry points
tests/       probe checkpoints
```

Both backends share `src/core` and one `htmlview.ini` -- the GTK edition
reads and writes QSettings' IniFormat quoting so a file written by either
is valid for the other.

## Known limits

- `lc_setpercent` returns an error: scrolling to a position needs script
  execution, which is what this plugin refuses to do by default.
- `lcs_wholewords` has no counterpart in either engine's find API; the
  search runs as a substring match rather than reporting a false miss.
- Documents past ~900KB skip the decode-and-inject path (Qt's `setHtml`
  cannot carry them; the ceiling is shared so both editions behave alike)
  and load as a `file://` URL, so the theme prelude and "View Source" do
  not apply to them. The charset is then the engine's guess.
- `lcp_wraptext`, `lcp_fittowindow`, `lcp_ansi` and friends are accepted
  and ignored. They are text-viewer concepts; wrapping and codepage are
  decided by the document's CSS and by the charset resolved at load.
- GTK only: every reload -- `ListLoadNext`, a theme change, a menu toggle
  -- is deferred to the next main-loop idle rather than run inline. This
  is not politeness. Calling `webkit_web_view_load_html()` while a load is
  still settling crashes WebKit, and doing the work inside a signal
  handler's own emission crashes DC; both are documented from live
  backtraces in this repo's other WebKit plugin. A search issued in the
  same tick as `ListLoadNext` therefore still searches the outgoing
  document.
