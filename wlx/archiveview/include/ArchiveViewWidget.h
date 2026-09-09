#pragma once

#include <QWidget>
#include <QPointer>
#include <QString>
#include <QTemporaryDir>

#include <memory>

#include "core/ArchiveEntry.h"
#include "core/ArchiveSettings.h"

class QDialog;
class QLineEdit;
class QLabel;
class QSortFilterProxyModel;
class ArchiveModel;
class ArchiveScanner;
class ArchiveExtractor;
class ArchiveTreeView;

namespace QtWlPlugin {
class FocusManager;
class PluginStatusBar;
class FindReplacePanel;
}

/// The plugin's root widget: a tree of archive members over a status bar.
///
/// Owns the scanner thread and guarantees it is stopped before anything it
/// signals into is torn down.
class ArchiveViewWidget : public QWidget {
    Q_OBJECT
public:
    explicit ArchiveViewWidget(QWidget *parent = nullptr);
    ~ArchiveViewWidget() override;

    /// Starts an asynchronous scan. Returns false only if the file cannot be
    /// recognised as an archive at all; a successful return means the widget
    /// is showing, not that the scan has finished.
    bool loadFile(const QString &path);
    QString currentFilePath() const { return m_path; }

    /// Blocks until the scanner thread has exited.
    void stopScan();

    /// Apply DC's lcp_* presentation flags from ListLoad/ListLoadNext.
    void applyShowFlags(int showFlags);
    /// DC's lc_setpercent: scroll so `percent` of the listing is above the top.
    void scrollToPercent(int percent);
    /// Show the plugin's own find panel (ListSearchDialog).
    void showFindPanel(bool findNext);
    /// The listing as an HTML document, in flat arrival order.
    /// Separated from printListing() so the document can be generated and
    /// checked without touching a printer.
    QString listingAsHtml() const;
    /// Render the listing to a printer (ListPrint). An empty name means the
    /// system default; a name that is not an available printer is refused
    /// rather than silently redirected to the default.
    bool printListing(const QString &printerName);

    // WLX bridge accessors
    QtWlPlugin::FocusManager *focusManager() const { return m_focus; }
    ArchiveTreeView *view() const { return m_view; }
    ArchiveModel *model() const { return m_model; }
    QString selectionAsText(QChar separator = QLatin1Char('\t')) const;

    /// Map a view index (which may be a filter-proxy index) to the entry
    /// behind it. Every caller must go through this — the view's model is the
    /// proxy, so passing its indices to ArchiveModel directly yields the
    /// wrong row whenever a filter is active.
    const archiveview::Entry *entryFor(const QModelIndex &viewIndex) const;
    QString pathFor(const QModelIndex &viewIndex) const;

private slots:
    void onFormatDetected(const QString &format, const QString &filters);
    void onCommentFound(const QString &comment);
    void onPassphraseRequested(int attempt);
    void onExtractSelection();
    void onPreviewSelection();
    void onEntriesReady(const archiveview::EntryBatch &batch);
    void onProgress(qint64 bytesRead, qint64 totalBytes);
    void onScanFinished(bool ok, const QString &error,
                        const archiveview::Summary &summary);

private:
    void setupUi();
    void toggleFlat();
    void onFindRequested(bool forward);
    void onFilterChanged(const QString &text);
    void onCurrentChanged(const QModelIndex &current);
    void updateDetailPanel(const archiveview::Entry *entry, const QString &path);
    void setupContextMenu();
    /// Normalised in-archive paths of every selected member.
    QStringList selectedMembers() const;
    /// Extract `members` to `destination`, showing a cancellable modal
    /// progress dialog. Returns the paths written.
    QStringList extractMembers(const QStringList &members,
                               const QString &destination);

    QtWlPlugin::FindReplacePanel *m_find = nullptr;

    ArchiveTreeView *m_view = nullptr;
    ArchiveModel *m_model = nullptr;
    QSortFilterProxyModel *m_filterProxy = nullptr;
    QLineEdit *m_filterBox = nullptr;
    QLabel *m_detail = nullptr;
    ArchiveScanner *m_scanner = nullptr;
    QtWlPlugin::PluginStatusBar *m_status = nullptr;
    QtWlPlugin::FocusManager *m_focus = nullptr;
    ArchiveExtractor *m_extractor = nullptr;
    /// Holds files extracted for preview and drag-out. Destroyed with the
    /// widget, which is what cleans them up.
    std::unique_ptr<QTemporaryDir> m_scratch;

    /// The passphrase prompt runs a nested event loop on the GUI thread, so
    /// it has to be closed before the scanner is joined or the widget torn
    /// down — otherwise teardown waits on a dialog nobody is looking at.
    QPointer<QDialog> m_passphrasePrompt;

    QString m_path;
    archiveview::Summary m_summary;
    archiveview::Settings m_settings;
};
