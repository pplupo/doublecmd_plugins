#include "DbEngine.h"

#include "SqliteEngine.h"
#include "DuckDbEngine.h"
#include "LevelDbEngine.h"

#ifdef ENABLE_ROCKSDB
#include "RocksDbEngine.h"
#endif

#include <QFileInfo>
#include <QFile>

std::unique_ptr<DbEngine> DbEngine::createForFile(const QString &filepath)
{
    QFileInfo info(filepath);
    QString ext = info.suffix().toLower();

    // --- SQL engines ---

    // SQLite
    if (ext == QStringLiteral("sqlite") || ext == QStringLiteral("sqlite3")
        || ext == QStringLiteral("db") || ext == QStringLiteral("db3")) {
        auto engine = std::make_unique<SqliteEngine>();
        if (engine->open(filepath)) {
            if (!engine->tableNames().isEmpty() || !engine->viewNames().isEmpty())
                return engine;
        }
        // Fallback: might be a DuckDB file with .db extension
        auto duckEngine = std::make_unique<DuckDbEngine>();
        if (duckEngine->open(filepath)) {
            if (!duckEngine->tableNames().isEmpty() || !duckEngine->viewNames().isEmpty())
                return duckEngine;
        }
        // If neither has tables, return SQLite anyway as default
        auto fallbackEngine = std::make_unique<SqliteEngine>();
        if (fallbackEngine->open(filepath))
            return fallbackEngine;
    }

    // DuckDB
    if (ext == QStringLiteral("duckdb")) {
        auto engine = std::make_unique<DuckDbEngine>();
        if (engine->open(filepath))
            return engine;
    }

    // --- KV engines ---

    // LevelDB: files inside a LevelDB directory
    if (ext == QStringLiteral("ldb") || ext == QStringLiteral("sst")
        || ext == QStringLiteral("log")) {
        // Check if parent dir looks like a LevelDB directory
        QString parentDir = info.absolutePath();
        if (QFile::exists(parentDir + QStringLiteral("/CURRENT"))) {
            auto engine = std::make_unique<LevelDbEngine>();
            if (engine->open(filepath))
                return engine;
        }
    }

#ifdef ENABLE_ROCKSDB
    // RocksDB: same file extensions as LevelDB, try if LevelDB failed
    if (ext == QStringLiteral("sst") || ext == QStringLiteral("log")) {
        QString parentDir = info.absolutePath();
        if (QFile::exists(parentDir + QStringLiteral("/CURRENT"))) {
            auto engine = std::make_unique<RocksDbEngine>();
            if (engine->open(filepath))
                return engine;
        }
    }
#endif

    // --- Fallback: try SQLite for any unknown extension ---
    {
        auto engine = std::make_unique<SqliteEngine>();
        if (engine->open(filepath)) {
            // Check if it actually has tables
            if (!engine->tableNames().isEmpty() || !engine->viewNames().isEmpty())
                return engine;
        }
    }

    return nullptr;
}
