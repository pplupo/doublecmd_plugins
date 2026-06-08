#include "LevelDbEngine.h"
#include "KeyValueModel.h"

#include <leveldb/db.h>
#include <leveldb/iterator.h>

#include <QFileInfo>
#include <QFile>

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

    // Resolve directory: if user selected a file inside a LevelDB dir, use parent
    QFileInfo info(filepath);
    QString dbPath = info.isDir() ? filepath : info.absolutePath();

    // Verify it's a LevelDB directory by checking for CURRENT file
    if (!QFile::exists(dbPath + QStringLiteral("/CURRENT")))
        return false;

    leveldb::Options options;
    options.create_if_missing = false;

    leveldb::DB *db = nullptr;
    leveldb::Status status = leveldb::DB::Open(options, dbPath.toStdString(), &db);
    if (!status.ok() || !db)
        return false;

    m_db = db;
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
    leveldb::Iterator *it = m_db->NewIterator(leveldb::ReadOptions());
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

    leveldb::DB *db = m_db;

    KeyValueModel::IteratorOps ops;

    ops.fetchWindow = [db](int startIndex, int count) -> QVector<KeyValueModel::Entry> {
        QVector<KeyValueModel::Entry> entries;
        entries.reserve(count);

        leveldb::Iterator *it = db->NewIterator(leveldb::ReadOptions());
        it->SeekToFirst();

        // Skip to startIndex
        for (int i = 0; i < startIndex && it->Valid(); ++i)
            it->Next();

        // Collect entries
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
        leveldb::Slice k(key.constData(), key.size());
        leveldb::Slice v(value.constData(), value.size());
        leveldb::Status s = db->Put(leveldb::WriteOptions(), k, v);
        return s.ok();
    };

    ops.deleteKey = [db](const QByteArray &key) -> bool {
        leveldb::Slice k(key.constData(), key.size());
        leveldb::Status s = db->Delete(leveldb::WriteOptions(), k);
        return s.ok();
    };

    m_model = new KeyValueModel(m_keyCount, std::move(ops), this);
    return m_model;
}
