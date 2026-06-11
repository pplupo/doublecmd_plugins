#pragma once

#include "DbEngine.h"
#include <memory>

namespace leveldb { class DB; }

/// LevelDB engine: opens LevelDB directories as Key-Value stores.
///
/// When the user selects a file inside a LevelDB directory (e.g. .sst, .ldb),
/// the engine resolves the parent directory and opens the entire DB.
/// Verifies the presence of a CURRENT file to confirm it's a valid LevelDB.
///
/// Returns a KeyValueModel with 2 columns: Key, Value.
class LevelDbEngine : public DbEngine {
    Q_OBJECT
public:
    explicit LevelDbEngine(QObject *parent = nullptr);
    ~LevelDbEngine() override;

    bool open(const QString &filepath) override;
    void close() override;

    QStringList tableNames() const override;
    QAbstractItemModel *modelForTable(const QString &tableName) override;
    QString currentTableName() const override;

    bool supportsMultipleTables() const override { return false; }
    bool supportsSubmitRevert() const override { return false; }
    QString engineName() const override { return QStringLiteral("LevelDB"); }

private:
    int countKeys() const;

    leveldb::DB *m_db = nullptr;
    int m_keyCount = 0;
    QAbstractItemModel *m_model = nullptr;
};
