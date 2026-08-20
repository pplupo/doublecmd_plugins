#include "EditorWidget.h"

#include "wlxbase_gtk/GtkFocusManager.h"
#include "wlxbase_gtk/GtkPluginToolBar.h"
#include "wlxbase_gtk/GtkFindReplacePanel.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cwctype>
#include <utility>

using namespace GtkWlPlugin;

namespace {

bool isSystemDark()
{
    GtkSettings *settings = gtk_settings_get_default();
    if (!settings) return false;
    gboolean preferDark = FALSE;
    g_object_get(settings, "gtk-application-prefer-dark-theme", &preferDark, nullptr);
    return preferDark;
}

std::string readFile(const std::string &path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// A GtkSourceView-shipped scheme literally named "kate" exists (mirrors
// Kate's own default palette) -- fitting choice for the light variant.
const char *lightSchemeId() { return "kate"; }
const char *darkSchemeId() { return "oblivion"; }

} // namespace

EditorWidget::EditorWidget()
{
    static bool inited = false;
    if (!inited) { gtk_source_init(); inited = true; }

    m_root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    m_diskChangeBar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(m_diskChangeBar), 4);
    m_diskChangeLabel = gtk_label_new("");
    gtk_widget_set_hexpand(m_diskChangeLabel, TRUE);
    gtk_label_set_xalign(GTK_LABEL(m_diskChangeLabel), 0.0);
    gtk_box_pack_start(GTK_BOX(m_diskChangeBar), m_diskChangeLabel, TRUE, TRUE, 0);
    GtkWidget *reloadBtn = gtk_button_new_with_label("Reload");
    g_signal_connect_swapped(reloadBtn, "clicked", G_CALLBACK(+[](EditorWidget *self) {
        self->reload();
        self->showDiskChangeBar(false);
    }), this);
    gtk_box_pack_start(GTK_BOX(m_diskChangeBar), reloadBtn, FALSE, FALSE, 0);
    GtkWidget *ignoreBtn = gtk_button_new_with_label("Ignore");
    g_signal_connect_swapped(ignoreBtn, "clicked", G_CALLBACK(+[](EditorWidget *self) {
        self->showDiskChangeBar(false);
    }), this);
    gtk_box_pack_start(GTK_BOX(m_diskChangeBar), ignoreBtn, FALSE, FALSE, 0);
    gtk_widget_set_no_show_all(m_diskChangeBar, TRUE);

    m_buffer = gtk_source_buffer_new(nullptr);
    m_view = gtk_source_view_new_with_buffer(m_buffer);
    gtk_source_view_set_show_line_numbers(GTK_SOURCE_VIEW(m_view), TRUE);
    gtk_source_view_set_highlight_current_line(GTK_SOURCE_VIEW(m_view), TRUE);
    gtk_source_view_set_show_right_margin(GTK_SOURCE_VIEW(m_view), FALSE);
    gtk_source_view_set_tab_width(GTK_SOURCE_VIEW(m_view), 4);
    gtk_source_view_set_smart_backspace(GTK_SOURCE_VIEW(m_view), TRUE);
    gtk_source_buffer_set_highlight_matching_brackets(m_buffer, TRUE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(m_view), TRUE);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(m_view), FALSE); // read-only by default, matches kate_qt6

    m_scrolled = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_container_add(GTK_CONTAINER(m_scrolled), m_view);
    gtk_widget_set_vexpand(m_scrolled, TRUE);

    m_fm = std::make_unique<GtkFocusManager>(m_root, m_view);

    setupToolbar();
    gtk_box_pack_start(GTK_BOX(m_root), m_toolbar->widget(), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(m_root), m_diskChangeBar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(m_root), m_scrolled, TRUE, TRUE, 0);

    setupFindReplace();
    gtk_box_pack_start(GTK_BOX(m_root), m_findPanel->widget(), FALSE, FALSE, 0);

    setupStatusBar();
    gtk_box_pack_start(GTK_BOX(m_root), m_statusBar, FALSE, FALSE, 0);

    g_signal_connect_swapped(m_buffer, "notify::cursor-position",
        G_CALLBACK(+[](EditorWidget *self) { self->updateStatusBar(); }), this);
    g_signal_connect_swapped(m_buffer, "modified-changed",
        G_CALLBACK(+[](EditorWidget *self) { self->updateDirtyIndicator(); }), this);
    g_signal_connect_swapped(m_view, "notify::overwrite",
        G_CALLBACK(+[](EditorWidget *self) { self->updateStatusBar(); }), this);

    m_fm->enableUndoShortcuts();
    m_fm->registerShortcut(GDK_KEY_f, GDK_CONTROL_MASK, GtkFocusManager::Always, [this]() {
        bool nowVisible = !m_findPanel->isPanelVisible();
        m_findPanel->showPanel(nowVisible);
        return true;
    });
    m_fm->registerShortcut(GDK_KEY_s, GDK_CONTROL_MASK, GtkFocusManager::Always, [this]() {
        save();
        return true;
    });
    m_fm->registerShortcut(GDK_KEY_g, GDK_CONTROL_MASK, GtkFocusManager::Always, [this]() {
        showGotoLineDialog();
        return true;
    });

    applyStyleScheme();
    m_findPanel->showPanel(false);
}

EditorWidget::~EditorWidget()
{
    if (m_monitor) {
        if (m_monitorHandler) g_signal_handler_disconnect(m_monitor, m_monitorHandler);
        g_object_unref(m_monitor);
    }
}

void EditorWidget::setupToolbar()
{
    m_toolbar = std::make_unique<GtkPluginToolBar>(m_fm.get());

    m_dirtyLabel = gtk_label_new("");
    gtk_widget_set_margin_start(m_dirtyLabel, 4);
    gtk_widget_set_margin_end(m_dirtyLabel, 4);
    gtk_box_pack_start(GTK_BOX(m_toolbar->widget()), m_dirtyLabel, FALSE, FALSE, 0);

    m_toolbar->addToolAction("Save", "document-save-symbolic", [this]() { save(); });
    m_toolbar->addToolAction("Save As...", "document-save-as-symbolic", [this]() { showSaveAsDialog(false); });
    m_toolbar->addToolAction("Save Copy As...", "document-multiple-symbolic", [this]() { showSaveAsDialog(true); });
    m_toolbar->addToolAction("Save With Encoding...", "text-x-generic-symbolic", [this]() { showEncodingPickerAndSave(); });
    m_undoBtn = m_toolbar->addToolAction("Undo", "edit-undo-symbolic", [this]() {
        if (gtk_source_buffer_can_undo(m_buffer))
            gtk_source_buffer_undo(m_buffer);
    });
    m_redoBtn = m_toolbar->addToolAction("Redo", "edit-redo-symbolic", [this]() {
        if (gtk_source_buffer_can_redo(m_buffer))
            gtk_source_buffer_redo(m_buffer);
    });
    m_toolbar->addToolAction("Reload", "view-refresh-symbolic", [this]() { reload(); });
    m_toolbar->addToolAction("Go to Line...", "go-jump-symbolic", [this]() { showGotoLineDialog(); });
    m_toolbar->addToggleAction("Find/Replace", "edit-find-replace-symbolic", false, [this](bool active) {
        m_findPanel->showPanel(active);
    });
    m_wrapToggle = m_toolbar->addToggleAction("Word Wrap", "format-text-wrap-symbolic", false, [this](bool active) {
        setWordWrap(active);
    });
    m_readOnlyToggle = m_toolbar->addToggleAction("Read-Only", "changes-prevent-symbolic", true, [this](bool active) {
        setReadOnly(active);
    });

    // Capitalization-conversion menu -- mirrors kate_qt6's Edit >
    // Capitalization submenu. A single GtkMenuButton with a dropdown,
    // rather than 6 separate toolbar buttons, to keep the toolbar from
    // getting too wide.
    GtkWidget *caseBtn = gtk_menu_button_new();
    GtkWidget *caseIcon = gtk_image_new_from_icon_name("format-text-strikethrough-symbolic", GTK_ICON_SIZE_SMALL_TOOLBAR);
    gtk_container_add(GTK_CONTAINER(caseBtn), caseIcon);
    gtk_widget_set_tooltip_text(caseBtn, "Change Case");
    gtk_widget_set_can_focus(caseBtn, FALSE);

    // Preprocessor macro-expansion of g_signal_connect() doesn't
    // understand C++ template angle brackets -- a raw, un-parenthesized
    // comma inside a std::pair<A, B> template argument list used
    // directly as a macro argument gets misparsed as an extra macro
    // argument. Using an alias sidesteps that entirely.
    using CaseCtx = std::pair<EditorWidget *, int>;
    GtkWidget *caseMenu = gtk_menu_new();
    struct { const char *label; int mode; } items[] = {
        {"UPPERCASE", 0}, {"lowercase", 1}, {"Title Case", 2},
        {"Proper case", 3}, {"Sentence case", 4}, {"camelCase", 5},
    };
    for (auto &it : items) {
        GtkWidget *item = gtk_menu_item_new_with_label(it.label);
        int mode = it.mode;
        g_signal_connect(item, "activate", G_CALLBACK(+[](GtkMenuItem *, gpointer data) {
            auto *pair = static_cast<CaseCtx *>(data);
            pair->first->applyCaseTransform(pair->second);
        }), new CaseCtx(this, mode));
        gtk_menu_shell_append(GTK_MENU_SHELL(caseMenu), item);
    }
    gtk_widget_show_all(caseMenu);
    gtk_menu_button_set_popup(GTK_MENU_BUTTON(caseBtn), caseMenu);
    gtk_box_pack_start(GTK_BOX(m_toolbar->widget()), caseBtn, FALSE, FALSE, 0);
    gtk_widget_show(caseBtn);
}

void EditorWidget::setupFindReplace()
{
    m_findPanel = std::make_unique<GtkFindReplacePanel>(m_fm.get());
    m_findPanel->onFindRequested = [this](bool forward) { doFind(forward); };
    m_findPanel->onReplaceRequested = [this]() { doReplace(); };
    m_findPanel->onReplaceAllRequested = [this]() { doReplaceAll(); };
}

void EditorWidget::setupStatusBar()
{
    m_statusBar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(m_statusBar), 2);

    m_posLabel = gtk_label_new("Line 1, Col 1");
    m_encodingLabel = gtk_label_new("UTF-8");
    m_modeLabel = gtk_label_new("INS");
    m_langLabel = gtk_label_new("Plain Text");

    gtk_box_pack_start(GTK_BOX(m_statusBar), m_posLabel, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(m_statusBar), m_encodingLabel, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(m_statusBar), m_modeLabel, FALSE, FALSE, 0);
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_pack_start(GTK_BOX(m_statusBar), spacer, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(m_statusBar), m_langLabel, FALSE, FALSE, 0);
}

void EditorWidget::detectAndApplyLanguage()
{
    GtkSourceLanguageManager *mgr = gtk_source_language_manager_get_default();
    gboolean uncertain = FALSE;
    gchar *contentType = g_content_type_guess(m_currentFile.c_str(), nullptr, 0, &uncertain);
    GtkSourceLanguage *lang = gtk_source_language_manager_guess_language(mgr, m_currentFile.c_str(), contentType);
    g_free(contentType);

    gtk_source_buffer_set_language(m_buffer, lang);
    gtk_label_set_text(GTK_LABEL(m_langLabel), lang ? gtk_source_language_get_name(lang) : "Plain Text");
}

void EditorWidget::applyStyleScheme()
{
    GtkSourceStyleSchemeManager *mgr = gtk_source_style_scheme_manager_get_default();
    const char *id = isSystemDark() ? darkSchemeId() : lightSchemeId();
    GtkSourceStyleScheme *scheme = gtk_source_style_scheme_manager_get_scheme(mgr, id);
    if (scheme) gtk_source_buffer_set_style_scheme(m_buffer, scheme);
}

// --- Encoding ---------------------------------------------------------

std::string EditorWidget::decodeToUtf8(const std::string &rawBytes, std::string &detectedEncoding)
{
    if (rawBytes.empty() || g_utf8_validate(rawBytes.data(), (gssize)rawBytes.size(), nullptr)) {
        detectedEncoding = "UTF-8";
        return rawBytes;
    }

    GSList *candidates = gtk_source_encoding_get_default_candidates();
    for (GSList *l = candidates; l; l = l->next) {
        auto *enc = static_cast<const GtkSourceEncoding *>(l->data);
        const char *charset = gtk_source_encoding_get_charset(enc);
        if (!charset || g_ascii_strcasecmp(charset, "UTF-8") == 0) continue;

        gsize bytesRead = 0, bytesWritten = 0;
        GError *error = nullptr;
        gchar *converted = g_convert(rawBytes.data(), (gssize)rawBytes.size(), "UTF-8", charset,
                                      &bytesRead, &bytesWritten, &error);
        if (converted && !error && bytesRead == rawBytes.size()) {
            std::string result(converted, bytesWritten);
            g_free(converted);
            g_slist_free(candidates);
            detectedEncoding = charset;
            return result;
        }
        if (converted) g_free(converted);
        if (error) g_error_free(error);
    }
    g_slist_free(candidates);

    // Nothing decoded cleanly -- fall back to UTF-8 with invalid
    // sequences replaced, same "don't just crash on garbage bytes"
    // spirit as most editors' last-resort behavior.
    detectedEncoding = "UTF-8 (invalid bytes replaced)";
    gchar *fallback = g_utf8_make_valid(rawBytes.c_str(), (gssize)rawBytes.size());
    std::string result = fallback ? fallback : std::string();
    g_free(fallback);
    return result;
}

bool EditorWidget::encodeFromUtf8(const std::string &utf8Text, const std::string &encoding, std::string &out)
{
    if (encoding.empty() || g_ascii_strcasecmp(encoding.c_str(), "UTF-8") == 0 ||
        encoding.rfind("UTF-8 ", 0) == 0) {
        out = utf8Text;
        return true;
    }
    gsize bytesRead = 0, bytesWritten = 0;
    GError *error = nullptr;
    gchar *converted = g_convert(utf8Text.data(), (gssize)utf8Text.size(), encoding.c_str(), "UTF-8",
                                  &bytesRead, &bytesWritten, &error);
    if (!converted || error) {
        if (converted) g_free(converted);
        if (error) g_error_free(error);
        return false;
    }
    out.assign(converted, bytesWritten);
    g_free(converted);
    return true;
}

// --- Load / save --------------------------------------------------------

bool EditorWidget::loadFile(const std::string &path)
{
    m_currentFile = path;
    std::string raw = readFile(path);
    std::string utf8 = decodeToUtf8(raw, m_encoding);

    gtk_text_buffer_set_text(GTK_TEXT_BUFFER(m_buffer), utf8.c_str(), (gint)utf8.size());
    gtk_text_buffer_set_modified(GTK_TEXT_BUFFER(m_buffer), FALSE);

    GtkTextIter start;
    gtk_text_buffer_get_start_iter(GTK_TEXT_BUFFER(m_buffer), &start);
    gtk_text_buffer_place_cursor(GTK_TEXT_BUFFER(m_buffer), &start);

    detectAndApplyLanguage();
    if (m_encodingLabel) gtk_label_set_text(GTK_LABEL(m_encodingLabel), m_encoding.c_str());
    updateStatusBar();
    updateDirtyIndicator();

    // Disk-change detection, same idea as diagramview's GFileMonitor use.
    if (m_monitor) {
        if (m_monitorHandler) g_signal_handler_disconnect(m_monitor, m_monitorHandler);
        g_object_unref(m_monitor);
        m_monitor = nullptr;
    }
    GFile *gfile = g_file_new_for_path(path.c_str());
    GError *error = nullptr;
    m_monitor = g_file_monitor_file(gfile, G_FILE_MONITOR_NONE, nullptr, &error);
    g_object_unref(gfile);
    if (m_monitor) {
        m_monitorHandler = g_signal_connect_swapped(m_monitor, "changed", G_CALLBACK(+[](EditorWidget *self, GFile *, GFile *, GFileMonitorEvent event) {
            if (self->m_ignoreNextDiskChange) { self->m_ignoreNextDiskChange = false; return; }
            if (event == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT || event == G_FILE_MONITOR_EVENT_CREATED)
                self->showDiskChangeBar(true);
        }), this);
    } else if (error) {
        g_error_free(error);
    }
    showDiskChangeBar(false);

    return true;
}

bool EditorWidget::save()
{
    return saveAs(m_currentFile);
}

bool EditorWidget::saveAs(const std::string &path)
{
    if (path.empty()) return false;

    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(m_buffer), &start, &end);
    gchar *text = gtk_text_buffer_get_text(GTK_TEXT_BUFFER(m_buffer), &start, &end, TRUE);

    std::string encoded;
    bool ok = encodeFromUtf8(text, m_encoding, encoded);
    g_free(text);
    if (!ok) return false;

    m_ignoreNextDiskChange = true;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ok = (bool)out;
    if (ok) out << encoded;

    if (ok) {
        m_currentFile = path;
        gtk_text_buffer_set_modified(GTK_TEXT_BUFFER(m_buffer), FALSE);
        updateDirtyIndicator();
    }
    return ok;
}

bool EditorWidget::saveCopyAs(const std::string &path, const std::string &encoding)
{
    if (path.empty()) return false;

    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(m_buffer), &start, &end);
    gchar *text = gtk_text_buffer_get_text(GTK_TEXT_BUFFER(m_buffer), &start, &end, TRUE);

    std::string encoded;
    bool ok = encodeFromUtf8(text, encoding.empty() ? m_encoding : encoding, encoded);
    g_free(text);
    if (!ok) return false;

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ok = (bool)out;
    if (ok) out << encoded;
    return ok;
}

void EditorWidget::setReadOnly(bool readOnly)
{
    gtk_text_view_set_editable(GTK_TEXT_VIEW(m_view), !readOnly);
}

bool EditorWidget::isReadOnly() const
{
    return !gtk_text_view_get_editable(GTK_TEXT_VIEW(m_view));
}

void EditorWidget::setWordWrap(bool wrap)
{
    m_wordWrap = wrap;
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(m_view), wrap ? GTK_WRAP_WORD_CHAR : GTK_WRAP_NONE);
}

void EditorWidget::reload()
{
    if (!m_currentFile.empty()) loadFile(m_currentFile);
}

void EditorWidget::gotoLine(int oneBasedLine)
{
    GtkTextIter iter;
    gtk_text_buffer_get_iter_at_line(GTK_TEXT_BUFFER(m_buffer), &iter, std::max(0, oneBasedLine - 1));
    gtk_text_buffer_place_cursor(GTK_TEXT_BUFFER(m_buffer), &iter);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(m_view), &iter, 0.1, FALSE, 0, 0);
    gtk_widget_grab_focus(m_view);
}

bool EditorWidget::isDirty() const
{
    return gtk_text_buffer_get_modified(GTK_TEXT_BUFFER(m_buffer));
}

void EditorWidget::updateStatusBar()
{
    GtkTextIter iter;
    gtk_text_buffer_get_iter_at_mark(GTK_TEXT_BUFFER(m_buffer), &iter, gtk_text_buffer_get_insert(GTK_TEXT_BUFFER(m_buffer)));
    int line = gtk_text_iter_get_line(&iter) + 1;
    int col = gtk_text_iter_get_line_offset(&iter) + 1;
    char buf[64];
    snprintf(buf, sizeof(buf), "Line %d, Col %d", line, col);
    gtk_label_set_text(GTK_LABEL(m_posLabel), buf);

    gtk_label_set_text(GTK_LABEL(m_modeLabel), gtk_text_view_get_overwrite(GTK_TEXT_VIEW(m_view)) ? "OVR" : "INS");
}

void EditorWidget::updateDirtyIndicator()
{
    bool dirty = isDirty();
    gtk_label_set_text(GTK_LABEL(m_dirtyLabel), dirty ? "●" : "✓");
    if (m_undoBtn) gtk_widget_set_sensitive(m_undoBtn, gtk_source_buffer_can_undo(m_buffer));
    if (m_redoBtn) gtk_widget_set_sensitive(m_redoBtn, gtk_source_buffer_can_redo(m_buffer));
}

void EditorWidget::showDiskChangeBar(bool show)
{
    if (show) {
        std::string msg = "The file \"" + m_currentFile + "\" was modified on disk.";
        gtk_label_set_text(GTK_LABEL(m_diskChangeLabel), msg.c_str());
        gtk_widget_set_no_show_all(m_diskChangeBar, FALSE);
        gtk_widget_show_all(m_diskChangeBar);
    } else {
        gtk_widget_hide(m_diskChangeBar);
    }
}

// --- Dialogs --------------------------------------------------------------

void EditorWidget::showGotoLineDialog()
{
    GtkWidget *toplevel = gtk_widget_get_toplevel(m_root);
    GtkWidget *dlg = gtk_dialog_new_with_buttons("Go to Line",
        GTK_IS_WINDOW(toplevel) ? GTK_WINDOW(toplevel) : nullptr,
        GTK_DIALOG_MODAL,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Go", GTK_RESPONSE_ACCEPT, nullptr);

    int lineCount = gtk_text_buffer_get_line_count(GTK_TEXT_BUFFER(m_buffer));
    GtkTextIter cur;
    gtk_text_buffer_get_iter_at_mark(GTK_TEXT_BUFFER(m_buffer), &cur, gtk_text_buffer_get_insert(GTK_TEXT_BUFFER(m_buffer)));
    int curLine = gtk_text_iter_get_line(&cur) + 1;

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(hbox), 8);
    gtk_box_pack_start(GTK_BOX(hbox), gtk_label_new("Line number:"), FALSE, FALSE, 0);
    GtkWidget *spin = gtk_spin_button_new_with_range(1, lineCount, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), curLine);
    gtk_entry_set_activates_default(GTK_ENTRY(spin), TRUE);
    gtk_box_pack_start(GTK_BOX(hbox), spin, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(content), hbox);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_ACCEPT);
    gtk_widget_show_all(dlg);

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        gotoLine(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin)));
    }
    gtk_widget_destroy(dlg);
}

void EditorWidget::showSaveAsDialog(bool copyOnly)
{
    GtkWidget *toplevel = gtk_widget_get_toplevel(m_root);
    GtkWidget *dlg = gtk_file_chooser_dialog_new(copyOnly ? "Save Copy As" : "Save As",
        GTK_IS_WINDOW(toplevel) ? GTK_WINDOW(toplevel) : nullptr,
        GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT, nullptr);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
    if (!m_currentFile.empty())
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dlg), m_currentFile.c_str());

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        if (copyOnly)
            saveCopyAs(filename, m_encoding);
        else
            saveAs(filename);
        g_free(filename);
    }
    gtk_widget_destroy(dlg);
}

void EditorWidget::showEncodingPickerAndSave()
{
    GtkWidget *toplevel = gtk_widget_get_toplevel(m_root);
    GtkWidget *dlg = gtk_dialog_new_with_buttons("Save With Encoding",
        GTK_IS_WINDOW(toplevel) ? GTK_WINDOW(toplevel) : nullptr,
        GTK_DIALOG_MODAL,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT, nullptr);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(hbox), 8);
    gtk_box_pack_start(GTK_BOX(hbox), gtk_label_new("Encoding:"), FALSE, FALSE, 0);
    GtkWidget *combo = gtk_combo_box_text_new();

    GSList *candidates = gtk_source_encoding_get_all();
    int activeIdx = 0, i = 0;
    for (GSList *l = candidates; l; l = l->next, ++i) {
        auto *enc = static_cast<const GtkSourceEncoding *>(l->data);
        const char *charset = gtk_source_encoding_get_charset(enc);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), charset, charset);
        if (m_encoding == charset) activeIdx = i;
    }
    g_slist_free(candidates);
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), activeIdx);
    gtk_box_pack_start(GTK_BOX(hbox), combo, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(content), hbox);
    gtk_widget_show_all(dlg);

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        const char *chosen = gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo));
        if (chosen) {
            m_encoding = chosen;
            if (m_encodingLabel) gtk_label_set_text(GTK_LABEL(m_encodingLabel), m_encoding.c_str());
            save();
        }
    }
    gtk_widget_destroy(dlg);
}

// --- Case transforms --------------------------------------------------

namespace {
std::string toUpper(const std::string &s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return std::toupper(c); });
    return r;
}
std::string toLower(const std::string &s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return std::tolower(c); });
    return r;
}
bool isWordChar(unsigned char c) { return std::isalnum(c) || c == '_'; }

std::string toTitleCase(const std::string &s) {
    std::string r = toLower(s);
    bool atStart = true;
    for (auto &c : r) {
        if (atStart && std::isalpha((unsigned char)c)) { c = std::toupper((unsigned char)c); atStart = false; }
        else if (!isWordChar((unsigned char)c)) atStart = true;
        else atStart = false;
    }
    return r;
}
// "Proper case": same rule as Title Case here (capitalize each word) --
// kept as a distinct menu entry to match kate_qt6's menu 1:1, even
// though this project's simple word-boundary rule doesn't distinguish
// them (kate_qt6's KTextEditor-backed version may apply locale-specific
// rules Proper/Title Case differ on; not reproduced here).
std::string toProperCase(const std::string &s) { return toTitleCase(s); }

std::string toSentenceCase(const std::string &s) {
    std::string r = toLower(s);
    bool atStart = true;
    for (auto &c : r) {
        if (atStart && std::isalpha((unsigned char)c)) { c = std::toupper((unsigned char)c); atStart = false; }
        else if (c == '.' || c == '!' || c == '?') atStart = true;
        else if (!std::isspace((unsigned char)c)) atStart = false;
    }
    return r;
}

std::string toCamelCase(const std::string &s) {
    std::string r;
    bool nextUpper = false;
    bool first = true;
    for (unsigned char c : s) {
        if (std::isspace(c) || c == '_' || c == '-') { nextUpper = true; continue; }
        if (nextUpper && !first) { r += std::toupper(c); nextUpper = false; }
        else { r += first ? std::tolower(c) : c; nextUpper = false; }
        first = false;
    }
    return r;
}
} // namespace

void EditorWidget::applyCaseTransform(int mode)
{
    GtkTextBuffer *buf = GTK_TEXT_BUFFER(m_buffer);
    GtkTextIter start, end;
    bool hadSelection = gtk_text_buffer_get_selection_bounds(buf, &start, &end);
    if (!hadSelection)
        gtk_text_buffer_get_bounds(buf, &start, &end); // whole document, matches kate_qt6's no-selection fallback

    gchar *text = gtk_text_buffer_get_text(buf, &start, &end, TRUE);
    std::string input = text ? text : "";
    g_free(text);

    std::string output;
    switch (mode) {
        case 0: output = toUpper(input); break;
        case 1: output = toLower(input); break;
        case 2: output = toTitleCase(input); break;
        case 3: output = toProperCase(input); break;
        case 4: output = toSentenceCase(input); break;
        case 5: output = toCamelCase(input); break;
        default: return;
    }

    gtk_text_buffer_begin_user_action(buf);
    gtk_text_buffer_delete(buf, &start, &end);
    gtk_text_buffer_insert(buf, &start, output.c_str(), -1);
    gtk_text_buffer_end_user_action(buf);
}

// --- Find/Replace matching, honoring the scope combo (All Cells / Current Column) ---

namespace {
bool textMatches(const std::string &hay, const std::string &needle, bool matchCase)
{
    if (needle.empty()) return false;
    if (matchCase) return hay.find(needle) != std::string::npos;
    std::string h = hay, n = needle;
    std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c) { return std::tolower(c); });
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return std::tolower(c); });
    return h.find(n) != std::string::npos;
}
}

void EditorWidget::doFind(bool forward)
{
    std::string query = m_findPanel->findText();
    if (query.empty()) return;
    GtkTextIter start, matchStart, matchEnd;
    GtkTextIter insertIter;
    gtk_text_buffer_get_iter_at_mark(GTK_TEXT_BUFFER(m_buffer), &insertIter, gtk_text_buffer_get_insert(GTK_TEXT_BUFFER(m_buffer)));
    start = insertIter;

    GtkSourceSearchSettings *settings = gtk_source_search_settings_new();
    gtk_source_search_settings_set_search_text(settings, query.c_str());
    gtk_source_search_settings_set_case_sensitive(settings, m_findPanel->matchCase());
    gtk_source_search_settings_set_regex_enabled(settings, m_findPanel->useRegex());
    GtkSourceSearchContext *ctx = gtk_source_search_context_new(m_buffer, settings);

    gboolean found;
    if (forward)
        found = gtk_source_search_context_forward(ctx, &start, &matchStart, &matchEnd, nullptr);
    else
        found = gtk_source_search_context_backward(ctx, &start, &matchStart, &matchEnd, nullptr);

    if (found) {
        gtk_text_buffer_select_range(GTK_TEXT_BUFFER(m_buffer), &matchStart, &matchEnd);
        gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(m_view), &matchStart, 0.1, FALSE, 0, 0);
        m_findPanel->setStatusText("Match found");
    } else {
        m_findPanel->setStatusText("No matches");
    }

    g_object_unref(ctx);
    g_object_unref(settings);
}

void EditorWidget::doReplace()
{
    GtkTextIter start, end;
    if (!gtk_text_buffer_get_selection_bounds(GTK_TEXT_BUFFER(m_buffer), &start, &end)) {
        doFind(true);
        return;
    }
    gchar *selected = gtk_text_buffer_get_text(GTK_TEXT_BUFFER(m_buffer), &start, &end, FALSE);
    bool matches = textMatches(selected, m_findPanel->findText(), m_findPanel->matchCase());
    g_free(selected);
    if (matches) {
        std::string replacement = m_findPanel->replaceText();
        gtk_text_buffer_delete(GTK_TEXT_BUFFER(m_buffer), &start, &end);
        gtk_text_buffer_insert(GTK_TEXT_BUFFER(m_buffer), &start, replacement.c_str(), -1);
    }
    doFind(true);
}

void EditorWidget::doReplaceAll()
{
    std::string query = m_findPanel->findText();
    std::string replacement = m_findPanel->replaceText();
    if (query.empty()) return;

    GtkSourceSearchSettings *settings = gtk_source_search_settings_new();
    gtk_source_search_settings_set_search_text(settings, query.c_str());
    gtk_source_search_settings_set_case_sensitive(settings, m_findPanel->matchCase());
    gtk_source_search_settings_set_regex_enabled(settings, m_findPanel->useRegex());
    GtkSourceSearchContext *ctx = gtk_source_search_context_new(m_buffer, settings);

    GError *error = nullptr;
    gtk_source_search_context_replace_all(ctx, replacement.c_str(), -1, &error);
    if (error) { g_error_free(error); }
    gint count = gtk_source_search_context_get_occurrences_count(ctx);

    g_object_unref(ctx);
    g_object_unref(settings);

    m_findPanel->setStatusText(std::to_string(std::max(0, count)) + " replaced");
}
