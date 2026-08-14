// GTK3 UI for dbview: a GtkTreeView schema list (tables/views) on the
// left, a GtkEditableGridWidget-backed grid on the right, a Submit/Revert
// toolbar, and (for SQL engines) a query console -- all on top of the
// Qt-free DbEngineCore/KeyValueEngineCoreBase engines.

#include "core/DbEngineCore.h"
#include "wlxbase_gtk/GtkFocusManager.h"
#include "wlxbase_gtk/GtkEditableGridWidget.h"

#include <gtk/gtk.h>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "wlxplugin.h"

#define EXPORT __attribute__((visibility("default")))

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
    std::unique_ptr<GtkWlPlugin::GtkFocusManager> focusManager;
    std::unique_ptr<GtkWlPlugin::GtkEditableGridWidget> grid;

    std::string filepath;
};

enum { COL_NAME = 0, N_COLS };

void updateStatus(DbViewState *st) {
    if (!st->statusLabel || !st->engine) return;
    std::string text = st->engine->engineName() + " - " + st->engine->currentTableName();
    if (st->engine->isReadOnly()) text += " (read-only)";
    gtk_label_set_text(GTK_LABEL(st->statusLabel), text.c_str());
}

void loadGridFromEngine(DbViewState *st) {
    if (st->grid) {
        gtk_container_remove(GTK_CONTAINER(st->gridContainer), st->grid->widget());
        st->grid.reset();
    }
    int cols = std::max(1, st->engine->columnCount());
    st->grid = std::make_unique<GtkWlPlugin::GtkEditableGridWidget>(cols, st->focusManager.get());
    for (int c = 0; c < st->engine->columnCount(); c++)
        st->grid->setColumnTitle(c, st->engine->columnName(c));

    std::vector<std::vector<std::string>> rows;
    int nrows = st->engine->rowCount();
    rows.reserve(nrows);
    for (int r = 0; r < nrows; r++) {
        std::vector<std::string> row;
        row.reserve(st->engine->columnCount());
        for (int c = 0; c < st->engine->columnCount(); c++)
            row.push_back(st->engine->cellText(r, c));
        rows.push_back(std::move(row));
    }
    st->grid->setRowData(rows);

    gtk_box_pack_start(GTK_BOX(st->gridContainer), st->grid->widget(), TRUE, TRUE, 0);
    gtk_widget_show_all(st->grid->widget());
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

void onSubmitClicked(GtkButton *, gpointer data) {
    auto *st = (DbViewState *)data;
    if (st->engine->submitAll()) updateStatus(st);
}
void onRevertClicked(GtkButton *, gpointer data) {
    auto *st = (DbViewState *)data;
    if (st->engine->revertAll()) { loadGridFromEngine(st); updateStatus(st); }
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

    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    if (st->engine->supportsSubmitRevert()) {
        GtkWidget *submitBtn = gtk_button_new_with_label("Submit");
        g_signal_connect(submitBtn, "clicked", G_CALLBACK(onSubmitClicked), st);
        gtk_box_pack_start(GTK_BOX(toolbar), submitBtn, FALSE, FALSE, 2);
        GtkWidget *revertBtn = gtk_button_new_with_label("Revert");
        g_signal_connect(revertBtn, "clicked", G_CALLBACK(onRevertClicked), st);
        gtk_box_pack_start(GTK_BOX(toolbar), revertBtn, FALSE, FALSE, 2);
    }
    st->statusLabel = gtk_label_new("");
    gtk_box_pack_end(GTK_BOX(toolbar), st->statusLabel, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(st->root), toolbar, FALSE, FALSE, 2);

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

    st->focusManager = std::make_unique<GtkWlPlugin::GtkFocusManager>(st->root, st->treeView);
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
