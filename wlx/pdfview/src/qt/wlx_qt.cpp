#include "viewer_widget.h"
#include <QString>
#include <QObject>
#include <QApplication>
#include <cstring>

// WLX API Definitions
#define DCPCALL
typedef void* HWND;

#define lc_focus 5

extern "C" {

HWND DCPCALL ListLoad(HWND ParentWin, char* FileToLoad, int ShowFlags) {
    QWidget* parent = reinterpret_cast<QWidget*>(ParentWin);
    ViewerWidget* viewer = new ViewerWidget(parent);
    
    if (viewer->loadFile(QString::fromLocal8Bit(FileToLoad))) {
        viewer->show();
        return reinterpret_cast<HWND>(viewer);
    }
    
    delete viewer;
    return nullptr;
}

void DCPCALL ListCloseWindow(HWND ListWin) {
    ViewerWidget* viewer = reinterpret_cast<ViewerWidget*>(ListWin);
    if (viewer) {
        viewer->setParent(nullptr);
        delete viewer;
    }
}

int DCPCALL ListSearchText(HWND ListWin, char* SearchString, int SearchParameter) {
    // Stub
    return 0;
}

int DCPCALL ListSendCommand(HWND ListWin, int Command, int Parameter) {
    ViewerWidget* viewer = reinterpret_cast<ViewerWidget*>(ListWin);
    if (!viewer) return 1;

    if (Command == lc_focus) {
        if (Parameter) {
            viewer->setFocus(Qt::OtherFocusReason);
        } else if (QWidget* fw = QApplication::focusWidget()) {
            if (fw == viewer || viewer->isAncestorOf(fw)) fw->clearFocus();
        }
        return 0;
    }

    // Stub for everything else
    return 0;
}

void DCPCALL ListGetDetectString(char* DetectString, int maxlen) {
    strncpy(DetectString, "EXT=\"PDF\" | EXT=\"EPUB\" | EXT=\"MOBI\" | EXT=\"FB2\" | EXT=\"XPS\" | EXT=\"CBZ\" | EXT=\"DJVU\" | EXT=\"DJV\"", maxlen);
}

}
