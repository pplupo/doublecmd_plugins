#pragma once

#include <QAbstractTableModel>
#include <QVariant>
#include <QVector>
#include <QByteArray>
#include <QStringList>

/// Custom QAbstractTableModel for Firebird tables/query results, backed
/// directly by the classic Firebird C API (isc_dsql_*, XSQLDA) — see
/// FirebirdEngine, which replaces the previous QSqlDatabase/QIBASE-driver
/// based approach.
///
/// Editable tables are loaded via `SELECT RDB$DB_KEY, * FROM table`, using
/// Firebird's RDB$DB_KEY pseudo-column as the row identifier (Firebird has
/// no generic ROWID; RDB$DB_KEY is the closest equivalent and is valid for
/// the lifetime of the transaction that read it — the engine keeps one
/// transaction open the whole time a database is open, same pattern as
/// SqliteEngine/DuckDbEngine). Edits write immediately via a parameterized
/// `UPDATE ... WHERE RDB$DB_KEY = ?`.
///
/// NOTE: this class has not been exercised against a live Firebird
/// database (none was available in the environment this was written in).
/// Verify against a real .fdb file via the HITL workflow (see project
/// testing docs) before relying on it.
class FirebirdTableModel : public QAbstractTableModel {
    Q_OBJECT
public:
    /// dbHandle/trHandle: real isc_db_handle*/isc_tr_handle* from the engine,
    /// passed as void* to keep ibase.h out of this header.
    FirebirdTableModel(void *dbHandle, void *trHandle, const QString &tableNameOrQuery,
                        bool isQuery, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole) override;

    bool select();

    bool hasError() const { return !m_lastError.isEmpty(); }
    QString lastError() const { return m_lastError; }

    bool isBinaryValue(int row, int col) const;
    QByteArray rawValue(int row, int col) const;

private:
    void *m_dbHandle;
    void *m_trHandle;
    QString m_tableName;
    QString m_query;
    bool m_isQuery;
    bool m_hasDbKey = false;

    QStringList m_columnNames;
    QVector<QVector<QVariant>> m_data;
    QVector<QByteArray> m_dbKeys;
    QString m_lastError;
};
