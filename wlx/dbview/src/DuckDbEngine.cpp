#include "DuckDbEngine.h"
#include "DuckDbModel.h"

#include "duckdb.hpp"

DuckDbEngine::DuckDbEngine(QObject *parent)
    : DbEngine(parent)
{
}

DuckDbEngine::~DuckDbEngine()
{
    close();
}

bool DuckDbEngine::open(const QString &filepath)
{
    close();

    try {
        duckdb::DBConfig config;
        config.SetOptionByName("threads", duckdb::Value::INTEGER(1));

        m_duckdb = std::make_unique<duckdb::DuckDB>(filepath.toStdString(), &config);
        m_conn = std::make_unique<duckdb::Connection>(*m_duckdb);

        // Start a transaction for submit/revert support
        m_conn->Query("BEGIN TRANSACTION");
        m_inTransaction = true;

        return true;
    } catch (const std::exception &e) {
        m_lastError = QString::fromStdString(e.what());
        return false;
    }
}

void DuckDbEngine::close()
{
    delete m_currentModel;
    m_currentModel = nullptr;

    if (m_inTransaction && m_conn) {
        try { m_conn->Query("ROLLBACK"); } catch (...) {}
        m_inTransaction = false;
    }

    m_conn.reset();
    m_duckdb.reset();
    m_currentTable.clear();
}

QStringList DuckDbEngine::tableNames() const
{
    QStringList result;
    if (!m_conn) return result;

    try {
        auto qr = m_conn->Query(
            "SELECT table_name FROM information_schema.tables "
            "WHERE table_schema = 'main' AND table_type = 'BASE TABLE' "
            "ORDER BY table_name");
        if (qr && !qr->HasError()) {
            for (auto &row : *qr) {
                result.append(QString::fromStdString(row.GetValue<std::string>(0)));
            }
        }
    } catch (...) {}
    return result;
}

QStringList DuckDbEngine::viewNames() const
{
    QStringList result;
    if (!m_conn) return result;

    try {
        auto qr = m_conn->Query(
            "SELECT table_name FROM information_schema.tables "
            "WHERE table_schema = 'main' AND table_type = 'VIEW' "
            "ORDER BY table_name");
        if (qr && !qr->HasError()) {
            for (auto &row : *qr) {
                result.append(QString::fromStdString(row.GetValue<std::string>(0)));
            }
        }
    } catch (...) {}
    return result;
}

QAbstractItemModel *DuckDbEngine::modelForTable(const QString &tableName)
{
    if (!m_conn) return nullptr;

    delete m_currentModel;
    m_currentModel = nullptr;
    m_currentTable = tableName;

    auto *model = new DuckDbModel(m_conn.get(), tableName, this);
    if (!model->select()) {
        delete model;
        return nullptr;
    }

    m_currentModel = model;
    return model;
}

QString DuckDbEngine::currentTableName() const
{
    return m_currentTable;
}

bool DuckDbEngine::submitAll()
{
    if (!m_conn || !m_inTransaction) return false;

    try {
        auto qr = m_conn->Query("COMMIT");
        if (qr && qr->HasError()) {
            m_lastError = QString::fromStdString(qr->GetError());
            return false;
        }
        m_conn->Query("BEGIN TRANSACTION");
        m_inTransaction = true;
        return true;
    } catch (const std::exception &e) {
        m_lastError = QString::fromStdString(e.what());
        return false;
    }
}

bool DuckDbEngine::revertAll()
{
    if (!m_conn || !m_inTransaction) return false;

    try {
        m_conn->Query("ROLLBACK");
        m_conn->Query("BEGIN TRANSACTION");
        m_inTransaction = true;

        if (!m_currentTable.isEmpty()) {
            auto *model = qobject_cast<DuckDbModel*>(m_currentModel);
            if (model) model->select();
        }
        return true;
    } catch (const std::exception &e) {
        m_lastError = QString::fromStdString(e.what());
        return false;
    }
}

QString DuckDbEngine::lastError() const
{
    return m_lastError;
}
