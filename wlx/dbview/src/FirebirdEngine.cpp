#include "FirebirdEngine.h"
#include "FirebirdTableModel.h"

#include <ibase.h>
#include <cstring>
#include <QSet>

static_assert(sizeof(isc_db_handle) <= sizeof(unsigned long),
              "FirebirdEngine's opaque unsigned long storage is too small for isc_db_handle");
static_assert(sizeof(isc_tr_handle) <= sizeof(unsigned long),
              "FirebirdEngine's opaque unsigned long storage is too small for isc_tr_handle");

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

void appendDpbStr(QByteArray &dpb, int tag, const QByteArray &value)
{
    dpb.append(static_cast<char>(tag));
    dpb.append(static_cast<char>(value.size()));
    dpb.append(value);
}

QString getFirebirdTypeName(int typeId)
{
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

} // namespace

FirebirdEngine::FirebirdEngine(QObject *parent)
    : DbEngine(parent)
{
}

FirebirdEngine::~FirebirdEngine()
{
    close();
}

bool FirebirdEngine::open(const QString &filepath)
{
    close();

    QByteArray dpb;
    dpb.append(static_cast<char>(isc_dpb_version1));
    appendDpbStr(dpb, isc_dpb_user_name, QByteArrayLiteral("SYSDBA"));
    appendDpbStr(dpb, isc_dpb_password, QByteArrayLiteral("masterkey"));
    appendDpbStr(dpb, isc_dpb_lc_ctype, QByteArrayLiteral("UTF8"));

    QByteArray pathUtf8 = filepath.toUtf8();
    ISC_STATUS status[20];
    auto *db = reinterpret_cast<isc_db_handle *>(&m_dbHandle);

    isc_attach_database(status, static_cast<short>(pathUtf8.size()), pathUtf8.constData(),
                         db, static_cast<short>(dpb.size()), dpb.constData());

    if (isError(status)) {
        // Retry read-only: same DPB, most permission/lock failures still
        // surface through isc_attach_database itself (Firebird does not
        // have a separate "open read-only" attach flag the way SQLite
        // does — a failed write-capable attach with a lock/permission error
        // still generally means the file itself is readable via a plain
        // reattach only if the earlier failure was transient; if not, this
        // second attempt will also fail and open() returns false below).
        m_dbHandle = 0;
        isc_attach_database(status, static_cast<short>(pathUtf8.size()), pathUtf8.constData(),
                             db, static_cast<short>(dpb.size()), dpb.constData());
        if (isError(status)) {
            m_lastError = fbErrorText(status);
            m_dbHandle = 0;
            return false;
        }
    }

    auto *tr = reinterpret_cast<isc_tr_handle *>(&m_trHandle);
    isc_start_transaction(status, tr, 1, db, 0, nullptr);
    if (isError(status)) {
        m_lastError = fbErrorText(status);
        isc_detach_database(status, db);
        m_dbHandle = 0;
        return false;
    }
    m_inTransaction = true;

    return true;
}

void FirebirdEngine::close()
{
    if (m_currentModel) {
        delete m_currentModel;
        m_currentModel = nullptr;
    }
    m_currentTable.clear();

    ISC_STATUS status[20];
    auto *tr = reinterpret_cast<isc_tr_handle *>(&m_trHandle);
    auto *db = reinterpret_cast<isc_db_handle *>(&m_dbHandle);

    if (m_inTransaction) {
        isc_rollback_transaction(status, tr);
        m_inTransaction = false;
    }
    if (m_dbHandle != 0) {
        isc_detach_database(status, db);
        m_dbHandle = 0;
    }
}

QStringList FirebirdEngine::queryNames(const QString &sql) const
{
    // Introspection-only helper against RDB$ system tables — used for
    // tableNames()/viewNames() where there's no user-controlled input to
    // parameterize, so a plain read-only query via FirebirdTableModel's
    // query mode is simplest.
    if (m_dbHandle == 0) return {};
    auto *self = const_cast<FirebirdEngine *>(this);
    FirebirdTableModel model(reinterpret_cast<void *>(&self->m_dbHandle),
                              reinterpret_cast<void *>(&self->m_trHandle),
                              sql, /*isQuery=*/true);
    if (!model.select())
        return {};
    QStringList result;
    for (int i = 0; i < model.rowCount(); ++i)
        result << model.data(model.index(i, 0), Qt::DisplayRole).toString().trimmed();
    return result;
}

QStringList FirebirdEngine::tableNames() const
{
    return queryNames(QStringLiteral(
        "SELECT TRIM(rdb$relation_name) FROM rdb$relations "
        "WHERE rdb$view_blr IS NULL AND (rdb$system_flag IS NULL OR rdb$system_flag = 0) "
        "ORDER BY rdb$relation_name"));
}

QStringList FirebirdEngine::viewNames() const
{
    return queryNames(QStringLiteral(
        "SELECT TRIM(rdb$relation_name) FROM rdb$relations "
        "WHERE rdb$view_blr IS NOT NULL AND (rdb$system_flag IS NULL OR rdb$system_flag = 0) "
        "ORDER BY rdb$relation_name"));
}

QList<ColumnInfo> FirebirdEngine::columnInfos(const QString &tableName) const
{
    QList<ColumnInfo> result;
    if (m_dbHandle == 0) return result;

    // Table name is validated indirectly (it always comes from tableNames()
    // results, not arbitrary user text), so embedding it quoted in the
    // RDB$ introspection queries below is acceptable here — unlike the
    // UPDATE path in FirebirdTableModel::setData(), which does use bound
    // parameters for actual user-entered cell values.
    QString upperName = tableName.trimmed().toUpper();
    upperName.replace(QStringLiteral("'"), QStringLiteral("''"));

    auto *self = const_cast<FirebirdEngine *>(this);
    QSet<QString> pks, fks;

    {
        FirebirdTableModel pkModel(reinterpret_cast<void *>(&self->m_dbHandle),
                                    reinterpret_cast<void *>(&self->m_trHandle),
                                    QStringLiteral(
                                        "SELECT TRIM(isg.rdb$field_name) "
                                        "FROM rdb$relation_constraints rc "
                                        "JOIN rdb$index_segments isg ON rc.rdb$index_name = isg.rdb$index_name "
                                        "WHERE rc.rdb$relation_name = '%1' AND rc.rdb$constraint_type = 'PRIMARY KEY'")
                                        .arg(upperName),
                                    /*isQuery=*/true);
        if (pkModel.select())
            for (int i = 0; i < pkModel.rowCount(); ++i)
                pks.insert(pkModel.data(pkModel.index(i, 0)).toString());
    }
    {
        FirebirdTableModel fkModel(reinterpret_cast<void *>(&self->m_dbHandle),
                                    reinterpret_cast<void *>(&self->m_trHandle),
                                    QStringLiteral(
                                        "SELECT TRIM(isg.rdb$field_name) "
                                        "FROM rdb$relation_constraints rc "
                                        "JOIN rdb$index_segments isg ON rc.rdb$index_name = isg.rdb$index_name "
                                        "WHERE rc.rdb$relation_name = '%1' AND rc.rdb$constraint_type = 'FOREIGN KEY'")
                                        .arg(upperName),
                                    /*isQuery=*/true);
        if (fkModel.select())
            for (int i = 0; i < fkModel.rowCount(); ++i)
                fks.insert(fkModel.data(fkModel.index(i, 0)).toString());
    }

    FirebirdTableModel colModel(reinterpret_cast<void *>(&self->m_dbHandle),
                                 reinterpret_cast<void *>(&self->m_trHandle),
                                 QStringLiteral(
                                     "SELECT TRIM(rf.rdb$field_name), f.rdb$field_type, f.rdb$field_length "
                                     "FROM rdb$relation_fields rf "
                                     "JOIN rdb$fields f ON rf.rdb$field_source = f.rdb$field_name "
                                     "WHERE rf.rdb$relation_name = '%1' "
                                     "ORDER BY rf.rdb$field_position")
                                     .arg(upperName),
                                 /*isQuery=*/true);
    if (colModel.select()) {
        for (int i = 0; i < colModel.rowCount(); ++i) {
            ColumnInfo info;
            info.name = colModel.data(colModel.index(i, 0)).toString();
            int typeId = colModel.data(colModel.index(i, 1)).toInt();
            int len = colModel.data(colModel.index(i, 2)).toInt();
            info.type = getFirebirdTypeName(typeId);
            if (typeId == 14 || typeId == 37)
                info.type += QStringLiteral("(%1)").arg(len);
            info.isPrimaryKey = pks.contains(info.name);
            info.isForeignKey = fks.contains(info.name);
            result.append(info);
        }
    }
    return result;
}

QStringList FirebirdEngine::indexes(const QString &tableName) const
{
    QString upperName = tableName.trimmed().toUpper();
    upperName.replace(QStringLiteral("'"), QStringLiteral("''"));
    return queryNames(QStringLiteral(
        "SELECT TRIM(rdb$index_name) FROM rdb$indices "
        "WHERE rdb$relation_name = '%1' AND (rdb$system_flag IS NULL OR rdb$system_flag = 0)")
        .arg(upperName));
}

QAbstractItemModel *FirebirdEngine::modelForTable(const QString &tableName)
{
    if (m_dbHandle == 0)
        return nullptr;

    if (m_currentModel) {
        delete m_currentModel;
        m_currentModel = nullptr;
    }

    m_currentTable = tableName;

    auto *model = new FirebirdTableModel(reinterpret_cast<void *>(&m_dbHandle),
                                          reinterpret_cast<void *>(&m_trHandle),
                                          tableName, /*isQuery=*/false, this);
    if (!model->select()) {
        m_lastError = model->lastError();
        delete model;
        return nullptr;
    }

    m_currentModel = model;
    return model;
}

QString FirebirdEngine::currentTableName() const
{
    return m_currentTable;
}

bool FirebirdEngine::submitAll()
{
    if (m_dbHandle == 0 || !m_inTransaction) return false;

    ISC_STATUS status[20];
    auto *tr = reinterpret_cast<isc_tr_handle *>(&m_trHandle);
    auto *db = reinterpret_cast<isc_db_handle *>(&m_dbHandle);

    isc_commit_transaction(status, tr);
    if (isError(status)) {
        m_lastError = fbErrorText(status);
        return false;
    }
    m_inTransaction = false;

    isc_start_transaction(status, tr, 1, db, 0, nullptr);
    if (isError(status)) {
        m_lastError = fbErrorText(status);
        return false;
    }
    m_inTransaction = true;
    return true;
}

bool FirebirdEngine::revertAll()
{
    if (m_dbHandle == 0 || !m_inTransaction) return false;

    ISC_STATUS status[20];
    auto *tr = reinterpret_cast<isc_tr_handle *>(&m_trHandle);
    auto *db = reinterpret_cast<isc_db_handle *>(&m_dbHandle);

    isc_rollback_transaction(status, tr);
    m_inTransaction = false;

    isc_start_transaction(status, tr, 1, db, 0, nullptr);
    if (isError(status)) {
        m_lastError = fbErrorText(status);
        return false;
    }
    m_inTransaction = true;

    if (m_currentModel)
        m_currentModel->select();
    return true;
}

QAbstractItemModel *FirebirdEngine::executeQuery(const QString &query)
{
    if (m_dbHandle == 0) return nullptr;

    m_lastQueryError = false;
    m_lastError.clear();

    auto *model = new FirebirdTableModel(reinterpret_cast<void *>(&m_dbHandle),
                                          reinterpret_cast<void *>(&m_trHandle),
                                          query, /*isQuery=*/true, this);
    if (!model->select()) {
        m_lastQueryError = true;
        m_lastError = model->lastError();
        delete model;
        return nullptr;
    }

    if (model->columnCount() > 0)
        return model;

    delete model;
    m_lastError = QStringLiteral("Query executed successfully.");
    return nullptr;
}

QString FirebirdEngine::lastError() const
{
    if (m_lastQueryError || !m_lastError.isEmpty())
        return m_lastError;
    if (m_currentModel && m_currentModel->hasError())
        return m_currentModel->lastError();
    return QString();
}
