#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace archiveview {

/// Reads a ZIP file's End of Central Directory record and central directory.
///
/// Why this exists at all: libarchive exposes **no archive comment**, **no
/// per-entry compressed size**, and **no stored CRC**. The plugin this one
/// replaces got the comment by running
///     /bin/sh -c "7z l <filename> | pcregrep ..."
/// with the previewed file's name spliced into the command string, which made
/// previewing a file named x$(...).zip arbitrary code execution — and it
/// still had no packed size or CRC to show.
///
/// All three live in a structure at a known offset in the file. Parsing it
/// needs no subprocess and works identically on every machine, as against
/// regex-scraping the human-readable, locale-translated, version-dependent
/// output of whatever `7z` happens to be installed.
///
/// Everything here treats the file as hostile: every length is bounds-checked
/// against what was actually read, and a malformed record ends the parse
/// instead of walking off the buffer.
class ZipCentralDirectory {
public:
    struct EntryDetail {
        int64_t compressedSize = -1;
        int64_t uncompressedSize = -1;
        uint32_t crc32 = 0;
        bool hasCrc = false;
        bool encrypted = false;   ///< general purpose bit 0
    };

    /// Parse `archivePath`. Returns false when the file is not a ZIP, has no
    /// locatable End of Central Directory record, or is too damaged to walk.
    /// A false return is not an error worth surfacing — most archives are not
    /// ZIPs, and the columns this fills are simply left blank.
    bool read(const std::string &archivePath);

    /// The archive-level comment, empty when there is none.
    const std::string &comment() const { return m_comment; }

    /// Details for `path`, which must already be normalised through
    /// names::normalize().
    ///
    /// `occurrence` disambiguates duplicate paths: a ZIP may legitimately
    /// store two members under one name, and the central directory lists both
    /// in the same order libarchive walks them.
    const EntryDetail *detailFor(const std::string &path, int occurrence = 0) const;

    int entryCount() const { return m_entryCount; }
    bool isZip64() const { return m_zip64; }

private:
    std::unordered_map<std::string, std::vector<EntryDetail>> m_details;
    std::string m_comment;
    int m_entryCount = 0;
    bool m_zip64 = false;
};

} // namespace archiveview
