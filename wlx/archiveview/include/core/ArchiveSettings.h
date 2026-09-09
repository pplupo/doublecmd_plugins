#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace archiveview {

/// Persistent options, read from the .ini path DC hands over through
/// ListSetDefaultParams — the same idiom the other plugins in this repository
/// follow.
///
/// Parsed by hand rather than with QSettings or GKeyFile so that one
/// implementation serves both toolkits, and so the two variants cannot drift
/// into reading the same file differently.
///
/// There is no settings dialog: this is a viewer, and the repo's convention
/// for these plugins is a hand-edited ini section.
struct Settings {
    /// Section name inside the shared plugin ini.
    static constexpr const char *kSection = "archiveview";

    bool startFlat = false;         ///< open in flat mode instead of a tree
    bool showFilterBox = true;
    int64_t maxEntries = 500000;    ///< ceiling before the listing is truncated

    /// Codec for member names that are neither valid UTF-8 nor decodable in
    /// the current locale — e.g. "windows-1251" or "Shift-JIS" for archives
    /// from a machine that predates UTF-8. Empty means the lossless Latin-1
    /// fallback, which preserves the bytes but shows the wrong glyphs.
    std::string nameCodec;

    /// Columns hidden by default, by header name (case-insensitive).
    std::vector<std::string> hiddenColumns;

    /// Load from `iniPath`; returns defaults when the path is empty or the
    /// file does not exist.
    static Settings load(const std::string &iniPath);
};

} // namespace archiveview
