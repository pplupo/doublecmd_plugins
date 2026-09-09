/// Minimal stand-in for Double Commander: dlopen()s the built .wlx and drives
/// its exported entry points the way DC does.
///
/// The scanner and model are covered by scan_smoke. This covers the part that
/// scan_smoke cannot: the C ABI surface where a null dereference is a segfault
/// inside the *file manager*, not inside a helper process. The predecessor
/// dereferenced findChild<QTableWidget*>("table") unchecked in three separate
/// entry points, so every one of those was a crash in the host.
///
/// Exercises, in DC's order: load, search, commands, the destroy/recreate
/// reload cycle, switching to a different file on the same parent, calls with
/// null and bogus handles, and teardown by destroying the parent widget.
///
///   wlx_host <plugin.wlx> <archive> [more archives...]

#include <QApplication>
#include <QElapsedTimer>
#include <QTextStream>
#include <QTimer>
#include <QWidget>

#include <dlfcn.h>

#include "wlxplugin.h"

namespace {

struct PluginApi {
    HWND (*load)(HWND, char *, int) = nullptr;
    HWND (*loadW)(HWND, WCHAR *, int) = nullptr;
    int (*loadNext)(HWND, HWND, char *, int) = nullptr;
    void (*closeWindow)(HWND) = nullptr;
    void (*detectString)(char *, int) = nullptr;
    void (*setDefaultParams)(ListDefaultParamStruct *) = nullptr;
    int (*sendCommand)(HWND, int, int) = nullptr;
    int (*searchText)(HWND, char *, int) = nullptr;
    int (*searchDialog)(HWND, int) = nullptr;
    int (*print)(HWND, char *, char *, int, RECT *) = nullptr;
    int (*notification)(HWND, int, WPARAM, LPARAM) = nullptr;
};

template <typename T>
bool resolve(void *handle, const char *name, T &slot, QTextStream &out)
{
    slot = reinterpret_cast<T>(dlsym(handle, name));
    if (!slot) {
        out << "  MISSING export: " << name << '\n';
        return false;
    }
    return true;
}

/// Let queued signals from the scanner thread land.
void pump(int milliseconds)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

} // namespace

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QTextStream out(stdout);

    if (argc < 3) {
        out << "usage: wlx_host <plugin.wlx> <archive> [archive...]\n";
        return 2;
    }

    void *handle = dlopen(argv[1], RTLD_NOW);
    if (!handle) {
        out << "dlopen failed: " << dlerror() << '\n';
        return 1;
    }

    PluginApi api;
    bool complete = true;
    complete &= resolve(handle, "ListLoad", api.load, out);
    complete &= resolve(handle, "ListLoadW", api.loadW, out);
    complete &= resolve(handle, "ListCloseWindow", api.closeWindow, out);
    complete &= resolve(handle, "ListGetDetectString", api.detectString, out);
    complete &= resolve(handle, "ListSendCommand", api.sendCommand, out);
    complete &= resolve(handle, "ListSearchText", api.searchText, out);
    complete &= resolve(handle, "ListLoadNext", api.loadNext, out);
    complete &= resolve(handle, "ListSetDefaultParams", api.setDefaultParams, out);
    complete &= resolve(handle, "ListSearchDialog", api.searchDialog, out);
    complete &= resolve(handle, "ListPrint", api.print, out);
    complete &= resolve(handle, "ListNotificationReceived", api.notification, out);
    if (!complete)
        return 1;

    // DC calls this once at load time, before any ListLoad, to hand over the
    // ini path plugins are expected to use.
    ListDefaultParamStruct params = {};
    params.size = sizeof(params);
    params.PluginInterfaceVersionLow = 20;
    params.PluginInterfaceVersionHi = 2;
    qstrncpy(params.DefaultIniName, "/tmp/archiveview-host-test.ini",
             sizeof(params.DefaultIniName));
    api.setDefaultParams(&params);
    out << "ListSetDefaultParams: accepted\n";

    char detect[2048] = {};
    api.detectString(detect, sizeof(detect));
    out << "detect string   : " << QString::fromUtf8(detect).left(60) << "...\n";
    out << "detect length   : " << qstrlen(detect) << " chars\n\n";

    // DC hands the plugin a container widget it owns.
    auto *container = new QWidget;
    container->resize(900, 600);
    container->show();

    for (int i = 2; i < argc; ++i) {
        const QString path = QString::fromLocal8Bit(argv[i]);
        out << "--- " << path.section('/', -1) << " ---\n";

        HWND window = api.load(reinterpret_cast<HWND>(container),
                               argv[i], lcp_forceshow);
        if (!window) {
            out << "  ListLoad declined (expected for non-archives)\n\n";
            continue;
        }
        pump(400);

        char needle[] = "file";
        const int found = api.searchText(window, needle, lcs_findfirst);
        out << "  ListSearchText  : "
            << (found == LISTPLUGIN_OK ? "match" : "no match") << '\n';
        out << "  find-next       : "
            << (api.searchText(window, needle, 0) == LISTPLUGIN_OK ? "match" : "no match")
            << '\n';
        out << "  backwards       : "
            << (api.searchText(window, needle, lcs_backwards) == LISTPLUGIN_OK
                    ? "match" : "no match") << '\n';

        out << "  lc_selectall    : " << api.sendCommand(window, lc_selectall, 0) << '\n';
        out << "  lc_copy         : " << api.sendCommand(window, lc_copy, 0) << '\n';
        out << "  lc_focus on/off : " << api.sendCommand(window, lc_focus, 1)
            << "/" << api.sendCommand(window, lc_focus, 0) << '\n';
        out << "  lc_newparams    : " << api.sendCommand(window, lc_newparams, 0)
            << " (0 = accepted, so DC will not recreate us)\n";
        out << "  unknown command : " << api.sendCommand(window, 9999, 0)
            << " (1 = declined)\n";

        out << "  lc_setpercent   : " << api.sendCommand(window, lc_setpercent, 50) << '\n';
        out << "  ListSearchDialog: " << api.searchDialog(window, 1) << '\n';
        out << "  itm_percent     : " << api.notification(window, itm_percent, 25, 0) << '\n';
        out << "  itm_fit         : " << api.notification(window, itm_fit, 0, 0) << '\n';

        // A printer name DC would never send, so no job reaches a device.
        char bogusPrinter[] = "archiveview-nonexistent-printer";
        char printName[] = "listing";
        out << "  ListPrint(bogus): " << api.print(window, printName, bogusPrinter, 0, nullptr)
            << " (1 = refused, no job queued)\n";

        // DC's reload cycle: close then load the same file again.
        api.closeWindow(window);
        HWND again = api.load(reinterpret_cast<HWND>(container), argv[i], 0);
        out << "  reload cycle    : " << (again ? "widget reused" : "FAILED") << '\n';
        pump(200);

        // ListLoadNext is what DC uses when arrowing to the next file: the
        // widget tree is kept and only the contents change. Load the *next*
        // archive on the command line through it.
        if (i + 1 < argc && again) {
            const int rc = api.loadNext(reinterpret_cast<HWND>(container), again,
                                        argv[i + 1], 0);
            out << "  ListLoadNext    : " << rc
                << (rc == LISTPLUGIN_OK ? " (reused, no rebuild)" : " (declined)") << '\n';
            pump(300);
            // Put the original file back so the loop's next iteration starts clean.
            api.loadNext(reinterpret_cast<HWND>(container), again, argv[i], 0);
            pump(100);
        }

        api.closeWindow(again);
        out << '\n';
    }

    // Hostile handles. DC should never send these, but a plugin that
    // segfaults on one takes the file manager with it.
    out << "--- defensive checks ---\n";
    api.closeWindow(nullptr);
    out << "  ListCloseWindow(null)   : survived\n";
    out << "  ListSendCommand(null)   : " << api.sendCommand(nullptr, lc_copy, 0) << '\n';
    out << "  ListSearchText(null)    : "
        << api.searchText(nullptr, const_cast<char *>("x"), lcs_findfirst) << '\n';
    out << "  ListSearchDialog(null)  : " << api.searchDialog(nullptr, 0) << '\n';
    out << "  ListPrint(null)         : " << api.print(nullptr, nullptr, nullptr, 0, nullptr) << '\n';
    out << "  notification(null)      : " << api.notification(nullptr, itm_percent, 0, 0) << '\n';
    out << "  ListLoadNext(null,null) : "
        << api.loadNext(nullptr, nullptr, const_cast<char *>("/nope.zip"), 0) << '\n';
    api.setDefaultParams(nullptr);
    out << "  setDefaultParams(null)  : survived\n";

    char missing[] = "/nonexistent/path/to/nothing.zip";
    out << "  ListLoad(missing file)  : "
        << (api.load(reinterpret_cast<HWND>(container), missing, 0) ? "returned a window"
                                                                    : "declined") << '\n';

    WCHAR wide[] = { 't', 'e', 's', 't', '.', 'z', 'i', 'p', 0 };
    out << "  ListLoadW(missing file) : "
        << (api.loadW(reinterpret_cast<HWND>(container), wide, 0) ? "returned a window"
                                                                  : "declined") << '\n';

    // Teardown through the parent, which is how the widget is really freed.
    delete container;
    pump(100);
    out << "  parent destroyed        : survived\n";

    out << "\nWLX HOST OK\n";
    out.flush();
    return 0;
}
