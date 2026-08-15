#ifdef BUILD_GTK_TARGET

#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <string.h>

#include "wlxplugin.h"
#include "../core/markdown_engine.h"

extern "C" {

HWND DCPCALL ListLoad(HWND ParentWin, char* FileToLoad, int ShowFlags)
{
    GtkWidget *parent = GTK_WIDGET(ParentWin);
    GtkWidget *scrolledWin = gtk_scrolled_window_new(NULL, NULL);
    // GTK_CONTAINER(parent)->add() doesn't register the child the way
    // GtkLayout expects: DC's own ResizeWindow (uwlxmodule.pas) later calls
    // gtk_layout_move() on this widget, which asserts the widget's parent
    // is exactly this GtkLayout -- only gtk_layout_put() sets that up.
    gtk_layout_put(GTK_LAYOUT(parent), scrolledWin, 0, 0);

    GtkWidget *webView = webkit_web_view_new();
    gtk_widget_set_name(webView, "markdown_webview");

    std::string html = MarkdownEngine::renderFileToHtml(FileToLoad, false);
    webkit_web_view_load_html(WEBKIT_WEB_VIEW(webView), html.c_str(), NULL);

    gtk_container_add(GTK_CONTAINER(scrolledWin), webView);
    gtk_widget_show_all(scrolledWin);

    return (HWND)scrolledWin;
}

void DCPCALL ListCloseWindow(HWND ListWin)
{
    if (ListWin) {
        gtk_widget_destroy(GTK_WIDGET(ListWin));
    }
}

void DCPCALL ListGetDetectString(char* DetectString, int maxlen)
{
    snprintf(DetectString, maxlen - 1, "(EXT=\"MD\" | EXT=\"MARKDOWN\" | EXT=\"MDOWN\" | EXT=\"MKD\") & SIZE<30000000");
}

int DCPCALL ListSendCommand(HWND ListWin, int Command, int Parameter)
{
    if (!ListWin) return LISTPLUGIN_ERROR;

    switch (Command) {
    case lc_copy:
        // Copy in WebKit
        return LISTPLUGIN_OK;
    default:
        return LISTPLUGIN_ERROR;
    }
}

} // extern "C"

#endif // BUILD_GTK_TARGET
