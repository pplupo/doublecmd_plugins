#ifdef ENABLE_ROCKSDB_LEVELDB

#include "LevelDbEngine.h"
#include "KeyValueModel.h"

#include <rocksdb/db.h>
#include <rocksdb/iterator.h>

#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QStandardPaths>

namespace {
class RocksDbNoOpLogger : public rocksdb::Logger {
public:
    using rocksdb::Logger::Logv;
    void Logv(const char* format, va_list ap) override {
        Q_UNUSED(format);
        Q_UNUSED(ap);
    }
};
}

LevelDbEngine::LevelDbEngine(QObject *parent)
    : DbEngine(parent)
{
}

LevelDbEngine::~LevelDbEngine()
{
    close();
}

bool LevelDbEngine::open(const QString &filepath)
{
    close();

    QFileInfo info(filepath);
    QString dbPath = info.isDir() ? filepath : info.absolutePath();

    if (!QFile::exists(dbPath + QStringLiteral("/CURRENT")))
        return false;

    rocksdb::Options options;
    options.create_if_missing = false;
    // No-op logger suppresses LOG file writes in the DB directory
    options.info_log = std::make_shared<RocksDbNoOpLogger>();
    // Redirect info log dir away from the watched DB directory
    QString tmpDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
                     + QStringLiteral("/dbview_logs");
    QDir().mkpath(tmpDir);
    options.db_log_dir = tmpDir.toStdString();

    rocksdb::DB *db = nullptr;
    rocksdb::Status status = rocksdb::DB::OpenForReadOnly(options, dbPath.toStdString(), &db);
    if (!status.ok() || !db)
        return false;

    m_db = db;
    m_readOnly = true;
    m_keyCount = countKeys();
    return true;
}

void LevelDbEngine::close()
{
    delete m_model;
    m_model = nullptr;

    delete m_db;
    m_db = nullptr;

    m_keyCount = 0;
}

int LevelDbEngine::countKeys() const
{
    if (!m_db) return 0;

    int count = 0;
    rocksdb::Iterator *it = m_db->NewIterator(rocksdb::ReadOptions());
    for (it->SeekToFirst(); it->Valid(); it->Next())
        ++count;
    delete it;
    return count;
}

QStringList LevelDbEngine::tableNames() const
{
    return { QStringLiteral("<keys>") };
}

QString LevelDbEngine::currentTableName() const
{
    return QStringLiteral("<keys>");
}

QAbstractItemModel *LevelDbEngine::modelForTable(const QString &tableName)
{
    Q_UNUSED(tableName);
    if (!m_db) return nullptr;

    delete m_model;
    m_model = nullptr;

    rocksdb::DB *db = m_db;

    KeyValueModel::IteratorOps ops;

    ops.fetchWindow = [db](int startIndex, int count) -> QVector<KeyValueModel::Entry> {
        QVector<KeyValueModel::Entry> entries;
        entries.reserve(count);

        rocksdb::Iterator *it = db->NewIterator(rocksdb::ReadOptions());
        it->SeekToFirst();

        for (int i = 0; i < startIndex && it->Valid(); ++i)
            it->Next();

        for (int i = 0; i < count && it->Valid(); ++i) {
            KeyValueModel::Entry e;
            e.key = QByteArray(it->key().data(), static_cast<int>(it->key().size()));
            e.value = QByteArray(it->value().data(), static_cast<int>(it->value().size()));
            entries.append(std::move(e));
            it->Next();
        }

        delete it;
        return entries;
    };

    ops.putValue = [db](const QByteArray &key, const QByteArray &value) -> bool {
        rocksdb::Slice k(key.constData(), key.size());
        rocksdb::Slice v(value.constData(), value.size());
        rocksdb::Status s = db->Put(rocksdb::WriteOptions(), k, v);
        return s.ok();
    };

    ops.deleteKey = [db](const QByteArray &key) -> bool {
        rocksdb::Slice k(key.constData(), key.size());
        rocksdb::Status s = db->Delete(rocksdb::WriteOptions(), k);
        return s.ok();
    };

    m_model = new KeyValueModel(m_keyCount, std::move(ops), this);
    return m_model;
}

#endif // ENABLE_ROCKSDB_LEVELDB
