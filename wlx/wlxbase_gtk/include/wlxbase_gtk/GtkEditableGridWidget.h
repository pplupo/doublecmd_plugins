#pragma once

#include <gtk/gtk.h>
#include <vector>
#include <string>
#include <functional>

namespace GtkWlPlugin {

class GtkFocusManager;

/// GTK counterpart to QtWlPlugin::EditableGridWidget — a GtkTreeView (all
/// columns G_TYPE_STRING) wrapper with undo/redo (via GtkFocusManager's
/// command stack), tab-separated copy/paste, and row insert/delete. Scope
/// note: does not port EditableGridWidget's drag-to-reorder-columns,
/// filter-row, or theme-toggle context-menu integration — those are
/// secondary UI chrome, not the undo/redo + editing functionality this
/// was built to bring to parity.
class GtkEditableGridWidget {
public:
    /// Takes ownership of building a GtkTreeView with `columnCount`
    /// string columns inside a GtkScrolledWindow.
    GtkEditableGridWidget(int columnCount, GtkFocusManager *fm);
    ~GtkEditableGridWidget();

    GtkEditableGridWidget(const GtkEditableGridWidget &) = delete;
    GtkEditableGridWidget &operator=(const GtkEditableGridWidget &) = delete;

    /// The GtkScrolledWindow containing the tree view — add this to your
    /// layout.
    GtkWidget *widget() const { return m_scrolled; }
    GtkWidget *treeView() const { return m_treeView; }
    GtkListStore *store() const { return m_store; }

    void setColumnTitle(int col, const std::string &title);
    void setColumnEditable(int col, bool editable);

    /// Replace all row data. Does not go through the undo stack (matches
    /// EditableGridWidget's behavior on a fresh load).
    void setRowData(const std::vector<std::vector<std::string>> &rows);
    std::vector<std::vector<std::string>> rowData() const;

    int rowCount() const;
    int columnCount() const { return m_columnCount; }

    // --- Data operations (undo-tracked) ---
    void copySelection(char separator = '\t');
    void pasteSelection(char separator = '\t');
    void insertRows(int count, int atRow);
    void deleteSelectedRows();

    /// Programmatic single-cell edit (e.g. from Replace/Replace All),
    /// pushed through the same undo stack as interactive edits.
    void setCellValue(int row, int col, const std::string &text);
    std::string cellValue(int row, int col) const;

    /// Selects (and scrolls to) a single cell — used to highlight Find
    /// results.
    void selectCell(int row, int col);

    bool isDirty() const { return m_dirty; }
    void setDirty(bool dirty);
    /// Called whenever a cell edit, insert, or delete happens (after undo
    /// tracking). Use this to trigger a "save" affordance.
    void setDirtyChangedCallback(std::function<void(bool)> cb) { m_dirtyChangedCb = std::move(cb); }

private:
    void onCellEdited(int col, const std::string &pathStr, const std::string &newText);
    static void cellEditedTrampoline(GtkCellRendererText *, gchar *path, gchar *newText, gpointer data);

    GtkFocusManager *m_fm;
    GtkWidget *m_scrolled;
    GtkWidget *m_treeView;
    GtkListStore *m_store;
    int m_columnCount;
    bool m_dirty = false;
    std::function<void(bool)> m_dirtyChangedCb;

    // Column-edit trampoline context (one per column, freed with the widget)
    struct ColCtx { GtkEditableGridWidget *self; int col; };
    std::vector<ColCtx *> m_colContexts;
};

} // namespace GtkWlPlugin
