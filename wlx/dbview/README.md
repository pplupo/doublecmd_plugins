# dbview — Multi-Engine Database WLX Plugin

A Qt6 WLX plugin for [Double Commander](https://doublecmd.github.io/) that views and edits database files: **SQLite**, **DuckDB**, **LevelDB**, and optionally **RocksDB**.

Built on the [`wayland_qt_base`](../../plugin-platform/wlx/wayland_qt_base/) platform library.

## Features

### All Engines
- **Find** (Ctrl+F) — search across all visible cells
- **Copy selection** — Ctrl+C copies tab-separated text
- **Row count display** — shows total row count in toolbar
- **Engine label** — toolbar displays which engine is active
- **`GridMode::LiveDatabase`** — no RAM snapshots, zero memory overhead

### SQL Engines (SQLite, DuckDB)
- **Table/View selector** — dropdown lists all tables and views
- **In-place editing** — cells are editable, changes buffered
- **Submit / Revert** (Ctrl+S / Ctrl+Z) — commit or rollback pending changes
- **Lazy loading** — DuckDB uses LIMIT/OFFSET chunks, SQLite uses QSqlTableModel

### KV Engines (LevelDB, RocksDB)
- **Two-column grid** — Key | Value
- **Auto-save** — writes go directly to the database (no buffering)
- **Binary detection** — non-UTF-8 values shown as `[Binary Data - X bytes]`
- **Hex view toggle** — context menu to switch to hex string representation
- **Save value as file** — context menu to export raw binary value
- **Load value from file** — context menu to overwrite a value from a file
- **Sliding window cache** — only ~1000 entries in RAM, re-centered on scroll
- **Directory detection** — selecting a `.sst`/`.ldb` file opens the parent LevelDB/RocksDB directory

## Architecture

```
dbview_qt6.wlx (shared library)
    ├── wlx_entry.cpp           → WLX C interface
    ├── DbViewWidget            → Engine-agnostic UI shell
    │
    ├── DbEngine (abstract)     → Unified backend interface + factory
    │   ├── SqliteEngine        → QSQLITE driver + QSqlTableModel
    │   ├── DuckDbEngine        → DuckDB C++ API + DuckDbModel
    │   ├── LevelDbEngine       → LevelDB C++ API + KeyValueModel
    │   └── RocksDbEngine       → RocksDB C++ API + KeyValueModel [optional]
    │
    ├── DuckDbModel             → QAbstractTableModel (LIMIT/OFFSET chunks)
    ├── KeyValueModel           → QAbstractTableModel (sliding window cache)
    │
    └── wayland_qt_base (static library via submodule)
```

### Adding a New Database Engine

1. Create `include/NewEngine.h` inheriting `DbEngine`
2. Implement `open()`, `close()`, `tableNames()`, `modelForTable()`, etc.
3. Add to the factory in `src/DbEngine.cpp`
4. Add to `CMakeLists.txt` sources and link the database library

## Building

### Default (SQLite + DuckDB + LevelDB)

```bash
cd wlx/dbview
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### With RocksDB (optional, large dependency)

```bash
cmake .. -DENABLE_ROCKSDB=ON
make -j$(nproc)
```

> **Note:** First build takes 5-10 minutes as DuckDB and LevelDB are downloaded
> and compiled from source via CMake FetchContent.

**Requirements:**
- Qt6 (Core, Gui, Widgets, Sql)
- CMake ≥ 3.20
- Git (for FetchContent downloads)
- The `plugin-platform` submodule (`git submodule update --init`)

**Output:** `dbview_qt6.wlx`

## Installation

Copy `dbview_qt6.wlx` to your Double Commander plugins directory and configure the detect string:

```
EXT="DB" | EXT="SQLITE" | EXT="SQLITE3" | EXT="DB3" | EXT="DUCKDB" | EXT="LDB" | EXT="SST"
```

## Keyboard Shortcuts

| Shortcut | Action |
|----------|--------|
| Ctrl+S | Submit changes (SQL engines only) |
| Ctrl+Z | Revert all pending changes (SQL engines only) |
| Ctrl+F | Toggle Find panel |
| Ctrl+C | Copy selection |

## Engine Selection Logic

The factory (`DbEngine::createForFile()`) dispatches by file extension:

| Extension | Engine |
|-----------|--------|
| `.sqlite`, `.sqlite3`, `.db`, `.db3` | SQLite |
| `.duckdb` | DuckDB |
| `.ldb`, `.sst` | LevelDB (checks parent dir for CURRENT file) |
| `.sst` (if LevelDB fails) | RocksDB (when `ENABLE_ROCKSDB=ON`) |
| Unknown | SQLite fallback (tries to open and checks for tables) |

## Future

- Additional database backends (PostgreSQL, MySQL) via QSqlDatabase drivers
- Schema inspection panel (column types, indexes, triggers)
- SQL query editor
- Realm support (deferred)
