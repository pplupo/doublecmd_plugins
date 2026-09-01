#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <string.h>
#include <iostream>
#include "svg_core.h"
#include "wlxplugin.h"

struct PluginData {
    SvgCore core;
    GtkWidget* drawing_area;
    GtkAdjustment* hadj;
    GtkAdjustment* vadj;
    bool dragging;
    double last_x;
    double last_y;
    char filepath[2048];
};

static void update_scrollbars(PluginData* data, int width, int height) {
    double doc_w = data->core.get_intrinsic_width() * data->core.get_zoom();
    double doc_h = data->core.get_intrinsic_height() * data->core.get_zoom();

    gtk_adjustment_set_page_size(data->hadj, width);
    gtk_adjustment_set_upper(data->hadj, std::max((double)width, doc_w));
    gtk_adjustment_set_value(data->hadj, data->core.get_pan_x());

    gtk_adjustment_set_page_size(data->vadj, height);
    gtk_adjustment_set_upper(data->vadj, std::max((double)height, doc_h));
    gtk_adjustment_set_value(data->vadj, data->core.get_pan_y());
}

static void on_adjustment_value_changed(GtkAdjustment* adj, PluginData* data) {
    data->core.set_pan(gtk_adjustment_get_value(data->hadj), gtk_adjustment_get_value(data->vadj));
    gtk_widget_queue_draw(data->drawing_area);
}

static void save_png(PluginData* data) {
    GtkWidget* dialog = gtk_file_chooser_dialog_new("Save PNG",
        GTK_WINDOW(gtk_widget_get_toplevel(data->drawing_area)),
        GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Save", GTK_RESPONSE_ACCEPT,
        NULL);
    
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), "export.png");

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char* filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        data->core.save_png(filename);
        g_free(filename);
    }
    gtk_widget_destroy(dialog);
}

static void show_context_menu(PluginData* data, GdkEventButton* event) {
    GtkWidget* menu = gtk_menu_new();
    GtkWidget* item = gtk_menu_item_new_with_label("Save as PNG...");
    
    g_signal_connect_swapped(item, "activate", G_CALLBACK(save_png), data);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    gtk_widget_show_all(menu);
    
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent*)event);
}

static gboolean on_draw(GtkWidget* widget, cairo_t* cr, PluginData* data) {
    int width = gtk_widget_get_allocated_width(widget);
    int height = gtk_widget_get_allocated_height(widget);

    data->core.render(width, height);
    
    const unsigned char* pixels = data->core.get_image_data();
    int stride = data->core.get_image_stride();
    
    if (pixels) {
        cairo_surface_t* surface = cairo_image_surface_create_for_data(
            (unsigned char*)pixels, CAIRO_FORMAT_ARGB32, width, height, stride);
        cairo_set_source_surface(cr, surface, 0, 0);
        cairo_paint(cr);
        cairo_surface_destroy(surface);
    }
    return FALSE;
}

static gboolean on_scroll(GtkWidget* widget, GdkEventScroll* event, PluginData* data) {
    if (event->state & GDK_CONTROL_MASK) {
        double factor = (event->direction == GDK_SCROLL_UP) ? 1.1 : 0.9;
        data->core.zoom_delta(factor, event->x, event->y);
        update_scrollbars(data, gtk_widget_get_allocated_width(widget), gtk_widget_get_allocated_height(widget));
        gtk_widget_queue_draw(widget);
        return TRUE;
    }
    return FALSE;
}

static gboolean on_key_press(GtkWidget* widget, GdkEventKey* event, PluginData* data) {
    bool is_ctrl = (event->state & GDK_CONTROL_MASK);
    if (is_ctrl && (event->keyval == GDK_KEY_s || event->keyval == GDK_KEY_S)) {
        save_png(data);
        return TRUE;
    }
    
    if (event->keyval == GDK_KEY_plus || event->keyval == GDK_KEY_KP_Add || event->keyval == GDK_KEY_equal) {
        int w = gtk_widget_get_allocated_width(widget);
        int h = gtk_widget_get_allocated_height(widget);
        data->core.zoom_delta(1.1, w/2.0, h/2.0);
        update_scrollbars(data, w, h);
        gtk_widget_queue_draw(widget);
        return TRUE;
    }
    
    if (event->keyval == GDK_KEY_minus || event->keyval == GDK_KEY_KP_Subtract) {
        int w = gtk_widget_get_allocated_width(widget);
        int h = gtk_widget_get_allocated_height(widget);
        data->core.zoom_delta(0.9, w/2.0, h/2.0);
        update_scrollbars(data, w, h);
        gtk_widget_queue_draw(widget);
        return TRUE;
    }
    
    return FALSE;
}

static gboolean on_button_press(GtkWidget* widget, GdkEventButton* event, PluginData* data) {
    if (event->button == 1) { // Left click
        data->dragging = true;
        data->last_x = event->x;
        data->last_y = event->y;
        return TRUE;
    } else if (event->button == 3) { // Right click
        show_context_menu(data, event);
        return TRUE;
    }
    return FALSE;
}

static gboolean on_button_release(GtkWidget* widget, GdkEventButton* event, PluginData* data) {
    if (event->button == 1) {
        data->dragging = false;
        return TRUE;
    }
    return FALSE;
}

static gboolean on_motion_notify(GtkWidget* widget, GdkEventMotion* event, PluginData* data) {
    if (data->dragging) {
        double dx = event->x - data->last_x;
        double dy = event->y - data->last_y;
        data->last_x = event->x;
        data->last_y = event->y;
        
        data->core.set_pan(data->core.get_pan_x() - dx, data->core.get_pan_y() - dy);
        
        // Update scrollbars silently
        g_signal_handlers_block_by_func(data->hadj, (gpointer)on_adjustment_value_changed, data);
        g_signal_handlers_block_by_func(data->vadj, (gpointer)on_adjustment_value_changed, data);
        gtk_adjustment_set_value(data->hadj, data->core.get_pan_x());
        gtk_adjustment_set_value(data->vadj, data->core.get_pan_y());
        g_signal_handlers_unblock_by_func(data->hadj, (gpointer)on_adjustment_value_changed, data);
        g_signal_handlers_unblock_by_func(data->vadj, (gpointer)on_adjustment_value_changed, data);
        
        gtk_widget_queue_draw(widget);
        return TRUE;
    }
    return FALSE;
}

static void on_size_allocate(GtkWidget* widget, GtkAllocation* allocation, PluginData* data) {
    update_scrollbars(data, allocation->width, allocation->height);
}

extern "C" HANDLE DCPCALL ListLoad(HANDLE ParentWin, char* FileToLoad, int ShowFlags) {
    PluginData* data = new PluginData();
    if (!data->core.load(FileToLoad)) {
        delete data;
        return nullptr;
    }
    strncpy(data->filepath, FileToLoad, sizeof(data->filepath) - 1);
    data->dragging = false;

    GtkWidget* grid = gtk_grid_new();
    
    data->drawing_area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(data->drawing_area, TRUE);
    gtk_widget_set_vexpand(data->drawing_area, TRUE);
    gtk_widget_set_can_focus(data->drawing_area, TRUE);
    
    gtk_widget_add_events(data->drawing_area, 
        GDK_SCROLL_MASK | 
        GDK_BUTTON_PRESS_MASK | 
        GDK_BUTTON_RELEASE_MASK | 
        GDK_POINTER_MOTION_MASK | 
        GDK_KEY_PRESS_MASK
    );

    data->hadj = gtk_adjustment_new(0, 0, 100, 10, 100, 100);
    data->vadj = gtk_adjustment_new(0, 0, 100, 10, 100, 100);
    
    GtkWidget* hscrollbar = gtk_scrollbar_new(GTK_ORIENTATION_HORIZONTAL, data->hadj);
    GtkWidget* vscrollbar = gtk_scrollbar_new(GTK_ORIENTATION_VERTICAL, data->vadj);

    gtk_grid_attach(GTK_GRID(grid), data->drawing_area, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), vscrollbar, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), hscrollbar, 0, 1, 1, 1);

    g_signal_connect(data->drawing_area, "draw", G_CALLBACK(on_draw), data);
    g_signal_connect(data->drawing_area, "scroll-event", G_CALLBACK(on_scroll), data);
    g_signal_connect(data->drawing_area, "key-press-event", G_CALLBACK(on_key_press), data);
    g_signal_connect(data->drawing_area, "button-press-event", G_CALLBACK(on_button_press), data);
    g_signal_connect(data->drawing_area, "button-release-event", G_CALLBACK(on_button_release), data);
    g_signal_connect(data->drawing_area, "motion-notify-event", G_CALLBACK(on_motion_notify), data);
    g_signal_connect(data->drawing_area, "size-allocate", G_CALLBACK(on_size_allocate), data);

    g_signal_connect(data->hadj, "value-changed", G_CALLBACK(on_adjustment_value_changed), data);
    g_signal_connect(data->vadj, "value-changed", G_CALLBACK(on_adjustment_value_changed), data);

    gtk_container_add(GTK_CONTAINER(GTK_WIDGET(ParentWin)), grid);
    gtk_widget_show_all(grid);
    gtk_widget_grab_focus(data->drawing_area);

    g_object_set_data(G_OBJECT(grid), "plugin_data", data);

    return (HANDLE)grid;
}

extern "C" void DCPCALL ListCloseWindow(HANDLE ListWin) {
    GtkWidget* grid = GTK_WIDGET(ListWin);
    PluginData* data = (PluginData*)g_object_get_data(G_OBJECT(grid), "plugin_data");
    if (data) {
        delete data;
    }
    gtk_widget_destroy(grid);
}

extern "C" int DCPCALL ListSearchText(HWND ListWin, char* SearchString, int SearchParameter) {
    return LISTPLUGIN_ERROR;
}

extern "C" int DCPCALL ListSendCommand(HWND ListWin, int Command, int Parameter) {
    return LISTPLUGIN_ERROR;
}

extern "C" void DCPCALL ListSetDefaultParams(ListDefaultParamStruct* dps) {
}

extern "C" void DCPCALL ListGetDetectString(char* DetectString, int maxlen) {
    strncpy(DetectString, "EXT=\"SVG\"", maxlen);
}
