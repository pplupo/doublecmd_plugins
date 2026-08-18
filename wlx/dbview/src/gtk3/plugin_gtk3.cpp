// GTK3 UI for dbview: a GtkTreeView schema list (tables/views) on the
// left, a GtkEditableGridWidget-backed grid on the right, a Submit/Revert
// toolbar, and (for SQL engines) a query console -- all on top of the
// Qt-free DbEngineCore/KeyValueEngineCoreBase engines.

#include "core/DbEngineCore.h"
#include "wlxbase_gtk/GtkFocusManager.h"
#include "wlxbase_gtk/GtkEditableGridWidget.h"
#include "wlxbase_gtk/GtkPluginToolBar.h"
#include "wlxbase_gtk/GtkFindReplacePanel.h"

#include <gtk/gtk.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "wlxplugin.h"

#define EXPORT __attribute__((visibility("default")))

using namespace GtkWlPlugin;

namespace {

struct DbViewState {
    GtkWidget *root = nullptr;
    GtkWidget *paned = nullptr;
    GtkWidget *treeView = nullptr;
    GtkTreeStore *treeStore = nullptr;
    GtkWidget *rightBox = nullptr;
    GtkWidget *gridContainer = nullptr;
    GtkWidget *statusLabel = nullptr;
    GtkWidget *sqlEntry = nullptr;

    std::unique_ptr<DbEngineCore> engine;
    std::unique_ptr<GtkFocusManager> focusManager;
    std::unique_ptr<GtkEditableGridWidget> grid;
    std::unique_ptr<GtkPluginToolBar> toolbar;
    std::unique_ptr<GtkFindReplacePanel> findPanel;

    GtkWidget *submitBtn = nullptr;
    GtkWidget *revertBtn = nullptr;
    GtkWidget *findToggle = nullptr;
    bool wordWrap = false;
    bool gridLines = true;

    std::string filepath;
};

enum { COL_NAME = 0, N_COLS };

void onToggleWordWrap(DbViewState *st, bool active);
void onToggleGridLines(DbViewState *st, bool active);

void updateStatus(DbViewState *st) {
    if (!st->statusLabel || !st->engine) return;
    std::string text = st->engine->engineName() + " - " + st->engine->currentTableName() +
        " (" + std::to_string(st->engine->fetchedRowCount()) + "/" + std::to_string(st->engine->rowCount()) + " rows)";
    if (st->engine->isReadOnly()) text += " [read-only]";
    gtk_label_set_text(GTK_LABEL(st->statusLabel), text.c_str());
}

// Pulls rows [alreadyRead, engine->fetchedRowCount()) out of the engine
// and appends them to the grid -- called once after selectTable()'s first
// chunk, and again each time fetchMoreIfNearBottom() pulls in another
// chunk. Rows already materialized in the engine's own chunk cache are
// converted to grid rows here; the engine itself never holds more than a
// few chunks' worth of rows in memory for a table the UI hasn't scrolled
// through yet (see DbEngineCore::fetchMore()).
void appendEngineRowsToGrid(DbViewState *st, int fromRow) {
    int toRow = st->engine->fetchedRowCount();
    if (toRow <= fromRow) return;
    std::vector<std::vector<std::string>> rows;
    rows.reserve(toRow - fromRow);
    for (int r = fromRow; r < toRow; r++) {
        std::vector<std::string> row;
        row.reserve(st->engine->columnCount());
        for (int c = 0; c < st->engine->columnCount(); c++)
            row.push_back(st->engine->cellText(r, c));
        rows.push_back(std::move(row));
    }
    st->grid->appendRows(rows);
}

void loadGridFromEngine(DbViewState *st) {
    if (st->grid) {
        gtk_container_remove(GTK_CONTAINER(st->gridContainer), st->grid->widget());
        st->grid.reset();
    }
    int cols = std::max(1, st->engine->columnCount());
    st->grid = std::make_unique<GtkWlPlugin::GtkEditableGridWidget>(cols, st->focusManager.get());
    for (int c = 0; c < st->engine->columnCount(); c++) {
        st->grid->setColumnTitle(c, st->engine->columnName(c));
        // Editability is effectively per-table, not per-cell, for every
        // engine we have (a row either all has a rowid/DB_KEY or none do);
        // row 0 (if any rows are loaded) is representative.
        bool editable = st->engine->fetchedRowCount() > 0 && st->engine->cellEditable(0, c);
        st->grid->setColumnEditable(c, editable);
    }

    appendEngineRowsToGrid(st, 0);

    // Word Wrap / Grid Lines are per-GtkTreeView toggle state on the toolbar,
    // but each table switch tears down and rebuilds the grid's own
    // GtkTreeView -- reapply the persisted toggle state to the new one.
    onToggleWordWrap(st, st->wordWrap);
    onToggleGridLines(st, st->gridLines);

    gtk_box_pack_start(GTK_BOX(st->gridContainer), st->grid->widget(), TRUE, TRUE, 0);
    gtk_widget_show_all(st->grid->widget());

    // Trigger fetchMore() as the user scrolls near the bottom of what's
    // currently loaded, appending only the newly-fetched rows -- mirrors
    // QAbstractItemModel::canFetchMore()/fetchMore() on the Qt side.
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(st->grid->widget()));
    g_signal_connect(vadj, "value-changed", G_CALLBACK(+[](GtkAdjustment *adj, gpointer data) {
        auto *st2 = (DbViewState *)data;
        if (!st2->engine || !st2->engine->canFetchMore()) return;
        double value = gtk_adjustment_get_value(adj);
        double upper = gtk_adjustment_get_upper(adj);
        double page = gtk_adjustment_get_page_size(adj);
        if (value + page >= upper - page * 0.5) { // within half a page of the bottom
            int before = st2->engine->fetchedRowCount();
            st2->engine->fetchMore();
            appendEngineRowsToGrid(st2, before);
        }
    }), st);
}

void selectTable(DbViewState *st, const std::string &name) {
    if (!st->engine->selectTable(name)) return;
    loadGridFromEngine(st);
    updateStatus(st);
}

void populateTree(DbViewState *st) {
    gtk_tree_store_clear(st->treeStore);
    if (!st->engine) return;

    GtkTreeIter tablesIter;
    gtk_tree_store_append(st->treeStore, &tablesIter, nullptr);
    gtk_tree_store_set(st->treeStore, &tablesIter, COL_NAME, "Tables", -1);
    for (auto &t : st->engine->tableNames()) {
        GtkTreeIter it;
        gtk_tree_store_append(st->treeStore, &it, &tablesIter);
        gtk_tree_store_set(st->treeStore, &it, COL_NAME, t.c_str(), -1);
    }

    auto views = st->engine->viewNames();
    if (!views.empty()) {
        GtkTreeIter viewsIter;
        gtk_tree_store_append(st->treeStore, &viewsIter, nullptr);
        gtk_tree_store_set(st->treeStore, &viewsIter, COL_NAME, "Views", -1);
        for (auto &v : views) {
            GtkTreeIter it;
            gtk_tree_store_append(st->treeStore, &it, &viewsIter);
            gtk_tree_store_set(st->treeStore, &it, COL_NAME, v.c_str(), -1);
        }
    }

    gtk_tree_view_expand_all(GTK_TREE_VIEW(st->treeView));
}

void onTreeSelectionChanged(GtkTreeSelection *sel, gpointer data) {
    auto *st = (DbViewState *)data;
    GtkTreeIter iter;
    GtkTreeModel *model;
    if (!gtk_tree_selection_get_selected(sel, &model, &iter)) return;
    if (gtk_tree_model_iter_has_child(model, &iter)) return; // category node ("Tables"/"Views")

    gchar *name = nullptr;
    gtk_tree_model_get(model, &iter, COL_NAME, &name, -1);
    if (name) { selectTable(st, name); g_free(name); }
}

void onSubmitClicked(DbViewState *st) {
    // submitAll() may reopen the cursor internally (Firebird: statements
    // must be freed before COMMIT), which resets the engine's own fetched-
    // rows cache -- reload the grid from scratch to match, same as revert.
    if (st->engine->submitAll()) { loadGridFromEngine(st); updateStatus(st); }
}
void onRevertClicked(DbViewState *st) {
    if (st->engine->revertAll()) { loadGridFromEngine(st); updateStatus(st); }
}

void onToggleWordWrap(DbViewState *st, bool active) {
    st->wordWrap = active;
    if (!st->grid) return;
    GList *columns = gtk_tree_view_get_columns(GTK_TREE_VIEW(st->grid->treeView()));
    for (GList *l = columns; l; l = l->next) {
        GList *renderers = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(l->data));
        for (GList *r = renderers; r; r = r->next) {
            g_object_set(r->data,
                "wrap-width", active ? 300 : -1,
                "wrap-mode", active ? PANGO_WRAP_WORD_CHAR : PANGO_WRAP_WORD,
                nullptr);
        }
        g_list_free(renderers);
    }
    g_list_free(columns);
    gtk_widget_queue_resize(st->grid->treeView());
}

void onToggleGridLines(DbViewState *st, bool active) {
    st->gridLines = active;
    if (st->grid)
        gtk_tree_view_set_grid_lines(GTK_TREE_VIEW(st->grid->treeView()),
            active ? GTK_TREE_VIEW_GRID_LINES_BOTH : GTK_TREE_VIEW_GRID_LINES_NONE);
}

void onExportTableData(DbViewState *st) {
    if (!st->grid) return;
    GtkWidget *toplevel = gtk_widget_get_toplevel(st->root);
    GtkWidget *dialog = gtk_file_chooser_dialog_new("Export Table Data",
        GTK_IS_WINDOW(toplevel) ? GTK_WINDOW(toplevel) : nullptr,
        GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Export", GTK_RESPONSE_ACCEPT, nullptr);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog),
        (st->engine->currentTableName() + ".csv").c_str());
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        std::ofstream out(path);
        if (out) {
            int ncols = st->grid->columnCount();
            for (int c = 0; c < ncols; c++) {
                if (c) out << ',';
                out << '"' << st->engine->columnName(c) << '"';
            }
            out << '\n';
            for (auto &row : st->grid->rowData()) {
                for (int c = 0; c < (int)row.size(); c++) {
                    if (c) out << ',';
                    std::string cell = row[c];
                    std::string escaped;
                    for (char ch : cell) { if (ch == '"') escaped += '"'; escaped += ch; }
                    out << '"' << escaped << '"';
                }
                out << '\n';
            }
        }
        g_free(path);
    }
    gtk_widget_destroy(dialog);
}

bool dbCellMatches(const std::string &text, const std::string &query, bool matchCase) {
    if (query.empty()) return false;
    if (matchCase) return text.find(query) != std::string::npos;
    std::string t = text, q = query;
    std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return std::tolower(c); });
    std::transform(q.begin(), q.end(), q.begin(), [](unsigned char c) { return std::tolower(c); });
    return t.find(q) != std::string::npos;
}

void doFind(DbViewState *st, bool forward) {
    if (!st->grid) return;
    std::string query = st->findPanel->findText();
    if (query.empty()) return;
    bool matchCase = st->findPanel->matchCase();

    auto rows = st->grid->rowData();
    int nrows = (int)rows.size(), ncols = st->grid->columnCount();
    if (nrows == 0 || ncols == 0) return;
    int total = nrows * ncols;
    for (int step = 1; step <= total; ++step) {
        int idx = forward ? step - 1 : total - step;
        int r = idx / ncols, c = idx % ncols;
        if (r >= (int)rows.size() || c >= (int)rows[r].size()) continue;
        if (dbCellMatches(rows[r][c], query, matchCase)) {
            st->grid->selectCell(r, c);
            st->findPanel->setStatusText("Match found");
            return;
        }
    }
    st->findPanel->setStatusText("No matches");
}
void onRunSqlClicked(GtkButton *, gpointer data) {
    auto *st = (DbViewState *)data;
    if (!st->sqlEntry) return;
    const gchar *text = gtk_entry_get_text(GTK_ENTRY(st->sqlEntry));
    if (!text || !*text) return;
    if (st->engine->selectQuery(text)) {
        loadGridFromEngine(st);
        gtk_label_set_text(GTK_LABEL(st->statusLabel), (st->engine->engineName() + " - query result").c_str());
    } else {
        gtk_label_set_text(GTK_LABEL(st->statusLabel), ("Query error: " + st->engine->lastError()).c_str());
    }
}

std::string copySelectionAsText(DbViewState *st) {
    if (!st->grid) return {};
    st->grid->copySelection('\t');
    GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gchar *text = gtk_clipboard_wait_for_text(cb);
    std::string result = text ? text : "";
    if (text) g_free(text);
    return result;
}

void destroyState(GtkWidget *, gpointer data) { delete (DbViewState *)data; }

} // namespace

extern "C" {

EXPORT HWND DCPCALL ListLoad(HWND ParentWin, char *FileToLoad, int ShowFlags) {
    std::string filepath = FileToLoad;
    auto engine = DbEngineCore::createForFile(filepath);
    if (!engine) return nullptr;

    auto tables = engine->tableNames();
    if (tables.empty()) return nullptr;
    if (!engine->selectTable(tables[0])) return nullptr;

    auto *st = new DbViewState();
    st->filepath = filepath;
    st->engine = std::move(engine);

    st->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    // gtk_layout_put() (not gtk_container_add()) is required here: DC's
    // ResizeWindow later calls gtk_layout_move() on this widget, which
    // asserts the widget's parent is exactly this GtkLayout.
    gtk_layout_put(GTK_LAYOUT(ParentWin), st->root, 0, 0);

    GtkWidget *toolbarRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    st->statusLabel = gtk_label_new("");
    gtk_box_pack_end(GTK_BOX(toolbarRow), st->statusLabel, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(st->root), toolbarRow, FALSE, FALSE, 2);

    st->paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(st->root), st->paned, TRUE, TRUE, 0);

    st->treeStore = gtk_tree_store_new(N_COLS, G_TYPE_STRING);
    st->treeView = gtk_tree_view_new_with_model(GTK_TREE_MODEL(st->treeStore));
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes("Schema", renderer, "text", COL_NAME, nullptr);
    gtk_tree_view_append_column(GTK_TREE_VIEW(st->treeView), col);
    GtkWidget *treeScroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_container_add(GTK_CONTAINER(treeScroll), st->treeView);
    gtk_widget_set_size_request(treeScroll, 200, -1);
    gtk_paned_pack1(GTK_PANED(st->paned), treeScroll, FALSE, TRUE);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(st->treeView)), "changed",
                      G_CALLBACK(onTreeSelectionChanged), st);

    st->rightBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_paned_pack2(GTK_PANED(st->paned), st->rightBox, TRUE, TRUE);

    if (st->engine->supportsSqlConsole()) {
        GtkWidget *sqlBar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        st->sqlEntry = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(st->sqlEntry), "SQL query...");
        gtk_box_pack_start(GTK_BOX(sqlBar), st->sqlEntry, TRUE, TRUE, 2);
        GtkWidget *runBtn = gtk_button_new_with_label("Run");
        g_signal_connect(runBtn, "clicked", G_CALLBACK(onRunSqlClicked), st);
        gtk_box_pack_start(GTK_BOX(sqlBar), runBtn, FALSE, FALSE, 2);
        gtk_box_pack_start(GTK_BOX(st->rightBox), sqlBar, FALSE, FALSE, 2);
    }

    st->gridContainer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(st->rightBox), st->gridContainer, TRUE, TRUE, 0);

    st->focusManager = std::make_unique<GtkFocusManager>(st->root, st->treeView);

    st->toolbar = std::make_unique<GtkPluginToolBar>(st->focusManager.get());
    if (st->engine->supportsSubmitRevert()) {
        st->submitBtn = st->toolbar->addToolAction("Commit", "document-save-symbolic",
            [st]() { onSubmitClicked(st); });
        st->revertBtn = st->toolbar->addToolAction("Revert", "edit-undo-symbolic",
            [st]() { onRevertClicked(st); });
        if (st->engine->isReadOnly()) {
            gtk_widget_set_sensitive(st->submitBtn, FALSE);
            gtk_widget_set_sensitive(st->revertBtn, FALSE);
        }
        st->toolbar->addSeparator();
    }
    st->toolbar->addToggleAction("Word Wrap", "format-text-wrap-symbolic", false,
        [st](bool active) { onToggleWordWrap(st, active); });
    st->toolbar->addToggleAction("Grid Lines", "view-grid-symbolic", true,
        [st](bool active) { onToggleGridLines(st, active); });
    st->findToggle = st->toolbar->addToggleAction("Find", "edit-find-symbolic", false,
        [st](bool active) { st->findPanel->showPanel(active); });
    st->toolbar->addToolAction("Export", "document-send-symbolic",
        [st]() { onExportTableData(st); });
    gtk_box_pack_start(GTK_BOX(toolbarRow), st->toolbar->widget(), TRUE, TRUE, 0);
    gtk_box_reorder_child(GTK_BOX(toolbarRow), st->toolbar->widget(), 0);

    st->findPanel = std::make_unique<GtkFindReplacePanel>(st->focusManager.get());
    st->findPanel->setReplaceEnabled(false);
    st->findPanel->onFindRequested = [st](bool forward) { doFind(st, forward); };
    gtk_box_pack_start(GTK_BOX(st->root), st->findPanel->widget(), FALSE, FALSE, 0);
    gtk_box_reorder_child(GTK_BOX(st->root), st->findPanel->widget(), 1);

    loadGridFromEngine(st);

    g_signal_connect(st->root, "destroy", G_CALLBACK(destroyState), st);
    g_object_set_data(G_OBJECT(st->root), "__dbview_state_ptr", st);

    populateTree(st);
    updateStatus(st);

    gtk_widget_show_all(st->root);
    return (HWND)st->root;
}

EXPORT void DCPCALL ListCloseWindow(HWND ListWin) {
    GtkWidget *w = (GtkWidget *)ListWin;
    if (w) gtk_widget_destroy(w);
}

EXPORT void DCPCALL ListGetDetectString(char *DetectString, int maxlen) {
    snprintf(DetectString, maxlen - 1,
        "EXT=\"SQLITE\" | EXT=\"SQLITE3\" | EXT=\"DB\" | EXT=\"DB3\" | "
        "EXT=\"DUCKDB\" | EXT=\"PARQUET\" | EXT=\"PQ\" | EXT=\"FDB\" | "
        "EXT=\"LMDB\" | EXT=\"BDB\" | EXT=\"MDB\" | EXT=\"ACCDB\"");
}

EXPORT int DCPCALL ListSearchText(HWND ListWin, char *SearchString, int SearchParameter) {
    GtkWidget *w = (GtkWidget *)ListWin;
    if (!w) return LISTPLUGIN_ERROR;
    auto *st = (DbViewState *)g_object_get_data(G_OBJECT(w), "__dbview_state_ptr");
    if (!st || !st->grid) return LISTPLUGIN_ERROR;

    std::string needle = SearchString ? SearchString : "";
    bool matchCase = SearchParameter & lcs_matchcase;
    bool backward = SearchParameter & lcs_backwards;

    auto rows = st->grid->rowData();
    int nrows = (int)rows.size();
    int ncols = st->grid->columnCount();
    if (nrows == 0 || ncols == 0) return LISTPLUGIN_ERROR;

    auto toLower = [](std::string s) { for (auto &c : s) c = (char)tolower((unsigned char)c); return s; };
    std::string needleCmp = matchCase ? needle : toLower(needle);

    int total = nrows * ncols;
    for (int i = 0; i < total; ++i) {
        int idx = backward ? (total - 1 - i) : i;
        int r = idx / ncols, c = idx % ncols;
        if (r >= (int)rows.size() || c >= (int)rows[r].size()) continue;
        std::string cell = matchCase ? rows[r][c] : toLower(rows[r][c]);
        if (cell.find(needleCmp) != std::string::npos) {
            st->grid->selectCell(r, c);
            return LISTPLUGIN_OK;
        }
    }
    return LISTPLUGIN_ERROR;
}

EXPORT int DCPCALL ListSendCommand(HWND ListWin, int Command, int Parameter) {
    GtkWidget *w = (GtkWidget *)ListWin;
    if (!w) return LISTPLUGIN_ERROR;
    auto *st = (DbViewState *)g_object_get_data(G_OBJECT(w), "__dbview_state_ptr");
    if (!st) return LISTPLUGIN_ERROR;

    switch (Command) {
        case lc_copy: {
            std::string text = copySelectionAsText(st);
            return text.empty() ? LISTPLUGIN_ERROR : LISTPLUGIN_OK;
        }
        case lc_focus:
            if (Parameter != 0) gtk_widget_grab_focus(st->treeView);
            return LISTPLUGIN_OK;
        case lc_newparams:
            return LISTPLUGIN_OK;
        default:
            return LISTPLUGIN_ERROR;
    }
}

} // extern "C"
