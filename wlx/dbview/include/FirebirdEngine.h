#pragma once

#include "DbEngine.h"
#include <QSqlDatabase>
#include <QPointer>

class QSqlTableModel;

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
    QString engineName() const override { return QStringLiteral("Firebird Embedded"); }

    bool submitAll() override;
    bool revertAll() override;
    QAbstractItemModel *executeQuery(const QString &query) override;
    QString lastError() const override;

private:
    QString m_connectionName;
    QSqlDatabase m_db;
    QString m_currentTable;
    QPointer<QSqlTableModel> m_currentModel;
    bool m_lastQueryError = false;
    QString m_lastError;
};
