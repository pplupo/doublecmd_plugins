#include "wlxbase_gtk/GtkEditableGridWidget.h"
#include "wlxbase_gtk/GtkFocusManager.h"

#include <sstream>
#include <algorithm>

namespace GtkWlPlugin {

GtkEditableGridWidget::GtkEditableGridWidget(int columnCount, GtkFocusManager *fm)
    : m_fm(fm)
    , m_columnCount(columnCount)
{
    m_scrolled = gtk_scrolled_window_new(nullptr, nullptr);
    m_treeView = gtk_tree_view_new();
    gtk_container_add(GTK_CONTAINER(m_scrolled), m_treeView);
    gtk_tree_view_set_grid_lines(GTK_TREE_VIEW(m_treeView), GTK_TREE_VIEW_GRID_LINES_BOTH);
    gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(m_treeView)), GTK_SELECTION_MULTIPLE);

    std::vector<GType> types(columnCount, G_TYPE_STRING);
    m_store = gtk_list_store_newv(columnCount, types.data());
    gtk_tree_view_set_model(GTK_TREE_VIEW(m_treeView), GTK_TREE_MODEL(m_store));
    // The view holds its own ref via set_model; drop ours so the store is
    // owned solely by the view and freed correctly on teardown.
    g_object_unref(m_store);

    for (int c = 0; c < columnCount; ++c) {
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
        g_object_set(renderer, "editable", TRUE, nullptr);

        auto *ctx = new ColCtx{this, c};
        m_colContexts.push_back(ctx);
        g_signal_connect(renderer, "edited", G_CALLBACK(cellEditedTrampoline), ctx);

        GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
            ("Column " + std::to_string(c + 1)).c_str(), renderer, "text", c, nullptr);
        gtk_tree_view_column_set_resizable(col, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(m_treeView), col);
    }

    if (fm) fm->addInputWidget(m_treeView);
}

GtkEditableGridWidget::~GtkEditableGridWidget()
{
    if (m_fm) m_fm->removeInputWidget(m_treeView);
    for (auto *ctx : m_colContexts) delete ctx;
}

void GtkEditableGridWidget::setColumnTitle(int col, const std::string &title)
{
    GtkTreeViewColumn *c = gtk_tree_view_get_column(GTK_TREE_VIEW(m_treeView), col);
    if (c) gtk_tree_view_column_set_title(c, title.c_str());
}

void GtkEditableGridWidget::setColumnEditable(int col, bool editable)
{
    GtkTreeViewColumn *c = gtk_tree_view_get_column(GTK_TREE_VIEW(m_treeView), col);
    if (!c) return;
    GList *renderers = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(c));
    for (GList *l = renderers; l; l = l->next)
        g_object_set(l->data, "editable", editable ? TRUE : FALSE, nullptr);
    g_list_free(renderers);
}

void GtkEditableGridWidget::setRowData(const std::vector<std::vector<std::string>> &rows)
{
    gtk_list_store_clear(m_store);
    for (const auto &row : rows) {
        GtkTreeIter iter;
        gtk_list_store_append(m_store, &iter);
        for (int c = 0; c < m_columnCount; ++c)
            gtk_list_store_set(m_store, &iter, c, c < (int)row.size() ? row[c].c_str() : "", -1);
    }
}

std::vector<std::vector<std::string>> GtkEditableGridWidget::rowData() const
{
    std::vector<std::vector<std::string>> result;
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(m_store), &iter);
    while (valid) {
        std::vector<std::string> row;
        row.reserve(m_columnCount);
        for (int c = 0; c < m_columnCount; ++c) {
            gchar *text = nullptr;
            gtk_tree_model_get(GTK_TREE_MODEL(m_store), &iter, c, &text, -1);
            row.push_back(text ? text : "");
            g_free(text);
        }
        result.push_back(std::move(row));
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(m_store), &iter);
    }
    return result;
}

int GtkEditableGridWidget::rowCount() const
{
    return gtk_tree_model_iter_n_children(GTK_TREE_MODEL(m_store), nullptr);
}

void GtkEditableGridWidget::setDirty(bool dirty)
{
    if (m_dirty == dirty) return;
    m_dirty = dirty;
    if (m_dirtyChangedCb) m_dirtyChangedCb(dirty);
}

void GtkEditableGridWidget::cellEditedTrampoline(GtkCellRendererText *, gchar *path, gchar *newText, gpointer data)
{
    auto *ctx = static_cast<ColCtx *>(data);
    ctx->self->onCellEdited(ctx->col, path, newText);
}

void GtkEditableGridWidget::onCellEdited(int col, const std::string &pathStr, const std::string &newText)
{
    GtkTreePath *path = gtk_tree_path_new_from_string(pathStr.c_str());
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(m_store), &iter, path)) {
        gtk_tree_path_free(path);
        return;
    }
    gtk_tree_path_free(path);

    gchar *oldTextRaw = nullptr;
    gtk_tree_model_get(GTK_TREE_MODEL(m_store), &iter, col, &oldTextRaw, -1);
    std::string oldText = oldTextRaw ? oldTextRaw : "";
    g_free(oldTextRaw);

    if (oldText == newText) return;

    GtkTreeRowReference *rowRef = gtk_tree_row_reference_new(
        GTK_TREE_MODEL(m_store), gtk_tree_model_get_path(GTK_TREE_MODEL(m_store), &iter));

    GtkListStore *store = m_store;
    auto applyText = [store, rowRef, col](const std::string &text) {
        GtkTreePath *p = gtk_tree_row_reference_get_path(rowRef);
        if (!p) return;
        GtkTreeIter it;
        if (gtk_tree_model_get_iter(GTK_TREE_MODEL(store), &it, p))
            gtk_list_store_set(store, &it, col, text.c_str(), -1);
        gtk_tree_path_free(p);
    };

    GtkFocusManager::UndoCommand cmd;
    cmd.text = "Edit cell";
    cmd.undo = [applyText, oldText]() { applyText(oldText); };
    cmd.redo = [applyText, newText]() { applyText(newText); };

    if (m_fm) {
        // redo() already applies newText via pushUndo's immediate-redo
        // semantics — but the GtkCellRendererText has ALREADY written
        // newText into the model by the time "edited" fires, so calling
        // redo() again here is a harmless no-op re-application, not a
        // double-edit; it just keeps the undo stack authoritative.
        m_fm->pushUndo(std::move(cmd));
    }
    setDirty(true);
    gtk_tree_row_reference_free(rowRef);
}

void GtkEditableGridWidget::setCellValue(int row, int col, const std::string &text)
{
    GtkTreePath *path = gtk_tree_path_new_from_indices(row, -1);
    GtkTreeIter iter;
    bool ok = gtk_tree_model_get_iter(GTK_TREE_MODEL(m_store), &iter, path);
    gtk_tree_path_free(path);
    if (!ok) return;

    gchar *oldRaw = nullptr;
    gtk_tree_model_get(GTK_TREE_MODEL(m_store), &iter, col, &oldRaw, -1);
    std::string oldText = oldRaw ? oldRaw : "";
    g_free(oldRaw);
    if (oldText == text) return;

    GtkListStore *store = m_store;
    auto applyText = [store, row, col](const std::string &t) {
        GtkTreePath *p = gtk_tree_path_new_from_indices(row, -1);
        GtkTreeIter it;
        if (gtk_tree_model_get_iter(GTK_TREE_MODEL(store), &it, p))
            gtk_list_store_set(store, &it, col, t.c_str(), -1);
        gtk_tree_path_free(p);
    };

    GtkFocusManager::UndoCommand cmd;
    cmd.text = "Edit cell";
    cmd.undo = [applyText, oldText]() { applyText(oldText); };
    cmd.redo = [applyText, text]() { applyText(text); };
    if (m_fm) m_fm->pushUndo(std::move(cmd));
    else applyText(text);
    setDirty(true);
}

std::string GtkEditableGridWidget::cellValue(int row, int col) const
{
    GtkTreePath *path = gtk_tree_path_new_from_indices(row, -1);
    GtkTreeIter iter;
    bool ok = gtk_tree_model_get_iter(GTK_TREE_MODEL(m_store), &iter, path);
    gtk_tree_path_free(path);
    if (!ok) return {};
    gchar *text = nullptr;
    gtk_tree_model_get(GTK_TREE_MODEL(m_store), &iter, col, &text, -1);
    std::string result = text ? text : "";
    g_free(text);
    return result;
}

void GtkEditableGridWidget::selectCell(int row, int col)
{
    GtkTreePath *path = gtk_tree_path_new_from_indices(row, -1);
    GtkTreeViewColumn *column = gtk_tree_view_get_column(GTK_TREE_VIEW(m_treeView), col);
    gtk_tree_view_set_cursor(GTK_TREE_VIEW(m_treeView), path, column, FALSE);
    gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(m_treeView), path, column, TRUE, 0.5, 0.0);
    gtk_tree_path_free(path);
}

void GtkEditableGridWidget::copySelection(char separator)
{
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(m_treeView));
    GList *rows = gtk_tree_selection_get_selected_rows(sel, nullptr);
    if (!rows) return;

    std::vector<int> indices;
    for (GList *l = rows; l; l = l->next) {
        GtkTreePath *p = static_cast<GtkTreePath *>(l->data);
        int *idx = gtk_tree_path_get_indices(p);
        if (idx) indices.push_back(idx[0]);
    }
    std::sort(indices.begin(), indices.end());

    std::ostringstream out;
    auto data = rowData();
    for (size_t i = 0; i < indices.size(); ++i) {
        if (i > 0) out << "\n";
        int r = indices[i];
        if (r < 0 || r >= (int)data.size()) continue;
        for (int c = 0; c < m_columnCount; ++c) {
            if (c > 0) out << separator;
            out << data[r][c];
        }
    }

    GtkClipboard *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_text(clipboard, out.str().c_str(), -1);

    g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
}

void GtkEditableGridWidget::pasteSelection(char separator)
{
    GtkClipboard *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gchar *text = gtk_clipboard_wait_for_text(clipboard);
    if (!text) return;
    std::string clip(text);
    g_free(text);

    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(m_treeView));
    GList *rows = gtk_tree_selection_get_selected_rows(sel, nullptr);
    int startRow = 0;
    if (rows) {
        GtkTreePath *p = static_cast<GtkTreePath *>(rows->data);
        int *idx = gtk_tree_path_get_indices(p);
        if (idx) startRow = idx[0];
        g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
    }

    auto before = rowData();
    auto after = before;

    std::istringstream lineStream(clip);
    std::string line;
    int r = startRow;
    while (std::getline(lineStream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        while ((int)after.size() <= r) after.emplace_back(m_columnCount, "");

        std::vector<std::string> cells;
        std::istringstream cellStream(line);
        std::string cell;
        while (std::getline(cellStream, cell, separator)) cells.push_back(cell);
        if (line.empty()) cells.push_back("");

        for (int c = 0; c < (int)cells.size() && c < m_columnCount; ++c)
            after[r][c] = cells[c];
        ++r;
    }

    GtkEditableGridWidget *self = this;
    GtkFocusManager::UndoCommand cmd;
    cmd.text = "Paste";
    cmd.undo = [self, before]() { self->setRowData(before); };
    cmd.redo = [self, after]() { self->setRowData(after); };
    if (m_fm) m_fm->pushUndo(std::move(cmd));
    else setRowData(after);
    setDirty(true);
}

void GtkEditableGridWidget::insertRows(int count, int atRow)
{
    auto before = rowData();
    auto after = before;
    std::vector<std::string> blank(m_columnCount, "");
    int pos = std::clamp(atRow, 0, (int)after.size());
    after.insert(after.begin() + pos, count, blank);

    GtkEditableGridWidget *self = this;
    GtkFocusManager::UndoCommand cmd;
    cmd.text = "Insert rows";
    cmd.undo = [self, before]() { self->setRowData(before); };
    cmd.redo = [self, after]() { self->setRowData(after); };
    if (m_fm) m_fm->pushUndo(std::move(cmd));
    else setRowData(after);
    setDirty(true);
}

void GtkEditableGridWidget::deleteSelectedRows()
{
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(m_treeView));
    GList *rows = gtk_tree_selection_get_selected_rows(sel, nullptr);
    if (!rows) return;

    std::vector<int> indices;
    for (GList *l = rows; l; l = l->next) {
        int *idx = gtk_tree_path_get_indices(static_cast<GtkTreePath *>(l->data));
        if (idx) indices.push_back(idx[0]);
    }
    g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
    std::sort(indices.rbegin(), indices.rend()); // descending, so erase-by-index stays valid

    auto before = rowData();
    auto after = before;
    for (int idx : indices)
        if (idx >= 0 && idx < (int)after.size())
            after.erase(after.begin() + idx);

    GtkEditableGridWidget *self = this;
    GtkFocusManager::UndoCommand cmd;
    cmd.text = "Delete rows";
    cmd.undo = [self, before]() { self->setRowData(before); };
    cmd.redo = [self, after]() { self->setRowData(after); };
    if (m_fm) m_fm->pushUndo(std::move(cmd));
    else setRowData(after);
    setDirty(true);
}

} // namespace GtkWlPlugin
