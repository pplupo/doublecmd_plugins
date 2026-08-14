/*
 * CSV/TSV WLX plugin for Double Commander — GTK3 UI layer.
 *
 * Self-contained GtkTreeView-based grid (view + inline edit + save),
 * built on CsvCore (src/core/) — the same tokenizer/serializer the Qt6
 * build now uses. Deliberately does NOT port EditableGridWidget's
 * undo/redo stack, FocusManager's shortcut plumbing, or
 * ScopedFindReplacePanel — there is no wlxbase_gtk yet to build those on,
 * and reimplementing them per-plugin isn't worth it before a second
 * plugin actually needs the same primitives. See the plugin's commit
 * message for the full scope note.
 *
 * Encoding: reads/writes UTF-8 only in this GTK build (the Qt6 build's
 * enca-based encoding detection/conversion stays Qt-side for now, same
 * reasoning as above).
 */

#include <gtk/gtk.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "wlxplugin.h"
#include "CsvCore.h"

namespace {

struct CsvGtkState {
    GtkWidget *root = nullptr;
    GtkWidget *treeView = nullptr;
    GtkListStore *store = nullptr;
    GtkWidget *sepCombo = nullptr;
    GtkWidget *headerCheck = nullptr;

    std::string currentFile;
    char separator = ',';
    bool firstLineAsHeader = true;
    std::vector<bool> columnWasQuoted; // per-column "was any cell quoted" — used on save
    bool dirty = false;
};

std::string readFile(const std::string &path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::vector<std::string> splitLines(const std::string &data)
{
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= data.size()) {
        size_t pos = data.find('\n', start);
        if (pos == std::string::npos) {
            if (start < data.size()) lines.push_back(data.substr(start));
            break;
        }
        lines.push_back(data.substr(start, pos - start + 1));
        start = pos + 1;
    }
    return lines;
}

void rebuildStore(CsvGtkState *st, const std::vector<std::vector<CsvCore::Field>> &rows, int colCount)
{
    if (st->store) {
        gtk_tree_view_set_model(GTK_TREE_VIEW(st->treeView), nullptr);
        g_object_unref(st->store);
    }

    // Remove old columns.
    GList *columns = gtk_tree_view_get_columns(GTK_TREE_VIEW(st->treeView));
    for (GList *l = columns; l; l = l->next)
        gtk_tree_view_remove_column(GTK_TREE_VIEW(st->treeView), GTK_TREE_VIEW_COLUMN(l->data));
    g_list_free(columns);

    std::vector<GType> types(colCount, G_TYPE_STRING);
    st->store = gtk_list_store_newv(colCount, types.data());

    for (int c = 0; c < colCount; ++c) {
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
        g_object_set(renderer, "editable", TRUE, nullptr);
        std::string title = (st->firstLineAsHeader && !rows.empty() && c < (int)rows[0].size())
                                 ? rows[0][c].text
                                 : ("Column " + std::to_string(c + 1));
        GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
            title.c_str(), renderer, "text", c, nullptr);
        gtk_tree_view_column_set_resizable(col, TRUE);
        int colIndex = c;
        // A naked `std::pair<CsvGtkState *, int>` as a macro argument trips
        // up the preprocessor (g_signal_connect is a macro; it splits on
        // the comma inside the template argument list as if it were a
        // 5th macro argument) — route through a type alias instead.
        using ColEditCtx = std::pair<CsvGtkState *, int>;
        g_signal_connect(renderer, "edited", G_CALLBACK(+[](GtkCellRendererText *, gchar *pathStr, gchar *newText, gpointer data) {
            auto *ctx = static_cast<ColEditCtx *>(data);
            GtkTreePath *path = gtk_tree_path_new_from_string(pathStr);
            GtkTreeIter iter;
            if (gtk_tree_model_get_iter(GTK_TREE_MODEL(ctx->first->store), &iter, path)) {
                gtk_list_store_set(ctx->first->store, &iter, ctx->second, newText, -1);
                ctx->first->dirty = true;
            }
            gtk_tree_path_free(path);
        }), new ColEditCtx(st, colIndex));
        gtk_tree_view_append_column(GTK_TREE_VIEW(st->treeView), col);
    }

    size_t startRow = st->firstLineAsHeader ? 1 : 0;
    st->columnWasQuoted.assign(colCount, false);
    for (size_t r = startRow; r < rows.size(); ++r) {
        GtkTreeIter iter;
        gtk_list_store_append(st->store, &iter);
        for (int c = 0; c < colCount; ++c) {
            if (c < (int)rows[r].size()) {
                gtk_list_store_set(st->store, &iter, c, rows[r][c].text.c_str(), -1);
                if (rows[r][c].wasQuoted) st->columnWasQuoted[c] = true;
            } else {
                gtk_list_store_set(st->store, &iter, c, "", -1);
            }
        }
    }

    gtk_tree_view_set_model(GTK_TREE_VIEW(st->treeView), GTK_TREE_MODEL(st->store));
}

void loadFile(CsvGtkState *st, const std::string &path)
{
    st->currentFile = path;
    std::string data = readFile(path);
    std::vector<std::string> lines = splitLines(data);

    std::vector<std::vector<CsvCore::Field>> rows;
    int colCount = 1;
    for (auto &line : lines) {
        auto fields = CsvCore::parseLine(line, st->separator);
        colCount = std::max(colCount, (int)fields.size());
        rows.push_back(std::move(fields));
    }

    rebuildStore(st, rows, colCount);
    st->dirty = false;
}

void saveFile(CsvGtkState *st)
{
    if (st->currentFile.empty() || !st->store) return;

    std::ofstream out(st->currentFile, std::ios::binary | std::ios::trunc);
    if (!out) return;

    int colCount = gtk_tree_view_get_n_columns(GTK_TREE_VIEW(st->treeView));

    if (st->firstLineAsHeader) {
        std::vector<std::string> escaped;
        for (int c = 0; c < colCount; ++c) {
            GtkTreeViewColumn *col = gtk_tree_view_get_column(GTK_TREE_VIEW(st->treeView), c);
            const char *title = gtk_tree_view_column_get_title(col);
            escaped.push_back(CsvCore::escapeField(title ? title : "", st->separator,
                                                    c < (int)st->columnWasQuoted.size() && st->columnWasQuoted[c]));
        }
        out << CsvCore::joinRow(escaped, st->separator) << "\n";
    }

    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(st->store), &iter);
    while (valid) {
        std::vector<std::string> escaped;
        for (int c = 0; c < colCount; ++c) {
            gchar *text = nullptr;
            gtk_tree_model_get(GTK_TREE_MODEL(st->store), &iter, c, &text, -1);
            std::string cellText = text ? text : "";
            g_free(text);
            escaped.push_back(CsvCore::escapeField(cellText, st->separator,
                                                    c < (int)st->columnWasQuoted.size() && st->columnWasQuoted[c]));
        }
        out << CsvCore::joinRow(escaped, st->separator) << "\n";
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(st->store), &iter);
    }

    st->dirty = false;
}

void onSeparatorChanged(GtkComboBox *combo, gpointer userData)
{
    auto *st = static_cast<CsvGtkState *>(userData);
    const char *active = gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo));
    if (!active) return;
    st->separator = active[0] == 't' ? '\t' : active[0] == ';' ? ';' : active[0] == '|' ? '|' : ',';
    if (!st->currentFile.empty())
        loadFile(st, st->currentFile);
}

void onHeaderToggled(GtkToggleButton *btn, gpointer userData)
{
    auto *st = static_cast<CsvGtkState *>(userData);
    st->firstLineAsHeader = gtk_toggle_button_get_active(btn);
    if (!st->currentFile.empty())
        loadFile(st, st->currentFile);
}

void onSaveClicked(GtkButton *, gpointer userData)
{
    saveFile(static_cast<CsvGtkState *>(userData));
}

void destroyState(gpointer data)
{
    delete static_cast<CsvGtkState *>(data);
}

} // namespace

extern "C" {

HWND DCPCALL ListLoad(HWND ParentWin, char *FileToLoad, int ShowFlags)
{
    GtkWidget *parent = GTK_WIDGET(ParentWin);
    auto *st = new CsvGtkState();

    st->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(parent), st->root);

    // Toolbar: separator chooser, header toggle, save button.
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(toolbar), 4);

    gtk_box_pack_start(GTK_BOX(toolbar), gtk_label_new("Separator:"), FALSE, FALSE, 0);
    st->sepCombo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(st->sepCombo), ",", "Comma (,)");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(st->sepCombo), "t", "Tab");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(st->sepCombo), ";", "Semicolon (;)");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(st->sepCombo), "|", "Pipe (|)");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(st->sepCombo), ",");
    g_signal_connect(st->sepCombo, "changed", G_CALLBACK(onSeparatorChanged), st);
    gtk_box_pack_start(GTK_BOX(toolbar), st->sepCombo, FALSE, FALSE, 0);

    st->headerCheck = gtk_check_button_new_with_label("First line is header");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(st->headerCheck), TRUE);
    g_signal_connect(st->headerCheck, "toggled", G_CALLBACK(onHeaderToggled), st);
    gtk_box_pack_start(GTK_BOX(toolbar), st->headerCheck, FALSE, FALSE, 0);

    GtkWidget *saveBtn = gtk_button_new_with_label("Save");
    g_signal_connect(saveBtn, "clicked", G_CALLBACK(onSaveClicked), st);
    gtk_box_pack_end(GTK_BOX(toolbar), saveBtn, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(st->root), toolbar, FALSE, FALSE, 0);

    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_widget_set_vexpand(scroll, TRUE);
    st->treeView = gtk_tree_view_new();
    gtk_container_add(GTK_CONTAINER(scroll), st->treeView);
    gtk_box_pack_start(GTK_BOX(st->root), scroll, TRUE, TRUE, 0);

    g_object_set_data_full(G_OBJECT(st->root), "csv-state", st, destroyState);

    gtk_widget_show_all(st->root);
    loadFile(st, std::string(FileToLoad));

    return reinterpret_cast<HWND>(st->root);
}

void DCPCALL ListCloseWindow(HWND ListWin)
{
    GtkWidget *root = GTK_WIDGET(ListWin);
    if (root) gtk_widget_destroy(root);
}

int DCPCALL ListSendCommand(HWND ListWin, int Command, int Parameter)
{
    GtkWidget *root = GTK_WIDGET(ListWin);
    auto *st = static_cast<CsvGtkState *>(g_object_get_data(G_OBJECT(root), "csv-state"));
    if (!st) return LISTPLUGIN_ERROR;

    if (Command == lc_newparams) {
        loadFile(st, st->currentFile);
        return LISTPLUGIN_OK;
    }
    return LISTPLUGIN_ERROR;
}

int DCPCALL ListSearchText(HWND, char *, int)
{
    return LISTPLUGIN_ERROR;
}

void DCPCALL ListGetDetectString(char *DetectString, int maxlen)
{
    snprintf(DetectString, maxlen - 1, "(EXT=\"CSV\" | EXT=\"TSV\") & SIZE<50000000");
}

void DCPCALL ListSetDefaultParams(ListDefaultParamStruct *)
{
}

} // extern "C"
