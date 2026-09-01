# pdfview

Double Commander WLX plugin with a Thin UI Layer architecture for rendering documents across Qt6 and GTK3 using statically linked libraries.

## Capabilities

Supported Document Formats:
* **PDF, EPUB, MOBI, FB2, XPS, CBZ**: Powered by `libmupdf`.
* **DjVu**: Powered by `libdjvulibre`.

Features:
* **UI-Agnostic Core**: A unified C++ core rasterizes pages and extracts text.
* **Qt6 & GTK3 Wrappers**: Very thin wrappers handle displaying the buffer, input events, and clipboard interactions.
* **Zoom**: Use `Ctrl` + Scroll Wheel or `+` / `-` keys.
* **Text Selection**: Click and drag to highlight and select text.
* **Copy**: `Ctrl + C` copies the selected text to the clipboard.
* **Print**: `Ctrl + P` opens a native print dialog (Qt Print Dialog for Qt plugin, GTK Print Operation for GTK plugin).

## Building

The project relies on static versions of `mupdf` and `djvulibre`. Since Double Commander provides Qt6 and GTK3, these are linked dynamically, but the document libraries are statically linked so the plugin requires no additional runtime dependencies to view these files.

### 1. Build Static Dependencies

Run the dependency build script. This will download and compile the required libraries into `scripts/deps_install/`:
```bash
cd scripts
./build_deps.sh
```

### 2. Build the Plugin

```bash
mkdir build && cd build
cmake ..
cmake --build .
```

The resulting `pdfview_qt.wlx` and `pdfview_gtk.wlx` files can be added to Double Commander as Lister plugins.
