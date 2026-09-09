#include <QApplication>
#include <QClipboard>
#include <QHash>
#include <QSet>
#include <QTreeView>

#include "ArchiveTreeView.h"
#include <QWidget>

#include <wlxbase_wlqt/CrashLogger.h>
#include <wlxbase_wlqt/FocusManager.h>

#include "wlxplugin.h"

#include "ArchiveModel.h"
#include "ArchiveViewWidget.h"

// The build compiles with -fvisibility=hidden; DCPCALL is empty on Linux and
// carries no visibility attribute of its own, so the exports Double Commander
// looks up must be marked explicitly. Everything else — Qt symbols, the
// statically linked base library, libarchive — stays internal and cannot be
// interposed by, or collide with, whatever the host process already loaded.
#define WLX_EXPORT extern "C" __attribute__((visibility("default")))

// ---------------------------------------------------------------------------
// Widget instance tracking — DC destroys and recreates the plugin window
// during its file-change reload cycle. Keeping the widget alive across that
// avoids restarting the scan and losing focus. Same approach as dbview.
// ---------------------------------------------------------------------------

static QHash<void *, ArchiveViewWidget *> g_instances;

// Path DC gives us for our settings file. Empty until ListSetDefaultParams
// is called, which DC does once at load time, before any ListLoad.
static QString g_iniPath;

const QString &archiveviewIniPath()
{
    return g_iniPath;
}

// Parents already wired to clean up their widget. Without this, switching
// files through ListLoad (rather than ListLoadNext) would add another
// destroyed() connection to the same parent on every call — harmless in
// effect, since the second take() returns nullptr, but an accumulating leak
// of connections for as long as the panel lives.
static QSet<void *> g_trackedParents;

static void trackParentLifetime(QWidget *parent, void *key)
{
    if (g_trackedParents.contains(key))
        return;
    g_trackedParents.insert(key);

    QObject::connect(parent, &QObject::destroyed, parent, [key]() {
        g_trackedParents.remove(key);
        ArchiveViewWidget *widget = g_instances.take(key);
        delete widget;
    });
}

static HWND loadInto(HWND ParentWin, const QString &path, int showFlags)
{
    if (!QApplication::instance())
        return nullptr;

    auto *parentWidget = reinterpret_cast<QWidget *>(ParentWin);

    ArchiveViewWidget *widget = g_instances.value(ParentWin, nullptr);
    if (widget && widget->parentWidget() == parentWidget) {
        if (widget->currentFilePath() == path) {
            widget->show();
            return reinterpret_cast<HWND>(widget);
        }
        g_instances.remove(ParentWin);
        // deleteLater, not delete: a passphrase prompt runs a nested event
        // loop, so this call can be reached from inside the old widget's own
        // slot. Deferring the deletion to the next trip through the event
        // loop keeps that from freeing a widget whose frame is still live.
        // hide() first, because until that deferred deletion runs the old
        // widget would otherwise stay visible on top of the new one.
        widget->stopScan();
        widget->hide();
        widget->deleteLater();
        widget = nullptr;
    }

    widget = new ArchiveViewWidget(parentWidget);
    if (!widget->loadFile(path)) {
        delete widget;
        return nullptr;
    }
    widget->applyShowFlags(showFlags);

    g_instances.insert(ParentWin, widget);
    trackParentLifetime(parentWidget, ParentWin);
    widget->show();
    return reinterpret_cast<HWND>(widget);
}

WLX_EXPORT HWND DCPCALL ListLoad(HWND ParentWin, char *FileToLoad, int ShowFlags)
{
    WLX_TRY {
        return loadInto(ParentWin, QString::fromUtf8(FileToLoad), ShowFlags);
    } WLX_CATCH("ListLoad");
    return nullptr;
}

WLX_EXPORT HWND DCPCALL ListLoadW(HWND ParentWin, WCHAR *FileToLoad, int ShowFlags)
{
    WLX_TRY {
        // WCHAR is uint16_t here, so this is a UTF-16 string — decoding it as
        // such is the whole point of implementing the W variant: paths that
        // are not representable in the current locale survive intact.
        return loadInto(ParentWin, QString::fromUtf16(
            reinterpret_cast<const char16_t *>(FileToLoad)), ShowFlags);
    } WLX_CATCH("ListLoadW");
    return nullptr;
}

// ---------------------------------------------------------------------------
// ListLoadNext — the reason arrowing through a directory of archives no
// longer destroys and recreates the entire widget tree per file. The view,
// header layout, column widths and find panel are all kept; only the model
// contents are swapped.
// ---------------------------------------------------------------------------

static int loadNextInto(HWND ParentWin, HWND PluginWin, const QString &path,
                        int ShowFlags)
{
    auto *widget = reinterpret_cast<ArchiveViewWidget *>(PluginWin);
    if (!widget)
        return LISTPLUGIN_ERROR;

    // Only accept the handle DC was given for this parent; anything else and
    // we would be reinterpret_cast-ing a pointer we do not own.
    if (g_instances.value(ParentWin, nullptr) != widget)
        return LISTPLUGIN_ERROR;

    if (!widget->loadFile(path))
        return LISTPLUGIN_ERROR;   // not an archive — DC falls back

    widget->applyShowFlags(ShowFlags);
    widget->show();
    return LISTPLUGIN_OK;
}

WLX_EXPORT int DCPCALL ListLoadNext(HWND ParentWin, HWND PluginWin,
                                    char *FileToLoad, int ShowFlags)
{
    WLX_TRY {
        return loadNextInto(ParentWin, PluginWin, QString::fromUtf8(FileToLoad),
                            ShowFlags);
    } WLX_CATCH("ListLoadNext");
    return LISTPLUGIN_ERROR;
}

WLX_EXPORT int DCPCALL ListLoadNextW(HWND ParentWin, HWND PluginWin,
                                     WCHAR *FileToLoad, int ShowFlags)
{
    WLX_TRY {
        return loadNextInto(ParentWin, PluginWin,
                            QString::fromUtf16(
                                reinterpret_cast<const char16_t *>(FileToLoad)),
                            ShowFlags);
    } WLX_CATCH("ListLoadNextW");
    return LISTPLUGIN_ERROR;
}

WLX_EXPORT void DCPCALL ListCloseWindow(HWND ListWin)
{
    WLX_TRY {
        // DC calls this during its reload cycle too, so hide rather than
        // delete; the widget dies with its parent container. The scan is
        // stopped either way, because a hidden view has nothing to populate.
        auto *widget = reinterpret_cast<ArchiveViewWidget *>(ListWin);
        if (!widget)
            return;
        widget->stopScan();
        widget->hide();
    } WLX_CATCH("ListCloseWindow");
}

WLX_EXPORT void DCPCALL ListSetDefaultParams(ListDefaultParamStruct *dps)
{
    WLX_TRY {
        if (!dps || dps->size < int(sizeof(ListDefaultParamStruct)))
            return;
        // DC hands over the .ini path it expects plugins to use, following
        // the same idiom as the other Qt plugins in this repository.
        g_iniPath = QString::fromLocal8Bit(dps->DefaultIniName);
    } WLX_CATCH("ListSetDefaultParams");
}

WLX_EXPORT void DCPCALL ListGetDetectString(char *DetectString, int maxlen)
{
    // Without this DC invokes the plugin for every previewed file and the
    // plugin has to probe with libarchive before bailing out. Enumerating the
    // formats we actually handle keeps the lister out of the way of every
    // other viewer.
    snprintf(DetectString, maxlen - 1,
        "EXT=\"ZIP\" | EXT=\"ZIPX\" | EXT=\"JAR\" | EXT=\"WAR\" | EXT=\"APK\" | "
        "EXT=\"EPUB\" | EXT=\"XPI\" | EXT=\"WHL\" | EXT=\"7Z\" | EXT=\"RAR\" | "
        "EXT=\"TAR\" | EXT=\"GZ\" | EXT=\"TGZ\" | EXT=\"BZ2\" | EXT=\"TBZ\" | "
        "EXT=\"TBZ2\" | EXT=\"XZ\" | EXT=\"TXZ\" | EXT=\"ZST\" | EXT=\"TZST\" | "
        "EXT=\"LZ\" | EXT=\"LZ4\" | EXT=\"LZMA\" | EXT=\"LZO\" | EXT=\"Z\" | "
        "EXT=\"CPIO\" | EXT=\"AR\" | EXT=\"A\" | EXT=\"DEB\" | EXT=\"RPM\" | "
        "EXT=\"ISO\" | EXT=\"CAB\" | EXT=\"XAR\" | EXT=\"PAX\" | EXT=\"LHA\" | "
        "EXT=\"LZH\" | EXT=\"WARC\" | EXT=\"MTREE\"");
}

WLX_EXPORT int DCPCALL ListSendCommand(HWND ListWin, int Command, int Parameter)
{
    WLX_TRY {
        auto *widget = reinterpret_cast<ArchiveViewWidget *>(ListWin);
        if (!widget)
            return LISTPLUGIN_ERROR;

        switch (Command) {
        case lc_copy: {
            const QString text = widget->selectionAsText();
            if (text.isEmpty())
                return LISTPLUGIN_ERROR;
            QApplication::clipboard()->setText(text);
            break;
        }
        case lc_selectall:
            widget->view()->selectAll();
            break;
        case lc_setpercent:
            // DC's scrollbar driving the view.
            widget->scrollToPercent(Parameter);
            break;
        case lc_newparams:
            // Accept and ignore: returning an error makes DC destroy and
            // recreate the plugin, which restarts the scan from scratch.
            return LISTPLUGIN_OK;
        case lc_focus:
            if (Parameter) {
                widget->focusManager()->setActive(true);
                widget->view()->setFocus(Qt::OtherFocusReason);
            } else {
                widget->focusManager()->setActive(false);
                if (QWidget *focused = QApplication::focusWidget()) {
                    if (focused == widget || widget->isAncestorOf(focused))
                        focused->clearFocus();
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

WLX_EXPORT int DCPCALL ListSearchDialog(HWND ListWin, int FindNext)
{
    WLX_TRY {
        auto *widget = reinterpret_cast<ArchiveViewWidget *>(ListWin);
        if (!widget)
            return LISTPLUGIN_ERROR;
        widget->showFindPanel(FindNext != 0);
        return LISTPLUGIN_OK;
    } WLX_CATCH("ListSearchDialog");
    return LISTPLUGIN_ERROR;
}

WLX_EXPORT int DCPCALL ListPrint(HWND ListWin, char *FileToPrint,
                                 char *DefPrinter, int PrintFlags, RECT *Margins)
{
    WLX_TRY {
        Q_UNUSED(FileToPrint);
        Q_UNUSED(PrintFlags);
        Q_UNUSED(Margins);
        auto *widget = reinterpret_cast<ArchiveViewWidget *>(ListWin);
        if (!widget)
            return LISTPLUGIN_ERROR;
        return widget->printListing(QString::fromLocal8Bit(DefPrinter))
                   ? LISTPLUGIN_OK : LISTPLUGIN_ERROR;
    } WLX_CATCH("ListPrint");
    return LISTPLUGIN_ERROR;
}

WLX_EXPORT int DCPCALL ListNotificationReceived(HWND ListWin, int Message,
                                                WPARAM wParam, LPARAM lParam)
{
    WLX_TRY {
        Q_UNUSED(wParam);
        Q_UNUSED(lParam);
        auto *widget = reinterpret_cast<ArchiveViewWidget *>(ListWin);
        if (!widget)
            return LISTPLUGIN_ERROR;

        // DC forwards its own itm_* notifications here. The ones that carry
        // meaning for a table are handled; the text-viewer ones (itm_wrap,
        // itm_fontstyle) are accepted and ignored rather than refused, since
        // refusing makes DC treat the plugin as broken for that message.
        switch (Message) {
        case itm_percent:
            widget->scrollToPercent(int(wParam));
            break;
        case itm_fit:
            widget->applyShowFlags(lcp_fittowindow);
            break;
        default:
            break;
        }
        return LISTPLUGIN_OK;
    } WLX_CATCH("ListNotificationReceived");
    return LISTPLUGIN_ERROR;
}

// Note there is deliberately no ListGetPreviewBitmap. Its contract is to
// return an HBITMAP, which on Windows is a GDI handle DC can use directly.
// On Linux DC is an LCL application and that handle would have to be an
// LCL/widgetset bitmap object — something a Qt plugin has no way to
// manufacture. No plugin in this repository implements it. Leaving the symbol
// unexported makes DC fall back to its own thumbnailer, which is the correct
// outcome; exporting a stub that returns nullptr would only add a failed call
// per thumbnail.

// ---------------------------------------------------------------------------
// Find-next over the visible rows.
// ---------------------------------------------------------------------------

static int searchIn(ArchiveViewWidget *widget, const QString &needle, int flags)
{
    if (!widget || needle.isEmpty())
        return LISTPLUGIN_ERROR;

    ArchiveTreeView *view = widget->view();
    QAbstractItemModel *model = view->model();
    if (!model)
        return LISTPLUGIN_ERROR;

    const Qt::CaseSensitivity sensitivity =
        (flags & lcs_matchcase) ? Qt::CaseSensitive : Qt::CaseInsensitive;

    // Walk the tree in visual order. Collapsed subtrees are searched too —
    // a hit inside one is expanded into view rather than silently skipped.
    QVector<QModelIndex> order;
    QVector<QModelIndex> stack;
    for (int row = model->rowCount() - 1; row >= 0; --row)
        stack.append(model->index(row, ArchiveModel::NameColumn));
    while (!stack.isEmpty()) {
        const QModelIndex current = stack.takeLast();
        order.append(current);
        for (int row = model->rowCount(current) - 1; row >= 0; --row)
            stack.append(model->index(row, ArchiveModel::NameColumn, current));
    }
    if (order.isEmpty())
        return LISTPLUGIN_ERROR;

    const QModelIndex current = view->currentIndex();
    int start = 0;
    if (current.isValid()) {
        const int found = order.indexOf(current.siblingAtColumn(ArchiveModel::NameColumn));
        if (found >= 0)
            start = found;
    }

    const bool backwards = (flags & lcs_backwards) != 0;
    const bool fromStart = (flags & lcs_findfirst) != 0;
    const int total = order.size();

    for (int step = fromStart ? 0 : 1; step <= total; ++step) {
        const int offset = backwards ? -step : step;
        const QModelIndex candidate = order.at(((start + offset) % total + total) % total);
        if (candidate.data(Qt::DisplayRole).toString().contains(needle, sensitivity)) {
            for (QModelIndex ancestor = candidate.parent(); ancestor.isValid();
                 ancestor = ancestor.parent()) {
                view->expand(ancestor);
            }
            view->setCurrentIndex(candidate);
            view->scrollTo(candidate);
            return LISTPLUGIN_OK;
        }
    }

    return LISTPLUGIN_ERROR;
}

WLX_EXPORT int DCPCALL ListSearchText(HWND ListWin, char *SearchString, int SearchParameter)
{
    WLX_TRY {
        return searchIn(reinterpret_cast<ArchiveViewWidget *>(ListWin),
                        QString::fromUtf8(SearchString), SearchParameter);
    } WLX_CATCH("ListSearchText");
    return LISTPLUGIN_ERROR;
}

WLX_EXPORT int DCPCALL ListSearchTextW(HWND ListWin, WCHAR *SearchString, int SearchParameter)
{
    WLX_TRY {
        return searchIn(reinterpret_cast<ArchiveViewWidget *>(ListWin),
                        QString::fromUtf16(reinterpret_cast<const char16_t *>(SearchString)),
                        SearchParameter);
    } WLX_CATCH("ListSearchTextW");
    return LISTPLUGIN_ERROR;
}
