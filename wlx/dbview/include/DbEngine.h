#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <memory>

class QAbstractItemModel;

/// Abstract backend interface for all database engines.
///
/// Provides a polymorphic API that DbViewWidget programs against,
/// decoupling the UI from any specific database technology.
///
/// SQL engines (SQLite, DuckDB) return QSqlTableModel or custom models
/// with multiple tables. KV engines (LevelDB, RocksDB) return a two-column
/// KeyValueModel with a single "table".
class DbEngine : public QObject {
    Q_OBJECT
public:
    explicit DbEngine(QObject *parent = nullptr) : QObject(parent) {}
    ~DbEngine() override = default;

    /// Open a database file (or directory for KV stores).
    virtual bool open(const QString &filepath) = 0;

    /// Close the database and release resources.
    virtual void close() = 0;

    /// List of table/keyspace names. KV stores return {"<keys>"}.
    virtual QStringList tableNames() const = 0;

    /// List of view names (SQL engines only).
    virtual QStringList viewNames() const { return {}; }

    /// Get a model for the given table. Ownership stays with the engine.
    /// The model is replaced when a different table is selected.
    virtual QAbstractItemModel *modelForTable(const QString &tableName) = 0;

    /// Currently selected table name.
    virtual QString currentTableName() const = 0;

    /// Whether the engine has multiple switchable tables.
    virtual bool supportsMultipleTables() const = 0;

    /// Whether the engine buffers writes until explicit submit.
    /// SQL engines: true (OnManualSubmit). KV engines: false (direct writes).
    virtual bool supportsSubmitRevert() const = 0;

    /// Human-readable engine name for display in the toolbar.
    virtual QString engineName() const = 0;

    /// Submit buffered writes to the database. SQL engines only.
    virtual bool submitAll() { return false; }

    /// Revert all pending changes. SQL engines only.
    virtual bool revertAll() { return false; }

    /// Last error message from the engine.
    virtual QString lastError() const { return {}; }

    /// Factory: pick the right engine based on file extension / content.
    static std::unique_ptr<DbEngine> createForFile(const QString &filepath);
};
