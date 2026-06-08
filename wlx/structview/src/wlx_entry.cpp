#include <QApplication>
#include <QClipboard>
#include <QWidget>
#include <QTableView>

#include <wayland_qt_base/FocusManager.h>
#include <wayland_qt_base/EditableGridWidget.h>

#include "wlxplugin.h"
#include "StructViewWidget.h"

// ---------------------------------------------------------------------------
// WLX Plugin Entry Points
// ---------------------------------------------------------------------------

HWND DCPCALL ListLoad(HWND ParentWin, char* FileToLoad, int ShowFlags)
{
    Q_UNUSED(ShowFlags);
    if (!QApplication::instance())
        return nullptr;

    auto *widget = new StructViewWidget(reinterpret_cast<QWidget*>(ParentWin));
    if (!widget->loadFile(QString::fromUtf8(FileToLoad))) {
        delete widget;
        return nullptr;
    }

    widget->show();
    return reinterpret_cast<HWND>(widget);
}

void DCPCALL ListCloseWindow(HWND ListWin)
{
    auto *widget = reinterpret_cast<StructViewWidget*>(ListWin);
    delete widget;
}

int DCPCALL ListSendCommand(HWND ListWin, int Command, int Parameter)
{
    auto *widget = reinterpret_cast<StructViewWidget*>(ListWin);

    switch (Command) {
    case lc_copy: {
        QString text = widget->getSelectionAsText('\t');
        if (text.isEmpty()) return LISTPLUGIN_ERROR;
        QApplication::clipboard()->setText(text);
        break;
    }
    case lc_selectall:
        if (widget->grid() && widget->grid()->view())
            widget->grid()->view()->selectAll();
        break;
    case lc_focus:
        if (Parameter) {
            widget->focusManager()->setActive(true);
            if (widget->grid() && widget->grid()->view())
                widget->grid()->view()->setFocus(Qt::OtherFocusReason);
        } else {
            widget->focusManager()->setActive(false);
            if (QWidget *fw = QApplication::focusWidget()) {
                if (fw == widget || widget->isAncestorOf(fw))
                    fw->clearFocus();
            }
        }
        break;
    default:
        return LISTPLUGIN_ERROR;
    }
    return LISTPLUGIN_OK;
}

int DCPCALL ListSearchText(HWND ListWin, char* SearchString, int SearchParameter)
{
    auto *widget = reinterpret_cast<StructViewWidget*>(ListWin);
    if (!widget->grid() || !widget->grid()->view() || !widget->grid()->view()->model())
        return LISTPLUGIN_ERROR;

    QAbstractItemModel *model = widget->grid()->view()->model();
    Qt::MatchFlags flags = Qt::MatchContains;
    if (SearchParameter & lcs_matchcase) flags |= Qt::MatchCaseSensitive;

    QString needle = QString::fromUtf8(SearchString);
    QTableView *view = widget->grid()->view();

    // Use stored search state via properties
    QString prev = view->property("needle").value<QString>();
    view->setProperty("needle", needle);

    // Determine start position
    QModelIndex current = view->currentIndex();
    int startRow = 0, startCol = 0;
    if (current.isValid()) {
        startRow = current.row();
        startCol = current.column();
    }

    bool first = (needle != prev) || (SearchParameter & lcs_findfirst);
    bool backward = (SearchParameter & lcs_backwards);

    if (!first) {
        if (backward) {
            startCol--;
            if (startCol < 0) { startCol = model->columnCount() - 1; startRow--; }
        } else {
            startCol++;
            if (startCol >= model->columnCount()) { startCol = 0; startRow++; }
        }
    }

    int rows = model->rowCount();
    int cols = model->columnCount();
    int total = rows * cols;

    for (int i = 0; i < total; ++i) {
        int offset = backward ? -i : i;
        int linearStart = startRow * cols + startCol;
        int linearIdx = (linearStart + offset + total) % total;
        int r = linearIdx / cols;
        int c = linearIdx % cols;

        QModelIndex idx = model->index(r, c);
        QString cellText = model->data(idx, Qt::DisplayRole).toString();

        bool match = (SearchParameter & lcs_matchcase)
            ? cellText.contains(needle, Qt::CaseSensitive)
            : cellText.contains(needle, Qt::CaseInsensitive);

        if (match) {
            view->setCurrentIndex(idx);
            view->scrollTo(idx);
            return LISTPLUGIN_OK;
        }
    }

    return LISTPLUGIN_ERROR;
}

void DCPCALL ListGetDetectString(char* DetectString, int maxlen)
{
    snprintf(DetectString, maxlen - 1,
        "EXT=\"JSON\" | EXT=\"XML\" | EXT=\"INI\" | EXT=\"CBOR\"");
}
