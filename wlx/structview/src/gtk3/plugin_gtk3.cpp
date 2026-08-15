// GTK3 UI for structview: a GtkTreeStore document tree on the left, and a
// Grid/Text GtkNotebook on the right, built on wlxbase_gtk's
// GtkFocusManager/GtkEditableGridWidget, mirroring StructViewWidget's Qt
// layout (tree | grid+text tabs) over the same Qt-free structview_core
// (DocumentNode/TextFormatEngine + the 6 format engines).

#include "core/DocumentModel.h"
#include "wlxbase_gtk/GtkFocusManager.h"
#include "wlxbase_gtk/GtkEditableGridWidget.h"

#include <gtk/gtk.h>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <memory>
#include <string>
#include <vector>

#include "wlxplugin.h"

#define EXPORT __attribute__((visibility("default")))

namespace {

struct StructViewState {
    GtkWidget *root = nullptr;
    GtkWidget *paned = nullptr;
    GtkWidget *treeView = nullptr;
    GtkTreeStore *treeStore = nullptr;
    GtkWidget *notebook = nullptr;
    GtkWidget *gridContainer = nullptr; // holds the current GtkEditableGridWidget's widget()
    GtkWidget *textView = nullptr;
    GtkWidget *statusLabel = nullptr;

    std::unique_ptr<TextFormatEngine> engine;
    std::unique_ptr<GtkWlPlugin::GtkFocusManager> focusManager;
    std::unique_ptr<GtkWlPlugin::GtkEditableGridWidget> grid;

    std::string filepath;
    DocumentNode *currentNode = nullptr;
    bool dirty = false;

    // GtkTreeStore column 0 = display name, column 1 = DocumentNode* (as gpointer)
};

enum { COL_NAME = 0, COL_NODE = 1, N_COLS };

void updateStatus(StructViewState *st) {
    if (!st->statusLabel) return;
    std::string text = st->engine ? st->engine->formatName() : "";
    if (st->dirty) text += " *modified*";
    gtk_label_set_text(GTK_LABEL(st->statusLabel), text.c_str());
}

void showNode(StructViewState *st, DocumentNode *node);

void rebuildGrid(StructViewState *st, DocumentNode *node) {
    // Tear down the old grid widget (if any) and build a fresh one sized
    // for this node's column count -- GtkEditableGridWidget's column count
    // is fixed at construction, and different tree nodes have different
    // shapes, so we recreate it per-selection (mirrors the Qt side
    // resetting the QStandardItemModel's column count per node).
    if (st->grid) {
        gtk_container_remove(GTK_CONTAINER(st->gridContainer), st->grid->widget());
        st->grid.reset();
    }

    int cols = std::max(1, (int)node->columnNames.size());
    st->grid = std::make_unique<GtkWlPlugin::GtkEditableGridWidget>(cols, st->focusManager.get());
    for (size_t c = 0; c < node->columnNames.size(); c++)
        st->grid->setColumnTitle((int)c, node->columnNames[c]);
    st->grid->setRowData(node->rows);
    st->grid->setDirtyChangedCallback([st](bool d) {
        if (d) st->dirty = true;
        updateStatus(st);
    });

    gtk_box_pack_start(GTK_BOX(st->gridContainer), st->grid->widget(), TRUE, TRUE, 0);
    gtk_widget_show_all(st->grid->widget());
}

void showNode(StructViewState *st, DocumentNode *node) {
    st->currentNode = node;
    rebuildGrid(st, node);
    // Text tab always mirrors the whole document's raw text (matches the
    // Qt side's read-only Text tab), not just this node.
}

void populateTreeNode(GtkTreeStore *store, GtkTreeIter *parentIter, DocumentNode *node) {
    GtkTreeIter iter;
    gtk_tree_store_append(store, &iter, parentIter);
    gtk_tree_store_set(store, &iter, COL_NAME, node->name.c_str(), COL_NODE, (gpointer)node, -1);
    for (auto *child : node->children)
        populateTreeNode(store, &iter, child);
}

void populateTree(StructViewState *st) {
    gtk_tree_store_clear(st->treeStore);
    if (!st->engine || !st->engine->rootNode()) return;
    populateTreeNode(st->treeStore, nullptr, st->engine->rootNode());

    GtkTreeIter first;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(st->treeStore), &first)) {
        GtkTreePath *path = gtk_tree_model_get_path(GTK_TREE_MODEL(st->treeStore), &first);
        gtk_tree_view_expand_row(GTK_TREE_VIEW(st->treeView), path, FALSE);
        gtk_tree_view_set_cursor(GTK_TREE_VIEW(st->treeView), path, nullptr, FALSE);
        gtk_tree_path_free(path);
        showNode(st, st->engine->rootNode());
    }
}

void onTreeSelectionChanged(GtkTreeSelection *sel, gpointer data) {
    auto *st = (StructViewState *)data;
    GtkTreeIter iter;
    GtkTreeModel *model;
    if (!gtk_tree_selection_get_selected(sel, &model, &iter)) return;
    gpointer nodePtr = nullptr;
    gtk_tree_model_get(model, &iter, COL_NODE, &nodePtr, -1);
    if (nodePtr) showNode(st, (DocumentNode *)nodePtr);
}

bool doSave(StructViewState *st) {
    if (!st->engine || st->filepath.empty()) return false;
    // Flush the currently-visible grid's edits back into the tree before
    // serializing (matches the Qt side's syncGridToNode()).
    if (st->grid && st->currentNode) {
        st->currentNode->rows = st->grid->rowData();
    }
    std::string data = st->engine->serialize();
    if (data.empty()) return false;
    std::ofstream out(st->filepath, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << data;
    st->dirty = false;
    updateStatus(st);
    return true;
}

void onSaveClicked(GtkButton *, gpointer data) { doSave((StructViewState *)data); }

std::string copySelectionAsText(StructViewState *st) {
    if (!st->grid) return {};
    // GtkEditableGridWidget::copySelection() writes to the GTK clipboard
    // directly; read it back so ListSendCommand(lc_copy) can hand DC the
    // same text without duplicating the tab-join logic.
    st->grid->copySelection('\t');
    GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gchar *text = gtk_clipboard_wait_for_text(cb);
    std::string result = text ? text : "";
    if (text) g_free(text);
    return result;
}

void destroyState(GtkWidget *, gpointer data) { delete (StructViewState *)data; }

} // namespace

extern "C" {

EXPORT HWND DCPCALL ListLoad(HWND ParentWin, char *FileToLoad, int ShowFlags) {
    std::string filepath = FileToLoad;

    auto engine = TextFormatEngine::createForFile(filepath);
    if (!engine) return nullptr;

    std::ifstream f(filepath, std::ios::binary);
    if (!f) return nullptr;
    std::ostringstream ss; ss << f.rdbuf();
    if (!engine->parse(ss.str())) return nullptr;

    auto *st = new StructViewState();
    st->filepath = filepath;
    st->engine = std::move(engine);

    st->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    // gtk_layout_put() (not gtk_container_add()) is required here: DC's
    // ResizeWindow later calls gtk_layout_move() on this widget, which
    // asserts the widget's parent is exactly this GtkLayout.
    gtk_layout_put(GTK_LAYOUT(ParentWin), st->root, 0, 0);

    // Toolbar
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *saveBtn = gtk_button_new_with_label("Save");
    g_signal_connect(saveBtn, "clicked", G_CALLBACK(onSaveClicked), st);
    gtk_box_pack_start(GTK_BOX(toolbar), saveBtn, FALSE, FALSE, 2);
    st->statusLabel = gtk_label_new("");
    gtk_box_pack_end(GTK_BOX(toolbar), st->statusLabel, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(st->root), toolbar, FALSE, FALSE, 2);

    // Tree | Grid+Text
    st->paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(st->root), st->paned, TRUE, TRUE, 0);

    st->treeStore = gtk_tree_store_new(N_COLS, G_TYPE_STRING, G_TYPE_POINTER);
    st->treeView = gtk_tree_view_new_with_model(GTK_TREE_MODEL(st->treeStore));
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes("Document", renderer, "text", COL_NAME, nullptr);
    gtk_tree_view_append_column(GTK_TREE_VIEW(st->treeView), col);
    GtkWidget *treeScroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_container_add(GTK_CONTAINER(treeScroll), st->treeView);
    gtk_widget_set_size_request(treeScroll, 220, -1);
    gtk_paned_pack1(GTK_PANED(st->paned), treeScroll, FALSE, TRUE);

    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(st->treeView)), "changed",
                      G_CALLBACK(onTreeSelectionChanged), st);

    st->notebook = gtk_notebook_new();
    gtk_paned_pack2(GTK_PANED(st->paned), st->notebook, TRUE, TRUE);

    st->gridContainer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(st->notebook), st->gridContainer, gtk_label_new("Grid"));

    st->textView = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(st->textView), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(st->textView), TRUE);
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(st->textView));
    gtk_text_buffer_set_text(buf, st->engine->rawText().c_str(), -1);
    GtkWidget *textScroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_container_add(GTK_CONTAINER(textScroll), st->textView);
    gtk_notebook_append_page(GTK_NOTEBOOK(st->notebook), textScroll, gtk_label_new("Text"));

    st->focusManager = std::make_unique<GtkWlPlugin::GtkFocusManager>(st->root, st->treeView);
    st->focusManager->registerShortcut(GDK_KEY_s, GDK_CONTROL_MASK, GtkWlPlugin::GtkFocusManager::Always,
        [st]() { doSave(st); return true; });

    g_signal_connect(st->root, "destroy", G_CALLBACK(destroyState), st);
    g_object_set_data(G_OBJECT(st->root), "__structview_state_ptr", st);

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
        "EXT=\"JSON\" | EXT=\"XML\" | EXT=\"INI\" | EXT=\"CBOR\" | "
        "EXT=\"YAML\" | EXT=\"YML\" | EXT=\"TOML\" | "
        "EXT=\"DESKTOP\" | EXT=\"INF\"");
}

EXPORT int DCPCALL ListSearchText(HWND ListWin, char *SearchString, int SearchParameter) {
    GtkWidget *w = (GtkWidget *)ListWin;
    if (!w) return LISTPLUGIN_ERROR;
    auto *st = (StructViewState *)g_object_get_data(G_OBJECT(w), "__structview_state_ptr");
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
    auto *st = (StructViewState *)g_object_get_data(G_OBJECT(w), "__structview_state_ptr");
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
