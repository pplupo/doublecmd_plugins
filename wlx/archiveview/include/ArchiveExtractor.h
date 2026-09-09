#pragma once

#include <QStringList>
#include <QThread>
#include <atomic>

#include "PassphraseBroker.h"

/// Extracts selected members to a directory on a worker thread.
///
/// This is the one place in the plugin that writes to the filesystem, so it
/// is also the one place where a hostile member *name* becomes dangerous
/// rather than merely informative. The viewer shows "../../../../etc/passwd"
/// verbatim on purpose; extraction must refuse to write it.
///
/// Three independent layers stop that, because any single one of them could
/// be defeated by a case nobody thought of:
///
///  1. Every member path is checked before extraction: a ".." component or a
///     leading "/" is refused outright and counted.
///  2. The resolved destination path is required to stay inside the
///     destination directory, compared after canonicalisation.
///  3. libarchive's own ARCHIVE_EXTRACT_SECURE_* flags are enabled, so it
///     refuses to follow a symlink out of the tree even if a path passes the
///     first two checks.
///
/// Note the process working directory is never changed. bsdtar chdir()s into
/// the destination, which is fine for a standalone tool and unacceptable
/// inside a file manager's process — so absolute target paths are built
/// explicitly instead.
class ArchiveExtractor : public QThread {
    Q_OBJECT
public:
    explicit ArchiveExtractor(QObject *parent = nullptr);
    ~ArchiveExtractor() override;

    /// Extract `members` (normalised in-archive paths; empty means all) from
    /// `archivePath` into `destination`.
    void extract(const QString &archivePath, const QStringList &members,
                 const QString &destination);

    /// Reuse a passphrase already accepted for this archive, so extracting
    /// does not prompt again after listing.
    void setPassphrase(const QString &passphrase);
    void providePassphrase(const QString &passphrase, bool accepted);

    void cancel();
    void cancelAndWait();

    /// Where the last extraction actually wrote each member, in order.
    /// Used to build the drag payload and to open a previewed entry.
    QStringList writtenPaths() const;

signals:
    void passphraseRequested(int attempt);
    void extractProgress(int done, int total, const QString &member);
    /// `refused` counts members rejected by the path checks above.
    void extractFinished(bool ok, const QString &error, int extracted, int refused);

protected:
    void run() override;

private:
    static const char *passphraseTrampoline(struct archive *a, void *client);
    const char *requestPassphrase();

    QString m_archivePath;
    QString m_destination;
    QStringList m_members;
    QStringList m_written;
    mutable QMutex m_writtenMutex;
    std::atomic<bool> m_cancel{false};
    PassphraseBroker m_broker;
};
