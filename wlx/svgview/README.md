# svgview

`svgview` is a fast and lightweight WLX plugin for Double Commander to display Scalable Vector Graphics (SVG) files.
It relies on a shared core logic utilizing `librsvg` and `cairo` and provides wrappers for both GTK3 and Qt6 to ensure seamless integration with the respective Double Commander builds.

## Features

- **High-Quality Rendering:** Uses modern `librsvg` and `cairo` to render vector graphics.
- **Intrinsic Size Extraction:** Gets the native dimensions of the SVG (or falls back to viewBox/default geometries).
- **Zooming:** Zoom in/out via `+`/`-`, `Ctrl++`/`Ctrl+-`, or `Ctrl + Mouse Scroll`.
- **Panning:** Pan via Scrollbars or Click & Drag (Left Mouse Button).
- **Transparency Checkerboard:** Provides a classic checkerboard background to make SVG transparencies visible.
- **Export to PNG:** Context menu (Right Click) or `Ctrl + S` allows exporting the current SVG to a transparent PNG at its native dimensions and current zoom level.

## Shared Architecture

To prevent duplicated efforts, the plugin is separated into:
- **`svg_core`**: C/C++ backend doing all the heavy lifting (`librsvg` parsing, `cairo` surface generation, checkerboard rendering, offset calculations).
- **`wlx_gtk`**: Thin GTK3 UI layer parsing Double Commander's events.
- **`wlx_qt`**: Thin Qt6 UI layer parsing Double Commander's events.

## Build Requirements

- `cmake` >= 3.16
- `pkg-config`
- `librsvg-2.0` >= 2.52
- `cairo`
- `Qt6` (Core, Gui, Widgets)
- `GTK3`

## Compilation

```bash
mkdir build && cd build
cmake ..
make
```

The resulting `svgview_qt6.wlx` and `svgview_gtk.wlx` can be installed in Double Commander via `Configuration -> Options -> Plugins -> Plugins WLX`.
