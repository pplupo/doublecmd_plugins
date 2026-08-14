#pragma once

#include "DbEngine.h"

class FirebirdTableModel;

/// Firebird engine: talks to Firebird directly via the classic isc_ C API
/// (see src/FirebirdTableModel.cpp), not through Qt's QSqlDatabase/QIBASE
/// driver plugin as before. Removes a runtime dependency on the Qt6 SQL
/// module and its driver plugins for Firebird files.
///
/// isc_db_handle/isc_tr_handle are FB_API_HANDLE (an integer type), stored
/// here as opaque unsigned longs so this header doesn't need <ibase.h> —
/// only FirebirdEngine.cpp and FirebirdTableModel.cpp do.
///
/// Requires libfbclient (the Firebird client library) at link time and
/// runtime — per project policy this stays a documented system/runtime
/// dependency rather than being vendored (Firebird client libraries are
/// conventionally dynamically loaded, not statically linked).
///
/// NOTE: not exercised against a live Firebird database while writing this
/// — verify via the project's HITL testing workflow before relying on it.
class FirebirdEngine : public DbEngine {
    Q_OBJECT
public:
    explicit FirebirdEngine(QObject *parent = nullptr);
    ~FirebirdEngine() override;

    bool open(const QString &filepath) override;
    void close() override;

    QStringList tableNames() const override;
    QStringList viewNames() const override;
    QList<ColumnInfo> columnInfos(const QString &tableName) const override;
    QStringList indexes(const QString &tableName) const override;

    QAbstractItemModel *modelForTable(const QString &tableName) override;
    QString currentTableName() const override;

    bool supportsMultipleTables() const override { return true; }
    bool supportsSubmitRevert() const override { return true; }
    bool supportsSqlConsole() const override { return true; }
    bool lastQueryError() const override { return m_lastQueryError; }
    QString engineName() const override { return QStringLiteral("Firebird"); }

    bool submitAll() override;
    bool revertAll() override;
    QAbstractItemModel *executeQuery(const QString &query) override;
    QString lastError() const override;

private:
    QStringList queryNames(const QString &sql) const;

    // Opaque storage for isc_db_handle/isc_tr_handle (both FB_API_HANDLE,
    // an unsigned integer type) — sized generously; actual handles are
    // reinterpret_cast in the .cpp, which alone includes <ibase.h>.
    unsigned long m_dbHandle = 0;
    unsigned long m_trHandle = 0;
    bool m_inTransaction = false;

    QString m_currentTable;
    FirebirdTableModel *m_currentModel = nullptr;
    bool m_lastQueryError = false;
    QString m_lastError;
};
