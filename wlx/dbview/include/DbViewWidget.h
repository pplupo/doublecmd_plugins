#pragma once

#include <QWidget>
#include <memory>

class QComboBox;
class QLabel;
class QTableView;
class QAction;

namespace QtWlPlugin {
class FocusManager;
class PluginToolBar;
class EditableGridWidget;
class FindReplacePanel;
}

class DbEngine;
class KeyValueModel;

/// Main plugin widget for database file viewing/editing.
///
/// Engine-agnostic: works with any DbEngine subclass (SQLite, DuckDB,
/// LevelDB, RocksDB). Adapts the UI based on engine capabilities:
///   - supportsMultipleTables(): show/hide table selector
///   - supportsSubmitRevert(): show/hide Submit/Revert vs "Auto-save" label
///
/// KV-specific context menu: hex view toggle, save value as file,
/// load value from file.
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
    void onTableSelected(const QString &tableName);
    void onSubmitChanges();
    void onRevertChanges();
    void onFind(bool forward);
    void onContextMenu(const QPoint &pos);

private:
    void setupUi(const QString &firstTable);
    void setupToolbar();
    void setupFindReplace();
    void rebuildGrid(const QString &tableName);
    void updateRowCount();

    std::unique_ptr<DbEngine> m_engine;
    QtWlPlugin::FocusManager *m_fm = nullptr;
    QtWlPlugin::PluginToolBar *m_toolbar = nullptr;
    QtWlPlugin::EditableGridWidget *m_grid = nullptr;
    QtWlPlugin::FindReplacePanel *m_findPanel = nullptr;
    QTableView *m_tableView = nullptr;
    QComboBox *m_tableSelector = nullptr;
    QLabel *m_rowCountLabel = nullptr;
    QLabel *m_engineLabel = nullptr;
    QAction *m_actSubmit = nullptr;
    QAction *m_actRevert = nullptr;
};
