#pragma once

#include <QThread>
#include <QString>
#include <atomic>

#include "PassphraseBroker.h"

#include "ArchiveEntry.h"

/// Walks an archive with libarchive on a worker thread and streams entries
/// to the GUI thread in batches.
///
/// Two properties drive the design:
///
///  1. **The walk must never run on the host's GUI thread.** ListLoad is
///     called on Double Commander's UI thread; listing a solid .tar.xz means
///     decompressing the whole stream, which would freeze the file manager.
///
///  2. **Cancellation must interrupt libarchive from the inside.** libarchive
///     has no cancel API, and a flag checked between entries is useless while
///     archive_read_next_header() is blocked decompressing a large solid
///     block. So the archive is opened through archive_read_open2() with our
///     own read callback: when the cancel flag is set the callback reports a
///     fatal read error, which unwinds the decompressor immediately. This is
///     what makes closing a multi-gigabyte archive instant instead of a
///     multi-second freeze.
///
/// Entries are emitted in batches rather than one signal per entry — a queued
/// signal per entry across 100k entries costs more than the walk itself.
class ArchiveScanner : public QThread {
    Q_OBJECT
public:
    explicit ArchiveScanner(QObject *parent = nullptr);
    ~ArchiveScanner() override;

    /// Number of entries accumulated before a batch is shipped to the GUI.
    static constexpr int kBatchSize = 512;
    /// Default ceiling on entries; a hostile archive can declare far more
    /// members than there is memory to model them. Overridable per scan.
    static constexpr qint64 kDefaultMaxEntries = 500000;

    /// Bounded synchronous check that libarchive recognises this file.
    ///
    /// ListLoad must answer "can you show this?" before returning, so that DC
    /// falls through to another viewer when the answer is no. The full walk
    /// cannot answer that — it only finds out at the end — so this reads just
    /// far enough to identify the format. That is one header for a plain
    /// archive and one decompressed block for a solid one; it never walks the
    /// archive. An archive whose format is recognised but whose first header
    /// cannot be read (encrypted, damaged) still counts as readable: the view
    /// has something useful to say about it.
    static bool canRead(const QString &archivePath);

    /// Begin scanning. Safe to call only when the scanner is not running.
    void scan(const QString &archivePath, qint64 maxEntries = kDefaultMaxEntries);

    /// Request cancellation. Returns immediately; `finished` still fires.
    void cancel();

    /// Request cancellation and block until the thread has exited.
    /// Must be called before destroying anything the scanner's queued
    /// signals reference.
    void cancelAndWait();

    /// Answer a passphraseRequested() signal. Called on the GUI thread.
    /// `accepted` false means the user dismissed the prompt, which stops the
    /// scanner asking again for this archive.
    void providePassphrase(const QString &passphrase, bool accepted);

    /// Supply a passphrase up front, bypassing the prompt entirely.
    void setPassphrase(const QString &passphrase);

    /// The passphrase the user supplied for this archive, so extraction does
    /// not prompt a second time for the same file.
    QString acceptedPassphrase() const { return m_broker.accepted(); }

signals:
    /// Emitted once the format is known, before the bulk of the walk.
    void formatDetected(const QString &format, const QString &filters);
    /// The archive-level comment, if the format has one and it is non-empty.
    void commentFound(const QString &comment);
    /// The archive needs a passphrase. Emitted on the GUI thread while the
    /// scanner thread waits; answer with providePassphrase(). The scanner
    /// gives up waiting if cancelled, so never block indefinitely on this.
    void passphraseRequested(int attempt);
    /// A batch of entries, ready to be inserted into the model.
    void entriesReady(const ArchiveEntryBatch &batch);
    /// Bytes consumed from the file so far, and the file's total size.
    void progress(qint64 bytesRead, qint64 totalBytes);
    /// Terminal signal. `ok` is false only for a genuine read failure —
    /// a cancelled scan reports ok=true with summary.cancelled set.
    void scanFinished(bool ok, const QString &error, const ArchiveSummary &summary);

protected:
    void run() override;

private:
    /// libarchive's passphrase hook, called on the scanner thread.
    static const char *passphraseTrampoline(struct archive *a, void *client);
    const char *requestPassphrase();

    QString m_path;
    qint64 m_maxEntries = kDefaultMaxEntries;
    std::atomic<bool> m_cancel{false};

    PassphraseBroker m_broker;
};
