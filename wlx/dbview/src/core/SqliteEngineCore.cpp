#include "core/DbEngineCore.h"

#include <sqlite3.h>
#include <sstream>
#include <cstring>

namespace {
std::string quoteIdent(const std::string &ident) {
    std::string escaped;
    for (char c : ident) { if (c == '"') escaped += '"'; escaped += c; }
    return "\"" + escaped + "\"";
}
std::string cellToString(sqlite3_stmt *stmt, int col, bool &isNull, bool &isBinary) {
    isNull = false; isBinary = false;
    switch (sqlite3_column_type(stmt, col)) {
        case SQLITE_INTEGER: return std::to_string(sqlite3_column_int64(stmt, col));
        case SQLITE_FLOAT: { std::ostringstream ss; ss.precision(15); ss << sqlite3_column_double(stmt, col); return ss.str(); }
        case SQLITE_TEXT: {
            const unsigned char *t = sqlite3_column_text(stmt, col);
            int len = sqlite3_column_bytes(stmt, col);
            return std::string((const char *)t, len);
        }
        case SQLITE_BLOB: isBinary = true; return "";
        case SQLITE_NULL: default: isNull = true; return "";
    }
}
}

class SqliteEngineCore : public DbEngineCore {
public:
    ~SqliteEngineCore() override { close(); }

    bool open(const std::string &filepath) override {
        close();
        int flags = m_readOnly ? SQLITE_OPEN_READONLY : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
        int rc = sqlite3_open_v2(filepath.c_str(), &m_db, flags, nullptr);
        if (rc != SQLITE_OK && !m_readOnly) {
            if (m_db) sqlite3_close(m_db);
            m_db = nullptr;
            rc = sqlite3_open_v2(filepath.c_str(), &m_db, SQLITE_OPEN_READONLY, nullptr);
            if (rc == SQLITE_OK) m_readOnly = true;
        }
        if (rc != SQLITE_OK) { if (m_db) sqlite3_close(m_db); m_db = nullptr; return false; }
        if (!m_readOnly) { sqlite3_exec(m_db, "BEGIN TRANSACTION", nullptr, nullptr, nullptr); m_inTransaction = true; }
        return true;
    }

    void close() override {
        if (m_db) {
            if (m_inTransaction) { sqlite3_exec(m_db, "ROLLBACK", nullptr, nullptr, nullptr); m_inTransaction = false; }
            sqlite3_close(m_db);
            m_db = nullptr;
        }
        m_currentTable.clear(); m_isQuery = false;
        m_columns.clear(); m_rows.clear(); m_binary.clear(); m_rowIds.clear();
    }

    std::vector<std::string> tablesOrViews(const std::string &type) const {
        std::vector<std::string> result;
        if (!m_db) return result;
        static const char *sql = "SELECT name FROM sqlite_master WHERE type = ?1 AND name NOT LIKE 'sqlite\\_%' ESCAPE '\\' ORDER BY name";
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return result;
        sqlite3_bind_text(stmt, 1, type.c_str(), (int)type.size(), SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW)
            result.push_back((const char *)sqlite3_column_text(stmt, 0));
        sqlite3_finalize(stmt);
        return result;
    }
    std::vector<std::string> tableNames() const override { return tablesOrViews("table"); }
    std::vector<std::string> viewNames() const override { return tablesOrViews("view"); }

    std::vector<DbColumnInfo> columnInfos(const std::string &tableName) const override {
        std::vector<DbColumnInfo> result;
        if (!m_db) return result;
        std::string sql = "PRAGMA table_info(" + quoteIdent(tableName) + ")";
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return result;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            DbColumnInfo info;
            info.name = (const char *)sqlite3_column_text(stmt, 1);
            const unsigned char *t = sqlite3_column_text(stmt, 2);
            info.type = t ? (const char *)t : "VARIANT";
            if (info.type.empty()) info.type = "VARIANT";
            info.isPrimaryKey = sqlite3_column_int(stmt, 5) > 0;
            result.push_back(info);
        }
        sqlite3_finalize(stmt);
        return result;
    }

    bool runSelect(const std::string &sql, bool isQuery) {
        m_columns.clear(); m_rows.clear(); m_binary.clear(); m_rowIds.clear();
        m_hasRowId = false; m_isQuery = isQuery;

        sqlite3_stmt *stmt = nullptr;
        int rc = sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK) { m_lastError = sqlite3_errmsg(m_db); if (stmt) sqlite3_finalize(stmt); return false; }
        if (!isQuery) m_hasRowId = true;

        int totalCols = sqlite3_column_count(stmt);
        int firstDataCol = m_hasRowId ? 1 : 0;
        for (int i = firstDataCol; i < totalCols; i++) m_columns.push_back(sqlite3_column_name(stmt, i));

        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            std::vector<std::string> row; std::vector<bool> binRow;
            for (int i = firstDataCol; i < totalCols; i++) {
                bool isNull, isBin;
                std::string v = cellToString(stmt, i, isNull, isBin);
                row.push_back(isNull ? "NULL" : (isBin ? "[Binary Data]" : v));
                binRow.push_back(isBin);
            }
            m_rows.push_back(std::move(row));
            m_binary.push_back(std::move(binRow));
            if (m_hasRowId) m_rowIds.push_back(sqlite3_column_int64(stmt, 0));
        }
        bool ok = (rc == SQLITE_DONE);
        if (!ok) m_lastError = sqlite3_errmsg(m_db);
        sqlite3_finalize(stmt);
        return ok;
    }

    bool selectTable(const std::string &tableName) override {
        if (!m_db) return false;
        m_currentTable = tableName;
        std::string sql = "SELECT rowid, * FROM " + quoteIdent(tableName);
        if (!runSelect(sql, false)) {
            // WITHOUT ROWID fallback
            sql = "SELECT * FROM " + quoteIdent(tableName);
            m_hasRowId = false;
            return runSelect(sql, false);
        }
        return true;
    }
    bool selectQuery(const std::string &query) override {
        if (!m_db) return false;
        return runSelect(query, true);
    }

    int rowCount() const override { return (int)m_rows.size(); }
    int columnCount() const override { return (int)m_columns.size(); }
    std::string columnName(int col) const override { return col >= 0 && col < (int)m_columns.size() ? m_columns[col] : ""; }
    std::string cellText(int row, int col) const override {
        if (row < 0 || row >= (int)m_rows.size() || col < 0 || col >= (int)m_rows[row].size()) return "";
        return m_binary[row][col] ? ("[Binary Data]") : m_rows[row][col];
    }
    bool cellIsBinary(int row, int col) const override {
        return row >= 0 && row < (int)m_binary.size() && col >= 0 && col < (int)m_binary[row].size() && m_binary[row][col];
    }
    bool cellEditable(int row, int col) const override { return !m_isQuery && m_hasRowId; }

    bool setCellText(int row, int col, const std::string &text) override {
        if (m_isQuery || !m_hasRowId || row < 0 || row >= (int)m_rows.size()) return false;
        std::string sql = "UPDATE " + quoteIdent(m_currentTable) + " SET " + quoteIdent(m_columns[col]) + " = ?1 WHERE rowid = ?2";
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) { m_lastError = sqlite3_errmsg(m_db); return false; }
        sqlite3_bind_text(stmt, 1, text.c_str(), (int)text.size(), SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, m_rowIds[row]);
        bool ok = sqlite3_step(stmt) == SQLITE_DONE;
        if (!ok) m_lastError = sqlite3_errmsg(m_db);
        sqlite3_finalize(stmt);
        if (ok) { m_rows[row][col] = text; m_binary[row][col] = false; }
        return ok;
    }

    std::string currentTableName() const override { return m_currentTable; }
    bool supportsMultipleTables() const override { return true; }
    bool supportsSubmitRevert() const override { return true; }
    bool supportsSqlConsole() const override { return true; }
    std::string engineName() const override { return "SQLite"; }

    bool submitAll() override {
        if (!m_db || !m_inTransaction) return false;
        if (sqlite3_exec(m_db, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) { m_lastError = sqlite3_errmsg(m_db); return false; }
        sqlite3_exec(m_db, "BEGIN TRANSACTION", nullptr, nullptr, nullptr);
        return true;
    }
    bool revertAll() override {
        if (!m_db || !m_inTransaction) return false;
        sqlite3_exec(m_db, "ROLLBACK", nullptr, nullptr, nullptr);
        sqlite3_exec(m_db, "BEGIN TRANSACTION", nullptr, nullptr, nullptr);
        if (!m_currentTable.empty()) selectTable(m_currentTable);
        return true;
    }
    std::string lastError() const override { return !m_lastError.empty() ? m_lastError : (m_db ? sqlite3_errmsg(m_db) : ""); }

private:
    sqlite3 *m_db = nullptr;
    bool m_inTransaction = false;
    bool m_isQuery = false;
    bool m_hasRowId = false;
    std::string m_currentTable;
    std::vector<std::string> m_columns;
    std::vector<std::vector<std::string>> m_rows;
    std::vector<std::vector<bool>> m_binary;
    std::vector<int64_t> m_rowIds;
    mutable std::string m_lastError;
};

std::unique_ptr<DbEngineCore> createSqliteEngineCore() { return std::make_unique<SqliteEngineCore>(); }
