#pragma once

#include <gtk/gtk.h>
#include <gtksourceview/gtksource.h>
#include <memory>
#include <string>

#include "wlxbase_gtk/GtkFocusManager.h"
#include "wlxbase_gtk/GtkPluginToolBar.h"
#include "wlxbase_gtk/GtkFindReplacePanel.h"

namespace GtkWlPlugin { class GtkFocusManager; }

// GtkSourceView-based rich code/text editor widget -- the GTK analog of
// kate_qt6's EditorWidget (which wraps KDE's KTextEditor::View). See
// wlx/kate/src/editor_widget.h for the Qt6 sibling this mirrors.
class EditorWidget {
public:
    EditorWidget();
    ~EditorWidget();

    GtkWidget *rootWidget() const { return m_root; }
    GtkWidget *sourceView() const { return m_view; }

    bool loadFile(const std::string &path);
    bool save();
    bool saveAs(const std::string &path);

    void setReadOnly(bool readOnly);
    bool isReadOnly() const;
    void setWordWrap(bool wrap);
    bool wordWrap() const { return m_wordWrap; }
    void reload();

    bool isDirty() const;
    std::string currentFile() const { return m_currentFile; }

    GtkWlPlugin::GtkFocusManager *focusManager() const { return m_fm.get(); }

private:
    void setupToolbar();
    void setupStatusBar();
    void setupFindReplace();
    void detectAndApplyLanguage();
    void applyStyleScheme();
    void updateStatusBar();
    void updateDirtyIndicator();
    void showDiskChangeBar(bool show);
    void doFind(bool forward);
    void doReplace();
    void doReplaceAll();

    GtkWidget *m_root = nullptr;
    GtkWidget *m_scrolled = nullptr;
    GtkWidget *m_view = nullptr; // GtkSourceView*
    GtkSourceBuffer *m_buffer = nullptr;

    std::unique_ptr<GtkWlPlugin::GtkFocusManager> m_fm;
    std::unique_ptr<GtkWlPlugin::GtkPluginToolBar> m_toolbar;
    std::unique_ptr<GtkWlPlugin::GtkFindReplacePanel> m_findPanel;

    GtkWidget *m_dirtyLabel = nullptr;
    GtkWidget *m_undoBtn = nullptr;
    GtkWidget *m_redoBtn = nullptr;
    GtkWidget *m_readOnlyToggle = nullptr;
    GtkWidget *m_wrapToggle = nullptr;

    GtkWidget *m_statusBar = nullptr;
    GtkWidget *m_posLabel = nullptr;
    GtkWidget *m_langLabel = nullptr;
    GtkWidget *m_modeLabel = nullptr;

    GtkWidget *m_diskChangeBar = nullptr;
    GtkWidget *m_diskChangeLabel = nullptr;
    GFileMonitor *m_monitor = nullptr;
    gulong m_monitorHandler = 0;
    bool m_ignoreNextDiskChange = false;

    std::string m_currentFile;
    bool m_wordWrap = false;
    int m_findFromLine = 0, m_findFromCol = 0;
};
