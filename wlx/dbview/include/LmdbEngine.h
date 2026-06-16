#pragma once

#include "DbEngine.h"
#include <lmdb.h>
#undef mdb_open
#undef mdb_close

class LmdbEngine : public DbEngine {
    Q_OBJECT
public:
    explicit LmdbEngine(QObject *parent = nullptr);
    ~LmdbEngine() override;

    bool open(const QString &filepath) override;
    void close() override;

    QStringList tableNames() const override;
    QAbstractItemModel *modelForTable(const QString &tableName) override;
    QString currentTableName() const override;

    bool supportsMultipleTables() const override { return false; }
    bool supportsSubmitRevert() const override { return false; }
    QString engineName() const override { return QStringLiteral("LMDB"); }

private:
    int countKeys() const;

    MDB_env *m_env = nullptr;
    MDB_dbi m_dbi = 0;
    int m_keyCount = 0;
    QAbstractItemModel *m_model = nullptr;
};
