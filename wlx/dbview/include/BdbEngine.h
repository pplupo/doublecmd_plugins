#pragma once

#include "DbEngine.h"
#include <db.h>

class BdbEngine : public DbEngine {
    Q_OBJECT
public:
    explicit BdbEngine(QObject *parent = nullptr);
    ~BdbEngine() override;

    bool open(const QString &filepath) override;
    void close() override;

    QStringList tableNames() const override;
    QAbstractItemModel *modelForTable(const QString &tableName) override;
    QString currentTableName() const override;

    bool supportsMultipleTables() const override { return false; }
    bool supportsSubmitRevert() const override { return false; }
    QString engineName() const override { return QStringLiteral("Berkeley DB"); }

private:
    int countKeys() const;

    DB *m_db = nullptr;
    int m_keyCount = 0;
    QAbstractItemModel *m_model = nullptr;
};
