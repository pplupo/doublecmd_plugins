#include "FirebirdEngine.h"

#include <QSqlTableModel>
#include <QSqlQueryModel>
#include <QSqlQuery>
#include <QSqlError>
#include <QSqlRecord>
#include <QUuid>

namespace {
QString getFirebirdTypeName(int typeId) {
    switch (typeId) {
    case 7:   return QStringLiteral("SMALLINT");
    case 8:   return QStringLiteral("INTEGER");
    case 16:  return QStringLiteral("BIGINT");
    case 10:  return QStringLiteral("FLOAT");
    case 27:  return QStringLiteral("DOUBLE");
    case 14:  return QStringLiteral("CHAR");
    case 37:  return QStringLiteral("VARCHAR");
    case 35:  return QStringLiteral("TIMESTAMP");
    case 12:  return QStringLiteral("DATE");
    case 13:  return QStringLiteral("TIME");
    case 261: return QStringLiteral("BLOB");
    default:  return QStringLiteral("UNKNOWN(%1)").arg(typeId);
    }
}
}

FirebirdEngine::FirebirdEngine(QObject *parent)
    : DbEngine(parent)
    , m_connectionName(QUuid::createUuid().toString(QUuid::WithoutBraces))
{
}

FirebirdEngine::~FirebirdEngine()
{
    close();
}

bool FirebirdEngine::open(const QString &filepath)
{
    close();

    m_db = QSqlDatabase::addDatabase(QStringLiteral("QIBASE"), m_connectionName);
    m_db.setDatabaseName(filepath);
    m_db.setUserName(QStringLiteral("SYSDBA"));
    m_db.setPassword(QStringLiteral("masterkey"));

    // Set connection options
    m_db.setConnectOptions(QStringLiteral("ISC_DPB_LC_CTYPE=UTF8"));

    if (!m_db.open()) {
        m_readOnly = true;
        // Try opening read-only (which might succeed if write lock/permissions fail but DB is readable)
        m_db.setDatabaseName(filepath);
        if (!m_db.open()) {
            return false;
        }
    } else {
        m_readOnly = false;
    }

    return true;
}

void FirebirdEngine::close()
{
    if (m_currentModel) {
        delete m_currentModel;
        m_currentModel = nullptr;
    }
    m_currentTable.clear();

    if (m_db.isOpen())
        m_db.close();

    if (QSqlDatabase::connectionNames().contains(m_connectionName))
        QSqlDatabase::removeDatabase(m_connectionName);
}

QStringList FirebirdEngine::tableNames() const
{
    QStringList result;
    if (!m_db.isOpen()) return result;

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT TRIM(rdb$relation_name) FROM rdb$relations "
        "WHERE rdb$view_blr IS NULL AND (rdb$system_flag IS NULL OR rdb$system_flag = 0) "
        "ORDER BY rdb$relation_name"
    ));
    if (q.exec()) {
        while (q.next()) {
            result.append(q.value(0).toString());
        }
    }
    return result;
}

QStringList FirebirdEngine::viewNames() const
{
    QStringList result;
    if (!m_db.isOpen()) return result;

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT TRIM(rdb$relation_name) FROM rdb$relations "
        "WHERE rdb$view_blr IS NOT NULL AND (rdb$system_flag IS NULL OR rdb$system_flag = 0) "
        "ORDER BY rdb$relation_name"
    ));
    if (q.exec()) {
        while (q.next()) {
            result.append(q.value(0).toString());
        }
    }
    return result;
}

QList<ColumnInfo> FirebirdEngine::columnInfos(const QString &tableName) const
{
    QList<ColumnInfo> result;
    if (!m_db.isOpen()) return result;

    // First find primary keys
    QStringList pks;
    QSqlQuery pkQuery(m_db);
    pkQuery.prepare(QStringLiteral(
        "SELECT TRIM(isg.rdb$field_name) "
        "FROM rdb$relation_constraints rc "
        "JOIN rdb$index_segments isg ON rc.rdb$index_name = isg.rdb$index_name "
        "WHERE rc.rdb$relation_name = ? AND rc.rdb$constraint_type = 'PRIMARY KEY'"
    ));
    pkQuery.addBindValue(tableName.trimmed().toUpper());
    if (pkQuery.exec()) {
        while (pkQuery.next()) {
            pks.append(pkQuery.value(0).toString());
        }
    }

    // Find foreign keys
    QStringList fks;
    QSqlQuery fkQuery(m_db);
    fkQuery.prepare(QStringLiteral(
        "SELECT TRIM(isg.rdb$field_name) "
        "FROM rdb$relation_constraints rc "
        "JOIN rdb$index_segments isg ON rc.rdb$index_name = isg.rdb$index_name "
        "WHERE rc.rdb$relation_name = ? AND rc.rdb$constraint_type = 'FOREIGN KEY'"
    ));
    fkQuery.addBindValue(tableName.trimmed().toUpper());
    if (fkQuery.exec()) {
        while (fkQuery.next()) {
            fks.append(fkQuery.value(0).toString());
        }
    }

    // Query columns
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT TRIM(rf.rdb$field_name), f.rdb$field_type, f.rdb$field_length "
        "FROM rdb$relation_fields rf "
        "JOIN rdb$fields f ON rf.rdb$field_source = f.rdb$field_name "
        "WHERE rf.rdb$relation_name = ? "
        "ORDER BY rf.rdb$field_position"
    ));
    q.addBindValue(tableName.trimmed().toUpper());

    if (q.exec()) {
        while (q.next()) {
            ColumnInfo info;
            info.name = q.value(0).toString();
            int typeId = q.value(1).toInt();
            int len = q.value(2).toInt();
            info.type = getFirebirdTypeName(typeId);
            if (typeId == 14 || typeId == 37) { // CHAR/VARCHAR
                info.type += QStringLiteral("(%1)").arg(len);
            }
            info.isPrimaryKey = pks.contains(info.name);
            info.isForeignKey = fks.contains(info.name);
            result.append(info);
        }
    }
    return result;
}

QStringList FirebirdEngine::indexes(const QString &tableName) const
{
    QStringList result;
    if (!m_db.isOpen()) return result;

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT TRIM(rdb$index_name) FROM rdb$indices "
        "WHERE rdb$relation_name = ? AND (rdb$system_flag IS NULL OR rdb$system_flag = 0)"
    ));
    q.addBindValue(tableName.trimmed().toUpper());

    if (q.exec()) {
        while (q.next()) {
            result.append(q.value(0).toString());
        }
    }
    return result;
}

QAbstractItemModel *FirebirdEngine::modelForTable(const QString &tableName)
{
    if (!m_db.isOpen()) return nullptr;

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

QString FirebirdEngine::currentTableName() const
{
    return m_currentTable;
}

bool FirebirdEngine::submitAll()
{
    if (!m_currentModel) return false;
    return m_currentModel->submitAll();
}

bool FirebirdEngine::revertAll()
{
    if (!m_currentModel) return false;
    m_currentModel->revertAll();
    return true;
}

QAbstractItemModel *FirebirdEngine::executeQuery(const QString &query)
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

QString FirebirdEngine::lastError() const
{
    if (m_lastQueryError || !m_lastError.isEmpty())
        return m_lastError;
    if (m_currentModel)
        return m_currentModel->lastError().text();
    return m_db.lastError().text();
}
