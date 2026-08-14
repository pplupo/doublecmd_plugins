#include "FirebirdTableModel.h"

#include <ibase.h>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <QDateTime>

// NOTE: this file has not been exercised against a live Firebird database —
// none was available while writing it. The XSQLDA plumbing below follows
// the standard/documented classic-API pattern, but treat it as unverified
// until it has been run against a real .fdb file (see project HITL testing
// workflow). If something is wrong, it is most likely here: XSQLVAR buffer
// sizing/alignment, the SQL type table in variantFromVar(), or the DPB
// construction in FirebirdEngine.cpp.

namespace {

QString fbErrorText(const ISC_STATUS *status)
{
    QString msg;
    char buf[512];
    const ISC_STATUS *p = status;
    while (fb_interpret(buf, sizeof(buf), &p)) {
        if (!msg.isEmpty()) msg += QStringLiteral("; ");
        msg += QString::fromUtf8(buf);
    }
    return msg;
}

bool isError(const ISC_STATUS *status)
{
    return status[0] == 1 && status[1] != 0;
}

/// Allocate/resize an XSQLDA to hold `n` columns.
XSQLDA *allocXsqlda(short n)
{
    n = n < 1 ? 1 : n;
    auto *sqlda = static_cast<XSQLDA *>(std::malloc(XSQLDA_LENGTH(n)));
    std::memset(sqlda, 0, XSQLDA_LENGTH(n));
    sqlda->version = SQLDA_VERSION1;
    sqlda->sqln = n;
    return sqlda;
}

/// Allocate sqldata/sqlind storage for every XSQLVAR in a described XSQLDA.
void allocVarBuffers(XSQLDA *sqlda)
{
    for (int i = 0; i < sqlda->sqld; ++i) {
        XSQLVAR *var = &sqlda->sqlvar[i];
        int dtype = var->sqltype & ~1;
        int len = var->sqllen;
        if (dtype == SQL_VARYING)
            len += 2; // 2-byte length prefix
        else if (dtype == SQL_TEXT)
            len += 1; // headroom, not strictly required but harmless
        var->sqldata = static_cast<ISC_SCHAR *>(std::malloc(len > 0 ? len : 1));
        if (var->sqltype & 1)
            var->sqlind = static_cast<ISC_SHORT *>(std::malloc(sizeof(ISC_SHORT)));
        else
            var->sqlind = nullptr;
    }
}

void freeVarBuffers(XSQLDA *sqlda)
{
    if (!sqlda) return;
    for (int i = 0; i < sqlda->sqld; ++i) {
        XSQLVAR *var = &sqlda->sqlvar[i];
        std::free(var->sqldata);
        std::free(var->sqlind);
    }
}

QByteArray readBlob(isc_db_handle *db, isc_tr_handle *tr, ISC_QUAD *blobId)
{
    QByteArray result;
    ISC_STATUS status[20];
    isc_blob_handle blobHandle = 0;

    if (isc_open_blob2(status, db, tr, &blobHandle, blobId, 0, nullptr))
        return result;

    // isc_get_segment returns 0 while more segments remain, and a nonzero
    // status once the blob is exhausted (isc_segstr_eof) — or, per the
    // classic API, isc_segment when a single segment was larger than our
    // buffer (more of the *same* segment follows immediately). We
    // deliberately don't special-case that latter status code by name here
    // (its exact symbol wasn't confirmed available from the headers used
    // while writing this) and instead just keep calling isc_get_segment
    // until it stops returning 0 — worst case a single oversized segment
    // gets read in more calls than strictly minimal, which is still correct,
    // just not maximally efficient.
    char buf[4096];
    unsigned short actualLen = 0;
    while (isc_get_segment(status, &blobHandle, &actualLen, sizeof(buf), buf) == 0) {
        if (actualLen > 0)
            result.append(buf, actualLen);
    }

    isc_close_blob(status, &blobHandle);
    return result;
}

QVariant variantFromVar(XSQLVAR *var, isc_db_handle *db, isc_tr_handle *tr, bool *isBlobOut)
{
    if (isBlobOut) *isBlobOut = false;
    if (var->sqlind && *var->sqlind == -1)
        return QVariant(); // SQL NULL

    int dtype = var->sqltype & ~1;
    switch (dtype) {
    case SQL_TEXT: {
        QString s = QString::fromUtf8(var->sqldata, var->sqllen);
        // Firebird CHAR is space-padded to sqllen.
        while (s.endsWith(QLatin1Char(' ')))
            s.chop(1);
        return s;
    }
    case SQL_VARYING: {
        // First 2 bytes: length (little-endian short), matching ISC_USHORT.
        ISC_USHORT len = *reinterpret_cast<ISC_USHORT *>(var->sqldata);
        return QString::fromUtf8(var->sqldata + 2, len);
    }
    case SQL_SHORT: {
        ISC_SHORT v = *reinterpret_cast<ISC_SHORT *>(var->sqldata);
        if (var->sqlscale == 0) return QVariant(static_cast<qlonglong>(v));
        return QVariant(v * std::pow(10.0, var->sqlscale));
    }
    case SQL_LONG: {
        ISC_LONG v = *reinterpret_cast<ISC_LONG *>(var->sqldata);
        if (var->sqlscale == 0) return QVariant(static_cast<qlonglong>(v));
        return QVariant(v * std::pow(10.0, var->sqlscale));
    }
    case SQL_INT64: {
        ISC_INT64 v = *reinterpret_cast<ISC_INT64 *>(var->sqldata);
        if (var->sqlscale == 0) return QVariant(static_cast<qlonglong>(v));
        return QVariant(static_cast<double>(v) * std::pow(10.0, var->sqlscale));
    }
    case SQL_FLOAT:
        return QVariant(static_cast<double>(*reinterpret_cast<float *>(var->sqldata)));
    case SQL_DOUBLE:
    case SQL_D_FLOAT:
        return QVariant(*reinterpret_cast<double *>(var->sqldata));
    case SQL_BOOLEAN:
        return QVariant(static_cast<bool>(*reinterpret_cast<unsigned char *>(var->sqldata)));
    case SQL_TYPE_DATE: {
        struct tm t {};
        isc_decode_sql_date(reinterpret_cast<ISC_DATE *>(var->sqldata), &t);
        return QVariant(QDate(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday));
    }
    case SQL_TYPE_TIME: {
        struct tm t {};
        isc_decode_sql_time(reinterpret_cast<ISC_TIME *>(var->sqldata), &t);
        return QVariant(QTime(t.tm_hour, t.tm_min, t.tm_sec));
    }
    case SQL_TIMESTAMP: {
        struct tm t {};
        isc_decode_timestamp(reinterpret_cast<ISC_TIMESTAMP *>(var->sqldata), &t);
        return QVariant(QDateTime(QDate(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday),
                                   QTime(t.tm_hour, t.tm_min, t.tm_sec)));
    }
    case SQL_BLOB: {
        if (isBlobOut) *isBlobOut = true;
        return QVariant(readBlob(db, tr, reinterpret_cast<ISC_QUAD *>(var->sqldata)));
    }
    default:
        return QVariant(QStringLiteral("<unsupported type %1>").arg(dtype));
    }
}

QString varColumnName(XSQLVAR *var)
{
    QString name = QString::fromUtf8(var->aliasname, var->aliasname_length);
    if (name.isEmpty())
        name = QString::fromUtf8(var->sqlname, var->sqlname_length);
    return name;
}

QString quoteIdent(const QString &ident)
{
    QString escaped = ident;
    escaped.replace(QStringLiteral("\""), QStringLiteral("\"\""));
    return QStringLiteral("\"") + escaped + QStringLiteral("\"");
}

} // namespace

FirebirdTableModel::FirebirdTableModel(void *dbHandle, void *trHandle,
                                       const QString &tableNameOrQuery, bool isQuery,
                                       QObject *parent)
    : QAbstractTableModel(parent)
    , m_dbHandle(dbHandle)
    , m_trHandle(trHandle)
    , m_isQuery(isQuery)
{
    if (isQuery)
        m_query = tableNameOrQuery;
    else
        m_tableName = tableNameOrQuery;
}

bool FirebirdTableModel::select()
{
    beginResetModel();
    m_columnNames.clear();
    m_data.clear();
    m_dbKeys.clear();
    m_lastError.clear();
    m_hasDbKey = false;

    auto *db = reinterpret_cast<isc_db_handle *>(m_dbHandle);
    auto *tr = reinterpret_cast<isc_tr_handle *>(m_trHandle);

    QString sql = m_isQuery
        ? m_query
        : QStringLiteral("SELECT RDB$DB_KEY, %1.* FROM %1")
              .arg(quoteIdent(m_tableName));

    ISC_STATUS status[20];
    isc_stmt_handle stmt = 0;
    isc_dsql_allocate_statement(status, db, &stmt);
    if (isError(status)) { m_lastError = fbErrorText(status); endResetModel(); return false; }

    QByteArray sqlUtf8 = sql.toUtf8();
    isc_dsql_prepare(status, tr, &stmt, 0, sqlUtf8.constData(), SQL_DIALECT_CURRENT, nullptr);
    if (isError(status) && !m_isQuery) {
        // Likely no RDB$DB_KEY visibility for this relation type (e.g. a
        // view) — retry without it, read-only.
        sql = QStringLiteral("SELECT %1.* FROM %1").arg(quoteIdent(m_tableName));
        sqlUtf8 = sql.toUtf8();
        isc_dsql_prepare(status, tr, &stmt, 0, sqlUtf8.constData(), SQL_DIALECT_CURRENT, nullptr);
        m_hasDbKey = false;
    } else if (!isError(status) && !m_isQuery) {
        m_hasDbKey = true;
    }

    if (isError(status)) {
        m_lastError = fbErrorText(status);
        isc_dsql_free_statement(status, &stmt, DSQL_drop);
        endResetModel();
        return false;
    }

    XSQLDA *outSqlda = allocXsqlda(1);
    isc_dsql_describe(status, &stmt, SQLDA_VERSION1, outSqlda);
    if (isError(status)) {
        m_lastError = fbErrorText(status);
        std::free(outSqlda);
        isc_dsql_free_statement(status, &stmt, DSQL_drop);
        endResetModel();
        return false;
    }

    if (outSqlda->sqld == 0) {
        // No result columns — nothing to select (empty model, not an error).
        std::free(outSqlda);
        isc_dsql_free_statement(status, &stmt, DSQL_drop);
        endResetModel();
        return true;
    }

    if (outSqlda->sqld > outSqlda->sqln) {
        short need = outSqlda->sqld;
        std::free(outSqlda);
        outSqlda = allocXsqlda(need);
        isc_dsql_describe(status, &stmt, SQLDA_VERSION1, outSqlda);
    }
    allocVarBuffers(outSqlda);

    isc_dsql_execute(status, tr, &stmt, SQL_DIALECT_CURRENT, nullptr);
    if (isError(status)) {
        m_lastError = fbErrorText(status);
        freeVarBuffers(outSqlda);
        std::free(outSqlda);
        isc_dsql_free_statement(status, &stmt, DSQL_drop);
        endResetModel();
        return false;
    }

    int firstDataCol = m_hasDbKey ? 1 : 0;
    for (int i = firstDataCol; i < outSqlda->sqld; ++i)
        m_columnNames << varColumnName(&outSqlda->sqlvar[i]);

    ISC_STATUS fetchRc;
    while ((fetchRc = isc_dsql_fetch(status, &stmt, SQLDA_VERSION1, outSqlda)) == 0) {
        QVector<QVariant> row;
        row.reserve(outSqlda->sqld - firstDataCol);
        for (int i = firstDataCol; i < outSqlda->sqld; ++i)
            row << variantFromVar(&outSqlda->sqlvar[i], db, tr, nullptr);
        m_data << row;
        if (m_hasDbKey) {
            XSQLVAR *keyVar = &outSqlda->sqlvar[0];
            m_dbKeys << QByteArray(keyVar->sqldata, keyVar->sqllen);
        }
    }

    bool ok = (fetchRc == 100); // 100 == no more rows, expected end-of-fetch
    if (!ok && isError(status))
        m_lastError = fbErrorText(status);

    freeVarBuffers(outSqlda);
    std::free(outSqlda);
    isc_dsql_free_statement(status, &stmt, DSQL_drop);
    endResetModel();
    return true; // fetch loop completing (even on non-100 status) still yields usable rows
}

int FirebirdTableModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) return 0;
    return m_data.size();
}

int FirebirdTableModel::columnCount(const QModelIndex &parent) const
{
    if (parent.isValid()) return 0;
    return m_columnNames.size();
}

QVariant FirebirdTableModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_data.size() || index.column() >= m_columnNames.size())
        return {};
    if (role != Qt::DisplayRole && role != Qt::EditRole)
        return {};

    const QVariant &val = m_data[index.row()][index.column()];
    if (role == Qt::DisplayRole) {
        if (val.userType() == QMetaType::QByteArray)
            return QStringLiteral("[Binary Data - %1 bytes]").arg(val.toByteArray().size());
        if (val.isNull())
            return QStringLiteral("NULL");
    }
    return val;
}

QVariant FirebirdTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole || orientation != Qt::Horizontal)
        return {};
    if (section < 0 || section >= m_columnNames.size())
        return {};
    return m_columnNames[section];
}

Qt::ItemFlags FirebirdTableModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;
    Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (!m_isQuery && m_hasDbKey)
        f |= Qt::ItemIsEditable;
    return f;
}

bool FirebirdTableModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (role != Qt::EditRole || !index.isValid() || m_isQuery || !m_hasDbKey)
        return false;
    if (index.row() >= m_data.size() || index.column() >= m_columnNames.size())
        return false;

    auto *db = reinterpret_cast<isc_db_handle *>(m_dbHandle);
    auto *tr = reinterpret_cast<isc_tr_handle *>(m_trHandle);

    QString sql = QStringLiteral("UPDATE %1 SET %2 = ? WHERE RDB$DB_KEY = ?")
                      .arg(quoteIdent(m_tableName), quoteIdent(m_columnNames[index.column()]));
    QByteArray sqlUtf8 = sql.toUtf8();

    ISC_STATUS status[20];
    isc_stmt_handle stmt = 0;
    isc_dsql_allocate_statement(status, db, &stmt);
    if (isError(status)) { m_lastError = fbErrorText(status); return false; }

    isc_dsql_prepare(status, tr, &stmt, 0, sqlUtf8.constData(), SQL_DIALECT_CURRENT, nullptr);
    if (isError(status)) {
        m_lastError = fbErrorText(status);
        isc_dsql_free_statement(status, &stmt, DSQL_drop);
        return false;
    }

    XSQLDA *inSqlda = allocXsqlda(2);
    isc_dsql_describe_bind(status, &stmt, SQLDA_VERSION1, inSqlda);
    if (isError(status) || inSqlda->sqld != 2) {
        m_lastError = fbErrorText(status);
        std::free(inSqlda);
        isc_dsql_free_statement(status, &stmt, DSQL_drop);
        return false;
    }
    allocVarBuffers(inSqlda);

    // Param 1: new value. Bind by the column's described type where cheaply
    // possible; otherwise fall back to text and let Firebird coerce it.
    XSQLVAR *valVar = &inSqlda->sqlvar[0];
    int valDtype = valVar->sqltype & ~1;
    bool bound = false;
    if (!value.isNull()) {
        switch (valDtype) {
        case SQL_SHORT:
            *reinterpret_cast<ISC_SHORT *>(valVar->sqldata) = static_cast<ISC_SHORT>(value.toLongLong());
            bound = true;
            break;
        case SQL_LONG:
            *reinterpret_cast<ISC_LONG *>(valVar->sqldata) = static_cast<ISC_LONG>(value.toLongLong());
            bound = true;
            break;
        case SQL_INT64:
            *reinterpret_cast<ISC_INT64 *>(valVar->sqldata) = static_cast<ISC_INT64>(value.toLongLong());
            bound = true;
            break;
        case SQL_DOUBLE:
        case SQL_D_FLOAT:
            *reinterpret_cast<double *>(valVar->sqldata) = value.toDouble();
            bound = true;
            break;
        case SQL_FLOAT:
            *reinterpret_cast<float *>(valVar->sqldata) = static_cast<float>(value.toDouble());
            bound = true;
            break;
        default:
            break; // handled below as text
        }
    }
    if (!bound) {
        if (value.isNull()) {
            if (valVar->sqlind) *valVar->sqlind = -1;
        } else {
            QByteArray text = value.toString().toUtf8();
            int dtype = valVar->sqltype & ~1;
            if (dtype == SQL_VARYING) {
                int maxLen = valVar->sqllen;
                int n = qMin(text.size(), maxLen);
                *reinterpret_cast<ISC_USHORT *>(valVar->sqldata) = static_cast<ISC_USHORT>(n);
                std::memcpy(valVar->sqldata + 2, text.constData(), n);
            } else { // SQL_TEXT or fallback: space-pad/truncate to sqllen
                int n = qMin(text.size(), static_cast<int>(valVar->sqllen));
                std::memset(valVar->sqldata, ' ', valVar->sqllen);
                std::memcpy(valVar->sqldata, text.constData(), n);
            }
        }
    }
    if (valVar->sqlind && !value.isNull())
        *valVar->sqlind = 0;

    // Param 2: RDB$DB_KEY, bound as raw octets of the exact captured length.
    XSQLVAR *keyVar = &inSqlda->sqlvar[1];
    const QByteArray &key = m_dbKeys[index.row()];
    int keyDtype = keyVar->sqltype & ~1;
    if (keyDtype == SQL_VARYING) {
        int n = qMin(key.size(), static_cast<int>(keyVar->sqllen));
        *reinterpret_cast<ISC_USHORT *>(keyVar->sqldata) = static_cast<ISC_USHORT>(n);
        std::memcpy(keyVar->sqldata + 2, key.constData(), n);
    } else {
        int n = qMin(key.size(), static_cast<int>(keyVar->sqllen));
        std::memcpy(keyVar->sqldata, key.constData(), n);
    }
    if (keyVar->sqlind) *keyVar->sqlind = 0;

    isc_dsql_execute(status, tr, &stmt, SQLDA_VERSION1, inSqlda);
    bool ok = !isError(status);
    if (!ok)
        m_lastError = fbErrorText(status);

    freeVarBuffers(inSqlda);
    std::free(inSqlda);
    isc_dsql_free_statement(status, &stmt, DSQL_drop);

    if (ok) {
        m_data[index.row()][index.column()] = value;
        emit dataChanged(index, index, {Qt::DisplayRole, Qt::EditRole});
    }
    return ok;
}

bool FirebirdTableModel::isBinaryValue(int row, int col) const
{
    if (row < 0 || row >= m_data.size() || col < 0 || col >= m_columnNames.size())
        return false;
    return m_data[row][col].userType() == QMetaType::QByteArray;
}

QByteArray FirebirdTableModel::rawValue(int row, int col) const
{
    if (row < 0 || row >= m_data.size() || col < 0 || col >= m_columnNames.size())
        return {};
    return m_data[row][col].toByteArray();
}
