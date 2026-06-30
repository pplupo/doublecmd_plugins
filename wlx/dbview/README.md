# dbview — Multi-Engine Database WLX Plugin

![dbview screenshot](dbview.png)

A Qt6 WLX (Lister) plugin for [Double Commander](https://doublecmd.github.io/) that views and edits database files: **SQLite**, **DuckDB**, **LevelDB**, **RocksDB**, **LMDB**, **Berkeley DB**, **Firebird Embedded**, **MS Access**, and **Apache Parquet**.

Built on the [`wlxbase_wlqt`](../wlxbase_wlqt/) platform library.

> [!WARNING]  
> **DATA MUTATION & LOCKING WARNING**  
> 
> By default, this plugin attempts to open databases in **read-write** mode to allow direct grid editing and data mutation.
> - **File Locking:** Opening a database with read-write privileges may lock the file, preventing other applications from writing to it.
> - **Concurrent Access Fallback:** If the database file is locked by another process, the plugin will silently fall back to **read-only** mode.
> - **Buffered Edits:** All changes are buffered in memory and must be explicitly committed using the **Commit** button (`Ctrl+S` or `Ctrl+Shift+Z`). Uncommitted changes can be discarded with the **Revert** button (`Ctrl+Z`). Handle write mode with care to prevent unintended database modifications.

---

## Features

### All Engines
- **Schema Navigation Tree:** Hierarchical tree panel on the left displaying Tables, Views, Columns (with data types, Primary/Foreign keys), and Indexes.
- **Find** (`Ctrl+F`) — search across all visible cells in the selected table.
- **Copy Selection** (`Ctrl+C`) — copy selected cell values as tab-separated values.
- **Word Wrap & Grid Lines** — toolbar actions to toggle wrapping and gridlines.
- **`GridMode::LiveDatabase`** — direct table mapping with minimal memory overhead.
- **Commit / Revert** (`Ctrl+S` / `Ctrl+Z` or `Ctrl+Shift+Z`) — commit or discard pending changes. Available on all writable engines.

### SQL Engines (SQLite, DuckDB, Firebird Embedded, Apache Parquet)
- **SQL Console:** A vertical split panel containing a query editor (with execution via `Ctrl+Return` or `Execute` button), results grid, and CSV/TSV results exporter.
- **Apache Parquet Proxying:** Opening a `.parquet`/`.pq` file initializes an in-memory DuckDB database and reads it via a virtual `read_parquet` view, making it SQL-queryable.
- **In-place Grid Editing:** Cells are editable, with modifications buffered until committed.

### Key-Value & Non-Relational Engines (LevelDB, RocksDB, LMDB, Berkeley DB, MS Access)
- **Hidden SQL Console:** The SQL Console panel is automatically hidden as these engines do not support custom SQL.
- **Two-Column Grid:** Displays Key and Value columns.
- **Buffered Editing:** Value edits are buffered in memory until explicitly committed, consistent with SQL engines.
- **Binary/BLOB Value Detection:** Non-UTF-8 values and large binaries display placeholder information `[Binary Data - X bytes]`.
- **Right-Click Context Menu Options:**
  - **Hex View Toggle:** Displays binary data as space-separated hex strings.
  - **BLOB Export ("Save Cell to File"):** Save raw binary values to any local file.
  - **BLOB Import ("Load File into Cell"):** Import binary files into a cell (available in write mode only).
- **Directory Detection (LevelDB/RocksDB):** Selecting a `.sst`/`.ldb`/`.log` file inside a LevelDB/RocksDB directory automatically targets the parent database directory.

---

## Engine Capabilities

| Engine | Extensions | Writable | Commit/Revert | SQL Console | Notes |
|--------|-----------|:--------:|:-------------:|:-----------:|-------|
| SQLite | `.sqlite`, `.sqlite3`, `.db`, `.db3` | ✅ | ✅ | ✅ | QSQLITE driver |
| DuckDB | `.duckdb` | ✅ | ✅ | ✅ | Native C++ API |
| Apache Parquet | `.parquet`, `.pq` | ✅ | ✅ | ✅ | Via DuckDB `read_parquet()` |
| Firebird Embedded | `.fdb` | ✅ | ✅ | ✅ | QIBASE driver |
| RocksDB | `.sst` (with `CURRENT`) | ✅ | ✅ | — | Requires `ENABLE_ROCKSDB_LEVELDB` |
| LevelDB | `.ldb` (with `CURRENT`) | ✅ | ✅ | — | Via RocksDB API, bg threads disabled |
| LMDB | `.lmdb`, `data.mdb` | ✅ | ✅ | — | C API |
| Berkeley DB | `.bdb` | ✅ | ✅ | — | C API, B-Tree cursors |
| MS Access | `.mdb`, `.accdb` | ❌ | — | — | Read-only (`libmdb`) |

*All writable engines fall back to read-only if the database is locked by another process or lacks write permissions.*

---

## Keyboard Shortcuts

| Shortcut | Action |
|----------|--------|
| `Ctrl+S` | Commit changes |
| `Ctrl+Z` | Revert pending changes |
| `Ctrl+Shift+Z` | Alternative Commit/Redo shortcut |
| `Ctrl+Return` | Execute custom SQL query inside the SQL Console |
| `Ctrl+F` | Toggle Find panel |
| `Ctrl+C` | Copy selection |

---

## Building

### Prerequisites
- Qt6 (Core, Gui, Widgets, Sql)
- CMake ≥ 3.20
- Git (for FetchContent dependencies)
- Libraries: `liblmdb`, `libdb` (Berkeley DB), `libfbclient` (Firebird client)

### Compile
```bash
cd wlx/dbview
mkdir build && cd build
cmake ..
make -j$(nproc)
```

To enable support for **LevelDB** and **RocksDB**, you must compile with the `ENABLE_ROCKSDB_LEVELDB` flag set to `ON`. This flag is disabled by default because linking the RocksDB libraries increases the final `dbview_qt6.wlx` plugin size by over 12 MB.

```bash
cmake .. -DENABLE_ROCKSDB_LEVELDB=ON
make -j$(nproc)
```

**Output:** `dbview_qt6.wlx` (shared library)

---

## Installation

Add `dbview_qt6.wlx` to your Double Commander plugins list.

**Default Detection String:**
```
EXT="DB" | EXT="SQLITE" | EXT="SQLITE3" | EXT="DB3" | EXT="DUCKDB" | EXT="LMDB" | EXT="BDB" | EXT="FDB" | EXT="MDB" | EXT="ACCDB" | EXT="PARQUET" | EXT="PQ"
```

**If compiled with `ENABLE_ROCKSDB_LEVELDB=ON`:**
Include the extensions for LevelDB and RocksDB:
```
EXT="DB" | EXT="SQLITE" | EXT="SQLITE3" | EXT="DB3" | EXT="DUCKDB" | EXT="LDB" | EXT="SST" | EXT="LOG" | EXT="LMDB" | EXT="BDB" | EXT="FDB" | EXT="MDB" | EXT="ACCDB" | EXT="PARQUET" | EXT="PQ"
```
