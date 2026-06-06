#pragma once

#include <QString>
#include <QByteArray>
#include <QTableWidget>
#include <memory>

/// Abstract base class for structured text format engines.
///
/// Each engine knows how to:
///  1. Parse raw file bytes into a QTableWidget grid layout
///  2. Serialize the current grid state back to bytes for saving
///
/// The factory method createForFile() inspects the file extension and
/// returns the appropriate engine.  Adding a new format is a single
/// subclass + one line in the factory.
class TextFormatEngine {
public:
    virtual ~TextFormatEngine() = default;

    /// Parse file bytes into the table model.
    /// Returns false on parse error.
    virtual bool loadInto(QTableWidget *table, const QByteArray &data) = 0;

    /// Serialize current table state back to bytes for saving.
    virtual QByteArray serialize(const QTableWidget *table) const = 0;

    /// Human-readable format name for toolbar display (e.g. "JSON", "XML").
    virtual QString formatName() const = 0;

    /// Whether this engine uses a section navigation panel (e.g. INI).
    /// When true, StructViewWidget creates a split layout with a section
    /// list on the left instead of a full-width grid.
    virtual bool hasSectionNav() const { return false; }

    /// Return section names for engines that use section navigation.
    /// Default returns empty list.
    virtual QStringList sectionNames() const { return {}; }

    /// Load a specific section's data into the table.
    /// Only meaningful when hasSectionNav() returns true.
    virtual bool loadSection(QTableWidget *table, const QString &section) { Q_UNUSED(table); Q_UNUSED(section); return false; }

    /// Update internal data from the current table state for the given section.
    /// Called before switching sections to preserve edits.
    virtual void commitSection(const QTableWidget *table, const QString &section) { Q_UNUSED(table); Q_UNUSED(section); }

    /// Factory: detect format from file extension and return the right engine.
    static std::unique_ptr<TextFormatEngine> createForFile(const QString &filepath);
};
