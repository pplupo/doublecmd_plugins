#pragma once

#include <QWidget>
#include <QTableWidget>
#include <QListWidget>
#include <QSplitter>
#include <memory>

#include "TextFormatEngine.h"

namespace QtWlPlugin {
class FocusManager;
class PluginToolBar;
class EditableGridWidget;
class ScopedFindReplacePanel;
}

/// Main plugin widget for structured text file viewing/editing.
///
/// Assembles the full UI: toolbar, grid (via EditableGridWidget), and
/// find/replace panel.  Delegates format-specific parsing to a
/// TextFormatEngine subclass.
///
/// For formats with section navigation (INI), creates a split layout
/// with a section list on the left and the grid on the right.
class StructViewWidget : public QWidget {
    Q_OBJECT
public:
    explicit StructViewWidget(QWidget *parent = nullptr);
    ~StructViewWidget() override;

    bool loadFile(const QString &filepath);
    bool saveFile();
    bool saveFileAs(const QString &path);

    // WLX bridge accessors
    QtWlPlugin::FocusManager *focusManager() const;
    QtWlPlugin::EditableGridWidget *grid() const;
    QString getSelectionAsText(char sep = '\t');

private slots:
    void onSave();
    void onFind(bool forward);
    void onSectionSelected(const QString &sectionName);

private:
    void setupUi();
    void setupToolbar();
    void setupFindReplace();
    void populateGrid();

    QString m_filepath;
    std::unique_ptr<TextFormatEngine> m_engine;
    QtWlPlugin::FocusManager *m_fm;
    QtWlPlugin::PluginToolBar *m_toolbar;
    QtWlPlugin::EditableGridWidget *m_grid;
    QtWlPlugin::ScopedFindReplacePanel *m_findReplace;
    QTableWidget *m_table;

    // Section navigation (INI)
    QSplitter *m_splitter;
    QListWidget *m_sectionList;
    QString m_currentSection;
};
