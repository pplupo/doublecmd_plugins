#pragma once

#include <QObject>
#include <QString>

#include "core/ArchiveScanner.h"

Q_DECLARE_METATYPE(archiveview::EntryBatch)
Q_DECLARE_METATYPE(archiveview::Summary)

/// Qt adapter over archiveview::Scanner.
///
/// The core runs the walk on a std::thread and reports through plain
/// callbacks, deliberately knowing nothing about toolkits. This turns those
/// callbacks into signals.
///
/// Thread hop: the callbacks fire on the worker thread, and this object lives
/// on the GUI thread, so Qt::AutoConnection resolves every emit here to a
/// queued delivery — which is why the batch and summary types are registered
/// metatypes. Slots therefore run on the GUI thread, exactly as they did when
/// the scanner was a QThread.
class ArchiveScanner : public QObject {
    Q_OBJECT
public:
    explicit ArchiveScanner(QObject *parent = nullptr);
    ~ArchiveScanner() override;

    static bool canRead(const QString &archivePath);

    void scan(const QString &archivePath,
              qint64 maxEntries = archiveview::Scanner::kDefaultMaxEntries);
    void cancel() { m_core.cancel(); }
    void cancelAndWait() { m_core.cancelAndWait(); }

    void providePassphrase(const QString &passphrase, bool accepted);
    void setPassphrase(const QString &passphrase);
    QString acceptedPassphrase() const;

signals:
    void formatDetected(const QString &format, const QString &filters);
    void commentFound(const QString &comment);
    void entriesReady(const archiveview::EntryBatch &batch);
    void progress(qint64 bytesRead, qint64 totalBytes);
    void scanFinished(bool ok, const QString &error,
                      const archiveview::Summary &summary);
    void passphraseRequested(int attempt);

private:
    archiveview::Scanner m_core;
};
