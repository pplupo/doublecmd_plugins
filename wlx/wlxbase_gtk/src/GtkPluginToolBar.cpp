#include "wlxbase_gtk/GtkPluginToolBar.h"
#include "wlxbase_gtk/GtkFocusManager.h"

namespace GtkWlPlugin {

GtkPluginToolBar::GtkPluginToolBar(GtkFocusManager *fm)
    : m_fm(fm)
{
    m_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_container_set_border_width(GTK_CONTAINER(m_box), 2);
}

GtkWidget *GtkPluginToolBar::addToolAction(const std::string &label, const std::string &iconName,
                                            std::function<void()> onClicked)
{
    GtkWidget *btn;
    if (!iconName.empty()) {
        btn = gtk_button_new_from_icon_name(iconName.c_str(), GTK_ICON_SIZE_SMALL_TOOLBAR);
        gtk_widget_set_tooltip_text(btn, label.c_str());
    } else {
        btn = gtk_button_new_with_label(label.c_str());
    }
    gtk_widget_set_can_focus(btn, FALSE);
    gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);

    auto *cb = new std::function<void()>(std::move(onClicked));
    g_signal_connect_data(btn, "clicked", G_CALLBACK(+[](GtkButton *, gpointer data) {
        (*static_cast<std::function<void()> *>(data))();
    }), cb, +[](gpointer data, GClosure *) { delete static_cast<std::function<void()> *>(data); }, G_CONNECT_AFTER);

    gtk_box_pack_start(GTK_BOX(m_box), btn, FALSE, FALSE, 0);
    gtk_widget_show(btn);
    return btn;
}

GtkWidget *GtkPluginToolBar::addToggleAction(const std::string &label, const std::string &iconName,
                                              bool initialState, std::function<void(bool)> onToggled)
{
    GtkWidget *btn = gtk_toggle_button_new();
    if (!iconName.empty()) {
        GtkWidget *img = gtk_image_new_from_icon_name(iconName.c_str(), GTK_ICON_SIZE_SMALL_TOOLBAR);
        gtk_container_add(GTK_CONTAINER(btn), img);
        gtk_widget_set_tooltip_text(btn, label.c_str());
    } else {
        gtk_button_set_label(GTK_BUTTON(btn), label.c_str());
    }
    gtk_widget_set_can_focus(btn, FALSE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn), initialState);

    auto *cb = new std::function<void(bool)>(std::move(onToggled));
    g_signal_connect_data(btn, "toggled", G_CALLBACK(+[](GtkToggleButton *tb, gpointer data) {
        (*static_cast<std::function<void(bool)> *>(data))(gtk_toggle_button_get_active(tb));
    }), cb, +[](gpointer data, GClosure *) { delete static_cast<std::function<void(bool)> *>(data); }, G_CONNECT_AFTER);

    gtk_box_pack_start(GTK_BOX(m_box), btn, FALSE, FALSE, 0);
    gtk_widget_show_all(btn);
    return btn;
}

void GtkPluginToolBar::addSeparator()
{
    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_box_pack_start(GTK_BOX(m_box), sep, FALSE, FALSE, 4);
    gtk_widget_show(sep);
}

} // namespace GtkWlPlugin
