#include "core/ArchiveScanner.h"

#include <archive.h>
#include <archive_entry.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "core/ArchiveNames.h"
#include "core/ZipCentralDirectory.h"

namespace archiveview {
namespace {

constexpr int kReadBlockSize = 256 * 1024;

/// Everything the libarchive read callbacks need. Lives on the worker
/// thread's stack for the duration of the walk.
///
/// Plain stdio rather than a toolkit file class: this is the one place the
/// core touches the filesystem for reading, and FILE* keeps it free of both
/// Qt and glib.
struct ReadState {
    FILE *file = nullptr;
    int64_t size = 0;
    std::vector<char> buffer;
    const std::atomic<bool> *cancel = nullptr;
};

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

    const size_t n = std::fread(state->buffer.data(), 1, state->buffer.size(),
                                state->file);
    if (n == 0 && std::ferror(state->file)) {
        archive_set_error(a, EIO, "%s", std::strerror(errno));
        return -1;
    }

    *buffer = state->buffer.data();
    return static_cast<la_ssize_t>(n);
}

la_int64_t skipCallback(struct archive *a, void *clientData, la_int64_t request)
{
    auto *state = static_cast<ReadState *>(clientData);

    if (state->cancel->load(std::memory_order_relaxed)) {
        archive_set_error(a, ECANCELED, "scan cancelled");
        return -1;
    }

    const int64_t position = ::ftello(state->file);
    if (position < 0 || position + request > state->size)
        return 0;   // "cannot skip" — libarchive falls back to reading
    if (::fseeko(state->file, request, SEEK_CUR) != 0)
        return 0;
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

    if (::fseeko(state->file, offset, whence) != 0) {
        archive_set_error(a, EIO, "seek failed");
        return -1;
    }
    return ::ftello(state->file);
}

int closeCallback(struct archive *, void *clientData)
{
    auto *state = static_cast<ReadState *>(clientData);
    if (state->file) {
        std::fclose(state->file);
        state->file = nullptr;
    }
    return ARCHIVE_OK;
}

bool openReadState(ReadState *state, const std::string &path,
                   const std::atomic<bool> *cancel)
{
    state->file = std::fopen(path.c_str(), "rb");
    if (!state->file)
        return false;
    ::fseeko(state->file, 0, SEEK_END);
    state->size = ::ftello(state->file);
    ::fseeko(state->file, 0, SEEK_SET);
    state->buffer.resize(kReadBlockSize);
    state->cancel = cancel;
    return true;
}

/// Adapt libarchive's two name accessors onto names::decode(). Either can
/// return NULL: the *_utf8 form when the name is not representable, the raw
/// form when the locale conversion fails.
std::string decodeName(const char *utf8Name, const char *rawName)
{
    if (utf8Name && *utf8Name)
        return names::decode(utf8Name, /*declaredUtf8=*/true);
    if (!rawName || !*rawName)
        return {};
    return names::decode(rawName);
}

Entry::Type entryType(struct archive_entry *entry)
{
    if (archive_entry_hardlink(entry) || archive_entry_hardlink_utf8(entry))
        return Entry::Type::Hardlink;

    switch (archive_entry_filetype(entry)) {
    case AE_IFDIR: return Entry::Type::Directory;
    case AE_IFLNK: return Entry::Type::Symlink;
    case AE_IFREG: return Entry::Type::File;
    default:       return Entry::Type::Other;
    }
}

std::string filterChain(struct archive *a)
{
    std::string names;
    // archive_filter_count() is documented as >= 1 (index 0 is the format
    // reader itself), but index 0 is "none" for an uncompressed archive and
    // is not worth showing.
    for (int i = archive_filter_count(a) - 1; i >= 0; --i) {
        const char *name = archive_filter_name(a, i);
        if (name && std::strcmp(name, "none") != 0) {
            if (!names.empty())
                names += " -> ";
            names += name;
        }
    }
    return names;
}

int64_t nowMillis()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

Scanner::~Scanner()
{
    cancelAndWait();
}

bool Scanner::canRead(const std::string &archivePath)
{
    std::atomic<bool> neverCancel{false};

    ReadState state;
    if (!openReadState(&state, archivePath, &neverCancel))
        return false;

    struct archive *a = archive_read_new();
    if (!a) {
        std::fclose(state.file);
        return false;
    }

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
    } else if (state.file) {
        std::fclose(state.file);
        state.file = nullptr;
    }

    archive_read_free(a);
    return recognised;
}

void Scanner::scan(const std::string &archivePath, int64_t maxEntries)
{
    cancelAndWait();
    m_cancel.store(false, std::memory_order_relaxed);
    m_running.store(true, std::memory_order_relaxed);
    m_thread = std::thread(&Scanner::run, this, archivePath,
                           maxEntries > 0 ? maxEntries : kDefaultMaxEntries);
}

void Scanner::cancel()
{
    m_cancel.store(true, std::memory_order_relaxed);
    // Wake a worker parked on the passphrase prompt, so cancellation is
    // honoured even while a dialog is on screen.
    m_broker.wake();
}

void Scanner::cancelAndWait()
{
    cancel();
    if (m_thread.joinable())
        m_thread.join();
}

void Scanner::providePassphrase(const std::string &passphrase, bool accepted)
{
    m_broker.provide(passphrase, accepted);
}

void Scanner::setPassphrase(const std::string &passphrase)
{
    m_broker.preset(passphrase);
}

const char *Scanner::passphraseTrampoline(::archive *, void *client)
{
    return static_cast<Scanner *>(client)->requestPassphrase();
}

const char *Scanner::requestPassphrase()
{
    return m_broker.request(
        [this](int attempt) {
            if (m_callbacks.passphraseNeeded)
                m_callbacks.passphraseNeeded(attempt);
        },
        m_cancel);
}

void Scanner::run(std::string archivePath, int64_t maxEntries)
{
    Summary summary;

    const auto finish = [&](bool ok, const std::string &error) {
        m_running.store(false, std::memory_order_relaxed);
        if (m_callbacks.finished)
            m_callbacks.finished(ok, error, summary);
    };

    ReadState state;
    if (!openReadState(&state, archivePath, &m_cancel)) {
        finish(false, std::strerror(errno));
        return;
    }
    const int64_t totalBytes = state.size;

    struct archive *a = archive_read_new();
    if (!a) {
        // The original plugin passed this straight into
        // archive_read_support_filter_all() without checking.
        std::fclose(state.file);
        finish(false, "Out of memory allocating archive reader");
        return;
    }

    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);

    // Without a seek callback libarchive uses its *streaming* zip reader,
    // which only ever sees local file headers — so symlinks come back as
    // regular files whose contents are the target path, and the central
    // directory (where the real metadata lives) is never consulted.
    archive_read_set_seek_callback(a, seekCallback);
    // Without this an encrypted archive fails outright with "Passphrase
    // required" and shows nothing at all.
    archive_read_set_passphrase_callback(a, this, passphraseTrampoline);

    if (archive_read_open2(a, &state, openCallback, readCallback,
                           skipCallback, closeCallback) != ARCHIVE_OK) {
        const char *message = archive_error_string(a);
        const std::string error = message ? message : "cannot open archive";
        archive_read_free(a);
        if (state.file)
            std::fclose(state.file);
        finish(false, error);
        return;
    }

    // The ZIP central directory carries three things libarchive does not
    // expose at all: the archive comment, per-entry compressed size, and the
    // stored CRC. Parsed here, on the worker thread, before the walk — it is
    // a bounded read of one structure, not a second pass over the archive.
    // Returns false for every non-ZIP, which just leaves those columns blank.
    ZipCentralDirectory zipDirectory;
    if (zipDirectory.read(archivePath)) {
        summary.zip64 = zipDirectory.isZip64();
        if (!zipDirectory.comment().empty()) {
            summary.comment = zipDirectory.comment();
            if (m_callbacks.comment)
                m_callbacks.comment(summary.comment);
        }
    }
    // Duplicate member paths are legal in ZIP, and the central directory
    // lists them in the same order libarchive walks them — so occurrences are
    // matched positionally rather than collapsed onto one another.
    std::unordered_map<std::string, int> zipOccurrences;

    bool formatReported = false;
    bool ok = true;
    std::string error;

    EntryBatch batch;
    batch.reserve(kBatchSize);
    int64_t lastFlush = nowMillis();

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
                const char *message = archive_error_string(a);
                error = message ? message : "read failed";
            }
            break;
        }

        if (!formatReported) {
            formatReported = true;
            const char *format = archive_format_name(a);
            summary.format = format ? format : "unknown";
            summary.filters = filterChain(a);
            if (m_callbacks.format)
                m_callbacks.format(summary.format, summary.filters);
        }

        Entry item;
        item.path = names::normalize(decodeName(archive_entry_pathname_utf8(entry),
                                                archive_entry_pathname(entry)));
        if (item.path.empty())
            continue;   // unnameable member — nothing useful to show

        item.type = entryType(entry);

        if (item.type == Entry::Type::Symlink) {
            item.linkTarget = decodeName(archive_entry_symlink_utf8(entry),
                                         archive_entry_symlink(entry));
        } else if (item.type == Entry::Type::Hardlink) {
            item.linkTarget = decodeName(archive_entry_hardlink_utf8(entry),
                                         archive_entry_hardlink(entry));
        }

        // Guard the classic bug: archive_entry_size() is int64_t and is -1
        // or 0 when unset. Casting to size_t turns -1 into SIZE_MAX and
        // wraps every running total that touches it.
        if (archive_entry_size_is_set(entry)) {
            const la_int64_t size = archive_entry_size(entry);
            if (size >= 0) {
                item.size = static_cast<int64_t>(size);
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

        if (archive_entry_mtime_is_set(entry)) {
            item.modified = static_cast<int64_t>(archive_entry_mtime(entry));
            item.hasModified = true;
        }

        if (archive_entry_perm_is_set(entry))
            item.mode = archive_entry_perm(entry);

        if (const char *owner = archive_entry_uname_utf8(entry))
            item.owner = owner;
        if (const char *group = archive_entry_gname_utf8(entry))
            item.group = group;

        item.encrypted = item.encrypted || archive_entry_is_data_encrypted(entry) != 0;
        item.metadataEncrypted = archive_entry_is_metadata_encrypted(entry) != 0;
        if (item.encrypted || item.metadataEncrypted)
            summary.hasEncryptedEntries = true;

        batch.push_back(item);
        ++summary.entryCount;

        // Flush on batch size, or on elapsed time so a slow archive still
        // populates the view progressively instead of in silent chunks.
        if (static_cast<int>(batch.size()) >= kBatchSize
            || nowMillis() - lastFlush > 100) {
            if (m_callbacks.entries)
                m_callbacks.entries(batch);
            if (m_callbacks.progress)
                m_callbacks.progress(::ftello(state.file), totalBytes);
            batch.clear();
            batch.reserve(kBatchSize);
            lastFlush = nowMillis();
        }

        if (summary.entryCount >= maxEntries) {
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
                const char *message = archive_error_string(a);
                error = message ? message : "read failed";
            }
            break;
        }
    }

    if (!batch.empty() && m_callbacks.entries)
        m_callbacks.entries(batch);

    if (m_cancel.load(std::memory_order_relaxed))
        summary.cancelled = true;

    // Note archive_filter_bytes(a, -1) is NOT the archive's compressed size:
    // it counts bytes actually consumed, and the walk skips entry bodies, so
    // for a seekable zip it lands well under the file size. The file's own
    // size is the compressed size — the flaw in the predecessor's ratio was
    // the wrapped uncompressed total it was divided by, not the stat().
    summary.compressedBytes = totalBytes;

    // ARCHIVE_READ_FORMAT_ENCRYPTION_DONT_KNOW (-2) and _UNSUPPORTED (-1) are
    // both negative; only a positive count means "yes, and this many".
    if (archive_read_has_encrypted_entries(a) > 0)
        summary.hasEncryptedEntries = true;
    summary.passphraseDeclined = m_broker.wasDeclined();

    archive_read_free(a);

    if (m_callbacks.progress)
        m_callbacks.progress(totalBytes, totalBytes);
    finish(ok || summary.cancelled, error);
}

} // namespace archiveview
