/*
 * CSV/TSV WLX plugin for Double Commander — GTK3 UI layer.
 *
 * Full-featured build on wlxbase_gtk: GtkFocusManager (shortcuts),
 * GtkEditableGridWidget (undo/redo, copy/paste, insert/delete rows), and
 * GtkScopedFindReplacePanel (find/replace with a column-scope selector) —
 * bringing this to parity with the Qt6 build's use of the equivalent
 * wlxbase_wlqt components. CsvCore (src/core/) provides the actual CSV
 * tokenizing/serialization, same as before.
 */

#include <gtk/gtk.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <regex>
#include <algorithm>
#include <cctype>

#include "wlxplugin.h"
#include "CsvCore.h"
#include "wlxbase_gtk/GtkFocusManager.h"
#include "wlxbase_gtk/GtkPluginToolBar.h"
#include "wlxbase_gtk/GtkEditableGridWidget.h"
#include "wlxbase_gtk/GtkScopedFindReplacePanel.h"

using namespace GtkWlPlugin;

namespace {

struct CsvGtkState {
    GtkWidget *root = nullptr;
    std::unique_ptr<GtkFocusManager> fm;
    std::unique_ptr<GtkPluginToolBar> toolbar;
    std::unique_ptr<GtkEditableGridWidget> grid;
    std::unique_ptr<GtkScopedFindReplacePanel> findPanel;

    std::string currentFile;
    char separator = ',';
    bool firstLineAsHeader = true;
    std::vector<bool> columnWasQuoted;

    // Find state
    int findRow = 0, findCol = 0;
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

    // Rebuild the grid widget with the right column count.
    GtkWidget *oldGridWidget = st->grid ? st->grid->widget() : nullptr;
    st->grid = std::make_unique<GtkEditableGridWidget>(colCount, st->fm.get());
    st->grid->setDirtyChangedCallback([](bool) {});

    std::vector<std::vector<std::string>> tableRows;
    size_t startRow = st->firstLineAsHeader ? 1 : 0;
    st->columnWasQuoted.assign(colCount, false);
    for (int c = 0; c < colCount; ++c) {
        std::string title = (st->firstLineAsHeader && !rows.empty() && c < (int)rows[0].size())
                                 ? rows[0][c].text
                                 : ("Column " + std::to_string(c + 1));
        st->grid->setColumnTitle(c, title);
    }
    for (size_t r = startRow; r < rows.size(); ++r) {
        std::vector<std::string> row(colCount, "");
        for (int c = 0; c < colCount && c < (int)rows[r].size(); ++c) {
            row[c] = rows[r][c].text;
            if (rows[r][c].wasQuoted) st->columnWasQuoted[c] = true;
        }
        tableRows.push_back(std::move(row));
    }
    st->grid->setRowData(tableRows);

    if (oldGridWidget) {
        GtkWidget *parent = gtk_widget_get_parent(oldGridWidget);
        gtk_container_remove(GTK_CONTAINER(parent), oldGridWidget);
        gtk_box_pack_start(GTK_BOX(parent), st->grid->widget(), TRUE, TRUE, 0);
        gtk_widget_show_all(st->grid->widget());
        gtk_box_reorder_child(GTK_BOX(parent), st->grid->widget(), 1); // after toolbar
    }
}

void saveFile(CsvGtkState *st)
{
    if (st->currentFile.empty() || !st->grid) return;

    std::ofstream out(st->currentFile, std::ios::binary | std::ios::trunc);
    if (!out) return;

    int colCount = st->grid->columnCount();

    if (st->firstLineAsHeader) {
        std::vector<std::string> escaped;
        for (int c = 0; c < colCount; ++c) {
            GtkTreeViewColumn *col = gtk_tree_view_get_column(GTK_TREE_VIEW(st->grid->treeView()), c);
            const char *title = col ? gtk_tree_view_column_get_title(col) : "";
            escaped.push_back(CsvCore::escapeField(title ? title : "", st->separator,
                                                    c < (int)st->columnWasQuoted.size() && st->columnWasQuoted[c]));
        }
        out << CsvCore::joinRow(escaped, st->separator) << "\n";
    }

    for (const auto &row : st->grid->rowData()) {
        std::vector<std::string> escaped;
        for (int c = 0; c < colCount; ++c) {
            escaped.push_back(CsvCore::escapeField(c < (int)row.size() ? row[c] : "", st->separator,
                                                    c < (int)st->columnWasQuoted.size() && st->columnWasQuoted[c]));
        }
        out << CsvCore::joinRow(escaped, st->separator) << "\n";
    }
}

// --- Find/Replace matching, honoring the scope combo (All Cells / Current Column) ---

bool cellMatches(const std::string &text, const std::string &query, bool matchCase, bool entireCell, bool useRegex)
{
    if (query.empty()) return false;
    if (useRegex) {
        try {
            auto flags = std::regex::ECMAScript;
            if (!matchCase) flags |= std::regex::icase;
            std::regex re(query, flags);
            return entireCell ? std::regex_match(text, re) : std::regex_search(text, re);
        } catch (...) {
            return false;
        }
    }
    std::string t = text, q = query;
    if (!matchCase) {
        std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return std::tolower(c); });
        std::transform(q.begin(), q.end(), q.begin(), [](unsigned char c) { return std::tolower(c); });
    }
    return entireCell ? (t == q) : (t.find(q) != std::string::npos);
}

void doFind(CsvGtkState *st, bool forward)
{
    if (!st->grid) return;
    std::string query = st->findPanel->findText();
    if (query.empty()) return;

    auto data = st->grid->rowData();
    int rows = (int)data.size();
    int cols = st->grid->columnCount();
    if (rows == 0 || cols == 0) return;

    bool matchCase = st->findPanel->matchCase();
    bool entireCell = st->findPanel->matchEntireCell();
    bool useRegex = st->findPanel->useRegex();
    std::string scope = st->findPanel->currentScope();

    int total = rows * cols;
    int cur = st->findRow * cols + st->findCol;

    for (int step = 1; step <= total; ++step) {
        int idx = forward ? (cur + step) % total : ((cur - step) % total + total) % total;
        int r = idx / cols, c = idx % cols;
        if (scope == "Current Column" && c != st->findCol) continue;
        if (cellMatches(data[r][c], query, matchCase, entireCell, useRegex)) {
            st->findRow = r; st->findCol = c;
            st->grid->selectCell(r, c);
            st->findPanel->setStatusText("Match found");
            return;
        }
    }
    st->findPanel->setStatusText("No matches");
}

void doReplace(CsvGtkState *st)
{
    if (!st->grid) return;
    std::string current = st->grid->cellValue(st->findRow, st->findCol);
    std::string query = st->findPanel->findText();
    if (cellMatches(current, query, st->findPanel->matchCase(), st->findPanel->matchEntireCell(), st->findPanel->useRegex())) {
        st->grid->setCellValue(st->findRow, st->findCol, st->findPanel->replaceText());
    }
    doFind(st, true);
}

void doReplaceAll(CsvGtkState *st)
{
    if (!st->grid) return;
    std::string query = st->findPanel->findText();
    std::string replacement = st->findPanel->replaceText();
    bool matchCase = st->findPanel->matchCase();
    bool entireCell = st->findPanel->matchEntireCell();
    bool useRegex = st->findPanel->useRegex();
    std::string scope = st->findPanel->currentScope();

    auto data = st->grid->rowData();
    int count = 0;
    for (int r = 0; r < (int)data.size(); ++r) {
        for (int c = 0; c < st->grid->columnCount(); ++c) {
            if (scope == "Current Column" && c != st->findCol) continue;
            if (cellMatches(data[r][c], query, matchCase, entireCell, useRegex)) {
                st->grid->setCellValue(r, c, replacement);
                ++count;
            }
        }
    }
    st->findPanel->setStatusText(std::to_string(count) + " replaced");
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
    // gtk_container_add() doesn't register the child the way GtkLayout
    // expects: DC's ResizeWindow later calls gtk_layout_move() on this
    // widget, which asserts the parent is exactly this GtkLayout -- only
    // gtk_layout_put() sets that up.
    gtk_layout_put(GTK_LAYOUT(parent), st->root, 0, 0);

    // Placeholder tree view just so FocusManager has a primaryView to
    // construct with — replaced immediately once loadFile() builds the
    // real grid with the correct column count.
    GtkWidget *placeholder = gtk_tree_view_new();
    st->fm = std::make_unique<GtkFocusManager>(st->root, placeholder);

    st->toolbar = std::make_unique<GtkPluginToolBar>(st->fm.get());
    GtkWidget *sepCombo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(sepCombo), ",", "Comma (,)");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(sepCombo), "t", "Tab");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(sepCombo), ";", "Semicolon (;)");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(sepCombo), "|", "Pipe (|)");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(sepCombo), ",");
    gtk_widget_set_can_focus(sepCombo, FALSE);
    g_signal_connect(sepCombo, "changed", G_CALLBACK(+[](GtkComboBox *combo, gpointer data) {
        auto *st = static_cast<CsvGtkState *>(data);
        const char *active = gtk_combo_box_get_active_id(combo);
        if (!active) return;
        st->separator = active[0] == 't' ? '\t' : active[0] == ';' ? ';' : active[0] == '|' ? '|' : ',';
        if (!st->currentFile.empty()) loadFile(st, st->currentFile);
    }), st);
    gtk_box_pack_start(GTK_BOX(st->toolbar->widget()), sepCombo, FALSE, FALSE, 0);
    gtk_widget_show(sepCombo);

    st->toolbar->addToggleAction("First line is header", "", true, [st](bool active) {
        st->firstLineAsHeader = active;
        if (!st->currentFile.empty()) loadFile(st, st->currentFile);
    });
    st->toolbar->addToolAction("Save (Ctrl+S)", "document-save-symbolic", [st]() { saveFile(st); });
    st->toolbar->addToolAction("Find/Replace (Ctrl+F)", "edit-find-symbolic", [st]() {
        st->findPanel->showPanel(!st->findPanel->isPanelVisible());
    });
    gtk_box_pack_start(GTK_BOX(st->root), st->toolbar->widget(), FALSE, FALSE, 0);

    // Real grid gets built by loadFile() below (needs the file's actual
    // column count); pack a temporary empty box as its future slot.
    GtkWidget *gridSlot = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(gridSlot, TRUE);
    gtk_box_pack_start(GTK_BOX(st->root), gridSlot, TRUE, TRUE, 0);
    st->grid = std::make_unique<GtkEditableGridWidget>(1, st->fm.get());
    gtk_box_pack_start(GTK_BOX(gridSlot), st->grid->widget(), TRUE, TRUE, 0);

    st->findPanel = std::make_unique<GtkScopedFindReplacePanel>(st->fm.get());
    st->findPanel->setScopes({"All Cells", "Current Column"});
    st->findPanel->onFindRequested = [st](bool forward) { doFind(st, forward); };
    st->findPanel->onReplaceRequested = [st]() { doReplace(st); };
    st->findPanel->onReplaceAllRequested = [st]() { doReplaceAll(st); };
    gtk_box_pack_start(GTK_BOX(st->root), st->findPanel->widget(), FALSE, FALSE, 0);

    st->fm->enableUndoShortcuts();
    st->fm->registerShortcut(GDK_KEY_f, GDK_CONTROL_MASK, GtkFocusManager::Always, [st]() {
        st->findPanel->showPanel(!st->findPanel->isPanelVisible());
        return true;
    });
    st->fm->registerShortcut(GDK_KEY_s, GDK_CONTROL_MASK, GtkFocusManager::Always, [st]() {
        saveFile(st);
        return true;
    });

    g_object_set_data_full(G_OBJECT(st->root), "csv-state", st, destroyState);

    gtk_widget_show_all(st->root);
    st->findPanel->showPanel(false); // hidden until Ctrl+F / toolbar toggle
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
    if (Command == lc_copy) {
        st->grid->copySelection(st->separator);
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
