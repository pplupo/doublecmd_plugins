#include "viewer_widget.h"
#include <cstring>

#define DCPCALL
typedef void* HWND;

#define lc_focus 5

extern "C" {

HWND DCPCALL ListLoad(HWND ParentWin, char* FileToLoad, int ShowFlags) {
    GtkWidget* parent = reinterpret_cast<GtkWidget*>(ParentWin);
    GtkViewerWidget* viewer = new GtkViewerWidget(parent);
    
    if (viewer->loadFile(FileToLoad)) {
        return reinterpret_cast<HWND>(viewer);
    }
    
    delete viewer;
    return nullptr;
}

void DCPCALL ListCloseWindow(HWND ListWin) {
    GtkViewerWidget* viewer = reinterpret_cast<GtkViewerWidget*>(ListWin);
    if (viewer) {
        GtkWidget* widget = viewer->getWidget();
        if (widget) {
            gtk_widget_destroy(widget);
        }
        delete viewer;
    }
}

int DCPCALL ListSearchText(HWND ListWin, char* SearchString, int SearchParameter) {
    return 0;
}

int DCPCALL ListSendCommand(HWND ListWin, int Command, int Parameter) {
    GtkViewerWidget* viewer = reinterpret_cast<GtkViewerWidget*>(ListWin);
    if (!viewer) return 1;

    if (Command == lc_focus) {
        if (Parameter) {
            viewer->grabFocus();
        } else {
            viewer->clearFocus();
        }
        return 0;
    }

    return 0;
}

void DCPCALL ListGetDetectString(char* DetectString, int maxlen) {
    strncpy(DetectString, "EXT=\"PDF\" | EXT=\"EPUB\" | EXT=\"MOBI\" | EXT=\"FB2\" | EXT=\"XPS\" | EXT=\"CBZ\" | EXT=\"DJVU\" | EXT=\"DJV\"", maxlen);
}

}
