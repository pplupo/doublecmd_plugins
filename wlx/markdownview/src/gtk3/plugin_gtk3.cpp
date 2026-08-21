#ifdef BUILD_GTK_TARGET

#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <string.h>
#include <cstdio>
#include <sys/resource.h>

#include "wlxplugin.h"
#include "../core/markdown_engine.h"

namespace {

// Root cause of the live crash (caught under gdb with full symbols against
// the real dcgtk process, reproducible only there -- never in a standalone
// harness): libstdc++'s std::regex compiler (_M_disjunction/_M_alternative/
// _M_term/_M_atom, all mutually recursive-descent) is unusually stack-hungry.
// This runs on DC's OWN GUI thread, deep inside its call chain (the
// kastoolitems.pas/kasbutton.pas/customform.inc/wincontrol.inc frames present
// in every crash report) by the time it calls our ListLoad -- so the
// remaining stack headroom at that point, not the regex compile in
// isolation, is what determines whether this overflows.
//
// A first attempt moved the rendering work onto a dedicated pthread with an
// explicit large stack, isolating it from DC's stack depth entirely. That
// backfired: constructing ANY std::regex from that freshly-spawned thread
// crashed with SIGSEGV inside std::codecvt::do_unshift, called from the
// regex compiler's locale/facet setup -- reproduced twice, for two entirely
// different regex patterns (this file's codeBlockRe and diagramview's
// foreignObjRe), both at the identical faulting instruction. That points to
// a libstdc++ locale-facet thread-safety issue specific to a brand-new
// thread being the first to touch std::locale in a process that also does
// its own C setlocale() (DC does, per its own startup log) -- not something
// a bigger stack fixes.
//
// So: stay on DC's calling thread (where locale state is already consistent
// -- every prior test, on DC's main thread, succeeded up until the original
// stack-depth crash), and instead raise THIS thread's own stack ceiling via
// setrlimit(RLIMIT_STACK). Unlike a pthread's fixed-size mmap'd stack, the
// original/main thread's stack grows on demand via page faults up to
// RLIMIT_STACK -- raising the limit (even after the thread has been
// running and using stack for a while) gives the kernel room to keep
// growing it on the NEXT fault, which is exactly what's needed here.
void ensureLargeStackLimit()
{
    static bool done = false;
    if (done) return;
    done = true;

    struct rlimit rl;
    if (getrlimit(RLIMIT_STACK, &rl) != 0) return;

    const rlim_t want = 256ul * 1024 * 1024;
    rlim_t target = want;
    if (rl.rlim_max != RLIM_INFINITY && rl.rlim_max < target)
        target = rl.rlim_max; // can't exceed the hard limit without CAP_SYS_RESOURCE
    if (rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur >= target)
        return; // already generous enough

    rl.rlim_cur = target;
    setrlimit(RLIMIT_STACK, &rl); // best-effort; ignore failure, nothing else to fall back to
}

} // namespace

extern "C" {

HWND DCPCALL ListLoad(HWND ParentWin, char* FileToLoad, int ShowFlags)
try {
    // Function-try-block: this had NO exception handling at all, unlike
    // diagramview's ListLoad. renderFileToHtml() runs md4c parsing,
    // MicroTeX LaTeX rendering, and diagram (mermaid/plantuml) rendering
    // synchronously right here -- any C++ exception thrown anywhere in that
    // chain (a std::bad_alloc from a pathological allocation, a std::regex
    // throw, anything) would unwind straight across this extern "C"
    // boundary into DC's Pascal caller, which is undefined behavior. Same
    // fix already applied to diagramview_gtk3's ListLoad for the same
    // reason.
    GtkWidget *parent = GTK_WIDGET(ParentWin);
    GtkWidget *scrolledWin = gtk_scrolled_window_new(NULL, NULL);
    // GTK_CONTAINER(parent)->add() doesn't register the child the way
    // GtkLayout expects: DC's own ResizeWindow (uwlxmodule.pas) later calls
    // gtk_layout_move() on this widget, which asserts the widget's parent
    // is exactly this GtkLayout -- only gtk_layout_put() sets that up.
    gtk_layout_put(GTK_LAYOUT(parent), scrolledWin, 0, 0);

    GtkWidget *webView = webkit_web_view_new();
    gtk_widget_set_name(webView, "markdown_webview");

    ensureLargeStackLimit();
    std::string html = MarkdownEngine::renderFileToHtml(FileToLoad, false);
    webkit_web_view_load_html(WEBKIT_WEB_VIEW(webView), html.c_str(), NULL);

    gtk_container_add(GTK_CONTAINER(scrolledWin), webView);
    gtk_widget_show_all(scrolledWin);

    return (HWND)scrolledWin;
} catch (const std::exception &e) {
    fprintf(stderr, "[markdownview_gtk3] ListLoad EXCEPTION: %s\n", e.what());
    return nullptr;
} catch (...) {
    fprintf(stderr, "[markdownview_gtk3] ListLoad UNKNOWN EXCEPTION\n");
    return nullptr;
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
