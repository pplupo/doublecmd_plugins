#pragma once

#include <cstdint>
#include <string>
#include <vector>

/// Toolkit-free description of an archive's contents.
///
/// Everything below the UI is expressed in these types so that one
/// implementation serves both the Qt6 and GTK3 variants — the ZIP
/// central-directory parser and the extraction path checks are
/// security-relevant, and two copies of those would eventually be two
/// behaviours.
namespace archiveview {

/// One member of an archive.
///
/// Deliberately a plain value type: entries are produced on a worker thread
/// and handed to the UI thread in batches, so they must be copyable and must
/// not reference libarchive state (the `struct archive_entry*` libarchive
/// hands out is only valid until the next call to archive_read_next_header).
struct Entry {
    enum class Type { File, Directory, Symlink, Hardlink, Other };

    /// Full path inside the archive, '/'-separated, UTF-8, no leading "./".
    std::string path;
    /// Target of a symlink/hardlink, empty otherwise.
    std::string linkTarget;
    std::string owner;
    std::string group;

    /// Uncompressed size, or -1 when the archive does not record one.
    /// libarchive returns int64_t here and legitimately reports "unset" for
    /// streamed zip entries — this must never be cast to an unsigned type.
    int64_t size = -1;
    /// Compressed size. libarchive exposes no per-entry compressed size;
    /// this is filled in from the ZIP central directory where available and
    /// stays -1 for every other format.
    int64_t compressedSize = -1;

    uint32_t crc32 = 0;
    bool hasCrc = false;

    /// Seconds since the epoch; `hasModified` false when the archive records
    /// no timestamp, which is distinct from a timestamp of zero.
    int64_t modified = 0;
    bool hasModified = false;

    uint32_t mode = 0;

    Type type = Type::File;
    bool encrypted = false;
    bool metadataEncrypted = false;

    bool isDir() const { return type == Type::Directory; }
};

using EntryBatch = std::vector<Entry>;

/// Whole-archive facts, known only once the walk completes.
struct Summary {
    std::string format;   ///< e.g. "ZIP 2.0 (deflate)"
    std::string filters;  ///< e.g. "xz" — decompression filter chain
    /// Archive-level comment. ZIP only, read from the End of Central
    /// Directory record, because libarchive exposes no comment API.
    std::string comment;

    int64_t entryCount = 0;
    int64_t totalUncompressed = 0;
    /// The archive file's own size. Note this is deliberately not
    /// archive_filter_bytes(), which counts bytes *consumed* — the walk skips
    /// entry bodies, so for a seekable zip that lands well under the truth.
    int64_t compressedBytes = 0;
    /// Sum of per-entry compressed sizes, where the format records them.
    int64_t totalCompressedEntries = 0;

    bool zip64 = false;
    bool truncated = false;   ///< hit the configured entry ceiling
    bool cancelled = false;
    bool hasEncryptedEntries = false;
    /// The archive wanted a passphrase and did not get a usable one, so the
    /// listing is whatever could be read without it.
    bool passphraseDeclined = false;
};

} // namespace archiveview
