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
    m_columnNames.clear();
    m_totalRows = 0;
    m_allFetched = false;

    try {
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
        std::string sql = "SELECT * FROM \"" + m_tableName.toStdString() + "\" "
                         "LIMIT " + std::to_string(limit) + " "
                         "OFFSET " + std::to_string(offset);

        auto result = m_conn->Query(sql);
        if (!result || result->HasError())
            return;

        int fetchedCount = 0;
        for (auto &row : *result) {
            QVector<QVariant> rowData;
            rowData.reserve(m_columnNames.size());
            for (int c = 0; c < m_columnNames.size(); ++c) {
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
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable;
}

bool DuckDbModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (role != Qt::EditRole || !index.isValid())
        return false;

    if (index.row() >= m_data.size() || index.column() >= m_columnNames.size())
        return false;

    try {
        std::string colName = m_columnNames[index.column()].toStdString();
        std::string newVal = value.toString().toStdString();

        // Use rowid-based UPDATE
        std::string sql = "UPDATE \"" + m_tableName.toStdString() + "\" SET \"" +
            colName + "\" = '" + newVal + "' WHERE rowid = " +
            std::to_string(index.row() + 1);

        auto result = m_conn->Query(sql);
        if (result && !result->HasError()) {
            m_data[index.row()][index.column()] = value;
            emit dataChanged(index, index, {Qt::DisplayRole, Qt::EditRole});
            return true;
        }
    } catch (...) {}

    return false;
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
