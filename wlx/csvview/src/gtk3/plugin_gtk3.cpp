/*
 * CSV/TSV WLX plugin for Double Commander — GTK3 UI layer.
 *
 * Full-featured build on wlxbase_gtk: GtkFocusManager (shortcuts, undo/redo
 * stack), GtkEditableGridWidget (copy/paste, insert/delete rows), and
 * GtkScopedFindReplacePanel (find/replace with a column-scope selector) —
 * brought to toolbar/feature parity with the Qt6 build's PluginToolBar
 * (Save, Save As, Undo, Redo, Print, Reload, Header Row, Find/Replace, Show
 * Text, Line Wrap, Open Externally) plus a row-number gutter column, which
 * this GTK3 build was previously missing entirely. CsvCore (src/core/)
 * provides the actual CSV tokenizing/serialization, same as before.
 *
 * Separator is now auto-detected from the file content (matching the Qt6
 * build: try ',' ';' '\t' in order on the first line, keep the first that
 * yields more than one column, else fall back on the file extension) --
 * previously this GTK build had a manual delimiter dropdown that doesn't
 * exist in the Qt6 toolbar at all.
 */

#include <gtk/gtk.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <regex>
#include <algorithm>
#include <cctype>
#include <cstdio>

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
    GtkWidget *gridSlot = nullptr; // holds st->grid->widget(), swapped on reload
    std::unique_ptr<GtkFocusManager> fm;
    std::unique_ptr<GtkPluginToolBar> toolbar;
    std::unique_ptr<GtkEditableGridWidget> grid;
    std::unique_ptr<GtkScopedFindReplacePanel> findPanel;

    GtkWidget *dirtyLabel = nullptr;
    GtkWidget *undoBtn = nullptr;
    GtkWidget *redoBtn = nullptr;
    GtkWidget *headerToggle = nullptr;
    GtkWidget *findToggle = nullptr;
    GtkWidget *textToggle = nullptr;
    GtkWidget *wrapToggle = nullptr;

    // "Show Text" mode: a plain GtkTextView shown in place of the grid.
    GtkWidget *textScroll = nullptr;
    GtkWidget *textView = nullptr;
    bool showingText = false;
    bool wordWrap = false;

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

bool endsWithNoCase(const std::string &s, const std::string &suffix)
{
    if (s.size() < suffix.size()) return false;
    return std::equal(suffix.rbegin(), suffix.rend(), s.rbegin(), [](char a, char b) {
        return std::tolower((unsigned char)a) == std::tolower((unsigned char)b);
    });
}

// Mirrors the Qt6 build's detection: try ',' ';' '\t' in turn on the first
// line, keep the first that yields more than one field; fall back to the
// file extension (.tsv -> tab, else comma) if none of them do.
char detectSeparator(const std::string &firstLine, const std::string &path)
{
    static const char candidates[] = {',', ';', '\t'};
    for (char c : candidates) {
        auto fields = CsvCore::parseLine(firstLine, c);
        if (fields.size() > 1) return c;
    }
    return endsWithNoCase(path, ".tsv") ? '\t' : ',';
}

// Renders the 1-based row index into the leading gutter column -- purely
// cosmetic, not backed by any model column, so it doesn't touch
// GtkEditableGridWidget at all.
void rowNumberCellDataFunc(GtkTreeViewColumn *, GtkCellRenderer *cell,
                            GtkTreeModel *model, GtkTreeIter *iter, gpointer)
{
    GtkTreePath *path = gtk_tree_model_get_path(model, iter);
    gint *indices = gtk_tree_path_get_indices(path);
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", indices[0] + 1);
    g_object_set(cell, "text", buf, nullptr);
    gtk_tree_path_free(path);
}

void addRowNumberColumn(GtkWidget *treeView)
{
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    g_object_set(renderer, "xalign", 1.0, "foreground", "#888888", nullptr);
    GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes("#", renderer, nullptr);
    gtk_tree_view_column_set_cell_data_func(col, renderer, rowNumberCellDataFunc, nullptr, nullptr);
    gtk_tree_view_column_set_resizable(col, FALSE);
    gtk_tree_view_column_set_min_width(col, 32);
    gtk_tree_view_insert_column(GTK_TREE_VIEW(treeView), col, 0);
}

void refreshUndoRedoSensitivity(CsvGtkState *st)
{
    if (st->undoBtn) gtk_widget_set_sensitive(st->undoBtn, st->fm->canUndo());
    if (st->redoBtn) gtk_widget_set_sensitive(st->redoBtn, st->fm->canRedo());
}

void updateDirtyLabel(CsvGtkState *st, bool dirty)
{
    if (st->dirtyLabel) gtk_label_set_text(GTK_LABEL(st->dirtyLabel), dirty ? "●" : "✓");
    refreshUndoRedoSensitivity(st);
}

std::string joinRowPlain(const std::vector<std::string> &row, char sep)
{
    std::string out;
    for (size_t c = 0; c < row.size(); ++c) {
        if (c) out += sep;
        out += row[c];
    }
    return out;
}

void updateTextView(CsvGtkState *st)
{
    if (!st->textView || !st->grid) return;
    std::string plain;
    if (st->firstLineAsHeader) {
        std::vector<std::string> titles;
        for (int c = 0; c < st->grid->columnCount(); ++c) {
            GtkTreeViewColumn *col = gtk_tree_view_get_column(GTK_TREE_VIEW(st->grid->treeView()), c + 1); // +1: row-number gutter
            const char *title = col ? gtk_tree_view_column_get_title(col) : "";
            titles.push_back(title ? title : "");
        }
        plain += joinRowPlain(titles, st->separator) + "\n";
    }
    for (const auto &row : st->grid->rowData())
        plain += joinRowPlain(row, st->separator) + "\n";

    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(st->textView));
    gtk_text_buffer_set_text(buf, plain.c_str(), -1);
}

void loadFile(CsvGtkState *st, const std::string &path)
{
    st->currentFile = path;
    std::string data = readFile(path);
    std::vector<std::string> lines = splitLines(data);

    st->separator = detectSeparator(lines.empty() ? std::string() : lines[0], path);

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
    st->grid->setDirtyChangedCallback([st](bool dirty) { updateDirtyLabel(st, dirty); });
    addRowNumberColumn(st->grid->treeView());

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
        if (st->showingText) gtk_widget_hide(st->grid->widget());
    }

    st->fm->clearUndoStack();
    updateDirtyLabel(st, false);
    if (st->showingText) updateTextView(st);
}

void saveFile(CsvGtkState *st, const std::string &path, char separator)
{
    if (path.empty() || !st->grid) return;

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return;

    int colCount = st->grid->columnCount();

    if (st->firstLineAsHeader) {
        std::vector<std::string> escaped;
        for (int c = 0; c < colCount; ++c) {
            GtkTreeViewColumn *col = gtk_tree_view_get_column(GTK_TREE_VIEW(st->grid->treeView()), c + 1);
            const char *title = col ? gtk_tree_view_column_get_title(col) : "";
            escaped.push_back(CsvCore::escapeField(title ? title : "", separator,
                                                    c < (int)st->columnWasQuoted.size() && st->columnWasQuoted[c]));
        }
        out << CsvCore::joinRow(escaped, separator) << "\n";
    }

    for (const auto &row : st->grid->rowData()) {
        std::vector<std::string> escaped;
        for (int c = 0; c < colCount; ++c) {
            escaped.push_back(CsvCore::escapeField(c < (int)row.size() ? row[c] : "", separator,
                                                    c < (int)st->columnWasQuoted.size() && st->columnWasQuoted[c]));
        }
        out << CsvCore::joinRow(escaped, separator) << "\n";
    }
}

void onSaveClicked(CsvGtkState *st)
{
    saveFile(st, st->currentFile, st->separator);
    updateDirtyLabel(st, false);
}

void onSaveAsClicked(CsvGtkState *st)
{
    GtkWidget *toplevel = gtk_widget_get_toplevel(st->root);
    GtkWidget *dlg = gtk_file_chooser_dialog_new("Save As",
        GTK_IS_WINDOW(toplevel) ? GTK_WINDOW(toplevel) : nullptr,
        GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT, nullptr);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
    if (!st->currentFile.empty())
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dlg), st->currentFile.c_str());

    GtkFileFilter *csvFilter = gtk_file_filter_new();
    gtk_file_filter_set_name(csvFilter, "CSV - Comma Separated (*.csv)");
    gtk_file_filter_add_pattern(csvFilter, "*.csv");
    GtkFileFilter *tsvFilter = gtk_file_filter_new();
    gtk_file_filter_set_name(tsvFilter, "TSV - Tab Separated (*.tsv)");
    gtk_file_filter_add_pattern(tsvFilter, "*.tsv");
    if (st->separator == '\t') {
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), tsvFilter);
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), csvFilter);
    } else {
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), csvFilter);
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), tsvFilter);
    }

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        GtkFileFilter *chosen = gtk_file_chooser_get_filter(GTK_FILE_CHOOSER(dlg));
        char sep = (chosen == tsvFilter) ? '\t' : ',';
        saveFile(st, filename, sep);
        st->currentFile = filename;
        st->separator = sep;
        updateDirtyLabel(st, false);
        if (st->showingText) updateTextView(st);
        g_free(filename);
    }
    gtk_widget_destroy(dlg);
}

void showSeparatorMismatchDialog(CsvGtkState *st)
{
    bool isCsvExt = endsWithNoCase(st->currentFile, ".csv");
    bool isTsvExt = endsWithNoCase(st->currentFile, ".tsv");
    if (!((isCsvExt && st->separator == '\t') || (isTsvExt && st->separator == ','))) return;

    const char *msg = isCsvExt
        ? "This .csv file appears to use tab separators instead of commas."
        : "This .tsv file appears to use comma separators instead of tabs.";
    GtkWidget *toplevel = gtk_widget_get_toplevel(st->root);
    GtkWidget *dlg = gtk_message_dialog_new(GTK_IS_WINDOW(toplevel) ? GTK_WINDOW(toplevel) : nullptr,
        GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE, "%s", msg);
    gtk_window_set_title(GTK_WINDOW(dlg), "Separator Mismatch");
    gtk_dialog_add_button(GTK_DIALOG(dlg), "Ignore", GTK_RESPONSE_CANCEL);
    gtk_dialog_add_button(GTK_DIALOG(dlg), "Fix Separator", 1);
    gtk_dialog_add_button(GTK_DIALOG(dlg), "Rename Extension", 2);
    int response = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);

    if (response == 1) {
        // Rewrite the raw bytes on disk, swapping the separator outside quotes.
        char oldSep = st->separator;
        char newSep = isCsvExt ? ',' : '\t';
        std::string raw = readFile(st->currentFile);
        bool inQuote = false;
        for (char &ch : raw) {
            if (ch == '"') inQuote = !inQuote;
            else if (!inQuote && ch == oldSep) ch = newSep;
        }
        std::ofstream out(st->currentFile, std::ios::binary | std::ios::trunc);
        out << raw;
        out.close();
        loadFile(st, st->currentFile);
    } else if (response == 2) {
        std::string newPath = st->currentFile.substr(0, st->currentFile.find_last_of('.')) +
                               (isCsvExt ? ".tsv" : ".csv");
        if (std::rename(st->currentFile.c_str(), newPath.c_str()) == 0)
            loadFile(st, newPath);
    }
}

// --- Print (monospace, tab-joined rows paginated by line count) ---

struct PrintCtx {
    std::vector<std::string> lines;
    int linesPerPage = 1;
};

void onPrintBeginPrint(GtkPrintOperation *op, GtkPrintContext *ctx, gpointer data)
{
    auto *pc = static_cast<PrintCtx *>(data);
    double pageHeight = gtk_print_context_get_height(ctx);
    PangoLayout *layout = gtk_print_context_create_pango_layout(ctx);
    pango_layout_set_font_description(layout, pango_font_description_from_string("Monospace 9"));
    pango_layout_set_text(layout, "Ag", -1);
    int lineHeightPx;
    pango_layout_get_pixel_size(layout, nullptr, &lineHeightPx);
    g_object_unref(layout);
    pc->linesPerPage = std::max(1, (int)(pageHeight / std::max(1, lineHeightPx)));
    int pages = ((int)pc->lines.size() + pc->linesPerPage - 1) / pc->linesPerPage;
    gtk_print_operation_set_n_pages(op, std::max(1, pages));
}

void onPrintDrawPage(GtkPrintOperation *, GtkPrintContext *ctx, gint pageNr, gpointer data)
{
    auto *pc = static_cast<PrintCtx *>(data);
    cairo_t *cr = gtk_print_context_get_cairo_context(ctx);
    PangoLayout *layout = gtk_print_context_create_pango_layout(ctx);
    pango_layout_set_font_description(layout, pango_font_description_from_string("Monospace 9"));

    int start = pageNr * pc->linesPerPage;
    int end = std::min((int)pc->lines.size(), start + pc->linesPerPage);
    double y = 0;
    for (int i = start; i < end; ++i) {
        pango_layout_set_text(layout, pc->lines[i].c_str(), -1);
        cairo_move_to(cr, 0, y);
        pango_cairo_show_layout(cr, layout);
        int h;
        pango_layout_get_pixel_size(layout, nullptr, &h);
        y += h;
    }
    g_object_unref(layout);
}

void onPrintClicked(CsvGtkState *st)
{
    if (!st->grid) return;
    auto pc = std::make_unique<PrintCtx>();
    if (st->firstLineAsHeader) {
        std::vector<std::string> titles;
        for (int c = 0; c < st->grid->columnCount(); ++c) {
            GtkTreeViewColumn *col = gtk_tree_view_get_column(GTK_TREE_VIEW(st->grid->treeView()), c + 1);
            const char *title = col ? gtk_tree_view_column_get_title(col) : "";
            titles.push_back(title ? title : "");
        }
        pc->lines.push_back(joinRowPlain(titles, '\t'));
    }
    for (const auto &row : st->grid->rowData())
        pc->lines.push_back(joinRowPlain(row, '\t'));

    GtkPrintOperation *op = gtk_print_operation_new();
    g_signal_connect(op, "begin-print", G_CALLBACK(onPrintBeginPrint), pc.get());
    g_signal_connect(op, "draw-page", G_CALLBACK(onPrintDrawPage), pc.get());
    GtkWidget *toplevel = gtk_widget_get_toplevel(st->root);
    gtk_print_operation_run(op, GTK_PRINT_OPERATION_ACTION_PRINT_DIALOG,
        GTK_IS_WINDOW(toplevel) ? GTK_WINDOW(toplevel) : nullptr, nullptr);
    g_object_unref(op);
}

void onOpenExternallyClicked(CsvGtkState *st)
{
    if (st->currentFile.empty()) return;
    GError *error = nullptr;
    std::string uri = "file://" + st->currentFile;
    if (!g_app_info_launch_default_for_uri(uri.c_str(), nullptr, &error)) {
        if (error) g_error_free(error);
    }
}

void onToggleShowText(CsvGtkState *st, bool active)
{
    st->showingText = active;
    if (!st->grid) return;
    if (active) {
        updateTextView(st);
        gtk_widget_hide(st->grid->widget());
        gtk_widget_show(st->textScroll);
        if (st->findPanel->isPanelVisible()) st->findPanel->showPanel(false);
    } else {
        gtk_widget_hide(st->textScroll);
        gtk_widget_show(st->grid->widget());
    }
}

void onToggleWordWrap(CsvGtkState *st, bool active)
{
    st->wordWrap = active;
    if (st->textView)
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(st->textView), active ? GTK_WRAP_WORD_CHAR : GTK_WRAP_NONE);
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

    st->dirtyLabel = gtk_label_new("✓");
    gtk_widget_set_margin_start(st->dirtyLabel, 4);
    gtk_widget_set_margin_end(st->dirtyLabel, 4);
    gtk_box_pack_start(GTK_BOX(st->toolbar->widget()), st->dirtyLabel, FALSE, FALSE, 0);

    st->toolbar->addToolAction("Save", "document-save-symbolic", [st]() { onSaveClicked(st); });
    st->toolbar->addToolAction("Save As...", "document-save-as-symbolic", [st]() { onSaveAsClicked(st); });
    st->undoBtn = st->toolbar->addToolAction("Undo", "edit-undo-symbolic", [st]() {
        st->fm->undo();
        refreshUndoRedoSensitivity(st);
    });
    st->redoBtn = st->toolbar->addToolAction("Redo", "edit-redo-symbolic", [st]() {
        st->fm->redo();
        refreshUndoRedoSensitivity(st);
    });
    st->toolbar->addToolAction("Print", "document-print-symbolic", [st]() { onPrintClicked(st); });
    st->toolbar->addToolAction("Reload", "view-refresh-symbolic", [st]() {
        if (!st->currentFile.empty()) loadFile(st, st->currentFile);
    });
    st->headerToggle = st->toolbar->addToggleAction("Header Row", "view-list-symbolic", true, [st](bool active) {
        st->firstLineAsHeader = active;
        if (!st->currentFile.empty()) loadFile(st, st->currentFile);
    });
    st->findToggle = st->toolbar->addToggleAction("Find/Replace", "edit-find-replace-symbolic", false, [st](bool active) {
        st->findPanel->showPanel(active);
    });
    st->textToggle = st->toolbar->addToggleAction("Show Text", "view-reveal-symbolic", false, [st](bool active) {
        onToggleShowText(st, active);
    });
    st->wrapToggle = st->toolbar->addToggleAction("Line Wrap", "format-text-wrap-symbolic", false, [st](bool active) {
        onToggleWordWrap(st, active);
    });
    st->toolbar->addToolAction("Open Externally", "document-open-symbolic", [st]() { onOpenExternallyClicked(st); });
    gtk_box_pack_start(GTK_BOX(st->root), st->toolbar->widget(), FALSE, FALSE, 0);

    // Real grid gets built by loadFile() below (needs the file's actual
    // column count); pack a temporary empty box as its future slot, plus
    // the (initially hidden) plain-text view used by "Show Text".
    st->gridSlot = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(st->gridSlot, TRUE);
    gtk_box_pack_start(GTK_BOX(st->root), st->gridSlot, TRUE, TRUE, 0);
    st->grid = std::make_unique<GtkEditableGridWidget>(1, st->fm.get());
    addRowNumberColumn(st->grid->treeView());
    gtk_box_pack_start(GTK_BOX(st->gridSlot), st->grid->widget(), TRUE, TRUE, 0);

    st->textView = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(st->textView), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(st->textView), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(st->textView), GTK_WRAP_NONE);
    st->textScroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_container_add(GTK_CONTAINER(st->textScroll), st->textView);
    gtk_widget_set_vexpand(st->textScroll, TRUE);
    gtk_box_pack_start(GTK_BOX(st->gridSlot), st->textScroll, TRUE, TRUE, 0);
    gtk_widget_set_no_show_all(st->textScroll, TRUE); // stays hidden until "Show Text" is toggled

    st->findPanel = std::make_unique<GtkScopedFindReplacePanel>(st->fm.get());
    st->findPanel->setScopes({"All Cells", "Current Column"});
    st->findPanel->onFindRequested = [st](bool forward) { doFind(st, forward); };
    st->findPanel->onReplaceRequested = [st]() { doReplace(st); };
    st->findPanel->onReplaceAllRequested = [st]() { doReplaceAll(st); };
    gtk_box_pack_start(GTK_BOX(st->root), st->findPanel->widget(), FALSE, FALSE, 0);

    st->fm->enableUndoShortcuts();
    st->fm->registerShortcut(GDK_KEY_f, GDK_CONTROL_MASK, GtkFocusManager::Always, [st]() {
        bool nowVisible = !st->findPanel->isPanelVisible();
        // Toggling the button (rather than calling showPanel() directly)
        // keeps its pressed-state in sync with the panel's actual
        // visibility, since addToggleAction's callback is what calls
        // showPanel().
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(st->findToggle), nowVisible);
        return true;
    });
    st->fm->registerShortcut(GDK_KEY_s, GDK_CONTROL_MASK, GtkFocusManager::Always, [st]() {
        onSaveClicked(st);
        return true;
    });

    g_object_set_data_full(G_OBJECT(st->root), "csv-state", st, destroyState);

    gtk_widget_show_all(st->root);
    gtk_widget_hide(st->textScroll);
    st->findPanel->showPanel(false); // hidden until Ctrl+F / toolbar toggle
    loadFile(st, std::string(FileToLoad));
    refreshUndoRedoSensitivity(st);
    showSeparatorMismatchDialog(st);

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
