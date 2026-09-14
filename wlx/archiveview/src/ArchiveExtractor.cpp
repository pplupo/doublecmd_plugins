#include "ArchiveExtractor.h"

ArchiveExtractor::ArchiveExtractor(QObject *parent)
    : QObject(parent)
{
    archiveview::Extractor::Callbacks callbacks;
    callbacks.progress = [this](int done, int total, const std::string &member) {
        emit extractProgress(done, total, QString::fromStdString(member));
    };
    callbacks.finished = [this](bool ok, const std::string &error,
                                int extracted, int refused) {
        emit extractFinished(ok, QString::fromStdString(error), extracted, refused);
    };
    callbacks.passphraseNeeded = [this](int attempt) {
        emit passphraseRequested(attempt);
    };
    m_core.setCallbacks(std::move(callbacks));
}

ArchiveExtractor::~ArchiveExtractor()
{
    m_core.cancelAndWait();
}

void ArchiveExtractor::extract(const QString &archivePath,
                               const QStringList &members,
                               const QString &destination)
{
    std::vector<std::string> wanted;
    wanted.reserve(members.size());
    for (const QString &member : members)
        wanted.push_back(member.toStdString());

    m_core.extract(archivePath.toStdString(), wanted, destination.toStdString());
}

void ArchiveExtractor::setPassphrase(const QString &passphrase)
{
    m_core.setPassphrase(passphrase.toStdString());
}

void ArchiveExtractor::providePassphrase(const QString &passphrase, bool accepted)
{
    m_core.providePassphrase(passphrase.toStdString(), accepted);
}

QStringList ArchiveExtractor::writtenPaths() const
{
    QStringList paths;
    for (const std::string &path : m_core.writtenPaths())
        paths << QString::fromStdString(path);
    return paths;
}
