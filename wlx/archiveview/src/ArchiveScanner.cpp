#include "ArchiveScanner.h"

#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>

#include "ArchiveNames.h"
#include "ZipCentralDirectory.h"

#include <archive.h>
#include <archive_entry.h>

#include <cerrno>

namespace {

/// Everything the libarchive read callbacks need. Lives on the scanner
/// thread's stack for the duration of the walk.
struct ReadState {
    QFile file;
    QByteArray buffer;
    std::atomic<bool> *cancel = nullptr;
};

constexpr int kReadBlockSize = 256 * 1024;

int openCallback(struct archive *, void *)
{
    // The file is already open by the time archive_read_open2() is called;
    // opening here would only duplicate the error handling.
    return ARCHIVE_OK;
}

la_ssize_t readCallback(struct archive *a, void *clientData, const void **buffer)
{
    auto *state = static_cast<ReadState *>(clientData);

    // The cancellation point. Returning a fatal error here unwinds whatever
    // decompressor is currently blocked on us, which a flag checked between
    // entries cannot do.
    if (state->cancel->load(std::memory_order_relaxed)) {
        archive_set_error(a, ECANCELED, "scan cancelled");
        return -1;
    }

    const qint64 n = state->file.read(state->buffer.data(), state->buffer.size());
    if (n < 0) {
        archive_set_error(a, EIO, "%s", qUtf8Printable(state->file.errorString()));
        return -1;
    }

    *buffer = state->buffer.constData();
    return static_cast<la_ssize_t>(n);
}

la_int64_t skipCallback(struct archive *a, void *clientData, la_int64_t request)
{
    auto *state = static_cast<ReadState *>(clientData);

    if (state->cancel->load(std::memory_order_relaxed)) {
        archive_set_error(a, ECANCELED, "scan cancelled");
        return -1;
    }

    const qint64 pos = state->file.pos();
    const qint64 target = pos + request;
    if (target > state->file.size() || !state->file.seek(target))
        return 0;  // "cannot skip" — libarchive falls back to reading

    return request;
}

la_int64_t seekCallback(struct archive *a, void *clientData,
                        la_int64_t offset, int whence)
{
    auto *state = static_cast<ReadState *>(clientData);

    if (state->cancel->load(std::memory_order_relaxed)) {
        archive_set_error(a, ECANCELED, "scan cancelled");
        return -1;
    }

    qint64 target = 0;
    switch (whence) {
    case SEEK_SET: target = offset; break;
    case SEEK_CUR: target = state->file.pos() + offset; break;
    case SEEK_END: target = state->file.size() + offset; break;
    default:
        archive_set_error(a, EINVAL, "bad seek origin");
        return -1;
    }

    if (target < 0 || !state->file.seek(target)) {
        archive_set_error(a, EIO, "seek failed");
        return -1;
    }
    return state->file.pos();
}

int closeCallback(struct archive *, void *clientData)
{
    static_cast<ReadState *>(clientData)->file.close();
    return ARCHIVE_OK;
}

/// Adapt libarchive's two name accessors onto ArchiveNames::decode().
/// Either can return NULL: the *_utf8 form when the name is not
/// representable, the raw form when the locale conversion fails.
QString decodeName(const char *utf8Name, const char *rawName)
{
    if (utf8Name && *utf8Name)
        return QString::fromUtf8(utf8Name);
    if (!rawName || !*rawName)
        return QString();
    return ArchiveNames::decode(QByteArray(rawName));
}

ArchiveEntry::Type entryType(struct archive_entry *entry)
{
    if (archive_entry_hardlink(entry) || archive_entry_hardlink_utf8(entry))
        return ArchiveEntry::Hardlink;

    switch (archive_entry_filetype(entry)) {
    case AE_IFDIR: return ArchiveEntry::Directory;
    case AE_IFLNK: return ArchiveEntry::Symlink;
    case AE_IFREG: return ArchiveEntry::File;
    default:       return ArchiveEntry::Other;
    }
}

QString filterChain(struct archive *a)
{
    QStringList names;
    // archive_filter_count() is documented as >= 1 (index 0 is the format
    // reader itself), but index 0 is "none" for an uncompressed archive and
    // is not worth showing.
    for (int i = archive_filter_count(a) - 1; i >= 0; --i) {
        const char *name = archive_filter_name(a, i);
        if (name && qstrcmp(name, "none") != 0)
            names << QString::fromUtf8(name);
    }
    return names.join(QLatin1String(" → "));
}

} // namespace

ArchiveScanner::ArchiveScanner(QObject *parent)
    : QThread(parent)
{
}

ArchiveScanner::~ArchiveScanner()
{
    cancelAndWait();
}

bool ArchiveScanner::canRead(const QString &archivePath)
{
    std::atomic<bool> neverCancel{false};

    ReadState state;
    state.cancel = &neverCancel;
    state.buffer.resize(kReadBlockSize);
    state.file.setFileName(archivePath);
    if (!state.file.open(QIODevice::ReadOnly))
        return false;

    struct archive *a = archive_read_new();
    if (!a)
        return false;

    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);

    // Deliberately no seek callback here, unlike the real scan. With one,
    // libarchive's seeking zip reader parses the entire central directory
    // before yielding the first header — measured at 122 ms for a
    // 100k-entry archive, all of it on DC's UI thread inside ListLoad. The
    // streaming reader identifies the format from the first local header
    // instead. Fidelity does not matter for a yes/no answer; latency does.
    bool recognised = false;
    if (archive_read_open2(a, &state, openCallback, readCallback,
                           skipCallback, closeCallback) == ARCHIVE_OK) {
        struct archive_entry *entry = nullptr;
        recognised = archive_read_next_header(a, &entry) >= ARCHIVE_WARN
                  || archive_format(a) != 0;
    }

    archive_read_free(a);
    return recognised;
}

void ArchiveScanner::scan(const QString &archivePath, qint64 maxEntries)
{
    m_path = archivePath;
    m_maxEntries = maxEntries > 0 ? maxEntries : kDefaultMaxEntries;
    m_cancel.store(false, std::memory_order_relaxed);
    start();
}

void ArchiveScanner::cancel()
{
    m_cancel.store(true, std::memory_order_relaxed);
    // Wake a scanner thread parked on the passphrase prompt, so cancellation
    // is honoured even while a dialog is on screen.
    m_broker.wake();
}

void ArchiveScanner::providePassphrase(const QString &passphrase, bool accepted)
{
    m_broker.provide(passphrase, accepted);
}

void ArchiveScanner::setPassphrase(const QString &passphrase)
{
    m_broker.preset(passphrase);
}

const char *ArchiveScanner::passphraseTrampoline(struct archive *, void *client)
{
    return static_cast<ArchiveScanner *>(client)->requestPassphrase();
}

const char *ArchiveScanner::requestPassphrase()
{
    return m_broker.request(
        [this](int attempt) { emit passphraseRequested(attempt); }, m_cancel);
}

void ArchiveScanner::cancelAndWait()
{
    cancel();
    if (isRunning())
        wait();
}

void ArchiveScanner::run()
{
    ArchiveSummary summary;

    ReadState state;
    state.cancel = &m_cancel;
    state.buffer.resize(kReadBlockSize);
    state.file.setFileName(m_path);

    if (!state.file.open(QIODevice::ReadOnly)) {
        emit scanFinished(false, state.file.errorString(), summary);
        return;
    }

    const qint64 totalBytes = state.file.size();

    struct archive *a = archive_read_new();
    if (!a) {
        // The original plugin passed this straight into
        // archive_read_support_filter_all() without checking.
        emit scanFinished(false, tr("Out of memory allocating archive reader"), summary);
        return;
    }

    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);

    // Without a seek callback libarchive uses its *streaming* zip reader,
    // which only ever sees local file headers — so symlinks come back as
    // regular files whose contents are the target path, and the central
    // directory (where the real metadata lives) is never consulted.
    // Registering this switches on the seeking reader.
    archive_read_set_seek_callback(a, seekCallback);

    // Without this an encrypted archive fails outright with "Passphrase
    // required" and shows nothing at all.
    archive_read_set_passphrase_callback(a, this, passphraseTrampoline);

    if (archive_read_open2(a, &state, openCallback, readCallback,
                           skipCallback, closeCallback) != ARCHIVE_OK) {
        const QString error = QString::fromUtf8(archive_error_string(a));
        archive_read_free(a);
        emit scanFinished(false, error, summary);
        return;
    }

    // The ZIP central directory carries three things libarchive does not
    // expose at all: the archive comment, per-entry compressed size, and the
    // stored CRC. Parsed here, on the scanner thread, before the walk — it is
    // a bounded read of one structure, not a second pass over the archive.
    // Returns false for every non-ZIP, which just leaves those columns blank.
    ZipCentralDirectory zipDirectory;
    if (zipDirectory.read(m_path)) {
        summary.zip64 = zipDirectory.isZip64();
        if (!zipDirectory.comment().isEmpty()) {
            summary.comment = zipDirectory.comment();
            emit commentFound(summary.comment);
        }
    }
    // Duplicate member paths are legal in ZIP, and the central directory
    // lists them in the same order libarchive walks them — so occurrences are
    // matched positionally rather than collapsed onto one another.
    QHash<QString, int> zipOccurrences;

    bool formatReported = false;
    bool ok = true;
    QString error;

    ArchiveEntryBatch batch;
    batch.reserve(kBatchSize);

    QElapsedTimer sinceLastFlush;
    sinceLastFlush.start();

    struct archive_entry *entry = nullptr;
    for (;;) {
        const int rc = archive_read_next_header(a, &entry);

        if (rc == ARCHIVE_EOF)
            break;

        if (rc < ARCHIVE_WARN) {
            if (m_cancel.load(std::memory_order_relaxed)) {
                summary.cancelled = true;
            } else {
                ok = false;
                error = QString::fromUtf8(archive_error_string(a));
            }
            break;
        }

        if (!formatReported) {
            formatReported = true;
            const char *format = archive_format_name(a);
            summary.format = format ? QString::fromUtf8(format) : tr("unknown");
            summary.filters = filterChain(a);
            emit formatDetected(summary.format, summary.filters);
        }

        ArchiveEntry item;
        item.path = ArchiveNames::normalize(
            decodeName(archive_entry_pathname_utf8(entry),
                       archive_entry_pathname(entry)));
        if (item.path.isEmpty())
            continue;  // unnameable member — nothing useful to show

        item.type = entryType(entry);

        if (item.type == ArchiveEntry::Symlink) {
            item.linkTarget = decodeName(archive_entry_symlink_utf8(entry),
                                         archive_entry_symlink(entry));
        } else if (item.type == ArchiveEntry::Hardlink) {
            item.linkTarget = decodeName(archive_entry_hardlink_utf8(entry),
                                         archive_entry_hardlink(entry));
        }

        // Guard the classic bug: archive_entry_size() is int64_t and is -1
        // or 0 when unset. Casting to size_t turns -1 into SIZE_MAX and
        // wraps every running total that touches it.
        if (archive_entry_size_is_set(entry)) {
            const la_int64_t size = archive_entry_size(entry);
            if (size >= 0) {
                item.size = static_cast<qint64>(size);
                summary.totalUncompressed += item.size;
            }
        }

        if (const auto *detail =
                zipDirectory.detailFor(item.path, zipOccurrences[item.path]++)) {
            item.compressedSize = detail->compressedSize;
            item.crc32 = detail->crc32;
            item.hasCrc = detail->hasCrc;
            // The central directory's encryption bit is authoritative for
            // ZIP; libarchive's per-entry flags do not report it for every
            // variant.
            if (detail->encrypted)
                item.encrypted = true;
            // Trust the directory's uncompressed size when libarchive left
            // it unset (streamed entries carry no size in the local header).
            if (item.size < 0 && detail->uncompressedSize >= 0) {
                item.size = detail->uncompressedSize;
                summary.totalUncompressed += item.size;
            }
            if (item.compressedSize >= 0)
                summary.totalCompressedEntries += item.compressedSize;
        }

        if (archive_entry_mtime_is_set(entry))
            item.modified = QDateTime::fromSecsSinceEpoch(archive_entry_mtime(entry));

        if (archive_entry_perm_is_set(entry))
            item.mode = archive_entry_perm(entry);

        if (const char *owner = archive_entry_uname_utf8(entry))
            item.owner = QString::fromUtf8(owner);
        if (const char *group = archive_entry_gname_utf8(entry))
            item.group = QString::fromUtf8(group);

        item.encrypted = archive_entry_is_data_encrypted(entry) != 0;
        item.metadataEncrypted = archive_entry_is_metadata_encrypted(entry) != 0;
        if (item.encrypted || item.metadataEncrypted)
            summary.hasEncryptedEntries = true;

        batch.append(item);
        ++summary.entryCount;

        // Flush on batch size, or on elapsed time so a slow archive still
        // populates the view progressively instead of in silent chunks.
        if (batch.size() >= kBatchSize || sinceLastFlush.elapsed() > 100) {
            emit entriesReady(batch);
            emit progress(state.file.pos(), totalBytes);
            batch.clear();
            batch.reserve(kBatchSize);
            sinceLastFlush.restart();
        }

        if (summary.entryCount >= m_maxEntries) {
            summary.truncated = true;
            break;
        }

        // Skipping the body is what keeps the walk cheap for seekable
        // formats; for solid streams libarchive must decompress anyway.
        if (archive_read_data_skip(a) < ARCHIVE_WARN) {
            if (m_cancel.load(std::memory_order_relaxed)) {
                summary.cancelled = true;
            } else {
                ok = false;
                error = QString::fromUtf8(archive_error_string(a));
            }
            break;
        }
    }

    if (!batch.isEmpty())
        emit entriesReady(batch);

    if (m_cancel.load(std::memory_order_relaxed))
        summary.cancelled = true;

    // Note archive_filter_bytes(a, -1) is NOT the archive's compressed size:
    // it counts bytes actually consumed, and the walk skips entry bodies, so
    // for a seekable zip it lands well under the file size (measured: 7.8 MiB
    // consumed for a 16 MiB archive). It is the right number for a solid
    // stream, where everything is read, and the wrong one everywhere else.
    // The file's own size is the compressed size, so use that — the flaw in
    // the predecessor's stat()-based ratio was the wrapped uncompressed total
    // it was divided by, not the stat() itself.
    summary.compressedBytes = totalBytes;

    // ARCHIVE_READ_FORMAT_ENCRYPTION_DONT_KNOW (-2) and _UNSUPPORTED (-1) are
    // both negative; only a positive count means "yes, and this many".
    const int encrypted = archive_read_has_encrypted_entries(a);
    if (encrypted > 0)
        summary.hasEncryptedEntries = true;
    summary.passphraseDeclined = m_broker.wasDeclined();

    archive_read_free(a);

    emit progress(totalBytes, totalBytes);
    emit scanFinished(ok || summary.cancelled, error, summary);
}
