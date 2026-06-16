#include "SqliteEngine.h"

#include <QSqlDatabase>
#include <QSqlTableModel>
#include <QSqlQuery>
#include <QSqlError>
#include <QUuid>

SqliteEngine::SqliteEngine(QObject *parent)
    : DbEngine(parent)
    , m_connectionName(QUuid::createUuid().toString(QUuid::WithoutBraces))
{
}

SqliteEngine::~SqliteEngine()
{
    close();
}

bool SqliteEngine::open(const QString &filepath)
{
    close();

    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_db.setDatabaseName(filepath);

    if (!m_db.open())
        return false;

    return true;
}

void SqliteEngine::close()
{
    if (m_currentModel) {
        delete m_currentModel;
        m_currentModel = nullptr;
    }
    m_currentTable.clear();

    if (m_db.isOpen())
        m_db.close();

    // Release the QSqlDatabase reference BEFORE removing the connection,
    // otherwise Qt warns "connection still in use".
    QString connName = m_connectionName;
    m_db = QSqlDatabase();

    if (QSqlDatabase::connectionNames().contains(connName))
        QSqlDatabase::removeDatabase(connName);
}

QStringList SqliteEngine::tableNames() const
{
    if (!m_db.isOpen())
        return {};
    return m_db.tables(QSql::Tables);
}

QStringList SqliteEngine::viewNames() const
{
    if (!m_db.isOpen())
        return {};
    return m_db.tables(QSql::Views);
}

QList<ColumnInfo> SqliteEngine::columnInfos(const QString &tableName) const
{
    QList<ColumnInfo> result;
    if (!m_db.isOpen()) return result;

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("PRAGMA table_info(\"%1\")").arg(tableName));
    if (q.exec()) {
        while (q.next()) {
            ColumnInfo info;
            info.name = q.value(1).toString();
            info.type = q.value(2).toString();
            if (info.type.isEmpty()) {
                info.type = QStringLiteral("VARIANT");
            }
            int pk = q.value(5).toInt();
            info.isPrimaryKey = (pk > 0);
            result.append(info);
        }
    }
    return result;
}

QStringList SqliteEngine::indexes(const QString &tableName) const
{
    QStringList result;
    if (!m_db.isOpen()) return result;

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("PRAGMA index_list(\"%1\")").arg(tableName));
    if (q.exec()) {
        while (q.next()) {
            result.append(q.value(1).toString());
        }
    }
    return result;
}

QAbstractItemModel *SqliteEngine::modelForTable(const QString &tableName)
{
    if (!m_db.isOpen())
        return nullptr;

    if (m_currentModel) {
        delete m_currentModel;
        m_currentModel = nullptr;
    }

    m_currentTable = tableName;

    auto *model = new QSqlTableModel(this, m_db);
    model->setTable(tableName);
    model->setEditStrategy(QSqlTableModel::OnManualSubmit);
    model->select();

    while (model->canFetchMore())
        model->fetchMore();

    m_currentModel = model;
    return model;
}

QString SqliteEngine::currentTableName() const
{
    return m_currentTable;
}

bool SqliteEngine::submitAll()
{
    if (!m_currentModel) return false;
    if (!m_currentModel->submitAll())
        return false;
    return true;
}

bool SqliteEngine::revertAll()
{
    if (!m_currentModel) return false;
    m_currentModel->revertAll();
    return true;
}

QAbstractItemModel *SqliteEngine::executeQuery(const QString &query)
{
    if (!m_db.isOpen()) return nullptr;

    m_lastQueryError = false;
    m_lastError.clear();

    QSqlQuery q(m_db);
    if (!q.exec(query)) {
        m_lastQueryError = true;
        m_lastError = q.lastError().text();
        return nullptr;
    }

    if (q.isSelect()) {
        m_lastQueryError = false;
        auto *model = new QSqlQueryModel(this);
        model->setQuery(std::move(q));
        return model;
    } else {
        m_lastQueryError = false;
        int affected = q.numRowsAffected();
        if (affected < 0) {
            m_lastError = QStringLiteral("Query executed successfully.");
        } else {
            m_lastError = QStringLiteral("Query executed successfully, %1 row(s) affected.").arg(affected);
        }
        return nullptr;
    }
}

QString SqliteEngine::lastError() const
{
    if (m_lastQueryError || !m_lastError.isEmpty())
        return m_lastError;
    if (m_currentModel)
        return m_currentModel->lastError().text();
    return m_db.lastError().text();
}
