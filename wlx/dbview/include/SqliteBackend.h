#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QSqlDatabase>

class QSqlTableModel;

/// Manages the SQLite database connection and provides model instances per table.
///
/// Each SqliteBackend instance creates a unique connection name via QUuid,
/// so multiple dbview plugin windows can coexist without QSqlDatabase name collisions.
///
/// Uses QSqlTableModel::OnManualSubmit so edits are buffered until
/// explicit submitAll() (aligns with the toolbar Save action).
class SqliteBackend : public QObject {
    Q_OBJECT
public:
    explicit SqliteBackend(QObject *parent = nullptr);
    ~SqliteBackend() override;

    bool openDatabase(const QString &filepath);
    void closeDatabase();

    QStringList tableNames() const;
    QStringList viewNames() const;

    /// Returns a QSqlTableModel for the given table.
    /// The model is owned by this backend and will be destroyed on table switch
    /// or backend destruction. Uses OnManualSubmit editing strategy.
    QSqlTableModel *modelForTable(const QString &tableName);

    QSqlDatabase database() const;
    QString currentTableName() const;

    /// Execute a raw SQL query (for DDL or schema inspection).
    bool execSql(const QString &sql);

private:
    QSqlDatabase m_db;
    QString m_connectionName;
    QString m_currentTable;
    QSqlTableModel *m_currentModel = nullptr;
};
