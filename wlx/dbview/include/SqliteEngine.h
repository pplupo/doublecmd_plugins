#pragma once

#include "DbEngine.h"
#include <QSqlDatabase>

class QSqlTableModel;

/// SQLite engine: wraps Qt's QSQLITE driver via QSqlTableModel.
///
/// Uses OnManualSubmit editing strategy so changes are buffered
/// until explicit submitAll() (wired to Ctrl+S in the toolbar).
///
/// Each instance creates a unique QSqlDatabase connection name
/// via QUuid to avoid collisions when multiple plugin windows are open.
class SqliteEngine : public DbEngine {
    Q_OBJECT
public:
    explicit SqliteEngine(QObject *parent = nullptr);
    ~SqliteEngine() override;

    bool open(const QString &filepath) override;
    void close() override;

    QStringList tableNames() const override;
    QStringList viewNames() const override;
    QAbstractItemModel *modelForTable(const QString &tableName) override;
    QString currentTableName() const override;

    bool supportsMultipleTables() const override { return true; }
    bool supportsSubmitRevert() const override { return true; }
    QString engineName() const override { return QStringLiteral("SQLite"); }

    bool submitAll() override;
    bool revertAll() override;
    QString lastError() const override;

private:
    QSqlDatabase m_db;
    QString m_connectionName;
    QString m_currentTable;
    QSqlTableModel *m_currentModel = nullptr;
};
