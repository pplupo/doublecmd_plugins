// GTK3 WLX entry points for cuda_gtk3 -- the GTK analog of kate_qt6
// (KTextEditor-based rich code viewer), here built on GtkSourceView 4.

#include <gtk/gtk.h>
#include <memory>

#include "wlxplugin.h"
#include "EditorWidget.h"

namespace {

void destroyState(gpointer data)
{
    delete static_cast<EditorWidget *>(data);
}

} // namespace

extern "C" {

HWND DCPCALL ListLoad(HWND ParentWin, char *FileToLoad, int ShowFlags)
{
    GtkWidget *parent = GTK_WIDGET(ParentWin);
    auto *editor = new EditorWidget();

    // gtk_container_add() doesn't register the child the way GtkLayout
    // expects: DC's ResizeWindow later calls gtk_layout_move() on this
    // widget, which asserts the parent is exactly this GtkLayout -- only
    // gtk_layout_put() sets that up. (See the identical fix applied to
    // every other GTK3 plugin's ListLoad() this session.)
    gtk_layout_put(GTK_LAYOUT(parent), editor->rootWidget(), 0, 0);

    g_object_set_data_full(G_OBJECT(editor->rootWidget()), "cuda-editor", editor, destroyState);

    editor->loadFile(std::string(FileToLoad));
    gtk_widget_show_all(editor->rootWidget());

    return reinterpret_cast<HWND>(editor->rootWidget());
}

void DCPCALL ListCloseWindow(HWND ListWin)
{
    GtkWidget *root = GTK_WIDGET(ListWin);
    if (root) gtk_widget_destroy(root);
}

int DCPCALL ListSendCommand(HWND ListWin, int Command, int Parameter)
{
    GtkWidget *root = GTK_WIDGET(ListWin);
    auto *editor = static_cast<EditorWidget *>(g_object_get_data(G_OBJECT(root), "cuda-editor"));
    if (!editor) return LISTPLUGIN_ERROR;

    if (Command == lc_newparams) {
        editor->reload();
        return LISTPLUGIN_OK;
    }
    if (Command == lc_copy) {
        GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(editor->sourceView()));
        gtk_text_buffer_copy_clipboard(buf, gtk_clipboard_get(GDK_SELECTION_CLIPBOARD));
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
    // Broad static fallback covering the same "common code/text file"
    // territory as kate_qt6's own fallback list. A follow-up could
    // enumerate GtkSourceLanguageManager's actual registered globs
    // instead, matching how kate_qt6 enumerates KTextEditor's MIME
    // types -- not done in this first pass.
    snprintf(DetectString, maxlen - 1,
        "EXT=\"TXT\" | EXT=\"PAS\" | EXT=\"C\" | EXT=\"CPP\" | EXT=\"H\" | EXT=\"HPP\" | "
        "EXT=\"PY\" | EXT=\"JS\" | EXT=\"TS\" | EXT=\"HTML\" | EXT=\"CSS\" | EXT=\"JSON\" | "
        "EXT=\"XML\" | EXT=\"YAML\" | EXT=\"YML\" | EXT=\"TOML\" | EXT=\"INI\" | EXT=\"MD\" | "
        "EXT=\"SH\" | EXT=\"RS\" | EXT=\"GO\" | EXT=\"JAVA\" | EXT=\"RB\" | EXT=\"PHP\" | "
        "EXT=\"SQL\" | EXT=\"LOG\" | EXT=\"CONF\" | EXT=\"CFG\"");
}

void DCPCALL ListSetDefaultParams(ListDefaultParamStruct *)
{
}

} // extern "C"
