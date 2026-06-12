#pragma once

#include <QWidget>
#include <QListWidget>
#include <QTableView>
#include <memory>

class QAction;

namespace QtWlPlugin {
class FocusManager;
class PluginToolBar;
class EditableGridWidget;
class FindReplacePanel;
class FilterableHeaderView;
class PluginStatusBar;
class PluginSplitView;
}

class DbEngine;
class KeyValueModel;
class QSortFilterProxyModel;
class QUndoStack;

/// Main plugin widget for database file viewing/editing.
///
/// Layout:
///   ┌────────────────────────────────────────────┐
///   │  PluginToolBar                             │
///   ├────────────┬───────────────────────────────┤
///   │ QListWidget│ FilterableHeaderView          │
///   │ (tables/   ├───────────────────────────────┤
///   │  views)    │ QTableView (grid)             │
///   │            │                               │
///   ├────────────┴───────────────────────────────┤
///   │  PluginStatusBar                           │
///   └────────────────────────────────────────────┘
///
/// Engine-agnostic: works with any DbEngine subclass (SQLite, DuckDB,
/// LevelDB, RocksDB). Adapts the UI based on engine capabilities.
class DbViewWidget : public QWidget {
    Q_OBJECT
public:
    explicit DbViewWidget(QWidget *parent = nullptr);
    ~DbViewWidget() override;

    bool loadFile(const QString &filepath);

    // WLX bridge accessors
    QtWlPlugin::FocusManager *focusManager() const;
    QtWlPlugin::EditableGridWidget *grid() const;
    QString getSelectionAsText(char sep = '\t');

private slots:
    void onTableSelected(QListWidgetItem *current, QListWidgetItem *previous);
    void onSubmitChanges();
    void onRevertChanges();
    void onFind(bool forward);

private:
    void setupUi(const QString &firstTable);
    void setupToolbar();
    void setupFindReplace();
    void rebuildGrid(const QString &tableName);
    void updateStatusBar();
    void populateTableList();
    void setupGridContextMenu();
    void openValueInspector(const QModelIndex &index);

    std::unique_ptr<DbEngine> m_engine;
    QtWlPlugin::FocusManager *m_fm = nullptr;
    QtWlPlugin::PluginToolBar *m_toolbar = nullptr;
    QtWlPlugin::EditableGridWidget *m_grid = nullptr;
    QtWlPlugin::FindReplacePanel *m_findPanel = nullptr;
    QtWlPlugin::FilterableHeaderView *m_filterHeader = nullptr;
    QtWlPlugin::PluginStatusBar *m_statusBar = nullptr;
    QtWlPlugin::PluginSplitView *m_splitView = nullptr;

    QTableView *m_tableView = nullptr;
    QListWidget *m_tableList = nullptr;
    QAction *m_actSubmit = nullptr;
    QAction *m_actRevert = nullptr;
    QSortFilterProxyModel *m_filterProxy = nullptr;
    QUndoStack *m_undoStack = nullptr;
    QString m_filepath;
};
