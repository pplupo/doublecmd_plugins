#pragma once

#include <QString>
#include <QDateTime>
#include <QVector>
#include <QMetaType>

/// One member of an archive, as produced by ArchiveScanner.
///
/// Deliberately a plain value type: entries are produced on the scanner
/// thread and shipped to the GUI thread in batches through a queued signal,
/// so they must be copyable and must not reference libarchive state (the
/// `struct archive_entry*` libarchive hands out is only valid until the next
/// call to archive_read_next_header).
struct ArchiveEntry {
    enum Type { File, Directory, Symlink, Hardlink, Other };

    /// Full path inside the archive, '/'-separated, no leading "./".
    QString path;
    /// Target of a symlink/hardlink, empty otherwise.
    QString linkTarget;
    QString owner;
    QString group;

    /// Uncompressed size, or -1 when the archive does not record one.
    /// libarchive returns int64_t here and legitimately reports "unset" for
    /// streamed zip entries — this must never be cast to an unsigned type.
    qint64 size = -1;
    /// Compressed size. libarchive exposes no per-entry compressed size;
    /// this is filled in from the ZIP central directory where available,
    /// and stays -1 for every other format.
    qint64 compressedSize = -1;

    quint32 crc32 = 0;
    bool hasCrc = false;

    /// Invalid when the archive records no mtime.
    QDateTime modified;
    quint32 mode = 0;

    Type type = File;
    bool encrypted = false;
    bool metadataEncrypted = false;

    bool isDir() const { return type == Directory; }
};

using ArchiveEntryBatch = QVector<ArchiveEntry>;

/// Whole-archive facts, known only once the walk completes.
struct ArchiveSummary {
    QString format;          ///< e.g. "ZIP 2.0 (deflate)"
    QString filters;         ///< e.g. "xz" — decompression filter chain
    qint64 entryCount = 0;
    qint64 totalUncompressed = 0;
    /// Bytes actually consumed from disk, from archive_filter_bytes(a, -1).
    /// This is a real compressed size, not a stat() of the file.
    qint64 compressedBytes = 0;
    /// Archive-level comment. ZIP only — read from the End of Central
    /// Directory record, because libarchive exposes no comment API.
    QString comment;
    /// Sum of per-entry compressed sizes, where the format records them.
    qint64 totalCompressedEntries = 0;
    bool zip64 = false;
    bool truncated = false;  ///< hit the configured entry ceiling
    bool cancelled = false;
    bool hasEncryptedEntries = false;
    /// The archive wanted a passphrase and did not get a usable one, so the
    /// listing is whatever could be read without it.
    bool passphraseDeclined = false;
};

// Both types cross a thread boundary through queued signals.
Q_DECLARE_METATYPE(ArchiveEntry)
Q_DECLARE_METATYPE(ArchiveSummary)
