/*
 * Diagram (Mermaid/PlantUML) WLX plugin for Double Commander — GTK3 UI.
 *
 * Renders SVG via librsvg onto a GtkDrawingArea with Cairo, mirroring the
 * Qt6 build's QGraphicsView + QSvgRenderer approach. All CLI subprocess
 * invocation (mmdc/plantuml.jar/curl) and settings logic lives in
 * DiagramRenderer (src/core/) — shared with the Qt6 build, unmodified.
 */

#include <gtk/gtk.h>
#include <librsvg/rsvg.h>
#include <gio/gio.h>
#include <cmath>
#include <cstring>
#include <string>

#include "wlxplugin.h"
#include "DiagramRenderer.h"

namespace {

DiagramRenderer::Settings g_settings;
std::string g_configPath;

bool isSystemDark()
{
    GtkSettings *settings = gtk_settings_get_default();
    if (!settings) return false;
    gboolean preferDark = FALSE;
    g_object_get(settings, "gtk-application-prefer-dark-theme", &preferDark, nullptr);
    return preferDark;
}

struct DiagramState {
    GtkWidget *root = nullptr;
    GtkWidget *drawingArea = nullptr;
    RsvgHandle *handle = nullptr;
    std::string currentFilePath;
    std::string lastSvgData;

    double zoom = 1.0;
    double panX = 0.0;
    double panY = 0.0;
    bool dragging = false;
    double dragStartX = 0.0, dragStartY = 0.0;
    double panStartX = 0.0, panStartY = 0.0;
    bool fitted = false;

    GFileMonitor *monitor = nullptr;
    guint debounceTimerId = 0;
};

void executeRender(DiagramState *st);

void setHandle(DiagramState *st, const std::string &svgData)
{
    if (st->handle) {
        g_object_unref(st->handle);
        st->handle = nullptr;
    }
    st->lastSvgData = svgData;
    if (svgData.empty()) return;

    GError *error = nullptr;
    st->handle = rsvg_handle_new_from_data(
        reinterpret_cast<const guint8 *>(svgData.data()), svgData.size(), &error);
    if (!st->handle && error) {
        g_warning("[diagramview_gtk3] SVG parse error: %s", error->message);
        g_error_free(error);
    }
    st->fitted = false;
    gtk_widget_queue_draw(st->drawingArea);
}

void showError(GtkWidget *parent, const char *title, const char *msg)
{
    GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(gtk_widget_get_toplevel(parent)),
        GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "%s", msg);
    gtk_window_set_title(GTK_WINDOW(dlg), title);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

gboolean onDebounceTimeout(gpointer userData)
{
    auto *st = static_cast<DiagramState *>(userData);
    st->debounceTimerId = 0;
    if (g_settings.autoReloadEnabled)
        executeRender(st);
    return G_SOURCE_REMOVE;
}

void onFileChanged(GFileMonitor *, GFile *, GFile *, GFileMonitorEvent, gpointer userData)
{
    auto *st = static_cast<DiagramState *>(userData);
    if (st->debounceTimerId) g_source_remove(st->debounceTimerId);
    st->debounceTimerId = g_timeout_add(200, onDebounceTimeout, st);
}

std::string extensionOf(const std::string &path)
{
    auto slash = path.find_last_of('/');
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
    std::string ext = path.substr(dot + 1);
    for (auto &c : ext) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    return ext;
}

void executeRender(DiagramState *st)
{
    if (st->currentFilePath.empty()) return;

    std::string ext = extensionOf(st->currentFilePath);
    bool activeDarkMode = g_settings.useSystemDarkMode ? isSystemDark() : g_settings.darkMode;
    std::string svg;

    if (ext == "mmd" || ext == "mermaid") {
        svg = DiagramRenderer::renderMermaid(g_settings, st->currentFilePath, activeDarkMode);
        if (svg.empty()) {
            showError(st->root, "Diagram Viewer Error",
                "Failed to render Mermaid diagram.\n"
                "Please ensure '@mermaid-js/mermaid-cli' is installed, 'npx' is available, or internet connection is active.");
            return;
        }
        svg = DiagramRenderer::fixMermaidSvgText(svg);
    } else if (ext == "puml" || ext == "plantuml") {
        svg = DiagramRenderer::renderPlantUml(g_settings, st->currentFilePath, activeDarkMode);
        if (svg.empty()) {
            showError(st->root, "Diagram Viewer Error",
                "Failed to render PlantUML diagram.\n"
                "Please ensure Java/PlantUML is installed locally, or internet connection is active.");
            return;
        }
    } else {
        showError(st->root, "Diagram Viewer Error", ("Unsupported file extension: " + ext).c_str());
        return;
    }

    setHandle(st, svg);
}

void loadFile(DiagramState *st, const std::string &path)
{
    st->currentFilePath = path;

    if (st->monitor) {
        g_object_unref(st->monitor);
        st->monitor = nullptr;
    }
    GFile *gfile = g_file_new_for_path(path.c_str());
    GError *error = nullptr;
    st->monitor = g_file_monitor_file(gfile, G_FILE_MONITOR_NONE, nullptr, &error);
    g_object_unref(gfile);
    if (st->monitor) {
        g_signal_connect(st->monitor, "changed", G_CALLBACK(onFileChanged), st);
    } else if (error) {
        g_error_free(error);
    }

    executeRender(st);
}

void getDocSize(DiagramState *st, double *w, double *h)
{
    *w = 800; *h = 600;
    if (!st->handle) return;
    gdouble dw = 0, dh = 0;
    if (rsvg_handle_get_intrinsic_size_in_pixels(st->handle, &dw, &dh) && dw > 0 && dh > 0) {
        *w = dw; *h = dh;
    }
}

void fitToView(DiagramState *st)
{
    if (!st->handle || st->fitted) return;
    double docW, docH;
    getDocSize(st, &docW, &docH);

    int allocW = gtk_widget_get_allocated_width(st->drawingArea);
    int allocH = gtk_widget_get_allocated_height(st->drawingArea);
    if (allocW < 10 || allocH < 10 || docW <= 0 || docH <= 0) return;

    double margin = 20.0;
    double sx = (allocW - margin) / docW;
    double sy = (allocH - margin) / docH;
    st->zoom = std::min(sx, sy);
    st->panX = (allocW - docW * st->zoom) / 2.0;
    st->panY = (allocH - docH * st->zoom) / 2.0;
    st->fitted = true;
}

gboolean onDraw(GtkWidget *widget, cairo_t *cr, gpointer userData)
{
    auto *st = static_cast<DiagramState *>(userData);

    int allocW = gtk_widget_get_allocated_width(widget);
    int allocH = gtk_widget_get_allocated_height(widget);

    bool activeDarkMode = g_settings.useSystemDarkMode ? isSystemDark() : g_settings.darkMode;
    if (activeDarkMode)
        cairo_set_source_rgb(cr, 30 / 255.0, 30 / 255.0, 46 / 255.0);
    else
        cairo_set_source_rgb(cr, 248 / 255.0, 249 / 255.0, 250 / 255.0);
    cairo_paint(cr);

    // Dot grid, matching the Qt build's drawBackground().
    cairo_set_source_rgb(cr, activeDarkMode ? 45 / 255.0 : 226 / 255.0,
                              activeDarkMode ? 45 / 255.0 : 232 / 255.0,
                              activeDarkMode ? 68 / 255.0 : 240 / 255.0);
    cairo_set_line_width(cr, 1.0);
    double spacing = 20.0;
    for (double x = 0; x < allocW; x += spacing) {
        cairo_move_to(cr, x, 0); cairo_line_to(cr, x, allocH);
    }
    for (double y = 0; y < allocH; y += spacing) {
        cairo_move_to(cr, 0, y); cairo_line_to(cr, allocW, y);
    }
    cairo_stroke(cr);

    if (!st->handle) return FALSE;

    fitToView(st);

    cairo_save(cr);
    cairo_translate(cr, st->panX, st->panY);
    cairo_scale(cr, st->zoom, st->zoom);

    double docW, docH;
    getDocSize(st, &docW, &docH);
    RsvgRectangle viewport{0, 0, docW, docH};
    GError *error = nullptr;
    if (!rsvg_handle_render_document(st->handle, cr, &viewport, &error)) {
        if (error) g_error_free(error);
    }
    cairo_restore(cr);
    return FALSE;
}

gboolean onScroll(GtkWidget *widget, GdkEventScroll *event, gpointer userData)
{
    auto *st = static_cast<DiagramState *>(userData);
    const double scaleFactor = 1.15;
    double factor = (event->direction == GDK_SCROLL_UP) ? scaleFactor
                   : (event->direction == GDK_SCROLL_DOWN) ? 1.0 / scaleFactor : 1.0;
    if (factor == 1.0) return TRUE;

    // Zoom anchored under the cursor.
    double mx = event->x, my = event->y;
    double docX = (mx - st->panX) / st->zoom;
    double docY = (my - st->panY) / st->zoom;
    st->zoom *= factor;
    st->panX = mx - docX * st->zoom;
    st->panY = my - docY * st->zoom;

    gtk_widget_queue_draw(widget);
    return TRUE;
}

gboolean onButtonPress(GtkWidget *, GdkEventButton *event, gpointer userData)
{
    auto *st = static_cast<DiagramState *>(userData);
    if (event->button == GDK_BUTTON_PRIMARY) {
        st->dragging = true;
        st->dragStartX = event->x;
        st->dragStartY = event->y;
        st->panStartX = st->panX;
        st->panStartY = st->panY;
    }
    return TRUE;
}

gboolean onButtonRelease(GtkWidget *, GdkEventButton *event, gpointer userData)
{
    auto *st = static_cast<DiagramState *>(userData);
    if (event->button == GDK_BUTTON_PRIMARY)
        st->dragging = false;
    return TRUE;
}

gboolean onMotion(GtkWidget *widget, GdkEventMotion *event, gpointer userData)
{
    auto *st = static_cast<DiagramState *>(userData);
    if (st->dragging) {
        st->panX = st->panStartX + (event->x - st->dragStartX);
        st->panY = st->panStartY + (event->y - st->dragStartY);
        gtk_widget_queue_draw(widget);
    }
    return TRUE;
}

void onReload(GtkMenuItem *, gpointer userData) { executeRender(static_cast<DiagramState *>(userData)); }

void onSaveSvg(GtkMenuItem *, gpointer userData)
{
    auto *st = static_cast<DiagramState *>(userData);
    if (st->lastSvgData.empty()) return;

    GtkWidget *dlg = gtk_file_chooser_dialog_new("Save as SVG", GTK_WINDOW(gtk_widget_get_toplevel(st->root)),
        GTK_FILE_CHOOSER_ACTION_SAVE, "_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT, nullptr);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), "diagram.svg");
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        GError *error = nullptr;
        if (!g_file_set_contents(path, st->lastSvgData.data(), st->lastSvgData.size(), &error)) {
            showError(st->root, "Error", error ? error->message : "Could not open file for writing.");
            if (error) g_error_free(error);
        }
        g_free(path);
    }
    gtk_widget_destroy(dlg);
}

cairo_surface_t *renderToImageSurface(DiagramState *st)
{
    if (!st->handle) return nullptr;
    double docW, docH;
    getDocSize(st, &docW, &docH);
    int w = static_cast<int>(std::max(1.0, docW));
    int h = static_cast<int>(std::max(1.0, docH));

    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(surface);
    RsvgRectangle viewport{0, 0, static_cast<double>(w), static_cast<double>(h)};
    GError *error = nullptr;
    rsvg_handle_render_document(st->handle, cr, &viewport, &error);
    if (error) g_error_free(error);
    cairo_destroy(cr);
    return surface;
}

void onSavePng(GtkMenuItem *, gpointer userData)
{
    auto *st = static_cast<DiagramState *>(userData);
    if (st->lastSvgData.empty()) return;

    GtkWidget *dlg = gtk_file_chooser_dialog_new("Save as PNG", GTK_WINDOW(gtk_widget_get_toplevel(st->root)),
        GTK_FILE_CHOOSER_ACTION_SAVE, "_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT, nullptr);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), "diagram.png");
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        cairo_surface_t *surface = renderToImageSurface(st);
        if (surface) {
            if (cairo_surface_write_to_png(surface, path) != CAIRO_STATUS_SUCCESS)
                showError(st->root, "Error", "Could not save PNG file.");
            cairo_surface_destroy(surface);
        }
        g_free(path);
    }
    gtk_widget_destroy(dlg);
}

void onCopyToClipboard(GtkMenuItem *, gpointer userData)
{
    auto *st = static_cast<DiagramState *>(userData);
    cairo_surface_t *surface = renderToImageSurface(st);
    if (!surface) return;

    int w = cairo_image_surface_get_width(surface);
    int h = cairo_image_surface_get_height(surface);
    GdkPixbuf *pixbuf = gdk_pixbuf_get_from_surface(surface, 0, 0, w, h);
    cairo_surface_destroy(surface);
    if (!pixbuf) return;

    GtkClipboard *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_image(clipboard, pixbuf);
    g_object_unref(pixbuf);
}

void saveSettingsNow() { g_settings.save(g_configPath, PLUGNAME); }

void onToggleAutoReload(GtkCheckMenuItem *item, gpointer) {
    g_settings.autoReloadEnabled = gtk_check_menu_item_get_active(item);
    saveSettingsNow();
}
void onToggleSystemDark(GtkCheckMenuItem *item, gpointer userData) {
    g_settings.useSystemDarkMode = gtk_check_menu_item_get_active(item);
    saveSettingsNow();
    executeRender(static_cast<DiagramState *>(userData));
}
void onToggleForceDark(GtkCheckMenuItem *item, gpointer userData) {
    g_settings.darkMode = gtk_check_menu_item_get_active(item);
    saveSettingsNow();
    executeRender(static_cast<DiagramState *>(userData));
}
void onSetMermaidLocal(GtkMenuItem *, gpointer userData) {
    g_settings.mermaidRenderer = "local"; saveSettingsNow();
    executeRender(static_cast<DiagramState *>(userData));
}
void onSetMermaidWeb(GtkMenuItem *, gpointer userData) {
    g_settings.mermaidRenderer = "web"; saveSettingsNow();
    executeRender(static_cast<DiagramState *>(userData));
}
void onSetPumlLocal(GtkMenuItem *, gpointer userData) {
    g_settings.renderer = "java"; saveSettingsNow();
    executeRender(static_cast<DiagramState *>(userData));
}
void onSetPumlWeb(GtkMenuItem *, gpointer userData) {
    g_settings.renderer = "web"; saveSettingsNow();
    executeRender(static_cast<DiagramState *>(userData));
}

gboolean onButtonPressForMenu(GtkWidget *widget, GdkEventButton *event, gpointer userData)
{
    auto *st = static_cast<DiagramState *>(userData);
    if (event->button != GDK_BUTTON_SECONDARY)
        return FALSE;

    GtkWidget *menu = gtk_menu_new();
    auto addItem = [&](const char *label, GCallback cb) {
        GtkWidget *item = gtk_menu_item_new_with_label(label);
        g_signal_connect(item, "activate", cb, st);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
        return item;
    };
    auto addCheckItem = [&](const char *label, bool active, bool enabled, GCallback cb) {
        GtkWidget *item = gtk_check_menu_item_new_with_label(label);
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), active);
        gtk_widget_set_sensitive(item, enabled);
        g_signal_connect(item, "toggled", cb, st);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
        return item;
    };

    addItem("Reload Diagram", G_CALLBACK(onReload));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    addItem("Save as SVG...", G_CALLBACK(onSaveSvg));
    addItem("Save as PNG...", G_CALLBACK(onSavePng));
    addItem("Copy Image to Clipboard", G_CALLBACK(onCopyToClipboard));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    addCheckItem("Auto-Reload on Save", g_settings.autoReloadEnabled, true, G_CALLBACK(onToggleAutoReload));
    addCheckItem("Use System Dark Mode", g_settings.useSystemDarkMode, true, G_CALLBACK(onToggleSystemDark));
    addCheckItem("Force Dark Mode", g_settings.darkMode, !g_settings.useSystemDarkMode, G_CALLBACK(onToggleForceDark));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    GtkWidget *mermaidSub = gtk_menu_new();
    GSList *group = nullptr;
    GtkWidget *mLocal = gtk_radio_menu_item_new_with_label(group, "Local (mmdc/npx)");
    group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(mLocal));
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(mLocal), g_settings.mermaidRenderer == "local");
    g_signal_connect(mLocal, "activate", G_CALLBACK(onSetMermaidLocal), st);
    gtk_menu_shell_append(GTK_MENU_SHELL(mermaidSub), mLocal);
    GtkWidget *mWeb = gtk_radio_menu_item_new_with_label(group, "Web (mermaid.ink)");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(mWeb), g_settings.mermaidRenderer == "web");
    g_signal_connect(mWeb, "activate", G_CALLBACK(onSetMermaidWeb), st);
    gtk_menu_shell_append(GTK_MENU_SHELL(mermaidSub), mWeb);
    GtkWidget *mermaidItem = gtk_menu_item_new_with_label("Mermaid Renderer");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(mermaidItem), mermaidSub);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mermaidItem);

    GtkWidget *pumlSub = gtk_menu_new();
    GSList *pgroup = nullptr;
    GtkWidget *pLocal = gtk_radio_menu_item_new_with_label(pgroup, "Local (native/java)");
    pgroup = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(pLocal));
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(pLocal), g_settings.renderer == "java");
    g_signal_connect(pLocal, "activate", G_CALLBACK(onSetPumlLocal), st);
    gtk_menu_shell_append(GTK_MENU_SHELL(pumlSub), pLocal);
    GtkWidget *pWeb = gtk_radio_menu_item_new_with_label(pgroup, "Web (plantuml.com)");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(pWeb), g_settings.renderer == "web");
    g_signal_connect(pWeb, "activate", G_CALLBACK(onSetPumlWeb), st);
    gtk_menu_shell_append(GTK_MENU_SHELL(pumlSub), pWeb);
    GtkWidget *pumlItem = gtk_menu_item_new_with_label("PlantUML Renderer");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(pumlItem), pumlSub);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), pumlItem);

    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
    return TRUE;
}

void destroyState(gpointer data)
{
    auto *st = static_cast<DiagramState *>(data);
    if (st->debounceTimerId) g_source_remove(st->debounceTimerId);
    if (st->monitor) g_object_unref(st->monitor);
    if (st->handle) g_object_unref(st->handle);
    delete st;
}

} // namespace

extern "C" {

HWND DCPCALL ListLoad(HWND ParentWin, char *FileToLoad, int ShowFlags)
{
    std::string path(FileToLoad);
    std::string ext;
    { auto dot = path.find_last_of('.'); if (dot != std::string::npos) ext = path.substr(dot + 1); }
    for (auto &c : ext) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    if (ext != "mmd" && ext != "mermaid" && ext != "puml" && ext != "plantuml")
        return nullptr;

    GtkWidget *parent = GTK_WIDGET(ParentWin);
    auto *st = new DiagramState();
    st->root = gtk_event_box_new();
    st->drawingArea = gtk_drawing_area_new();
    gtk_container_add(GTK_CONTAINER(st->root), st->drawingArea);

    gtk_widget_add_events(st->drawingArea,
        GDK_SCROLL_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
    g_signal_connect(st->drawingArea, "draw", G_CALLBACK(onDraw), st);
    g_signal_connect(st->drawingArea, "scroll-event", G_CALLBACK(onScroll), st);
    g_signal_connect(st->drawingArea, "button-press-event", G_CALLBACK(onButtonPress), st);
    g_signal_connect(st->drawingArea, "button-release-event", G_CALLBACK(onButtonRelease), st);
    g_signal_connect(st->drawingArea, "motion-notify-event", G_CALLBACK(onMotion), st);
    g_signal_connect(st->drawingArea, "button-press-event", G_CALLBACK(onButtonPressForMenu), st);

    gtk_container_add(GTK_CONTAINER(parent), st->root);
    g_object_set_data_full(G_OBJECT(st->root), "diagram-state", st, destroyState);

    gtk_widget_show_all(st->root);
    loadFile(st, path);

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
    auto *st = static_cast<DiagramState *>(g_object_get_data(G_OBJECT(root), "diagram-state"));
    if (!st) return LISTPLUGIN_ERROR;

    if (Command == lc_newparams) {
        executeRender(st);
        return LISTPLUGIN_OK;
    }
    if (Command == lc_copy) {
        onCopyToClipboard(nullptr, st);
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
    snprintf(DetectString, maxlen - 1, "(EXT=\"PUML\" | EXT=\"PLANTUML\" | EXT=\"MMD\" | EXT=\"MERMAID\") & SIZE<30000000");
}

void DCPCALL ListSetDefaultParams(ListDefaultParamStruct *dps)
{
    std::string iniName(dps->DefaultIniName);
    auto slash = iniName.find_last_of('/');
    std::string dir = slash == std::string::npos ? "." : iniName.substr(0, slash);
    g_configPath = dir + "/diagramview.ini";
    g_settings.loadOrInitDefaults(g_configPath, PLUGNAME);
}

} // extern "C"
