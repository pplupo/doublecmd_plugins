#pragma once

#include <QWidget>

class QComboBox;
class QLabel;
class QTableView;

namespace QtWlPlugin {
class FocusManager;
class PluginToolBar;
class EditableGridWidget;
class FindReplacePanel;
}

class SqliteBackend;

/// Main plugin widget for SQLite database file viewing/editing.
///
/// Assembles the full UI: toolbar with table selector combo,
/// grid (via EditableGridWidget in LiveDatabase mode), and
/// find panel (search-only, no replace).
///
/// The table selector combo allows switching between tables and views.
/// Changes are buffered via QSqlTableModel::OnManualSubmit until
/// the user clicks Submit or presses Ctrl+S.
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

private:
    void setupUi(const QString &firstTable);
    void setupToolbar();
    void setupFindReplace();
    void rebuildGrid(const QString &tableName);
    void updateRowCount();

    SqliteBackend *m_backend;
    QtWlPlugin::FocusManager *m_fm;
    QtWlPlugin::PluginToolBar *m_toolbar;
    QtWlPlugin::EditableGridWidget *m_grid;
    QtWlPlugin::FindReplacePanel *m_findPanel;
    QTableView *m_tableView;
    QComboBox *m_tableSelector;
    QLabel *m_rowCountLabel;
};
