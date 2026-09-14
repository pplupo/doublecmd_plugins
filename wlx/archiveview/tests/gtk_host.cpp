/// Stand-in for Double Commander's GTK3 build: dlopen()s the built .wlx and
/// drives its exported entry points the way DC does.
///
/// The counterpart to wlx_host, and it exists for the same reason — this is
/// the surface where a mistake is a crash inside the *file manager*. It also
/// checks the thing a unit test cannot: that the custom GtkTreeModel actually
/// reports the rows the scanner produced, by walking it through the public
/// GtkTreeModel API exactly as GtkTreeView would.
///
///   gtk_host <plugin.wlx> <archive> [more archives...]

#include <gtk/gtk.h>

#include <dlfcn.h>

#include <cstdio>
#include <string>

#include "wlxplugin.h"

namespace {

struct PluginApi {
    HWND (*load)(HWND, char *, int) = nullptr;
    int (*loadNext)(HWND, HWND, char *, int) = nullptr;
    void (*closeWindow)(HWND) = nullptr;
    void (*detectString)(char *, int) = nullptr;
    void (*setDefaultParams)(ListDefaultParamStruct *) = nullptr;
    int (*sendCommand)(HWND, int, int) = nullptr;
    int (*searchText)(HWND, char *, int) = nullptr;
};

template <typename T>
bool resolve(void *handle, const char *name, T &slot)
{
    slot = reinterpret_cast<T>(dlsym(handle, name));
    if (!slot) {
        std::printf("  MISSING export: %s\n", name);
        return false;
    }
    return true;
}

/// Let the scanner's g_idle_add callbacks land.
void pump(int milliseconds)
{
    const gint64 deadline = g_get_monotonic_time() + milliseconds * 1000;
    while (g_get_monotonic_time() < deadline) {
        while (gtk_events_pending())
            gtk_main_iteration_do(FALSE);
        g_usleep(2000);
    }
}

/// Walk the model through the GtkTreeModel interface, counting rows and
/// checking that get_path/get_iter round-trip — the two calls a view relies
/// on and a hand-written model most easily gets wrong.
int walkModel(GtkTreeModel *model, GtkTreeIter *parent, int depth,
              int *roundTripFailures, std::string *sample)
{
    int count = 0;
    GtkTreeIter iter;
    if (!gtk_tree_model_iter_children(model, &iter, parent))
        return 0;

    do {
        ++count;

        GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
        if (path) {
            GtkTreeIter recovered;
            if (!gtk_tree_model_get_iter(model, &recovered, path)
                || recovered.user_data != iter.user_data) {
                ++*roundTripFailures;
            }
            gtk_tree_path_free(path);
        } else {
            ++*roundTripFailures;
        }

        if (depth == 0 && sample->empty()) {
            gchar *name = nullptr;
            gtk_tree_model_get(model, &iter, 1 /* name column */, &name, -1);
            if (name) {
                *sample = name;
                g_free(name);
            }
        }

        if (gtk_tree_model_iter_has_child(model, &iter))
            count += walkModel(model, &iter, depth + 1, roundTripFailures, sample);
    } while (gtk_tree_model_iter_next(model, &iter));

    return count;
}

/// The filter entry the plugin packs above the view.
GtkWidget *filterBoxOf(GtkWidget *root)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(root));
    GtkWidget *entry = nullptr;
    for (GList *child = children; child; child = child->next) {
        if (GTK_IS_ENTRY(child->data))
            entry = GTK_WIDGET(child->data);
    }
    g_list_free(children);
    return entry;
}

GtkWidget *treeViewOf(GtkWidget *root)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(root));
    GtkWidget *view = nullptr;
    for (GList *child = children; child; child = child->next) {
        if (GTK_IS_SCROLLED_WINDOW(child->data)) {
            GList *inner = gtk_container_get_children(GTK_CONTAINER(child->data));
            for (GList *node = inner; node; node = node->next) {
                if (GTK_IS_TREE_VIEW(node->data))
                    view = GTK_WIDGET(node->data);
            }
            g_list_free(inner);
        }
    }
    g_list_free(children);
    return view;
}

GtkTreeModel *modelOf(GtkWidget *root)
{
    // The plugin hands back its container; the view is inside a scroller.
    GList *children = gtk_container_get_children(GTK_CONTAINER(root));
    GtkTreeModel *model = nullptr;
    for (GList *child = children; child; child = child->next) {
        auto *widget = GTK_WIDGET(child->data);
        if (GTK_IS_SCROLLED_WINDOW(widget)) {
            GList *inner = gtk_container_get_children(GTK_CONTAINER(widget));
            for (GList *node = inner; node; node = node->next) {
                if (GTK_IS_TREE_VIEW(node->data))
                    model = gtk_tree_view_get_model(GTK_TREE_VIEW(node->data));
            }
            g_list_free(inner);
        }
    }
    g_list_free(children);
    return model;
}

} // namespace

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);

    if (argc < 3) {
        std::printf("usage: gtk_host <plugin.wlx> <archive> [archive...]\n");
        return 2;
    }

    void *handle = dlopen(argv[1], RTLD_NOW);
    if (!handle) {
        std::printf("dlopen failed: %s\n", dlerror());
        return 1;
    }

    PluginApi api;
    bool complete = true;
    complete &= resolve(handle, "ListLoad", api.load);
    complete &= resolve(handle, "ListLoadNext", api.loadNext);
    complete &= resolve(handle, "ListCloseWindow", api.closeWindow);
    complete &= resolve(handle, "ListGetDetectString", api.detectString);
    complete &= resolve(handle, "ListSetDefaultParams", api.setDefaultParams);
    complete &= resolve(handle, "ListSendCommand", api.sendCommand);
    complete &= resolve(handle, "ListSearchText", api.searchText);
    if (!complete)
        return 1;

    ListDefaultParamStruct params = {};
    params.size = sizeof(params);
    params.PluginInterfaceVersionLow = 20;
    params.PluginInterfaceVersionHi = 2;
    g_strlcpy(params.DefaultIniName, "/tmp/archiveview-gtk-host.ini",
              sizeof(params.DefaultIniName));
    api.setDefaultParams(&params);

    char detect[2048] = {};
    api.detectString(detect, sizeof(detect));
    std::printf("detect length   : %zu chars\n\n", std::string(detect).size());

    // DC hands the plugin a container it owns.
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(window), 900, 600);
    GtkWidget *container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window), container);

    int failures = 0;

    for (int i = 2; i < argc; ++i) {
        std::printf("--- %s ---\n", argv[i]);

        HWND plugin = api.load(reinterpret_cast<HWND>(container), argv[i], 0);
        if (!plugin) {
            std::printf("  ListLoad declined (expected for non-archives)\n\n");
            continue;
        }
        pump(1200);

        GtkTreeModel *model = modelOf(reinterpret_cast<GtkWidget *>(plugin));
        if (model && GTK_IS_TREE_MODEL_FILTER(model)) {
            GtkTreeModel *child =
                gtk_tree_model_filter_get_model(GTK_TREE_MODEL_FILTER(model));
            int childFailures = 0;
            std::string childSample;
            std::printf("  child model rows: %d\n",
                        walkModel(child, nullptr, 0, &childFailures, &childSample));
        }
        if (!model) {
            std::printf("  NO MODEL FOUND\n");
            ++failures;
            continue;
        }

        int roundTripFailures = 0;
        std::string sample;
        const int rows = walkModel(model, nullptr, 0, &roundTripFailures, &sample);
        std::printf("  rows visible    : %d\n", rows);
        std::printf("  first row       : %s\n", sample.c_str());
        std::printf("  path round-trip : %s\n",
                    roundTripFailures == 0 ? "ok" : "FAILED");
        if (roundTripFailures != 0)
            ++failures;

        // --- live filter -------------------------------------------------
        if (GtkWidget *entry = filterBoxOf(reinterpret_cast<GtkWidget *>(plugin))) {
            gtk_entry_set_text(GTK_ENTRY(entry), "deep");
            pump(300);
            // Re-query: the plugin attaches the filter model to the view only
            // while a filter is active (attaching it during a scan is
            // quadratic), so the model pointer changes here.
            GtkTreeModel *filteredModel = modelOf(reinterpret_cast<GtkWidget *>(plugin));
            int filteredFailures = 0;
            std::string filteredSample;
            const int filtered = walkModel(filteredModel, nullptr, 0, &filteredFailures,
                                           &filteredSample);
            std::printf("  filter 'deep'   : %d of %d rows\n", filtered, rows);
            // A filter that changes nothing is not filtering.
            if (filtered >= rows || filteredFailures != 0)
                ++failures;

            gtk_entry_set_text(GTK_ENTRY(entry), "");
            pump(300);
            GtkTreeModel *clearedModel = modelOf(reinterpret_cast<GtkWidget *>(plugin));
            int clearedFailures = 0;
            std::string clearedSample;
            const int cleared = walkModel(clearedModel, nullptr, 0, &clearedFailures,
                                          &clearedSample);
            std::printf("  filter cleared  : %d rows %s\n", cleared,
                        cleared == rows ? "(restored)" : "(MISMATCH)");
            if (cleared != rows || clearedFailures != 0)
                ++failures;
        } else {
            std::printf("  NO FILTER BOX FOUND\n");
            ++failures;
        }

        // --- row activation dispatch --------------------------------------
        // Activating a *directory* must expand it, not open anything. Only
        // directories are activated here on purpose: activating a file would
        // extract it and launch a real application on the user's desktop,
        // which a test has no business doing.
        if (GtkWidget *treeView = treeViewOf(reinterpret_cast<GtkWidget *>(plugin))) {
            GtkTreeIter iter;
            bool checked = false;
            if (gtk_tree_model_iter_children(model, &iter, nullptr)) {
                do {
                    if (!gtk_tree_model_iter_has_child(model, &iter))
                        continue;
                    GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
                    if (!path)
                        continue;
                    gtk_tree_view_collapse_row(GTK_TREE_VIEW(treeView), path);
                    gtk_tree_view_row_activated(GTK_TREE_VIEW(treeView), path, nullptr);
                    pump(120);
                    const bool expanded =
                        gtk_tree_view_row_expanded(GTK_TREE_VIEW(treeView), path);
                    std::printf("  activate dir    : %s\n",
                                expanded ? "expanded" : "DID NOT EXPAND");
                    if (!expanded)
                        ++failures;
                    gtk_tree_path_free(path);
                    checked = true;
                    break;
                } while (gtk_tree_model_iter_next(model, &iter));
            }
            if (!checked)
                std::printf("  activate dir    : (no directory row in this archive)\n");
        }

        // --- drag source advertised? --------------------------------------
        if (GtkWidget *treeView = treeViewOf(reinterpret_cast<GtkWidget *>(plugin))) {
            // A GtkTreeView only becomes a drag source once the target list is
            // set; without it the drag-data-get handler is unreachable, which
            // is exactly how the Qt variant's drag was silently dead.
            GtkTargetList *targets = gtk_drag_source_get_target_list(treeView);
            const bool hasUriList =
                targets && gtk_target_list_find(targets,
                                                gdk_atom_intern("text/uri-list", FALSE),
                                                nullptr);
            std::printf("  drag source     : %s\n",
                        hasUriList ? "text/uri-list advertised" : "NOT ADVERTISED");
            if (!hasUriList)
                ++failures;
        }

        char needle[] = "file";
        std::printf("  ListSearchText  : %s\n",
                    api.searchText(plugin, needle, lcs_findfirst) == LISTPLUGIN_OK
                        ? "match" : "no match");
        std::printf("  lc_selectall    : %d\n", api.sendCommand(plugin, lc_selectall, 0));
        std::printf("  lc_setpercent   : %d\n", api.sendCommand(plugin, lc_setpercent, 50));
        std::printf("  lc_newparams    : %d\n", api.sendCommand(plugin, lc_newparams, 0));
        std::printf("  unknown command : %d\n", api.sendCommand(plugin, 9999, 0));

        if (i + 1 < argc) {
            const int rc = api.loadNext(reinterpret_cast<HWND>(container), plugin,
                                        argv[i + 1], 0);
            std::printf("  ListLoadNext    : %d%s\n", rc,
                        rc == LISTPLUGIN_OK ? " (reused)" : " (declined)");
            pump(1200);
            if (rc == LISTPLUGIN_OK) {
                int reloadFailures = 0;
                std::string reloadSample;
                const int reloadRows = walkModel(model, nullptr, 0,
                                                 &reloadFailures, &reloadSample);
                std::printf("  rows after next : %d\n", reloadRows);
                if (reloadFailures != 0)
                    ++failures;
            }
        }

        api.closeWindow(plugin);
        pump(200);
        std::printf("\n");
    }

    std::printf("--- defensive checks ---\n");
    api.closeWindow(nullptr);
    std::printf("  ListCloseWindow(null)  : survived\n");
    std::printf("  ListSendCommand(null)  : %d\n", api.sendCommand(nullptr, lc_copy, 0));
    std::printf("  ListSearchText(null)   : %d\n",
                api.searchText(nullptr, const_cast<char *>("x"), lcs_findfirst));
    char missing[] = "/nonexistent/nothing.zip";
    std::printf("  ListLoad(missing)      : %s\n",
                api.load(reinterpret_cast<HWND>(container), missing, 0) ? "returned" : "declined");
    api.setDefaultParams(nullptr);
    std::printf("  setDefaultParams(null) : survived\n");

    gtk_widget_destroy(window);
    pump(100);
    std::printf("  window destroyed       : survived\n");

    std::printf("\n%s\n", failures == 0 ? "GTK HOST OK" : "GTK HOST FAILURES");
    return failures == 0 ? 0 : 1;
}
