#include "ArchiveViewWidget.h"

#include <QDateTime>
#include <QFileInfo>

#include <algorithm>
#include <QHeaderView>
#include <QAbstractTextDocumentLayout>
#include <QLineEdit>
#include <QSortFilterProxyModel>
#include <QInputDialog>
#include <QPainter>
#include <QPrinter>
#include <QPrinterInfo>
#include <QScrollBar>
#include <QTextDocument>
#include <QLineEdit>
#include <QItemSelectionModel>
#include <QLocale>
#include <QAction>
#include <QDesktopServices>
#include <QFileDialog>
#include <QMenu>
#include <QEventLoop>
#include <QMessageBox>
#include <QProgressDialog>
#include <QUrl>
#include <QVBoxLayout>

#include <wlxbase_wlqt/FindReplacePanel.h>
#include <wlxbase_wlqt/FocusManager.h>
#include <wlxbase_wlqt/PluginStatusBar.h>
#include <wlxbase_wlqt/ThemeManager.h>

#include "wlxplugin.h"      // lcp_* presentation flags

#include "ArchiveExtractor.h"

/// Supplied by wlx_entry.cpp (the plugin) or the harness that links this.
const QString &archiveviewIniPath();
#include "ArchiveModel.h"
#include "ArchiveTreeView.h"
#include "ArchiveScanner.h"
#include "core/ArchiveNames.h"
#include "core/ArchiveSettings.h"

ArchiveViewWidget::ArchiveViewWidget(QWidget *parent)
    : QWidget(parent)
{
    // Both cross the scanner/GUI thread boundary as queued signal arguments.
    m_settings = archiveview::Settings::load(archiveviewIniPath().toStdString());
    // Both readers consult this, so it must be set before any scan starts.
    archiveview::names::setFallbackCodec(m_settings.nameCodec);

    setupUi();
}

ArchiveViewWidget::~ArchiveViewWidget()
{
    // Order matters: the scanner emits queued signals whose slots touch the
    // model, so it has to be fully stopped before anything else is destroyed.
    stopScan();
}

void ArchiveViewWidget::setupUi()
{
    m_model = new ArchiveModel(this);

    // Recursive filtering so a match deep in a collapsed directory still
    // shows, with its ancestors, instead of being invisible.
    m_filterProxy = new QSortFilterProxyModel(this);
    m_filterProxy->setSourceModel(m_model);
    m_filterProxy->setRecursiveFilteringEnabled(true);
    m_filterProxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_filterProxy->setFilterKeyColumn(ArchiveModel::NameColumn);

    m_view = new ArchiveTreeView(this);
    m_view->setModel(m_filterProxy);
    m_view->setUniformRowHeights(true);      // required for large archives
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_view->setAlternatingRowColors(true);
    m_view->setAllColumnsShowFocus(true);
    m_view->setExpandsOnDoubleClick(true);
    m_view->setRootIsDecorated(true);
    m_view->header()->setStretchLastSection(false);
    m_view->header()->setSectionResizeMode(ArchiveModel::LockColumn,
                                           QHeaderView::Fixed);
    m_view->setColumnWidth(ArchiveModel::LockColumn, 24);
    m_view->header()->setSectionResizeMode(ArchiveModel::NameColumn,
                                           QHeaderView::Interactive);
    m_view->setColumnWidth(ArchiveModel::NameColumn, 320);
    for (int column = ArchiveModel::SizeColumn; column < ArchiveModel::ColumnCount; ++column)
        m_view->setColumnWidth(column, 110);

    for (int column = 0; column < ArchiveModel::ColumnCount; ++column) {
        const QString header =
            m_model->headerData(column, Qt::Horizontal, Qt::DisplayRole).toString();
        if (!header.isEmpty()
            && std::any_of(m_settings.hiddenColumns.begin(),
                           m_settings.hiddenColumns.end(),
                           [&header](const std::string &hidden) {
                               return header.compare(QString::fromStdString(hidden),
                                                     Qt::CaseInsensitive) == 0;
                           })) {
            m_view->setColumnHidden(column, true);
        }
    }

    m_status = new QtWlPlugin::PluginStatusBar(this);

    m_filterBox = new QLineEdit(this);
    m_filterBox->setClearButtonEnabled(true);
    m_filterBox->setPlaceholderText(tr("Filter members…  (Ctrl+F)"));
    m_filterBox->setVisible(m_settings.showFilterBox);
    connect(m_filterBox, &QLineEdit::textChanged,
            this, &ArchiveViewWidget::onFilterChanged);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_filterBox, 0);
    layout->addWidget(m_view, 1);
    layout->addWidget(m_status, 0);

    m_focus = new QtWlPlugin::FocusManager(this, m_view, this);
    m_focus->addInputWidget(m_filterBox);
    m_focus->registerShortcut(QKeySequence(QStringLiteral("Ctrl+F")),
                              QtWlPlugin::FocusManager::WhenNoInput,
                              [this]() {
        m_filterBox->setVisible(true);
        m_filterBox->setFocus(Qt::ShortcutFocusReason);
        return true;
    });

    // The plugin's own find UI, for ListSearchDialog. DC's find-next
    // semantics suit a text viewer; a table wants its own panel.
    m_find = new QtWlPlugin::FindReplacePanel(m_focus, this);
    m_find->setReplaceEnabled(false);
    m_find->showPanel(false);
    layout->insertWidget(1, m_find, 0);
    connect(m_find, &QtWlPlugin::FindReplacePanel::findRequested,
            this, &ArchiveViewWidget::onFindRequested);

    m_focus->registerShortcut(QKeySequence(QStringLiteral("Ctrl+T")),
                              QtWlPlugin::FocusManager::WhenNoInput,
                              [this]() { toggleFlat(); return true; });

    if (m_settings.startFlat)
        toggleFlat();

    QtWlPlugin::ThemeManager::applyTheme(this, QtWlPlugin::ThemeManager::currentTheme());

    m_scanner = new ArchiveScanner(this);
    connect(m_scanner, &ArchiveScanner::formatDetected,
            this, &ArchiveViewWidget::onFormatDetected);
    connect(m_scanner, &ArchiveScanner::commentFound,
            this, &ArchiveViewWidget::onCommentFound);
    connect(m_scanner, &ArchiveScanner::passphraseRequested,
            this, &ArchiveViewWidget::onPassphraseRequested);
    connect(m_scanner, &ArchiveScanner::entriesReady,
            this, &ArchiveViewWidget::onEntriesReady);
    connect(m_scanner, &ArchiveScanner::progress,
            this, &ArchiveViewWidget::onProgress);
    connect(m_scanner, &ArchiveScanner::scanFinished,
            this, &ArchiveViewWidget::onScanFinished);

    m_extractor = new ArchiveExtractor(this);
    connect(m_extractor, &ArchiveExtractor::passphraseRequested,
            this, &ArchiveViewWidget::onPassphraseRequested);

    setupContextMenu();

    // Dragging rows out extracts them first. The callback runs on the GUI
    // thread inside startDrag, so it uses the same modal progress dialog as
    // the menu action rather than blocking silently.
    m_view->setMaterialiser([this]() -> QStringList {
        const QStringList members = selectedMembers();
        if (members.isEmpty())
            return {};
        if (!m_scratch)
            m_scratch = std::make_unique<QTemporaryDir>();
        if (!m_scratch->isValid())
            return {};
        return extractMembers(members, m_scratch->path());
    });
}

void ArchiveViewWidget::setupContextMenu()
{
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_view, &QWidget::customContextMenuRequested, this,
            [this](const QPoint &point) {
        QMenu menu(this);

        QAction *preview = menu.addAction(tr("Open with default application"));
        preview->setEnabled(!selectedMembers().isEmpty());
        connect(preview, &QAction::triggered, this,
                &ArchiveViewWidget::onPreviewSelection);

        QAction *extract = menu.addAction(tr("Extract selection to…"));
        extract->setEnabled(!selectedMembers().isEmpty());
        connect(extract, &QAction::triggered, this,
                &ArchiveViewWidget::onExtractSelection);

        menu.addSeparator();

        QAction *flat = menu.addAction(m_model->isFlat() ? tr("Show as tree")
                                                         : tr("Show as flat list"));
        connect(flat, &QAction::triggered, this, [this]() { toggleFlat(); });

        menu.exec(m_view->viewport()->mapToGlobal(point));
    });
}

QStringList ArchiveViewWidget::selectedMembers() const
{
    if (!m_view->selectionModel())
        return {};

    // Collect what is selected, then let the tree expand directories. Walking
    // the view's own rows to find children looks equivalent and is not: Qt
    // hands selections back in the name column, and only column-0 indexes
    // have children, so the descent silently found nothing.
    std::vector<std::string> roots;
    const QModelIndexList rows =
        m_view->selectionModel()->selectedRows(ArchiveModel::NameColumn);
    roots.reserve(rows.size());
    for (const QModelIndex &row : rows) {
        const QString path = pathFor(row);
        if (!path.isEmpty())
            roots.push_back(path.toStdString());
    }

    QStringList members;
    for (const std::string &member : m_model->membersUnder(roots))
        members << QString::fromStdString(member);
    return members;
}

QStringList ArchiveViewWidget::extractMembers(const QStringList &members,
                                              const QString &destination)
{
    // Re-entrancy guard. Extraction runs a nested event loop, so a second
    // request arriving from a menu click or a drag while one is in flight
    // would nest two loops over one extractor and wedge the host.
    if (m_extracting)
        return {};
    m_extracting = true;

    // Reuse a passphrase the user already gave for this archive rather than
    // asking again for the same file.
    m_extractor->setPassphrase(m_scanner->acceptedPassphrase());

    QProgressDialog progress(tr("Extracting from %1…")
                                 .arg(QFileInfo(m_path).fileName()),
                             tr("Cancel"), 0, members.size(), this);
    // Deliberately NOT WindowModal. This widget's top-level window belongs to
    // Double Commander, not to Qt — an LCL window that Qt's modality cannot
    // reason about. Blocking on it is what makes the file manager appear to
    // hang. Disabling our own view is the containment we actually want.
    progress.setWindowModality(Qt::NonModal);
    progress.setMinimumDuration(300);
    m_view->setEnabled(false);

    bool ok = false;
    QString error;
    int refused = 0;

    // A real event loop rather than a `while (!done) processEvents()` spin.
    // The spin had no exit if the terminal signal never arrived, so any
    // failure to emit it presented as a frozen file manager rather than an
    // error — the worst possible failure mode for the one operation here
    // that touches the filesystem.
    QEventLoop loop;

    const auto progressConnection = connect(
        m_extractor, &ArchiveExtractor::extractProgress, &progress,
        [&progress](int done, int total, const QString &member) {
            if (total > 0)
                progress.setValue(done);
            progress.setLabelText(member);
        });
    const auto finishedConnection = connect(
        m_extractor, &ArchiveExtractor::extractFinished, &loop,
        [&](bool succeeded, const QString &message, int, int refusedCount) {
            ok = succeeded;
            error = message;
            refused = refusedCount;
            loop.quit();
        });
    const auto cancelConnection = connect(
        &progress, &QProgressDialog::canceled, this,
        [this]() { m_extractor->cancel(); });

    m_extractor->extract(m_path, members, destination);
    loop.exec();

    disconnect(progressConnection);
    disconnect(finishedConnection);
    disconnect(cancelConnection);

    m_extractor->cancelAndWait();
    progress.reset();
    m_view->setEnabled(true);
    m_extracting = false;

    const QStringList written = m_extractor->writtenPaths();

    if (refused > 0) {
        // Worth saying out loud: these are the traversal and absolute-path
        // members the listing shows verbatim. Silently writing fewer files
        // than were selected would be the wrong kind of quiet.
        QMessageBox::warning(this, tr("Some members were refused"),
            tr("%n member(s) were not extracted because their stored paths "
               "point outside the destination directory (an absolute path, or "
               "one containing \"..\").", nullptr, refused));
    } else if (!ok && !error.isEmpty() && error != QLatin1String("Cancelled")) {
        QMessageBox::warning(this, tr("Extraction failed"), error);
    }

    return written;
}

void ArchiveViewWidget::onExtractSelection()
{
    const QStringList members = selectedMembers();
    if (members.isEmpty())
        return;

    // DontUseNativeDialog on purpose: the native chooser goes out to the
    // desktop portal, and a portal dialog raised from a plugin inside a host
    // that is not a Qt application is a known way to hang. Qt's own dialog
    // has no such dependency.
    const QString destination = QFileDialog::getExistingDirectory(
        this, tr("Extract %n member(s) to", nullptr, members.size()),
        QFileInfo(m_path).absolutePath(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontUseNativeDialog);
    if (destination.isEmpty())
        return;

    extractMembers(members, destination);
}

void ArchiveViewWidget::onPreviewSelection()
{
    const QStringList members = selectedMembers();
    if (members.isEmpty())
        return;

    if (!m_scratch)
        m_scratch = std::make_unique<QTemporaryDir>();
    if (!m_scratch->isValid()) {
        QMessageBox::warning(this, tr("Cannot preview"),
                             tr("No temporary directory available."));
        return;
    }

    // Only the first selected member: opening thirty files in thirty
    // applications is never what someone means by "open".
    const QStringList written = extractMembers({ members.first() },
                                               m_scratch->path());
    if (written.isEmpty())
        return;

    QDesktopServices::openUrl(QUrl::fromLocalFile(written.first()));
}

bool ArchiveViewWidget::loadFile(const QString &path)
{
    QFileInfo info(path);
    if (!info.isFile() || !info.isReadable())
        return false;

    // Answer "is this an archive?" before returning, so DC can fall through
    // to another viewer for anything we cannot show. Bounded — see canRead().
    if (!ArchiveScanner::canRead(path))
        return false;

    stopScan();
    m_model->clearEntries();
    m_summary = archiveview::Summary();
    m_path = path;

    m_status->setFormatInfo(tr("scanning…"));
    m_status->setRowCount(0, 0);
    m_status->removeExtraInfo(QStringLiteral("comment"));
    m_status->removeExtraInfo(QStringLiteral("encrypted"));
    m_status->removeExtraInfo(QStringLiteral("locked"));
    m_status->removeExtraInfo(QStringLiteral("truncated"));
    m_status->removeExtraInfo(QStringLiteral("cancelled"));
    m_status->removeExtraInfo(QStringLiteral("zip64"));
    m_filterBox->clear();

    m_scanner->scan(path, m_settings.maxEntries);
    return true;
}

void ArchiveViewWidget::stopScan()
{
    if (m_extractor)
        m_extractor->cancelAndWait();

    // Close the prompt first. cancelAndWait() would otherwise block the GUI
    // thread until the user answers a dialog that belongs to an archive we
    // are already navigating away from.
    if (m_passphrasePrompt)
        m_passphrasePrompt->reject();

    if (m_scanner)
        m_scanner->cancelAndWait();
}

void ArchiveViewWidget::onPassphraseRequested(int attempt)
{
    QInputDialog dialog(this);
    dialog.setWindowTitle(tr("Encrypted archive"));
    dialog.setLabelText(attempt > 1
        ? tr("That passphrase did not work. Try again for\n%1")
              .arg(QFileInfo(m_path).fileName())
        : tr("Passphrase for\n%1").arg(QFileInfo(m_path).fileName()));
    dialog.setTextEchoMode(QLineEdit::Password);
    dialog.setInputMode(QInputDialog::TextInput);

    m_passphrasePrompt = &dialog;
    const bool accepted = dialog.exec() == QDialog::Accepted;
    m_passphrasePrompt = nullptr;

    // Always answer, even on rejection — the scanner is parked waiting, and a
    // decline is what tells libarchive to stop asking.
    // Both workers may be waiting on this; whichever asked gets the answer,
    // and telling the other is harmless.
    const QString value = accepted ? dialog.textValue() : QString();
    m_scanner->providePassphrase(value, accepted);
    if (m_extractor)
        m_extractor->providePassphrase(value, accepted);
}

void ArchiveViewWidget::toggleFlat()
{
    const bool flat = !m_model->isFlat();
    m_model->setFlat(flat);
    m_view->setRootIsDecorated(!flat);
    m_status->setExtraInfo(QStringLiteral("view"), flat ? tr("Flat") : tr("Tree"));
}

const archiveview::Entry *ArchiveViewWidget::entryFor(const QModelIndex &viewIndex) const
{
    return m_model->entryAt(m_filterProxy->mapToSource(viewIndex));
}

QString ArchiveViewWidget::pathFor(const QModelIndex &viewIndex) const
{
    return m_model->pathAt(m_filterProxy->mapToSource(viewIndex));
}

void ArchiveViewWidget::onFilterChanged(const QString &text)
{
    m_filterProxy->setFilterFixedString(text);

    // Filtering a tree hides most of it; expanding the survivors is the only
    // way the result is legible. Guarded because expandAll() on an unfiltered
    // 100k-entry archive is not something to do on a keystroke.
    if (!text.isEmpty() && m_filterProxy->rowCount() < 500)
        m_view->expandAll();

    int visible = 0;
    QVector<QModelIndex> stack;
    for (int row = 0; row < m_filterProxy->rowCount(); ++row)
        stack.append(m_filterProxy->index(row, 0));
    while (!stack.isEmpty()) {
        const QModelIndex index = stack.takeLast();
        if (entryFor(index))
            ++visible;
        for (int row = 0; row < m_filterProxy->rowCount(index); ++row)
            stack.append(m_filterProxy->index(row, 0, index));
    }
    m_status->setRowCount(visible, m_model->entryCount());
}

void ArchiveViewWidget::applyShowFlags(int showFlags)
{
    // lcp_wraptext and lcp_ansi/ascii are text-viewer concepts with no
    // meaning for a table, and are deliberately ignored. lcp_fittowindow is
    // the one that translates directly.
    if (showFlags & lcp_fittowindow) {
        for (int column = 0; column < ArchiveModel::ColumnCount; ++column)
            m_view->resizeColumnToContents(column);
    }
}

void ArchiveViewWidget::scrollToPercent(int percent)
{
    QScrollBar *bar = m_view->verticalScrollBar();
    if (!bar)
        return;
    const int span = bar->maximum() - bar->minimum();
    bar->setValue(bar->minimum() + span * qBound(0, percent, 100) / 100);
}

void ArchiveViewWidget::showFindPanel(bool findNext)
{
    if (findNext && !m_find->findText().isEmpty()) {
        onFindRequested(true);
        return;
    }
    m_find->showPanel(true);
}

void ArchiveViewWidget::onFindRequested(bool forward)
{
    const QString needle = m_find->findText();
    if (needle.isEmpty())
        return;

    QAbstractItemModel *model = m_view->model();
    const Qt::CaseSensitivity sensitivity =
        m_find->matchCase() ? Qt::CaseSensitive : Qt::CaseInsensitive;

    // Walk in visual order, wrapping, starting from the row after the current.
    QVector<QModelIndex> order;
    QVector<QModelIndex> stack;
    for (int row = model->rowCount() - 1; row >= 0; --row)
        stack.append(model->index(row, ArchiveModel::NameColumn));
    while (!stack.isEmpty()) {
        const QModelIndex current = stack.takeLast();
        order.append(current);
        for (int row = model->rowCount(current) - 1; row >= 0; --row)
            stack.append(model->index(row, ArchiveModel::NameColumn, current));
    }
    if (order.isEmpty()) {
        m_find->setStatusText(tr("Nothing to search"));
        return;
    }

    int start = 0;
    const QModelIndex current = m_view->currentIndex();
    if (current.isValid()) {
        const int found = order.indexOf(current.siblingAtColumn(ArchiveModel::NameColumn));
        if (found >= 0)
            start = found;
    }

    const int total = order.size();
    for (int step = 1; step <= total; ++step) {
        const int offset = forward ? step : -step;
        const QModelIndex candidate = order.at(((start + offset) % total + total) % total);
        if (candidate.data(Qt::DisplayRole).toString().contains(needle, sensitivity)) {
            for (QModelIndex ancestor = candidate.parent(); ancestor.isValid();
                 ancestor = ancestor.parent()) {
                m_view->expand(ancestor);
            }
            m_view->setCurrentIndex(candidate);
            m_view->scrollTo(candidate);
            m_find->setStatusText(QString());
            return;
        }
    }
    m_find->setStatusText(tr("No match for \"%1\"").arg(needle));
}

QString ArchiveViewWidget::listingAsHtml() const
{
    QString html = QStringLiteral("<h3>%1</h3><p>%2</p><table cellspacing='0' "
                                  "cellpadding='2' border='1'><tr>")
                       .arg(QFileInfo(m_path).fileName().toHtmlEscaped(),
                            QString::fromStdString(m_summary.format).toHtmlEscaped());
    for (int column = ArchiveModel::NameColumn; column < ArchiveModel::ColumnCount; ++column) {
        html += QStringLiteral("<th>%1</th>")
                    .arg(m_model->headerData(column, Qt::Horizontal, Qt::DisplayRole)
                             .toString().toHtmlEscaped());
    }
    html += QStringLiteral("</tr>");

    // Printing is a snapshot of what is listed, in flat order — a printed
    // page has no expand/collapse, so the tree shape would only mislead.
    for (int row = 0; row < m_model->entryCount(); ++row) {
        html += QStringLiteral("<tr>");
        for (int column = ArchiveModel::NameColumn; column < ArchiveModel::ColumnCount; ++column) {
            const QModelIndex index = m_model->flatIndex(row, column);
            html += QStringLiteral("<td>%1</td>")
                        .arg(m_model->data(index, Qt::DisplayRole).toString().toHtmlEscaped());
        }
        html += QStringLiteral("</tr>");
    }
    html += QStringLiteral("</table>");
    return html;
}

bool ArchiveViewWidget::printListing(const QString &printerName)
{
    // Refuse a printer we do not recognise. Falling back to the default
    // printer here would mean a print job coming out of a device the caller
    // did not ask for.
    if (!printerName.isEmpty()
        && !QPrinterInfo::availablePrinterNames().contains(printerName)) {
        return false;
    }

    QPrinter printer;
    if (!printerName.isEmpty())
        printer.setPrinterName(printerName);
    if (!printer.isValid())
        return false;

    // A table of members is a document, so let QTextDocument paginate it
    // rather than hand-rolling page breaks.
    QTextDocument document;
    document.setHtml(listingAsHtml());
    document.print(&printer);
    return true;
}

void ArchiveViewWidget::onFormatDetected(const QString &format, const QString &filters)
{
    m_status->setFormatInfo(filters.isEmpty() ? format
                                              : QStringLiteral("%1 (%2)").arg(format, filters));
}

void ArchiveViewWidget::onCommentFound(const QString &comment)
{
    // Shown as a single status-bar line with the full text on hover. Comments
    // are free-form and can be long or multi-line, so the detail panel in M6
    // is where the whole thing belongs.
    QString oneLine = comment.simplified();
    if (oneLine.length() > 60)
        oneLine = oneLine.left(57) + QStringLiteral("...");
    m_status->setExtraInfo(QStringLiteral("comment"), tr("Comment: %1").arg(oneLine));
    m_status->setToolTip(comment);
}

void ArchiveViewWidget::onEntriesReady(const archiveview::EntryBatch &batch)
{
    m_model->appendEntries(batch);
    m_status->setRowCount(m_model->entryCount(), m_model->entryCount());
}

void ArchiveViewWidget::onProgress(qint64 bytesRead, qint64 totalBytes)
{
    if (totalBytes <= 0)
        return;
    const int percent = int(100 * bytesRead / totalBytes);
    if (percent < 100)
        m_status->setExtraInfo(QStringLiteral("progress"),
                               QStringLiteral("%1%").arg(percent));
    else
        m_status->removeExtraInfo(QStringLiteral("progress"));
}

void ArchiveViewWidget::onScanFinished(bool ok, const QString &error,
                                       const archiveview::Summary &summary)
{
    m_summary = summary;
    m_status->removeExtraInfo(QStringLiteral("progress"));

    if (!ok) {
        m_status->setFormatInfo(error.isEmpty() ? tr("unreadable archive") : error);
        return;
    }

    const QLocale locale;
    const QString format = QString::fromStdString(summary.format);
    const QString filters = QString::fromStdString(summary.filters);
    QString info = filters.isEmpty() ? format
                                     : QStringLiteral("%1 (%2)").arg(format, filters);

    // A ratio computed from bytes libarchive actually consumed, rather than
    // stat() of the file compared against a total that may have wrapped.
    if (summary.totalUncompressed > 0 && summary.compressedBytes > 0) {
        const double ratio = 100.0 * double(summary.compressedBytes)
                                   / double(summary.totalUncompressed);
        info += QStringLiteral("  %1 → %2  (%3%)")
                    .arg(locale.formattedDataSize(summary.compressedBytes),
                         locale.formattedDataSize(summary.totalUncompressed))
                    .arg(ratio, 0, 'f', 1);
    }

    if (summary.zip64)
        m_status->setExtraInfo(QStringLiteral("zip64"), tr("ZIP64"));
    if (summary.hasEncryptedEntries)
        m_status->setExtraInfo(QStringLiteral("encrypted"), tr("Encrypted"));
    if (summary.passphraseDeclined)
        m_status->setExtraInfo(QStringLiteral("locked"), tr("Locked — no passphrase"));
    if (summary.truncated)
        m_status->setExtraInfo(QStringLiteral("truncated"), tr("Truncated"));
    if (summary.cancelled)
        m_status->setExtraInfo(QStringLiteral("cancelled"), tr("Cancelled"));

    m_status->setFormatInfo(info);
    m_status->setRowCount(m_model->entryCount(), m_model->entryCount());
}

QString ArchiveViewWidget::selectionAsText(QChar separator) const
{
    const QModelIndexList selected = m_view->selectionModel()
        ? m_view->selectionModel()->selectedRows(ArchiveModel::NameColumn)
        : QModelIndexList();

    QStringList lines;
    lines.reserve(selected.size());
    for (const QModelIndex &index : selected) {
        QStringList fields;
        fields.reserve(ArchiveModel::ColumnCount);
        for (int column = 0; column < ArchiveModel::ColumnCount; ++column) {
            const QModelIndex cell = index.siblingAtColumn(column);
            fields << (column == ArchiveModel::NameColumn
                           ? pathFor(cell)
                           : cell.data(Qt::DisplayRole).toString());
        }
        lines << fields.join(separator);
    }
    return lines.join(QLatin1Char('\n'));
}
