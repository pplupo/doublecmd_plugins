/// Headless harness for the scanner and model.
///
/// The two properties that matter for M1 cannot be eyeballed in the file
/// manager: that entries arrive progressively rather than in one lump at the
/// end, and that cancellation actually interrupts libarchive mid-stream
/// instead of waiting for the current decompression to finish.
///
///   scan_smoke <archive>                  — walk it, print the summary
///   scan_smoke <archive> --cancel-after N — cancel N ms in, report latency
///
/// Runs offscreen; no display required.

#include <QApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QLocale>
#include <QTextStream>
#include <QTimer>

#include "ArchiveModel.h"
#include "core/ArchiveNames.h"
#include "ArchiveScanner.h"
#include "core/ArchiveSettings.h"

/// The ini path normally comes from DC via ListSetDefaultParams; the
/// harnesses do not link wlx_entry.cpp, so they supply their own.
const QString &archiveviewIniPath()
{
    static const QString empty;
    return empty;
}

static QString hiddenColumnsText(const archiveview::Settings &settings)
{
    QStringList names;
    for (const std::string &column : settings.hiddenColumns)
        names << QString::fromStdString(column);
    return names.join(QLatin1Char('+'));
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    QCommandLineParser parser;
    parser.addPositionalArgument("archive", "Archive to scan");
    QCommandLineOption cancelAfter("cancel-after", "Cancel after <ms>", "ms");
    QCommandLineOption flatMode("flat", "Populate the model in flat mode");
    QCommandLineOption listRows("list", "Print the first <n> entries", "n");
    QCommandLineOption maxEntries("max-entries", "Stop after <n> entries", "n");
    QCommandLineOption probeOnly("probe", "Only run the bounded format probe");
    QCommandLineOption passphrase("passphrase", "Passphrase for encrypted archives", "pw");
    QCommandLineOption iniFile("ini", "Read settings from this ini", "path");
    parser.addOption(cancelAfter);
    parser.addOption(flatMode);
    parser.addOption(listRows);
    parser.addOption(maxEntries);
    parser.addOption(probeOnly);
    parser.addOption(passphrase);
    parser.addOption(iniFile);
    parser.addHelpOption();
    parser.process(app);

    if (parser.positionalArguments().isEmpty()) {
        parser.showHelp(2);
        return 2;
    }

    QTextStream out(stdout);
    const QLocale locale;

    // What ListLoad does before returning: identify the format, or decline.
    if (parser.isSet(probeOnly)) {
        QElapsedTimer probeClock;
        probeClock.start();
        const bool readable =
            ArchiveScanner::canRead(parser.positionalArguments().first());
        out << "probe           : " << (readable ? "archive" : "not an archive")
            << "  (" << probeClock.elapsed() << " ms)\n";
        out.flush();
        return readable ? 0 : 1;
    }

    // The plugin gets this path from DC via ListSetDefaultParams; the harness
    // takes it on the command line.
    archiveview::Settings settings =
        archiveview::Settings::load(parser.value(iniFile).toStdString());
    archiveview::names::setFallbackCodec(settings.nameCodec);
    if (parser.isSet(iniFile)) {
        out << "settings        : flat=" << (settings.startFlat ? "yes" : "no")
            << " detail=" << (settings.showDetailPanel ? "yes" : "no")
            << " filter=" << (settings.showFilterBox ? "yes" : "no")
            << " maxEntries=" << settings.maxEntries
            << " codec=" << (settings.nameCodec.empty()
                                 ? QStringLiteral("-")
                                 : QString::fromStdString(settings.nameCodec))
            << " hidden=" << hiddenColumnsText(settings) << '\n';
    }

    ArchiveModel model;
    model.setFlat(parser.isSet(flatMode) || settings.startFlat);

    ArchiveScanner scanner;
    QElapsedTimer clock;
    qint64 firstBatchAt = -1;
    int batches = 0;
    qint64 cancelRequestedAt = -1;

    QObject::connect(&scanner, &ArchiveScanner::entriesReady,
                     &model, [&](const archiveview::EntryBatch &batch) {
        if (firstBatchAt < 0)
            firstBatchAt = clock.elapsed();
        ++batches;
        model.appendEntries(batch);
    });

    QObject::connect(&scanner, &ArchiveScanner::scanFinished, &app,
                     [&](bool ok, const QString &error,
                         const archiveview::Summary &summary) {
        const qint64 elapsed = clock.elapsed();

        out << "format          : " << QString::fromStdString(summary.format) << '\n';
        out << "filters         : "
            << (summary.filters.empty() ? QStringLiteral("-")
                                        : QString::fromStdString(summary.filters)) << '\n';
        out << "entries         : " << summary.entryCount
            << "  (model rows: " << model.entryCount() << ")\n";
        out << "uncompressed    : " << locale.formattedDataSize(summary.totalUncompressed) << '\n';
        out << "compressed      : " << locale.formattedDataSize(summary.compressedBytes) << '\n';
        out << "comment         : "
            << (summary.comment.empty()
                    ? QStringLiteral("-")
                    : QString::fromStdString(summary.comment).simplified()) << '\n';
        out << "packed (entries): "
            << (summary.totalCompressedEntries > 0
                    ? locale.formattedDataSize(summary.totalCompressedEntries)
                    : QStringLiteral("-")) << '\n';
        out << "zip64           : " << (summary.zip64 ? "yes" : "no") << '\n';
        out << "encrypted       : " << (summary.hasEncryptedEntries ? "yes" : "no") << '\n';
        out << "passphrase      : "
            << (summary.passphraseDeclined ? "declined/absent" : "not needed or accepted") << '\n';
        out << "truncated       : " << (summary.truncated ? "yes" : "no") << '\n';
        out << "cancelled       : " << (summary.cancelled ? "yes" : "no") << '\n';
        out << "duplicates      : " << model.duplicateCount() << '\n';
        out << "batches         : " << batches << '\n';
        out << "first batch at  : " << firstBatchAt << " ms\n";
        out << "total elapsed   : " << elapsed << " ms\n";
        if (cancelRequestedAt >= 0)
            out << "cancel latency  : " << (elapsed - cancelRequestedAt) << " ms\n";
        if (!ok)
            out << "ERROR           : " << error << '\n';

        // Sanity check on the tree: every entry-bearing node must be
        // reachable from the root, and rowCount must agree with the walk.
        int walked = 0;
        QVector<QModelIndex> stack;
        for (int row = 0; row < model.rowCount(); ++row)
            stack.append(model.index(row, 0));
        while (!stack.isEmpty()) {
            const QModelIndex index = stack.takeLast();
            if (model.entryAt(index))
                ++walked;
            for (int row = 0; row < model.rowCount(index); ++row)
                stack.append(model.index(row, 0, index));
        }
        out << "tree walk       : " << walked << " entry nodes\n";
        out << (walked == model.entryCount() ? "TREE OK\n" : "TREE MISMATCH\n");

        if (parser.isSet(listRows)) {
            const int limit = parser.value(listRows).toInt();
            out << "--- first " << limit << " rows (as the model renders them) ---\n";
            int printed = 0;
            QVector<QModelIndex> pending;
            for (int row = model.rowCount() - 1; row >= 0; --row)
                pending.append(model.index(row, 0));
            while (!pending.isEmpty() && printed < limit) {
                const QModelIndex index = pending.takeLast();
                QStringList columns;
                for (int column = ArchiveModel::NameColumn;
                     column < ArchiveModel::ColumnCount; ++column) {
                    columns << model.index(index.row(), column, index.parent())
                                   .data(Qt::DisplayRole).toString();
                }
                // The lock column is icon-only, so surface it as text here.
                const archiveview::Entry *entry = model.entryAt(index);
                const char *lock = "  ";
                if (entry && entry->metadataEncrypted)
                    lock = "LM";
                else if (entry && entry->encrypted)
                    lock = "L ";
                out << "  " << lock << " " << columns.join(QStringLiteral(" | ")) << '\n';
                ++printed;
                for (int row = model.rowCount(index) - 1; row >= 0; --row)
                    pending.append(model.index(row, 0, index));
            }
        }

        out.flush();
        app.exit(ok ? 0 : 1);
    });

    if (parser.isSet(cancelAfter)) {
        const int delay = parser.value(cancelAfter).toInt();
        QTimer::singleShot(delay, &app, [&]() {
            cancelRequestedAt = clock.elapsed();
            scanner.cancel();
        });
    }

    // No prompt in a headless harness: an unanswered request must degrade to
    // "declined" rather than hang, which is itself worth testing.
    if (parser.isSet(passphrase))
        scanner.setPassphrase(parser.value(passphrase));
    QObject::connect(&scanner, &ArchiveScanner::passphraseRequested, &app,
                     [&](int attempt) {
        out << "passphrase asked: attempt " << attempt << '\n';
        out.flush();
        scanner.providePassphrase(parser.value(passphrase),
                                  parser.isSet(passphrase));
    });

    clock.start();
    scanner.scan(parser.positionalArguments().first(),
                 parser.isSet(maxEntries) ? parser.value(maxEntries).toLongLong()
                                          : settings.maxEntries);
    return app.exec();
}
