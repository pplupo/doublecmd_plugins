#include "TextFormatEngine.h"

#include <QSettings>
#include <QHeaderView>
#include <QFile>
#include <QFileInfo>

/// INI engine: uses QSettings to parse INI files.
///
/// Section navigation: hasSectionNav() returns true.
/// The StructViewWidget creates a section list on the left.
/// Each section shows a 2-column grid: Key | Value.
///
/// The engine maintains an internal QMap mirroring the full INI state,
/// which is updated via commitSection() before switching sections.
class IniEngine : public TextFormatEngine {
public:
    bool loadInto(QTableWidget *table, const QByteArray &data) override;
    QByteArray serialize(const QTableWidget *table) const override;
    QString formatName() const override { return QStringLiteral("INI"); }

    bool hasSectionNav() const override { return true; }
    QStringList sectionNames() const override { return m_sections; }
    bool loadSection(QTableWidget *table, const QString &section) override;
    void commitSection(const QTableWidget *table, const QString &section) override;

    // Public for factory access (createIniEngine populates these after construction)
    QString m_filepath;
    QStringList m_sections;
    // Section -> list of (key, value) pairs, preserving order
    QMap<QString, QList<QPair<QString, QString>>> m_data;
    QString m_activeSection;
};

bool IniEngine::loadInto(QTableWidget *table, const QByteArray &data)
{
    Q_UNUSED(data);
    // QSettings needs a file path — this is called after the file is written.
    // We'll be called with the filepath stored externally.
    // For initial load, we use the data to write a temp file if needed.
    // But since StructViewWidget passes the filepath, we store it and read directly.
    // The actual load happens in a deferred way — see StructViewWidget.
    // This stub returns true; real loading via loadFromFile().
    Q_UNUSED(table);
    return true;
}

bool IniEngine::loadSection(QTableWidget *table, const QString &section)
{
    if (!m_data.contains(section))
        return false;

    const auto &pairs = m_data[section];
    table->clear();
    table->setColumnCount(2);
    table->setHorizontalHeaderLabels({ QStringLiteral("Key"), QStringLiteral("Value") });
    table->setRowCount(pairs.size());

    for (int r = 0; r < pairs.size(); ++r) {
        table->setItem(r, 0, new QTableWidgetItem(pairs[r].first));
        table->setItem(r, 1, new QTableWidgetItem(pairs[r].second));
    }

    table->horizontalHeader()->setStretchLastSection(true);
    m_activeSection = section;
    return true;
}

void IniEngine::commitSection(const QTableWidget *table, const QString &section)
{
    if (section.isEmpty()) return;

    QList<QPair<QString, QString>> pairs;
    for (int r = 0; r < table->rowCount(); ++r) {
        QString key = table->item(r, 0) ? table->item(r, 0)->text() : QString();
        QString value = table->item(r, 1) ? table->item(r, 1)->text() : QString();
        if (!key.isEmpty())
            pairs.append({ key, value });
    }
    m_data[section] = pairs;
}

QByteArray IniEngine::serialize(const QTableWidget *table) const
{
    // Commit the currently active section before serializing
    // (caller should call commitSection() before serialize())
    Q_UNUSED(table);

    QByteArray output;
    for (const QString &section : m_sections) {
        if (!section.isEmpty()) {
            output += "[" + section.toUtf8() + "]\n";
        }
        const auto &pairs = m_data.value(section);
        for (const auto &pair : pairs) {
            output += pair.first.toUtf8() + "=" + pair.second.toUtf8() + "\n";
        }
        output += "\n";
    }
    return output;
}

// ---------------------------------------------------------------------------
// Factory method (shared across all engines)
// ---------------------------------------------------------------------------

#include <QFileInfo>

// Forward-declare engine classes (defined in their own .cpp files)
// We create them here because the factory lives in one translation unit.
// Each engine class is defined as a file-local class in its own .cpp,
// so we need to either:
//   a) put the factory in its own file, or
//   b) use a registration pattern.
// For simplicity, we define concrete header-less classes and instantiate here.
// The engine .cpp files define classes that inherit TextFormatEngine but
// since they are in separate TUs, we forward-declare creation functions.

// --- Engine creation functions (defined in each engine .cpp) ---
// We can't forward-declare file-local classes, so instead we'll put the
// factory in this file and include the necessary headers.
// Actually, the cleanest approach: put factory in its own small TU.

// Since the engine classes are defined only in their .cpp files (no separate headers),
// we use a different approach: each engine .cpp registers itself, or we
// use a simple factory here that knows about the engines.

// For this build, the simplest correct approach: the engines ARE the
// file-local classes in each .cpp. The factory below uses QFileInfo to
// pick the right one, instantiating the public class name.

// We'll restructure: JsonEngine, XmlEngine, IniEngine are forward-declared
// with creation functions. Let's just use a simple approach.

namespace {

// These are defined in their respective .cpp files as file-scope classes
// inheriting TextFormatEngine. We need to instantiate them from here.
// The cleanest way: declare thin factory helpers.

// Actually, since the classes are file-local, we can't reference them.
// The solution: make the factory live in each .cpp as a static registrar,
// or just make the factory dumb and have it here with forward headers.

// SIMPLEST FIX: Move factory to a standalone file that includes headers
// for each engine. But we don't have separate headers for engines.

// FINAL approach: The IniEngine.cpp file contains the factory because it's
// the last engine .cpp in the list. We instantiate using a switch.
// We need to know the concrete types → define minimal internal headers.

} // namespace

// We define a helper function for each engine in its own .cpp:
//   std::unique_ptr<TextFormatEngine> createJsonEngine();
//   std::unique_ptr<TextFormatEngine> createXmlEngine();
// And the IniEngine is defined right here.

extern std::unique_ptr<TextFormatEngine> createJsonEngine();
extern std::unique_ptr<TextFormatEngine> createXmlEngine();

static std::unique_ptr<TextFormatEngine> createIniEngine(const QString &filepath)
{
    auto engine = std::make_unique<IniEngine>();

    // Parse the INI file using QSettings
    QSettings settings(filepath, QSettings::IniFormat);

    QStringList sections;
    QMap<QString, QList<QPair<QString, QString>>> data;

    // General (no-section) keys
    QStringList generalKeys;
    for (const auto &key : settings.allKeys()) {
        if (!key.contains('/'))
            generalKeys.append(key);
    }
    if (!generalKeys.isEmpty()) {
        sections.append(QStringLiteral("General"));
        QList<QPair<QString, QString>> pairs;
        for (const auto &key : generalKeys)
            pairs.append({ key, settings.value(key).toString() });
        data[QStringLiteral("General")] = pairs;
    }

    // Named sections
    for (const auto &group : settings.childGroups()) {
        sections.append(group);
        settings.beginGroup(group);
        QList<QPair<QString, QString>> pairs;
        for (const auto &key : settings.childKeys())
            pairs.append({ key, settings.value(key).toString() });
        data[group] = pairs;
        settings.endGroup();
    }

    engine->m_sections = sections;
    engine->m_data = data;
    engine->m_filepath = filepath;

    return engine;
}

std::unique_ptr<TextFormatEngine> TextFormatEngine::createForFile(const QString &filepath)
{
    QString ext = QFileInfo(filepath).suffix().toLower();

    if (ext == QStringLiteral("json"))
        return createJsonEngine();
    if (ext == QStringLiteral("xml"))
        return createXmlEngine();
    if (ext == QStringLiteral("ini") || ext == QStringLiteral("cfg") || ext == QStringLiteral("conf"))
        return createIniEngine(filepath);
    // CBOR: stubbed for future
    // if (ext == "cbor") return createCborEngine();

    return nullptr;
}
