#include "SqliteBackend.h"

#include <QSqlDatabase>
#include <QSqlTableModel>
#include <QSqlQuery>
#include <QSqlError>
#include <QUuid>

SqliteBackend::SqliteBackend(QObject *parent)
    : QObject(parent)
    , m_connectionName(QUuid::createUuid().toString(QUuid::WithoutBraces))
{
}

SqliteBackend::~SqliteBackend()
{
    closeDatabase();
}

bool SqliteBackend::openDatabase(const QString &filepath)
{
    closeDatabase();

    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_db.setDatabaseName(filepath);

    if (!m_db.open())
        return false;

    return true;
}

void SqliteBackend::closeDatabase()
{
    if (m_currentModel) {
        delete m_currentModel;
        m_currentModel = nullptr;
    }
    m_currentTable.clear();

    if (m_db.isOpen())
        m_db.close();

    // Remove the connection (must be done after closing)
    if (QSqlDatabase::connectionNames().contains(m_connectionName))
        QSqlDatabase::removeDatabase(m_connectionName);
}

QStringList SqliteBackend::tableNames() const
{
    if (!m_db.isOpen())
        return {};
    return m_db.tables(QSql::Tables);
}

QStringList SqliteBackend::viewNames() const
{
    if (!m_db.isOpen())
        return {};
    return m_db.tables(QSql::Views);
}

QSqlTableModel *SqliteBackend::modelForTable(const QString &tableName)
{
    if (!m_db.isOpen())
        return nullptr;

    // Destroy previous model
    if (m_currentModel) {
        delete m_currentModel;
        m_currentModel = nullptr;
    }

    m_currentTable = tableName;

    auto *model = new QSqlTableModel(this, m_db);
    model->setTable(tableName);
    model->setEditStrategy(QSqlTableModel::OnManualSubmit);
    model->select();

    // Fetch all rows for accurate count (QSqlTableModel lazy-loads by default)
    while (model->canFetchMore())
        model->fetchMore();

    m_currentModel = model;
    return model;
}

QSqlDatabase SqliteBackend::database() const
{
    return m_db;
}

QString SqliteBackend::currentTableName() const
{
    return m_currentTable;
}

bool SqliteBackend::execSql(const QString &sql)
{
    if (!m_db.isOpen())
        return false;

    QSqlQuery query(m_db);
    return query.exec(sql);
}
