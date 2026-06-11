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

QString SqliteEngine::lastError() const
{
    if (m_currentModel)
        return m_currentModel->lastError().text();
    return m_db.lastError().text();
}
