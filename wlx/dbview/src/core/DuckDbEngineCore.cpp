#include "core/DbEngineCore.h"

#include "duckdb.hpp"
#include <algorithm>

namespace {
constexpr int kChunkSize = 200;

bool endsWithCI(const std::string &s, const std::string &suffix) {
    if (s.size() < suffix.size()) return false;
    return std::equal(suffix.rbegin(), suffix.rend(), s.rbegin(),
        [](char a, char b) { return tolower((unsigned char)a) == tolower((unsigned char)b); });
}
std::string baseName(const std::string &path) {
    auto slash = path.find_last_of('/');
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    auto dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}
}

/// Rows fetched kChunkSize at a time via LIMIT/OFFSET (same principle as
/// the original Qt DuckDbModel's fetchMore()/canFetchMore(), just without
/// the extra ORDER BY/sort-column bookkeeping -- GTK's grid doesn't
/// support click-to-sort yet).
class DuckDbEngineCore : public DbEngineCore {
public:
    ~DuckDbEngineCore() override { close(); }

    bool open(const std::string &filepath) override {
        close();
        try {
            duckdb::DBConfig config;
            config.SetOptionByName("threads", duckdb::Value::INTEGER(1));

            bool isParquet = endsWithCI(filepath, ".parquet") || endsWithCI(filepath, ".pq");
            if (isParquet) {
                m_db = std::make_unique<duckdb::DuckDB>("", &config);
                m_conn = std::make_unique<duckdb::Connection>(*m_db);

                std::string cleanName;
                for (char c : baseName(filepath)) if (isalnum((unsigned char)c) || c == '_') cleanName += c;
                if (cleanName.empty()) cleanName = "parquet_data";

                std::string escapedPath = filepath;
                size_t pos = 0;
                while ((pos = escapedPath.find('\'', pos)) != std::string::npos) { escapedPath.replace(pos, 1, "''"); pos += 2; }

                auto res = m_conn->Query("CREATE VIEW \"" + cleanName + "\" AS SELECT * FROM read_parquet('" + escapedPath + "')");
                if (!res || res->HasError()) { m_lastError = res ? res->GetError() : "unknown error"; return false; }
                m_readOnly = true;
            } else {
                try {
                    m_db = std::make_unique<duckdb::DuckDB>(filepath, &config);
                    m_conn = std::make_unique<duckdb::Connection>(*m_db);
                    m_conn->Query("BEGIN TRANSACTION");
                    m_inTransaction = true;
                    m_readOnly = false;
                } catch (...) {
                    config.SetOptionByName("access_mode", duckdb::Value("READ_ONLY"));
                    m_db = std::make_unique<duckdb::DuckDB>(filepath, &config);
                    m_conn = std::make_unique<duckdb::Connection>(*m_db);
                    m_readOnly = true;
                }
            }
            return true;
        } catch (const std::exception &e) {
            m_lastError = e.what();
            return false;
        }
    }

    void close() override {
        if (m_inTransaction && m_conn) { try { m_conn->Query("ROLLBACK"); } catch (...) {} m_inTransaction = false; }
        m_conn.reset(); m_db.reset();
        resetState();
    }

    void resetState() {
        m_currentTable.clear(); m_columns.clear(); m_rows.clear(); m_binary.clear(); m_rowIds.clear();
        m_isQuery = false; m_hasRowId = false; m_totalRows = 0; m_baseSql.clear();
    }

    std::vector<std::string> tableNames() const override {
        std::vector<std::string> result;
        if (!m_conn) return result;
        try {
            auto qr = m_conn->Query("SELECT table_name FROM information_schema.tables WHERE table_schema='main' AND table_type='BASE TABLE' ORDER BY table_name");
            if (qr && !qr->HasError()) for (auto &row : *qr) result.push_back(row.GetValue<std::string>(0));
        } catch (...) {}
        return result;
    }
    std::vector<std::string> viewNames() const override {
        std::vector<std::string> result;
        if (!m_conn) return result;
        try {
            auto qr = m_conn->Query("SELECT table_name FROM information_schema.tables WHERE table_schema='main' AND table_type='VIEW' ORDER BY table_name");
            if (qr && !qr->HasError()) for (auto &row : *qr) result.push_back(row.GetValue<std::string>(0));
        } catch (...) {}
        return result;
    }
    std::vector<DbColumnInfo> columnInfos(const std::string &tableName) const override {
        std::vector<DbColumnInfo> result;
        if (!m_conn) return result;
        try {
            std::string q = "SELECT column_name, data_type FROM information_schema.columns WHERE table_name='" + tableName + "' AND table_schema='main' ORDER BY ordinal_position";
            auto qr = m_conn->Query(q);
            if (qr && !qr->HasError()) for (auto &row : *qr) result.push_back({row.GetValue<std::string>(0), row.GetValue<std::string>(1), false, false});
        } catch (...) {}
        return result;
    }

    int appendResultRows(duckdb::MaterializedQueryResult *result, int startCol) {
        int fetched = 0;
        for (auto &row : *result) {
            if (m_hasRowId) m_rowIds.push_back(row.GetValue<int64_t>(0));
            std::vector<std::string> r; std::vector<bool> b;
            for (int c = startCol; c < result->ColumnCount(); c++) {
                try {
                    auto val = row.GetValue<duckdb::Value>(c);
                    if (val.IsNull()) { r.push_back("NULL"); b.push_back(false); }
                    else if (val.type().id() == duckdb::LogicalTypeId::BLOB) { r.push_back("[Binary Data]"); b.push_back(true); }
                    else { r.push_back(val.ToString()); b.push_back(false); }
                } catch (...) { r.push_back("NULL"); b.push_back(false); }
            }
            m_rows.push_back(std::move(r));
            m_binary.push_back(std::move(b));
            fetched++;
        }
        return fetched;
    }

    int fetchChunk() {
        if (!m_conn || m_baseSql.empty()) return 0;
        try {
            std::string sql = m_baseSql + " LIMIT " + std::to_string(kChunkSize) + " OFFSET " + std::to_string((int)m_rows.size());
            auto result = m_conn->Query(sql);
            if (!result || result->HasError()) { m_lastError = result ? result->GetError() : "query failed"; return 0; }
            return appendResultRows(result.get(), m_hasRowId ? 1 : 0);
        } catch (const std::exception &e) { m_lastError = e.what(); return 0; }
    }

    bool selectTable(const std::string &tableName) override {
        if (!m_conn) return false;
        resetState();
        m_currentTable = tableName;
        try {
            auto testResult = m_conn->Query("SELECT rowid FROM \"" + tableName + "\" LIMIT 0");
            m_hasRowId = testResult && !testResult->HasError();
        } catch (...) {}

        try {
            auto countResult = m_conn->Query("SELECT COUNT(*) FROM \"" + tableName + "\"");
            if (countResult && !countResult->HasError())
                for (auto &row : *countResult) m_totalRows = (int)row.GetValue<int64_t>(0);
        } catch (...) {}

        // Column names via a LIMIT 0 probe.
        try {
            std::string fields = m_hasRowId ? "rowid, *" : "*";
            auto probe = m_conn->Query("SELECT " + fields + " FROM \"" + tableName + "\" LIMIT 0");
            if (!probe || probe->HasError()) { m_lastError = probe ? probe->GetError() : "query failed"; return false; }
            int startCol = m_hasRowId ? 1 : 0;
            for (int i = startCol; i < probe->ColumnCount(); i++) m_columns.push_back(probe->ColumnName(i));
        } catch (const std::exception &e) { m_lastError = e.what(); return false; }

        m_baseSql = "SELECT " + std::string(m_hasRowId ? "rowid, *" : "*") + " FROM \"" + tableName + "\"";
        fetchChunk();
        return true;
    }

    bool selectQuery(const std::string &query) override {
        if (!m_conn) return false;
        resetState();
        m_isQuery = true;
        try {
            auto result = m_conn->Query(query);
            if (!result || result->HasError()) { m_lastError = result ? result->GetError() : "query failed"; return false; }
            for (int i = 0; i < result->ColumnCount(); i++) m_columns.push_back(result->ColumnName(i));
            appendResultRows(result.get(), 0);
            m_totalRows = (int)m_rows.size();
            return true;
        } catch (const std::exception &e) { m_lastError = e.what(); return false; }
    }

    int rowCount() const override { return m_totalRows; }
    int fetchedRowCount() const override { return (int)m_rows.size(); }
    bool canFetchMore() const override { return (int)m_rows.size() < m_totalRows; }
    int fetchMore() override { return canFetchMore() ? fetchChunk() : 0; }

    int columnCount() const override { return (int)m_columns.size(); }
    std::string columnName(int col) const override { return col >= 0 && col < (int)m_columns.size() ? m_columns[col] : ""; }
    std::string cellText(int row, int col) const override {
        return (row >= 0 && row < (int)m_rows.size() && col >= 0 && col < (int)m_rows[row].size()) ? m_rows[row][col] : "";
    }
    bool cellIsBinary(int row, int col) const override {
        return row >= 0 && row < (int)m_binary.size() && col >= 0 && col < (int)m_binary[row].size() && m_binary[row][col];
    }
    bool cellEditable(int, int) const override { return m_hasRowId && !m_isQuery; }
    bool setCellText(int row, int col, const std::string &text) override {
        if (!m_hasRowId || m_isQuery || row < 0 || row >= (int)m_rows.size()) return false;
        try {
            std::string sql = "UPDATE \"" + m_currentTable + "\" SET \"" + m_columns[col] + "\" = '" + text + "' WHERE rowid = " + std::to_string(m_rowIds[row]);
            auto result = m_conn->Query(sql);
            if (result && !result->HasError()) { m_rows[row][col] = text; m_binary[row][col] = false; return true; }
        } catch (...) {}
        return false;
    }

    std::string currentTableName() const override { return m_currentTable; }
    bool supportsMultipleTables() const override { return true; }
    bool supportsSubmitRevert() const override { return true; }
    bool supportsSqlConsole() const override { return true; }
    std::string engineName() const override { return "DuckDB"; }

    bool submitAll() override {
        if (!m_conn || !m_inTransaction || m_readOnly) return false;
        try {
            auto qr = m_conn->Query("COMMIT");
            if (qr && qr->HasError()) { m_lastError = qr->GetError(); return false; }
            m_conn->Query("BEGIN TRANSACTION");
            return true;
        } catch (const std::exception &e) { m_lastError = e.what(); return false; }
    }
    bool revertAll() override {
        if (!m_conn || !m_inTransaction || m_readOnly) return false;
        try {
            m_conn->Query("ROLLBACK");
            m_conn->Query("BEGIN TRANSACTION");
            if (!m_currentTable.empty()) selectTable(m_currentTable);
            return true;
        } catch (const std::exception &e) { m_lastError = e.what(); return false; }
    }
    std::string lastError() const override { return m_lastError; }

private:
    std::unique_ptr<duckdb::DuckDB> m_db;
    std::unique_ptr<duckdb::Connection> m_conn;
    bool m_inTransaction = false, m_isQuery = false, m_hasRowId = false;
    int m_totalRows = 0;
    std::string m_baseSql;
    std::string m_currentTable, m_lastError;
    std::vector<std::string> m_columns;
    std::vector<std::vector<std::string>> m_rows;
    std::vector<std::vector<bool>> m_binary;
    std::vector<int64_t> m_rowIds;
};

std::unique_ptr<DbEngineCore> createDuckDbEngineCore() { return std::make_unique<DuckDbEngineCore>(); }
