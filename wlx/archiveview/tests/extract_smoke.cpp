/// Harness for ArchiveExtractor.
///
/// The listing side of this plugin shows hostile member names verbatim on
/// purpose — "../../../../etc/passwd" is the most useful thing it can tell
/// you about an archive. Extraction is where that stops being informative and
/// starts being dangerous, so this checks the opposite property: that nothing
/// lands outside the destination directory.
///
///   extract_smoke <archive> <destination> [--passphrase pw] [--member path]
///
/// Prints what was written and what was refused, and verifies every written
/// path is inside the destination.

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QTextStream>

#include "core/ArchiveExtractor.h"

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);

    QCommandLineParser parser;
    parser.addPositionalArgument("archive", "Archive to extract from");
    parser.addPositionalArgument("destination", "Directory to extract into");
    QCommandLineOption passphrase("passphrase", "Passphrase", "pw");
    QCommandLineOption member("member", "Extract only this member (repeatable)", "path");
    parser.addOption(passphrase);
    parser.addOption(member);
    parser.addHelpOption();
    parser.process(app);

    const QStringList positional = parser.positionalArguments();
    if (positional.size() < 2) {
        parser.showHelp(2);
        return 2;
    }

    const QString archive = positional.at(0);
    const QString destination = positional.at(1);

    archiveview::Extractor extractor;
    if (parser.isSet(passphrase))
        extractor.setPassphrase(parser.value(passphrase).toStdString());

    // No prompt in a headless harness: an unanswered request must degrade to
    // "declined" rather than hang.
    archiveview::Extractor::Callbacks callbacks;
    // No prompt in a headless harness: an unanswered request must degrade to
    // "declined" rather than hang.
    callbacks.passphraseNeeded = [&](int attempt) {
        out << "passphrase asked: attempt " << attempt << '\n';
        out.flush();
        extractor.providePassphrase(parser.value(passphrase).toStdString(),
                                    parser.isSet(passphrase));
    };
    callbacks.finished = [&](bool ok, const std::string &errorText,
                             int extracted, int refused) {
        const QString error = QString::fromStdString(errorText);
        out << "extracted       : " << extracted << '\n';
        out << "refused         : " << refused << '\n';
        if (!ok && !error.isEmpty())
            out << "ERROR           : " << error << '\n';

        const QString canonical = QDir(destination).canonicalPath();
        int escaped = 0;
        QStringList written;
        for (const std::string &path : extractor.writtenPaths())
            written << QString::fromStdString(path);

        for (const QString &path : written) {
            const QString clean = QDir::cleanPath(path);
            if (clean != canonical && !clean.startsWith(canonical + QLatin1Char('/'))) {
                out << "ESCAPED         : " << path << '\n';
                ++escaped;
            }
        }
        out << "written         : " << written.size() << '\n';
        for (const QString &path : written) {
            out << "  " << QDir(canonical).relativeFilePath(path) << '\n';
        }
        out << (escaped == 0 ? "CONTAINED OK\n" : "CONTAINMENT FAILED\n");
        out.flush();
        app.exit(escaped == 0 ? 0 : 1);
    };
    extractor.setCallbacks(std::move(callbacks));

    std::vector<std::string> members;
    for (const QString &value : parser.values(member))
        members.push_back(value.toStdString());

    extractor.extract(archive.toStdString(), members, destination.toStdString());
    return app.exec();
}
