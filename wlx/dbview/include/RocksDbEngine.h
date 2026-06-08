#pragma once

#include "DbEngine.h"
#include <memory>

namespace ROCKSDB_NAMESPACE { class DB; }

/// RocksDB engine: opens RocksDB directories as Key-Value stores.
///
/// Functionally identical to LevelDbEngine but uses the RocksDB API.
/// Only compiled when ENABLE_ROCKSDB=ON is set in CMake.
///
/// Like LevelDB, resolves file paths to their parent directory
/// and checks for a CURRENT file to verify validity.
class RocksDbEngine : public DbEngine {
    Q_OBJECT
public:
    explicit RocksDbEngine(QObject *parent = nullptr);
    ~RocksDbEngine() override;

    bool open(const QString &filepath) override;
    void close() override;

    QStringList tableNames() const override;
    QAbstractItemModel *modelForTable(const QString &tableName) override;
    QString currentTableName() const override;

    bool supportsMultipleTables() const override { return false; }
    bool supportsSubmitRevert() const override { return false; }
    QString engineName() const override { return QStringLiteral("RocksDB"); }

private:
    int countKeys() const;

    ROCKSDB_NAMESPACE::DB *m_db = nullptr;
    int m_keyCount = 0;
    QAbstractItemModel *m_model = nullptr;
};
