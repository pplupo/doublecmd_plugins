#include "EditorWidget.h"

#include "wlxbase_gtk/GtkFocusManager.h"
#include "wlxbase_gtk/GtkPluginToolBar.h"
#include "wlxbase_gtk/GtkFindReplacePanel.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>

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
    m_undoBtn = m_toolbar->addToolAction("Undo", "edit-undo-symbolic", [this]() {
        if (gtk_source_buffer_can_undo(m_buffer))
            gtk_source_buffer_undo(m_buffer);
    });
    m_redoBtn = m_toolbar->addToolAction("Redo", "edit-redo-symbolic", [this]() {
        if (gtk_source_buffer_can_redo(m_buffer))
            gtk_source_buffer_redo(m_buffer);
    });
    m_toolbar->addToolAction("Reload", "view-refresh-symbolic", [this]() { reload(); });
    m_toolbar->addToggleAction("Find/Replace", "edit-find-replace-symbolic", false, [this](bool active) {
        m_findPanel->showPanel(active);
    });
    m_wrapToggle = m_toolbar->addToggleAction("Word Wrap", "format-text-wrap-symbolic", false, [this](bool active) {
        setWordWrap(active);
    });
    m_readOnlyToggle = m_toolbar->addToggleAction("Read-Only", "changes-prevent-symbolic", true, [this](bool active) {
        setReadOnly(active);
    });
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
    m_langLabel = gtk_label_new("Plain Text");
    m_modeLabel = gtk_label_new("INS");

    gtk_box_pack_start(GTK_BOX(m_statusBar), m_posLabel, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(m_statusBar), gtk_label_new("UTF-8"), FALSE, FALSE, 0);
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

bool EditorWidget::loadFile(const std::string &path)
{
    m_currentFile = path;
    std::string data = readFile(path);

    gtk_text_buffer_set_text(GTK_TEXT_BUFFER(m_buffer), data.c_str(), (gint)data.size());
    gtk_text_buffer_set_modified(GTK_TEXT_BUFFER(m_buffer), FALSE);

    GtkTextIter start;
    gtk_text_buffer_get_start_iter(GTK_TEXT_BUFFER(m_buffer), &start);
    gtk_text_buffer_place_cursor(GTK_TEXT_BUFFER(m_buffer), &start);

    detectAndApplyLanguage();
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

    m_ignoreNextDiskChange = true;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    bool ok = (bool)out;
    if (ok) out << text;
    g_free(text);

    if (ok) {
        m_currentFile = path;
        gtk_text_buffer_set_modified(GTK_TEXT_BUFFER(m_buffer), FALSE);
        updateDirtyIndicator();
    }
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

    gint count = 0;
    GError *error = nullptr;
    gtk_source_search_context_replace_all(ctx, replacement.c_str(), -1, &error);
    if (error) { g_error_free(error); }
    count = gtk_source_search_context_get_occurrences_count(ctx);

    g_object_unref(ctx);
    g_object_unref(settings);

    m_findPanel->setStatusText(std::to_string(std::max(0, count)) + " replaced");
}
