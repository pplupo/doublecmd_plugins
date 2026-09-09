#pragma once

#include <QString>
#include <QStringList>

/// Persistent options, stored in the .ini path DC hands over through
/// ListSetDefaultParams — the same idiom the other Qt plugins in this
/// repository follow (see htmlconv_qt_crap, csvview).
///
/// Everything here is read once per widget construction. There is no settings
/// dialog: this is a viewer, and the repo's convention for these plugins is a
/// hand-edited ini section.
struct ArchiveSettings {
    /// Section name inside the shared plugin ini.
    static constexpr const char *kSection = "archiveview";

    bool startFlat = false;         ///< open in flat mode instead of a tree
    bool showDetailPanel = true;    ///< the per-entry panel on the right
    bool showFilterBox = true;
    qint64 maxEntries = 500000;     ///< ceiling before the listing is truncated

    /// Codec used for member names that are neither valid UTF-8 nor
    /// decodable in the current locale — e.g. "windows-1251" or "Shift-JIS"
    /// for archives from a machine that predates UTF-8. Empty means the
    /// lossless Latin-1 fallback, which preserves the bytes but shows the
    /// wrong glyphs.
    QString nameCodec;

    /// Columns hidden by default, by header name (case-insensitive).
    QStringList hiddenColumns;

    /// Load from `iniPath`; returns defaults when the path is empty or the
    /// file does not exist.
    static ArchiveSettings load(const QString &iniPath);
};

/// The ini path DC supplied through ListSetDefaultParams. Empty in the test
/// harnesses, which is why load() has to tolerate that.
const QString &archiveviewIniPath();
