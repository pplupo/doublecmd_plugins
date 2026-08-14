#include "core/DbEngineCore.h"

#include <ibase.h>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <sstream>
#include <set>

// Same classic-API XSQLDA plumbing as the Qt side's FirebirdTableModel.
// Read-only in this GTK core (browsing + custom SELECTs): the Qt side's
// RDB$DB_KEY-based cell editing is a niche feature on an already-flagged
// "unverified, no live Firebird available while writing it" path -- not
// duplicated here to keep this port's risk surface down. Table/column
// introspection and query execution work the same as the Qt side.

namespace {

std::string fbErrorText(const ISC_STATUS *status) {
    std::string msg;
    char buf[512];
    const ISC_STATUS *p = status;
    while (fb_interpret(buf, sizeof(buf), &p)) {
        if (!msg.empty()) msg += "; ";
        msg += buf;
    }
    return msg;
}
bool isError(const ISC_STATUS *status) { return status[0] == 1 && status[1] != 0; }

void appendDpbStr(std::string &dpb, int tag, const std::string &value) {
    dpb += (char)tag;
    dpb += (char)value.size();
    dpb += value;
}

XSQLDA *allocXsqlda(short n) {
    n = n < 1 ? 1 : n;
    auto *sqlda = (XSQLDA *)malloc(XSQLDA_LENGTH(n));
    memset(sqlda, 0, XSQLDA_LENGTH(n));
    sqlda->version = SQLDA_VERSION1;
    sqlda->sqln = n;
    return sqlda;
}
void allocVarBuffers(XSQLDA *sqlda) {
    for (int i = 0; i < sqlda->sqld; i++) {
        XSQLVAR *var = &sqlda->sqlvar[i];
        int dtype = var->sqltype & ~1;
        int len = var->sqllen;
        if (dtype == SQL_VARYING) len += 2;
        else if (dtype == SQL_TEXT) len += 1;
        var->sqldata = (ISC_SCHAR *)malloc(len > 0 ? len : 1);
        var->sqlind = (var->sqltype & 1) ? (ISC_SHORT *)malloc(sizeof(ISC_SHORT)) : nullptr;
    }
}
void freeVarBuffers(XSQLDA *sqlda) {
    if (!sqlda) return;
    for (int i = 0; i < sqlda->sqld; i++) { free(sqlda->sqlvar[i].sqldata); free(sqlda->sqlvar[i].sqlind); }
}

std::string rtrim(std::string s) {
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

std::string cellFromVar(XSQLVAR *var, bool &isBinary) {
    isBinary = false;
    if (var->sqlind && *var->sqlind == -1) return "NULL";
    int dtype = var->sqltype & ~1;
    std::ostringstream ss; ss.precision(15);
    switch (dtype) {
        case SQL_TEXT: return rtrim(std::string(var->sqldata, var->sqllen));
        case SQL_VARYING: { ISC_USHORT len = *(ISC_USHORT *)var->sqldata; return std::string(var->sqldata + 2, len); }
        case SQL_SHORT: { ISC_SHORT v = *(ISC_SHORT *)var->sqldata; if (var->sqlscale == 0) return std::to_string(v); ss << (v * pow(10.0, var->sqlscale)); return ss.str(); }
        case SQL_LONG: { ISC_LONG v = *(ISC_LONG *)var->sqldata; if (var->sqlscale == 0) return std::to_string(v); ss << (v * pow(10.0, var->sqlscale)); return ss.str(); }
        case SQL_INT64: { ISC_INT64 v = *(ISC_INT64 *)var->sqldata; if (var->sqlscale == 0) return std::to_string(v); ss << ((double)v * pow(10.0, var->sqlscale)); return ss.str(); }
        case SQL_FLOAT: ss << *(float *)var->sqldata; return ss.str();
        case SQL_DOUBLE: case SQL_D_FLOAT: ss << *(double *)var->sqldata; return ss.str();
        case SQL_BOOLEAN: return *(unsigned char *)var->sqldata ? "true" : "false";
        case SQL_TYPE_DATE: { struct tm t{}; isc_decode_sql_date((ISC_DATE *)var->sqldata, &t); char b[32]; snprintf(b, sizeof(b), "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday); return b; }
        case SQL_TYPE_TIME: { struct tm t{}; isc_decode_sql_time((ISC_TIME *)var->sqldata, &t); char b[16]; snprintf(b, sizeof(b), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec); return b; }
        case SQL_TIMESTAMP: { struct tm t{}; isc_decode_timestamp((ISC_TIMESTAMP *)var->sqldata, &t); char b[32]; snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d:%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec); return b; }
        case SQL_BLOB: isBinary = true; return "[Binary Data]";
        default: return "<unsupported type " + std::to_string(dtype) + ">";
    }
}
std::string varColumnName(XSQLVAR *var) {
    std::string name(var->aliasname, var->aliasname_length);
    if (name.empty()) name = std::string(var->sqlname, var->sqlname_length);
    return name;
}

std::string getFirebirdTypeName(int typeId) {
    switch (typeId) {
        case 7: return "SMALLINT"; case 8: return "INTEGER"; case 16: return "BIGINT";
        case 10: return "FLOAT"; case 27: return "DOUBLE"; case 14: return "CHAR"; case 37: return "VARCHAR";
        case 35: return "TIMESTAMP"; case 12: return "DATE"; case 13: return "TIME"; case 261: return "BLOB";
        default: return "UNKNOWN(" + std::to_string(typeId) + ")";
    }
}

} // namespace

class FirebirdEngineCore : public DbEngineCore {
public:
    ~FirebirdEngineCore() override { close(); }

    bool open(const std::string &filepath) override {
        close();
        std::string dpb;
        dpb += (char)isc_dpb_version1;
        appendDpbStr(dpb, isc_dpb_user_name, "SYSDBA");
        appendDpbStr(dpb, isc_dpb_password, "masterkey");
        appendDpbStr(dpb, isc_dpb_lc_ctype, "UTF8");

        ISC_STATUS status[20];
        isc_attach_database(status, (short)filepath.size(), filepath.c_str(), &m_db, (short)dpb.size(), dpb.data());
        if (isError(status)) { m_lastError = fbErrorText(status); m_db = 0; return false; }

        isc_start_transaction(status, &m_tr, 1, &m_db, 0, nullptr);
        if (isError(status)) { m_lastError = fbErrorText(status); isc_detach_database(status, &m_db); m_db = 0; return false; }
        return true;
    }

    void close() override {
        ISC_STATUS status[20];
        if (m_tr) { isc_rollback_transaction(status, &m_tr); m_tr = 0; }
        if (m_db) { isc_detach_database(status, &m_db); m_db = 0; }
        m_columns.clear(); m_rows.clear(); m_binary.clear();
    }

    std::vector<std::string> queryNames(const std::string &sql) const {
        std::vector<std::string> result;
        auto *self = const_cast<FirebirdEngineCore *>(this);
        if (!self->runQuery(sql)) return result;
        for (auto &row : self->m_rows) if (!row.empty()) result.push_back(row[0]);
        return result;
    }
    std::vector<std::string> tableNames() const override {
        return queryNames("SELECT TRIM(rdb$relation_name) FROM rdb$relations WHERE rdb$view_blr IS NULL AND (rdb$system_flag IS NULL OR rdb$system_flag = 0) ORDER BY rdb$relation_name");
    }
    std::vector<std::string> viewNames() const override {
        return queryNames("SELECT TRIM(rdb$relation_name) FROM rdb$relations WHERE rdb$view_blr IS NOT NULL AND (rdb$system_flag IS NULL OR rdb$system_flag = 0) ORDER BY rdb$relation_name");
    }
    std::vector<DbColumnInfo> columnInfos(const std::string &tableName) const override {
        std::vector<DbColumnInfo> result;
        std::string upper = tableName;
        for (auto &c : upper) c = toupper((unsigned char)c);
        auto *self = const_cast<FirebirdEngineCore *>(this);
        if (!self->runQuery("SELECT TRIM(rf.rdb$field_name), f.rdb$field_type, f.rdb$field_length FROM rdb$relation_fields rf "
                             "JOIN rdb$fields f ON rf.rdb$field_source = f.rdb$field_name WHERE rf.rdb$relation_name = '" + upper + "' "
                             "ORDER BY rf.rdb$field_position"))
            return result;
        for (auto &row : self->m_rows) {
            DbColumnInfo info;
            info.name = row.size() > 0 ? row[0] : "";
            int typeId = row.size() > 1 ? atoi(row[1].c_str()) : 0;
            info.type = getFirebirdTypeName(typeId);
            result.push_back(info);
        }
        return result;
    }

    bool runQuery(const std::string &sql) {
        m_columns.clear(); m_rows.clear(); m_binary.clear();
        if (!m_db) return false;

        ISC_STATUS status[20];
        isc_stmt_handle stmt = 0;
        isc_dsql_allocate_statement(status, &m_db, &stmt);
        if (isError(status)) { m_lastError = fbErrorText(status); return false; }

        isc_dsql_prepare(status, &m_tr, &stmt, 0, sql.c_str(), SQL_DIALECT_CURRENT, nullptr);
        if (isError(status)) { m_lastError = fbErrorText(status); isc_dsql_free_statement(status, &stmt, DSQL_drop); return false; }

        XSQLDA *outSqlda = allocXsqlda(1);
        isc_dsql_describe(status, &stmt, SQLDA_VERSION1, outSqlda);
        if (isError(status)) { m_lastError = fbErrorText(status); free(outSqlda); isc_dsql_free_statement(status, &stmt, DSQL_drop); return false; }

        if (outSqlda->sqld == 0) { free(outSqlda); isc_dsql_free_statement(status, &stmt, DSQL_drop); return true; }
        if (outSqlda->sqld > outSqlda->sqln) {
            short need = outSqlda->sqld;
            free(outSqlda);
            outSqlda = allocXsqlda(need);
            isc_dsql_describe(status, &stmt, SQLDA_VERSION1, outSqlda);
        }
        allocVarBuffers(outSqlda);

        isc_dsql_execute(status, &m_tr, &stmt, SQL_DIALECT_CURRENT, nullptr);
        if (isError(status)) { m_lastError = fbErrorText(status); freeVarBuffers(outSqlda); free(outSqlda); isc_dsql_free_statement(status, &stmt, DSQL_drop); return false; }

        for (int i = 0; i < outSqlda->sqld; i++) m_columns.push_back(varColumnName(&outSqlda->sqlvar[i]));

        ISC_STATUS fetchRc;
        while ((fetchRc = isc_dsql_fetch(status, &stmt, SQLDA_VERSION1, outSqlda)) == 0) {
            std::vector<std::string> row; std::vector<bool> bin;
            for (int i = 0; i < outSqlda->sqld; i++) {
                bool isBin;
                row.push_back(cellFromVar(&outSqlda->sqlvar[i], isBin));
                bin.push_back(isBin);
            }
            m_rows.push_back(std::move(row));
            m_binary.push_back(std::move(bin));
        }
        bool ok = (fetchRc == 100);
        if (!ok && isError(status)) m_lastError = fbErrorText(status);

        freeVarBuffers(outSqlda); free(outSqlda);
        isc_dsql_free_statement(status, &stmt, DSQL_drop);
        return ok;
    }

    bool selectTable(const std::string &tableName) override {
        m_currentTable = tableName;
        return runQuery("SELECT " + tableName + ".* FROM " + tableName);
    }
    bool selectQuery(const std::string &query) override { m_currentTable.clear(); return runQuery(query); }

    int rowCount() const override { return (int)m_rows.size(); }
    int columnCount() const override { return (int)m_columns.size(); }
    std::string columnName(int col) const override { return col >= 0 && col < (int)m_columns.size() ? m_columns[col] : ""; }
    std::string cellText(int row, int col) const override {
        return (row >= 0 && row < (int)m_rows.size() && col >= 0 && col < (int)m_rows[row].size()) ? m_rows[row][col] : "";
    }
    bool cellIsBinary(int row, int col) const override {
        return row >= 0 && row < (int)m_binary.size() && col >= 0 && col < (int)m_binary[row].size() && m_binary[row][col];
    }

    std::string currentTableName() const override { return m_currentTable; }
    bool supportsMultipleTables() const override { return true; }
    bool supportsSubmitRevert() const override { return false; }
    bool supportsSqlConsole() const override { return true; }
    std::string engineName() const override { return "Firebird"; }
    std::string lastError() const override { return m_lastError; }

private:
    isc_db_handle m_db = 0;
    isc_tr_handle m_tr = 0;
    std::string m_currentTable, m_lastError;
    std::vector<std::string> m_columns;
    std::vector<std::vector<std::string>> m_rows;
    std::vector<std::vector<bool>> m_binary;
};

std::unique_ptr<DbEngineCore> createFirebirdEngineCore() { return std::make_unique<FirebirdEngineCore>(); }
