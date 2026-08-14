#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>

/// Toolkit-neutral database browsing/editing core, sitting on top of the
/// same raw C/C++ database APIs the existing Qt engines already talk to
/// directly (vendored sqlite3, duckdb, lmdb, Berkeley DB, Firebird's
/// classic API, libmdb, and optionally RocksDB/LevelDB). No Qt, no GTK --
/// the UI layer (src/gtk3/) just calls into this for table lists, column
/// metadata, and row data.
///
/// Unlike the Qt QAbstractTableModel classes (which lazily/incrementally
/// fetch rows for scalability), this loads a table's full result set into
/// memory on selectTable()/selectQuery(), same as most of those Qt models
/// already effectively did (SqliteTableModel, KeyValueModel's window is
/// the exception) -- a deliberate simplification for the GTK port.

struct DbColumnInfo {
    std::string name;
    std::string type;
    bool isPrimaryKey = false;
    bool isForeignKey = false;
};

class DbEngineCore {
public:
    virtual ~DbEngineCore() = default;

    virtual bool open(const std::string &filepath) = 0;
    virtual void close() = 0;

    virtual std::vector<std::string> tableNames() const = 0;
    virtual std::vector<std::string> viewNames() const { return {}; }
    virtual std::vector<DbColumnInfo> columnInfos(const std::string &tableName) const { return {}; }
    virtual std::vector<std::string> indexes(const std::string &tableName) const { return {}; }

    /// Loads a table's (or keyspace's) full row set. Returns false on error
    /// (see lastError()).
    virtual bool selectTable(const std::string &tableName) = 0;
    /// Runs a custom query (SQL engines only) and loads its result set.
    virtual bool selectQuery(const std::string &query) { return false; }

    virtual int rowCount() const = 0;
    virtual int columnCount() const = 0;
    virtual std::string columnName(int col) const = 0;

    /// Display text for a cell -- "NULL" for SQL NULL, "[Binary Data - N
    /// bytes]" for BLOB/binary cells (matches the Qt side's placeholder).
    virtual std::string cellText(int row, int col) const = 0;
    virtual bool cellIsBinary(int row, int col) const { return false; }
    virtual std::vector<uint8_t> cellRawBytes(int row, int col) const { return {}; }

    /// Whether a cell is editable at all (false for query results, KV
    /// keys, and read-only engines like MS Access).
    virtual bool cellEditable(int row, int col) const { return false; }
    /// Writes a new value immediately (mirrors the Qt side's per-cell
    /// commit-inside-open-transaction pattern). Returns false on error.
    virtual bool setCellText(int row, int col, const std::string &text) { return false; }

    virtual std::string currentTableName() const = 0;
    virtual bool supportsMultipleTables() const = 0;
    virtual bool supportsSubmitRevert() const = 0;
    virtual bool supportsSqlConsole() const { return false; }
    virtual std::string engineName() const = 0;

    virtual bool submitAll() { return false; }
    virtual bool revertAll() { return false; }

    virtual std::string lastError() const { return {}; }

    bool isReadOnly() const { return m_readOnly; }

    /// Factory: probes the file/directory and returns the right engine
    /// (same detection order as the Qt side's DbEngine::createForFile).
    static std::unique_ptr<DbEngineCore> createForFile(const std::string &filepath);

protected:
    bool m_readOnly = false;
};
