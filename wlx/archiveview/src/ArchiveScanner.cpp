#include "ArchiveScanner.h"

ArchiveScanner::ArchiveScanner(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<archiveview::EntryBatch>("archiveview::EntryBatch");
    qRegisterMetaType<archiveview::Summary>("archiveview::Summary");

    archiveview::Scanner::Callbacks callbacks;
    callbacks.format = [this](const std::string &format, const std::string &filters) {
        emit formatDetected(QString::fromStdString(format),
                            QString::fromStdString(filters));
    };
    callbacks.comment = [this](const std::string &comment) {
        emit commentFound(QString::fromStdString(comment));
    };
    callbacks.entries = [this](const archiveview::EntryBatch &batch) {
        emit entriesReady(batch);
    };
    callbacks.progress = [this](int64_t read, int64_t total) {
        emit progress(read, total);
    };
    callbacks.finished = [this](bool ok, const std::string &error,
                                const archiveview::Summary &summary) {
        emit scanFinished(ok, QString::fromStdString(error), summary);
    };
    callbacks.passphraseNeeded = [this](int attempt) {
        emit passphraseRequested(attempt);
    };
    m_core.setCallbacks(std::move(callbacks));
}

ArchiveScanner::~ArchiveScanner()
{
    // The core joins its thread in its own destructor, but do it here too and
    // before any subclass state unwinds: the callbacks above capture `this`.
    m_core.cancelAndWait();
}

bool ArchiveScanner::canRead(const QString &archivePath)
{
    return archiveview::Scanner::canRead(archivePath.toStdString());
}

void ArchiveScanner::scan(const QString &archivePath, qint64 maxEntries)
{
    m_core.scan(archivePath.toStdString(), maxEntries);
}

void ArchiveScanner::providePassphrase(const QString &passphrase, bool accepted)
{
    m_core.providePassphrase(passphrase.toStdString(), accepted);
}

void ArchiveScanner::setPassphrase(const QString &passphrase)
{
    m_core.setPassphrase(passphrase.toStdString());
}

QString ArchiveScanner::acceptedPassphrase() const
{
    return QString::fromStdString(m_core.acceptedPassphrase());
}
