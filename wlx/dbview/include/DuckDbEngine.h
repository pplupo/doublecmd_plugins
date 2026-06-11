#pragma once

#include "DbEngine.h"
#include <memory>

namespace duckdb {
class DuckDB;
class Connection;
}

/// DuckDB engine: opens DuckDB database files via the native C++ API.
///
/// Uses a custom DuckDbModel (QAbstractTableModel) instead of QSqlTableModel
/// since there's no Qt SQL driver for DuckDB.
///
/// Supports multiple tables, submit/revert via BEGIN/COMMIT/ROLLBACK.
class DuckDbEngine : public DbEngine {
    Q_OBJECT
public:
    explicit DuckDbEngine(QObject *parent = nullptr);
    ~DuckDbEngine() override;

    bool open(const QString &filepath) override;
    void close() override;

    QStringList tableNames() const override;
    QStringList viewNames() const override;
    QAbstractItemModel *modelForTable(const QString &tableName) override;
    QString currentTableName() const override;

    bool supportsMultipleTables() const override { return true; }
    bool supportsSubmitRevert() const override { return true; }
    QString engineName() const override { return QStringLiteral("DuckDB"); }

    bool submitAll() override;
    bool revertAll() override;
    QString lastError() const override;

private:
    std::unique_ptr<duckdb::DuckDB> m_duckdb;
    std::unique_ptr<duckdb::Connection> m_conn;
    QString m_currentTable;
    QString m_lastError;
    QAbstractItemModel *m_currentModel = nullptr;
    bool m_inTransaction = false;
};
