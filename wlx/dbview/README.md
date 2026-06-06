# dbview — Transactional Database WLX Plugin

A Qt6 WLX plugin for [Double Commander](https://doublecmd.github.io/) that views and edits SQLite database files.

Built on the [`wayland_qt_base`](../../plugin-platform/wlx/wayland_qt_base/) platform library.

## Features

- **Table/View selector** — dropdown lists all tables and views in the database
- **Lazy loading** — uses `QSqlTableModel` which fetches rows on demand (no OOM on large files)
- **In-place editing** — cells are editable, changes buffered via `OnManualSubmit` strategy
- **Submit / Revert** — toolbar buttons to commit or rollback pending changes
- **Find** (Ctrl+F) — search across all cells (no replace — database integrity)
- **Copy selection** — Ctrl+C copies tab-separated text
- **Row count display** — shows total row count in toolbar
- **No RAM snapshots** — `GridMode::LiveDatabase` bypasses undo stack for sort/insert/delete
- **Per-widget connections** — unique `QSqlDatabase` connection name per instance avoids collisions

## Architecture

```
dbview_qt6.wlx (shared library)
    ├── wlx_entry.cpp        → WLX C interface (ListLoad, etc.)
    ├── DbViewWidget          → Main widget assembly
    ├── SqliteBackend          → QSqlDatabase + QSqlTableModel lifecycle
    └── wayland_qt_base (static library via submodule)
        ├── FocusManager
        ├── EditableGridWidget (GridMode::LiveDatabase)
        ├── PluginToolBar
        └── FindReplacePanel (no replace)
```

### Why LiveDatabase mode?

Traditional undo/redo (like in the structview plugin) snapshots the entire table state before and after each mutation. On a 1GB SQLite file with millions of rows, this would crash the application. `GridMode::LiveDatabase` delegates mutations directly to the model (`QSqlTableModel::insertRows`, `model->sort()` → SQL `ORDER BY`) with zero RAM overhead.

## Building

```bash
cd wlx/dbview
mkdir build && cd build
cmake ..
make -j$(nproc)
```

**Requirements:**
- Qt6 (Core, Gui, Widgets, Sql)
- CMake ≥ 3.20
- The `plugin-platform` submodule (`git submodule update --init`)
- Qt6 QSQLITE driver (usually included with Qt)

**Output:** `dbview_qt6.wlx`

## Installation

Copy `dbview_qt6.wlx` to your Double Commander plugins directory and configure the detect string:

```
EXT="DB" | EXT="SQLITE" | EXT="SQLITE3" | EXT="DB3"
```

## Keyboard Shortcuts

| Shortcut | Action |
|----------|--------|
| Ctrl+S | Submit changes |
| Ctrl+Z | Revert all pending changes |
| Ctrl+F | Toggle Find panel |
| Ctrl+C | Copy selection |

## Future

- Support for additional database backends (PostgreSQL, MySQL) via `QSqlDatabase` drivers
- Schema inspection panel (column types, indexes, triggers)
- SQL query editor
