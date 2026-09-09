#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include "core/ArchiveExtractor.h"

/// Qt adapter over archiveview::Extractor. See ArchiveScanner.h for why the
/// emits are safe from the worker thread.
class ArchiveExtractor : public QObject {
    Q_OBJECT
public:
    explicit ArchiveExtractor(QObject *parent = nullptr);
    ~ArchiveExtractor() override;

    void extract(const QString &archivePath, const QStringList &members,
                 const QString &destination);

    void setPassphrase(const QString &passphrase);
    void providePassphrase(const QString &passphrase, bool accepted);

    void cancel() { m_core.cancel(); }
    void cancelAndWait() { m_core.cancelAndWait(); }

    QStringList writtenPaths() const;

signals:
    void passphraseRequested(int attempt);
    void extractProgress(int done, int total, const QString &member);
    void extractFinished(bool ok, const QString &error, int extracted, int refused);

private:
    archiveview::Extractor m_core;
};
