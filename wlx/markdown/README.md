# WLX Markdown Lister Plugin for Double Commander

A high-performance Lister (WLX) plugin for [Double Commander](https://doublecmd.sourceforge.io/) built with **Qt6** that provides rich, interactive previews of Markdown files (`.md`, `.markdown`, `.mdown`, `.mkd`).

---

## Features & Capabilities

* **High-Fidelity Markdown Rendering**: Full support for CommonMark and GitHub-Flavored Markdown (tables, task lists, strikethrough, blockquotes, code blocks) powered by `md4qt`.
* **LaTeX Math Equations**: Renders inline (`$E=mc^2$`) and block (`$$\int_0^\infty f(x) dx$$`) mathematical equations locally using **MicroTeX**.
* **Mermaid & PlantUML Diagrams**: Automatically fetches and renders Mermaid and PlantUML diagrams into crisp high-DPI images with automatic dark/light mode color adaptation.
* **Live Auto-Reload**: Watches opened files for changes and automatically re-renders the document when saved in an external editor.
* **Theme Modes**: Seamlessly matches your desktop theme with **System**, **Dark**, and **Light** modes.
* **Interactive Navigation**:
  * **Zooming**: `Ctrl` + Mouse Wheel to zoom text and images in/out.
  * **Text Selection & Copy**: Native text selection with context menu and `Ctrl+C` copy support.
  * **In-Document Search**: Native text search support.

---

## Configuration (`wlx_markdown.ini`)

The plugin configuration is stored at:
```path
~/.config/doublecmd/plugins/wlx/wlx_markdown.ini
```

### Supported Settings

```ini
[wlx_markdown]
# Path to a custom CSS stylesheet file. Takes precedence over default plugin CSS.
theme_file_path=

# Theme rendering mode: system | dark | light (default: system)
mode=system

# Live auto-reload on file save: true | false (default: true)
auto_reload=true
```

---

## CSS Styling & Customization

### Plugin CSS Location
The default stylesheet is located alongside the plugin at:
```path
~/.config/doublecmd/plugins/wlx/wlx_markdown.css
```

### CSS Lookup Order
When loading stylesheet rules, the plugin uses a strict 4-step precedence lookup order:

1. **`theme_file_path`** setting specified in `wlx_markdown.ini` (if defined and the target file exists).
2. **Plugin Directory CSS**: `~/.config/doublecmd/plugins/wlx/wlx_markdown.css`.
3. **`markdownpart.css` Fallback**: `~/.config/doublecmd/plugins/wlx/markdownpart.css` or `~/.config/markdownpart.css`.
4. **Binary Embedded Fallback**: Built-in C++ string constants compiled directly into the binary.

---

## Context Menu Actions

Right-clicking inside the document viewer opens a context menu with options to:
* **Copy Text**: Copy selected text to clipboard.
* **Select All**: Select all text in the document.
* **Reload Document**: Manually refresh the rendered Markdown view.
* **Auto-Reload on Save**: Toggle automatic file watching.
* **Theme Mode**: Quickly switch between **System**, **Dark**, and **Light** rendering modes.

---

## Building and Installation

### Prerequisites
* CMake 3.16+
* C++17 Compiler (`g++` or `clang++`)
* Qt6 Development Libraries (`Qt6Widgets`, `Qt6Svg`, `Qt6Network`, `Qt6PrintSupport`)
* `clatexmath` font files (for MicroTeX LaTeX rendering, e.g. `/usr/share/clatexmath`)

### Build Steps

```bash
cd wlx/markdown
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

### Installation

Copy the built `.wlx` plugin binary and stylesheet to Double Commander's plugin directory:

```bash
mkdir -p ~/.config/doublecmd/plugins/wlx
cp wlx_markdown_qt6.wlx ~/.config/doublecmd/plugins/wlx/
cp ../wlx_markdown.css ~/.config/doublecmd/plugins/wlx/
```

Register the plugin in Double Commander under **Options -> Plugins -> WLX (Lister Plugins)** by pointing to `wlx_markdown_qt6.wlx`.

---

## License

MIT License.
