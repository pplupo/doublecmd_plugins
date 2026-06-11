# structview — Unified Structured Text Plugin

A Qt6 WLX plugin for [Double Commander](https://doublecmd.github.io/) that views and edits structured text files: **JSON**, **XML**, and **INI**.

Built on the [`wayland_qt_base`](../../plugin-platform/wlx/wayland_qt_base/) platform library.

## Features

- **JSON**: Flattens top-level arrays of objects into a grid. Columns = union of all keys. Nested values shown as compact JSON. Full roundtrip serialization preserving types.
- **XML**: Auto-detects repeating child elements as rows. Attributes shown as `@attr` columns. Non-tabular XML falls back to Name/Value layout.
- **INI**: Section navigation list on the left, 2-column Key/Value grid on the right. Sections switch without losing edits.
- **Find & Replace** with scope filtering (All Cells, Current Column, Current Row)
- **Full undo/redo** (Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y) via `GridMode::MemoryDocument`
- **Save** (Ctrl+S) writes back to the original file
- **Word wrap** and **grid lines** toggles
- **Encoding detection** via enca — auto-converts non-UTF-8 files

## Architecture

```
structview_qt6.wlx (shared library)
    ├── wlx_entry.cpp          → WLX C interface (ListLoad, etc.)
    ├── StructViewWidget        → Main widget assembly
    ├── TextFormatEngine        → Abstract parser base + factory
    │   ├── JsonEngine          → QJsonDocument
    │   ├── XmlEngine           → QDomDocument
    │   └── IniEngine           → QSettings (section-navigated)
    └── wayland_qt_base (static library via submodule)
        ├── FocusManager
        ├── EditableGridWidget  (GridMode::MemoryDocument)
        ├── PluginToolBar
        ├── ScopedFindReplacePanel
        └── EncodingUtils
```

### Adding a New Format

1. Create `src/NewFormatEngine.cpp` with a class inheriting `TextFormatEngine`
2. Implement `loadInto()`, `serialize()`, `formatName()`
3. Add a factory function `createNewFormatEngine()` and wire it in the factory switch in `IniEngine.cpp`
4. Add the source file to `CMakeLists.txt`

## Building

```bash
cd wlx/structview
mkdir build && cd build
cmake ..
make -j$(nproc)
```

**Requirements:**
- Qt6 (Core, Gui, Widgets, Xml)
- CMake ≥ 3.20
- The `plugin-platform` submodule (auto-fetched via `git submodule update --init`)

**Output:** `structview_qt6.wlx`

## Installation

Copy `structview_qt6.wlx` to your Double Commander plugins directory and configure the detect string:

```
EXT="JSON" | EXT="XML" | EXT="INI"
```

## Keyboard Shortcuts

| Shortcut | Action |
|----------|--------|
| Ctrl+S | Save file |
| Ctrl+Z | Undo |
| Ctrl+Shift+Z / Ctrl+Y | Redo |
| Ctrl+F | Toggle Find/Replace panel |
| Ctrl+C | Copy selection |
| Ctrl+V | Paste |

## Future

- **CBOR** engine (stubbed — architecture supports drop-in addition)
- Additional structured text formats
