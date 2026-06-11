#include "DuckDbModel.h"

#include "duckdb.hpp"

#include <QDebug>

DuckDbModel::DuckDbModel(duckdb::Connection *conn, const QString &tableName, QObject *parent)
    : QAbstractTableModel(parent)
    , m_conn(conn)
    , m_tableName(tableName)
{
}

bool DuckDbModel::select()
{
    beginResetModel();
    m_data.clear();
    m_rowIds.clear();
    m_columnNames.clear();
    m_totalRows = 0;
    m_allFetched = false;

    try {
        // Check if rowid is queryable
        m_hasRowId = false;
        try {
            auto testResult = m_conn->Query("SELECT rowid FROM \"" + m_tableName.toStdString() + "\" LIMIT 0");
            if (testResult && !testResult->HasError()) {
                m_hasRowId = true;
            }
        } catch (...) {
            m_hasRowId = false;
        }

        // Get column names
        auto colResult = m_conn->Query(
            "SELECT column_name FROM information_schema.columns "
            "WHERE table_name = '" + m_tableName.toStdString() + "' "
            "AND table_schema = 'main' ORDER BY ordinal_position");
        if (colResult && !colResult->HasError()) {
            for (auto &row : *colResult) {
                m_columnNames.append(QString::fromStdString(row.GetValue<std::string>(0)));
            }
        }

        // Get total row count
        auto countResult = m_conn->Query(
            "SELECT COUNT(*) FROM \"" + m_tableName.toStdString() + "\"");
        if (countResult && !countResult->HasError()) {
            for (auto &row : *countResult) {
                m_totalRows = static_cast<int>(row.GetValue<int64_t>(0));
            }
        }

        // Load initial chunk
        loadChunk(0, kChunkSize);

    } catch (const std::exception &e) {
        qWarning() << "DuckDbModel::select failed:" << e.what();
    }

    endResetModel();
    return !m_columnNames.isEmpty();
}

void DuckDbModel::loadChunk(int offset, int limit)
{
    try {
        std::string orderBy;
        if (m_sortColumn >= 0 && m_sortColumn < m_columnNames.size()) {
            std::string colName = m_columnNames[m_sortColumn].toStdString();
            std::string orderStr = (m_sortOrder == Qt::AscendingOrder) ? "ASC" : "DESC";
            orderBy = "ORDER BY \"" + colName + "\" " + orderStr + " ";
        }

        std::string selectFields = m_hasRowId ? "rowid, *" : "*";
        std::string sql = "SELECT " + selectFields + " FROM \"" + m_tableName.toStdString() + "\" " +
                          orderBy +
                          "LIMIT " + std::to_string(limit) + " " +
                          "OFFSET " + std::to_string(offset);

        auto result = m_conn->Query(sql);
        if (!result || result->HasError())
            return;

        int fetchedCount = 0;
        for (auto &row : *result) {
            if (m_hasRowId) {
                m_rowIds.append(row.GetValue<int64_t>(0));
            }
            QVector<QVariant> rowData;
            rowData.reserve(m_columnNames.size());
            int startCol = m_hasRowId ? 1 : 0;
            for (int c = startCol; c < startCol + m_columnNames.size(); ++c) {
                try {
                    auto val = row.GetValue<duckdb::Value>(c);
                    if (val.IsNull()) {
                        rowData.append(QVariant());
                    } else {
                        rowData.append(QString::fromStdString(val.ToString()));
                    }
                } catch (...) {
                    rowData.append(QVariant());
                }
            }
            m_data.append(std::move(rowData));
            ++fetchedCount;
        }

        if (fetchedCount < limit)
            m_allFetched = true;

    } catch (const std::exception &e) {
        qWarning() << "DuckDbModel::loadChunk failed:" << e.what();
    }
}

int DuckDbModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_data.size();
}

int DuckDbModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_columnNames.size();
}

QVariant DuckDbModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || (role != Qt::DisplayRole && role != Qt::EditRole))
        return {};

    if (index.row() >= m_data.size() || index.column() >= m_columnNames.size())
        return {};

    const QVariant &val = m_data[index.row()][index.column()];
    if (val.isNull() && role == Qt::DisplayRole)
        return QStringLiteral("NULL");
    return val;
}

QVariant DuckDbModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole || orientation != Qt::Horizontal)
        return {};
    if (section < 0 || section >= m_columnNames.size())
        return {};
    return m_columnNames[section];
}

Qt::ItemFlags DuckDbModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;
    Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (m_hasRowId)
        f |= Qt::ItemIsEditable;
    return f;
}

bool DuckDbModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (role != Qt::EditRole || !index.isValid() || !m_hasRowId)
        return false;

    if (index.row() >= m_data.size() || index.column() >= m_columnNames.size())
        return false;

    try {
        std::string colName = m_columnNames[index.column()].toStdString();
        std::string newVal = value.toString().toStdString();
        int64_t rowId = m_rowIds[index.row()];

        // Use rowid-based UPDATE
        std::string sql = "UPDATE \"" + m_tableName.toStdString() + "\" SET \"" +
            colName + "\" = '" + newVal + "' WHERE rowid = " +
            std::to_string(rowId);

        auto result = m_conn->Query(sql);
        if (result && !result->HasError()) {
            m_data[index.row()][index.column()] = value;
            emit dataChanged(index, index, {Qt::DisplayRole, Qt::EditRole});
            return true;
        }
    } catch (...) {}

    return false;
}

void DuckDbModel::sort(int column, Qt::SortOrder order)
{
    if (m_sortColumn == column && m_sortOrder == order)
        return;

    m_sortColumn = column;
    m_sortOrder = order;

    beginResetModel();
    m_data.clear();
    m_rowIds.clear();
    m_allFetched = false;
    loadChunk(0, kChunkSize);
    endResetModel();
}

void DuckDbModel::fetchMore(const QModelIndex &parent)
{
    if (parent.isValid() || m_allFetched)
        return;

    int currentSize = m_data.size();
    int toFetch = qMin(kChunkSize, m_totalRows - currentSize);
    if (toFetch <= 0) {
        m_allFetched = true;
        return;
    }

    beginInsertRows(QModelIndex(), currentSize, currentSize + toFetch - 1);
    loadChunk(currentSize, toFetch);
    endInsertRows();
}

bool DuckDbModel::canFetchMore(const QModelIndex &parent) const
{
    if (parent.isValid())
        return false;
    return !m_allFetched && m_data.size() < m_totalRows;
}
