#include <QApplication>
#include <QClipboard>
#include <QWidget>
#include <QTableView>
#include <QAbstractItemModel>

#include <wlxbase_wlqt/FocusManager.h>
#include <wlxbase_wlqt/EditableGridWidget.h>
#include <wlxbase_wlqt/CrashLogger.h>

#include "wlxplugin.h"
#include "DbViewWidget.h"

// ---------------------------------------------------------------------------
// WLX Plugin Entry Points — all wrapped with WLX_TRY/WLX_CATCH
// ---------------------------------------------------------------------------

HWND DCPCALL ListLoad(HWND ParentWin, char* FileToLoad, int ShowFlags)
{
    WLX_TRY {
        Q_UNUSED(ShowFlags);
        if (!QApplication::instance())
            return nullptr;

        auto *widget = new DbViewWidget(reinterpret_cast<QWidget*>(ParentWin));
        if (!widget->loadFile(QString::fromUtf8(FileToLoad))) {
            delete widget;
            return nullptr;
        }

        widget->show();
        return reinterpret_cast<HWND>(widget);
    } WLX_CATCH("ListLoad");
    return nullptr;
}

void DCPCALL ListCloseWindow(HWND ListWin)
{
    WLX_TRY {
        auto *widget = reinterpret_cast<DbViewWidget*>(ListWin);
        delete widget;
    } WLX_CATCH("ListCloseWindow");
}

int DCPCALL ListSendCommand(HWND ListWin, int Command, int Parameter)
{
    WLX_TRY {
        auto *widget = reinterpret_cast<DbViewWidget*>(ListWin);

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
    } WLX_CATCH("ListSendCommand");
    return LISTPLUGIN_ERROR;
}

int DCPCALL ListSearchText(HWND ListWin, char* SearchString, int SearchParameter)
{
    WLX_TRY {
        auto *widget = reinterpret_cast<DbViewWidget*>(ListWin);
        if (!widget->grid() || !widget->grid()->view() || !widget->grid()->view()->model())
            return LISTPLUGIN_ERROR;

        QAbstractItemModel *model = widget->grid()->view()->model();
        QString needle = QString::fromUtf8(SearchString);
        QTableView *view = widget->grid()->view();

        // Track search state
        QString prev = view->property("needle").value<QString>();
        view->setProperty("needle", needle);

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
    } WLX_CATCH("ListSearchText");
    return LISTPLUGIN_ERROR;
}

void DCPCALL ListGetDetectString(char* DetectString, int maxlen)
{
#ifdef ENABLE_ROCKSDB_LEVELDB
    snprintf(DetectString, maxlen - 1,
        "EXT=\"DB\" | EXT=\"SQLITE\" | EXT=\"SQLITE3\" | EXT=\"DB3\" | "
        "EXT=\"DUCKDB\" | EXT=\"LDB\" | EXT=\"SST\" | EXT=\"LMDB\" | "
        "EXT=\"BDB\" | EXT=\"FDB\" | EXT=\"MDB\" | EXT=\"ACCDB\" | "
        "EXT=\"PARQUET\" | EXT=\"PQ\"");
#else
    snprintf(DetectString, maxlen - 1,
        "EXT=\"DB\" | EXT=\"SQLITE\" | EXT=\"SQLITE3\" | EXT=\"DB3\" | "
        "EXT=\"DUCKDB\" | EXT=\"LMDB\" | "
        "EXT=\"BDB\" | EXT=\"FDB\" | EXT=\"MDB\" | EXT=\"ACCDB\" | "
        "EXT=\"PARQUET\" | EXT=\"PQ\"");
#endif
}
