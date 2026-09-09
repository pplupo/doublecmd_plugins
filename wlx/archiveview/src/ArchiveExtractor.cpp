#include "ArchiveExtractor.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QSet>

#include <archive.h>
#include <archive_entry.h>

#include <cerrno>

#include "ArchiveNames.h"

namespace {

constexpr int kReadBlockSize = 256 * 1024;

struct ReadState {
    QFile file;
    QByteArray buffer;
    std::atomic<bool> *cancel = nullptr;
};

la_ssize_t readCallback(struct archive *a, void *clientData, const void **buffer)
{
    auto *state = static_cast<ReadState *>(clientData);
    if (state->cancel->load(std::memory_order_relaxed)) {
        archive_set_error(a, ECANCELED, "extraction cancelled");
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

la_int64_t seekCallback(struct archive *a, void *clientData,
                        la_int64_t offset, int whence)
{
    auto *state = static_cast<ReadState *>(clientData);
    if (state->cancel->load(std::memory_order_relaxed)) {
        archive_set_error(a, ECANCELED, "extraction cancelled");
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

/// Layer 1: is this member name safe to write at all?
///
/// Rejects absolute paths and any ".." component. Checked on the split
/// components rather than with a substring search, so a legitimate name like
/// "..hidden" or "a..b.txt" is not caught by mistake.
bool isSafeMemberPath(const QString &path)
{
    if (path.isEmpty() || path.startsWith(QLatin1Char('/')))
        return false;

    // A Windows-style drive or UNC prefix is equally absolute.
    if (path.contains(QLatin1String(":\\")) || path.startsWith(QLatin1String("\\\\")))
        return false;

    const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        if (part == QLatin1String(".."))
            return false;
    }
    return !parts.isEmpty();
}

/// Layer 2: does the resolved target stay inside the destination?
///
/// Compared after canonicalisation of the destination, so a symlinked
/// destination is handled, and using QDir::cleanPath on the joined path so
/// that anything that slipped past layer 1 still cannot climb out.
bool isInsideDestination(const QString &canonicalDestination, const QString &target)
{
    const QString clean = QDir::cleanPath(target);
    return clean == canonicalDestination
        || clean.startsWith(canonicalDestination + QLatin1Char('/'));
}

} // namespace

ArchiveExtractor::ArchiveExtractor(QObject *parent)
    : QThread(parent)
{
}

ArchiveExtractor::~ArchiveExtractor()
{
    cancelAndWait();
}

void ArchiveExtractor::extract(const QString &archivePath,
                               const QStringList &members,
                               const QString &destination)
{
    m_archivePath = archivePath;
    m_members = members;
    m_destination = destination;
    {
        QMutexLocker lock(&m_writtenMutex);
        m_written.clear();
    }
    m_cancel.store(false, std::memory_order_relaxed);
    start();
}

void ArchiveExtractor::setPassphrase(const QString &passphrase)
{
    if (!passphrase.isEmpty())
        m_broker.preset(passphrase);
}

void ArchiveExtractor::providePassphrase(const QString &passphrase, bool accepted)
{
    m_broker.provide(passphrase, accepted);
}

void ArchiveExtractor::cancel()
{
    m_cancel.store(true, std::memory_order_relaxed);
    m_broker.wake();
}

void ArchiveExtractor::cancelAndWait()
{
    cancel();
    if (isRunning())
        wait();
}

QStringList ArchiveExtractor::writtenPaths() const
{
    QMutexLocker lock(&m_writtenMutex);
    return m_written;
}

const char *ArchiveExtractor::passphraseTrampoline(struct archive *, void *client)
{
    return static_cast<ArchiveExtractor *>(client)->requestPassphrase();
}

const char *ArchiveExtractor::requestPassphrase()
{
    return m_broker.request(
        [this](int attempt) { emit passphraseRequested(attempt); }, m_cancel);
}

void ArchiveExtractor::run()
{
    int extracted = 0;
    int refused = 0;

    const QDir destinationDir(m_destination);
    if (!destinationDir.exists() && !QDir().mkpath(m_destination)) {
        emit extractFinished(false, tr("Cannot create %1").arg(m_destination), 0, 0);
        return;
    }
    const QString canonicalDestination =
        QDir(m_destination).canonicalPath().isEmpty()
            ? QDir::cleanPath(m_destination)
            : QDir(m_destination).canonicalPath();

    ReadState state;
    state.cancel = &m_cancel;
    state.buffer.resize(kReadBlockSize);
    state.file.setFileName(m_archivePath);
    if (!state.file.open(QIODevice::ReadOnly)) {
        emit extractFinished(false, state.file.errorString(), 0, 0);
        return;
    }

    struct archive *reader = archive_read_new();
    struct archive *writer = archive_write_disk_new();
    if (!reader || !writer) {
        if (reader) archive_read_free(reader);
        if (writer) archive_write_free(writer);
        emit extractFinished(false, tr("Out of memory"), 0, 0);
        return;
    }

    archive_read_support_filter_all(reader);
    archive_read_support_format_all(reader);
    archive_read_set_seek_callback(reader, seekCallback);
    archive_read_set_passphrase_callback(reader, this, passphraseTrampoline);

    // Layer 3. NOABSOLUTEPATHS is deliberately absent: the target paths this
    // builds are absolute by construction, having already been validated by
    // layers 1 and 2. SECURE_SYMLINKS is the one that matters here — it stops
    // a member being written through a symlink planted by an earlier member.
    archive_write_disk_set_options(writer,
        ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM
        | ARCHIVE_EXTRACT_SECURE_SYMLINKS | ARCHIVE_EXTRACT_SECURE_NODOTDOT);
    archive_write_disk_set_standard_lookup(writer);

    if (archive_read_open2(reader, &state, nullptr, readCallback,
                           nullptr, closeCallback) != ARCHIVE_OK) {
        const QString error = QString::fromUtf8(archive_error_string(reader));
        archive_read_free(reader);
        archive_write_free(writer);
        emit extractFinished(false, error, 0, 0);
        return;
    }

    // Empty selection means the whole archive.
    const QSet<QString> wanted(m_members.begin(), m_members.end());
    const int total = wanted.isEmpty() ? 0 : wanted.size();

    bool ok = true;
    QString error;
    struct archive_entry *entry = nullptr;

    for (;;) {
        const int rc = archive_read_next_header(reader, &entry);
        if (rc == ARCHIVE_EOF)
            break;
        if (rc < ARCHIVE_WARN) {
            if (!m_cancel.load(std::memory_order_relaxed)) {
                ok = false;
                error = QString::fromUtf8(archive_error_string(reader));
            }
            break;
        }

        const char *utf8Name = archive_entry_pathname_utf8(entry);
        const char *rawName = archive_entry_pathname(entry);
        QString member = ArchiveNames::normalize(
            utf8Name && *utf8Name ? QString::fromUtf8(utf8Name)
                                  : (rawName ? ArchiveNames::decode(QByteArray(rawName))
                                             : QString()));
        if (member.isEmpty())
            continue;

        if (!wanted.isEmpty() && !wanted.contains(member))
            continue;

        if (!isSafeMemberPath(member)) {
            // The traversal and absolute-path members the viewer displays
            // verbatim end up here, counted and skipped.
            ++refused;
            continue;
        }

        const QString target = canonicalDestination + QLatin1Char('/') + member;
        if (!isInsideDestination(canonicalDestination, target)) {
            ++refused;
            continue;
        }

        archive_entry_set_pathname(entry, target.toUtf8().constData());
        // A stored link target is a path too, and an absolute or climbing one
        // is exactly as dangerous as a member name. libarchive's
        // SECURE_SYMLINKS covers traversal *through* existing symlinks; this
        // refuses creating one that points out of the tree in the first place.
        if (const char *link = archive_entry_symlink(entry)) {
            const QString linkTarget = QString::fromUtf8(link);
            if (linkTarget.startsWith(QLatin1Char('/'))
                || !isInsideDestination(canonicalDestination,
                                        QFileInfo(target).path()
                                            + QLatin1Char('/') + linkTarget)) {
                ++refused;
                continue;
            }
        }
        if (const char *link = archive_entry_hardlink(entry)) {
            if (!isSafeMemberPath(ArchiveNames::normalize(QString::fromUtf8(link)))) {
                ++refused;
                continue;
            }
            const QString linkTarget =
                canonicalDestination + QLatin1Char('/')
                + ArchiveNames::normalize(QString::fromUtf8(link));
            archive_entry_set_hardlink(entry, linkTarget.toUtf8().constData());
        }

        if (archive_write_header(writer, entry) != ARCHIVE_OK) {
            // A refusal from libarchive's own security checks lands here.
            ++refused;
            continue;
        }

        bool entryOk = true;
        const void *block = nullptr;
        size_t blockSize = 0;
        la_int64_t blockOffset = 0;
        for (;;) {
            if (m_cancel.load(std::memory_order_relaxed)) {
                entryOk = false;
                break;
            }
            const int readRc = archive_read_data_block(reader, &block,
                                                       &blockSize, &blockOffset);
            if (readRc == ARCHIVE_EOF)
                break;
            if (readRc < ARCHIVE_WARN) {
                if (!m_cancel.load(std::memory_order_relaxed)) {
                    ok = false;
                    error = QString::fromUtf8(archive_error_string(reader));
                }
                entryOk = false;
                break;
            }
            if (archive_write_data_block(writer, block, blockSize,
                                         blockOffset) < ARCHIVE_WARN) {
                ok = false;
                error = QString::fromUtf8(archive_error_string(writer));
                entryOk = false;
                break;
            }
        }

        archive_write_finish_entry(writer);

        if (!entryOk) {
            if (m_cancel.load(std::memory_order_relaxed))
                break;
            continue;
        }

        ++extracted;
        {
            QMutexLocker lock(&m_writtenMutex);
            m_written.append(target);
        }
        emit extractProgress(extracted, total, member);

        if (!ok)
            break;
    }

    archive_read_free(reader);
    archive_write_free(writer);

    if (m_cancel.load(std::memory_order_relaxed))
        error = tr("Cancelled");

    emit extractFinished(ok, error, extracted, refused);
}
