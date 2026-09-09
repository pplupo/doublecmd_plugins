/// GTK3 variant of the archive lister.
///
/// Shares everything below the UI with the Qt6 variant through
/// archiveview::Scanner and archiveview::EntryTree, so both listings are built
/// from one implementation of the parts where being wrong would mislead:
/// hostile member names shown verbatim, duplicate paths given their own rows,
/// the ZIP central directory's packed sizes and CRCs, encryption indicators.
///
/// Scope: listing. Extraction, drag-out and the filter box are the Qt
/// variant's today and are the next pass here.

#include <gtk/gtk.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "wlxplugin.h"

#include "ArchiveTreeModel.h"
#include "core/ArchiveNames.h"
#include "core/ArchiveScanner.h"
#include "core/ArchiveSettings.h"
#include "core/EntryFormat.h"
#include "core/EntryTree.h"

namespace {

std::string g_iniPath;

/// One previewed archive: the tree, the scanner filling it, and the widgets.
///
/// Lifetime is the tricky part. The scanner reports from a worker thread, and
/// GTK may only be touched from the main loop, so every callback is bounced
/// through g_idle_add. An idle handler can therefore still be queued after
/// the view is destroyed — so the instance is held by shared_ptr, the idle
/// payload holds a weak_ptr, and a destroyed view simply makes the handler a
/// no-op instead of a use-after-free.
struct ArchiveView : std::enable_shared_from_this<ArchiveView>,
                     archiveview::EntryTree::Listener {
    GtkWidget *root = nullptr;        ///< the box handed back to DC
    GtkWidget *view = nullptr;        ///< GtkTreeView
    GtkWidget *status = nullptr;      ///< GtkLabel
    ArchiveTreeModel *model = nullptr;

    archiveview::EntryTree tree;
    archiveview::Scanner scanner;
    archiveview::Settings settings;
    archiveview::Summary summary;

    std::string path;
    std::string formatText;
    int64_t percent = -1;

    /// Set when the widget is destroyed; idle handlers check it.
    std::atomic<bool> dead{false};

    void beforeInsert(const archiveview::EntryTree::Node *, int, int) override {}
    void afterInsert(const archiveview::EntryTree::Node *parent,
                     int first, int count) override
    {
        // GTK notifies after the fact, which is why EntryTree brackets its
        // insertions rather than returning them: the Qt model needs the
        // "before" half, this one needs the "after".
        archive_tree_model_rows_inserted(model, parent, first, count);
    }

    void refreshStatus()
    {
        if (dead.load(std::memory_order_relaxed) || !status)
            return;

        std::string text = formatText.empty() ? std::string("scanning…") : formatText;
        text += "   |   " + std::to_string(tree.entryCount()) + " members";
        if (tree.duplicateCount() > 0)
            text += "   |   " + std::to_string(tree.duplicateCount()) + " duplicate paths";
        if (summary.hasEncryptedEntries)
            text += "   |   encrypted";
        if (summary.zip64)
            text += "   |   ZIP64";
        if (summary.truncated)
            text += "   |   truncated";
        if (summary.cancelled)
            text += "   |   cancelled";
        if (!summary.comment.empty())
            text += "   |   comment: " + summary.comment;
        if (percent >= 0 && percent < 100)
            text += "   |   " + std::to_string(percent) + "%";

        gtk_label_set_text(GTK_LABEL(status), text.c_str());
    }
};

using ViewPtr = std::shared_ptr<ArchiveView>;
using ViewWeak = std::weak_ptr<ArchiveView>;

/// Instances keyed by the widget DC holds, so the entry points can recover
/// the shared_ptr from the raw HWND.
std::vector<ViewPtr> g_instances;

ViewPtr lookup(HWND handle)
{
    auto *widget = reinterpret_cast<GtkWidget *>(handle);
    for (const ViewPtr &view : g_instances) {
        if (view->root == widget)
            return view;
    }
    return nullptr;
}

/// Run `work` on the GTK main loop, unless the view died first.
template <typename Work>
void post(const ViewWeak &weak, Work work)
{
    struct Payload {
        ViewWeak weak;
        Work work;
    };
    auto *payload = new Payload{weak, std::move(work)};

    g_idle_add([](gpointer data) -> gboolean {
        auto *p = static_cast<Payload *>(data);
        if (ViewPtr view = p->weak.lock()) {
            if (!view->dead.load(std::memory_order_relaxed))
                p->work(view);
        }
        delete p;
        return G_SOURCE_REMOVE;
    }, payload);
}

void addTextColumn(GtkTreeView *view, const char *title, int column, int width)
{
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    if (column != ARCHIVE_COL_NAME)
        g_object_set(renderer, "xalign", 1.0, nullptr);

    GtkTreeViewColumn *col =
        gtk_tree_view_column_new_with_attributes(title, renderer, "text", column, nullptr);
    gtk_tree_view_column_set_resizable(col, TRUE);
    gtk_tree_view_column_set_sizing(col, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width(col, width);
    gtk_tree_view_append_column(view, col);
}

void buildUi(const ViewPtr &view)
{
    view->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    view->model = archive_tree_model_new(&view->tree);
    view->view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(view->model));
    auto *treeView = GTK_TREE_VIEW(view->view);

    // Fixed-height mode is what keeps a 100k-row listing scrollable: without
    // it GtkTreeView measures every row to compute its height.
    gtk_tree_view_set_fixed_height_mode(treeView, TRUE);
    gtk_tree_view_set_enable_search(treeView, TRUE);
    gtk_tree_view_set_search_column(treeView, ARCHIVE_COL_NAME);
    gtk_tree_selection_set_mode(gtk_tree_view_get_selection(treeView),
                                GTK_SELECTION_MULTIPLE);

    // The lock column renders an icon name rather than text.
    {
        GtkCellRenderer *renderer = gtk_cell_renderer_pixbuf_new();
        GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
            "", renderer, "icon-name", ARCHIVE_COL_LOCK, nullptr);
        gtk_tree_view_column_set_sizing(col, GTK_TREE_VIEW_COLUMN_FIXED);
        gtk_tree_view_column_set_fixed_width(col, 24);
        gtk_tree_view_append_column(treeView, col);
    }

    addTextColumn(treeView, "Name",        ARCHIVE_COL_NAME, 320);
    addTextColumn(treeView, "Size",        ARCHIVE_COL_SIZE, 110);
    addTextColumn(treeView, "Packed",      ARCHIVE_COL_PACKED, 110);
    addTextColumn(treeView, "Ratio",       ARCHIVE_COL_RATIO, 80);
    addTextColumn(treeView, "CRC-32",      ARCHIVE_COL_CRC, 100);
    addTextColumn(treeView, "Modified",    ARCHIVE_COL_MODIFIED, 140);
    addTextColumn(treeView, "Mode",        ARCHIVE_COL_MODE, 110);
    addTextColumn(treeView, "Owner",       ARCHIVE_COL_OWNER, 120);
    addTextColumn(treeView, "Link target", ARCHIVE_COL_LINK, 200);

    for (const std::string &hidden : view->settings.hiddenColumns) {
        for (int column = 0; column < ARCHIVE_N_COLUMNS; ++column) {
            GtkTreeViewColumn *col = gtk_tree_view_get_column(treeView, column);
            const char *title = col ? gtk_tree_view_column_get_title(col) : nullptr;
            if (title && g_ascii_strcasecmp(title, hidden.c_str()) == 0)
                gtk_tree_view_column_set_visible(col, FALSE);
        }
    }

    GtkWidget *scroller = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroller), view->view);
    gtk_box_pack_start(GTK_BOX(view->root), scroller, TRUE, TRUE, 0);

    view->status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(view->status), 0.0);
    gtk_widget_set_margin_start(view->status, 4);
    gtk_widget_set_margin_end(view->status, 4);
    gtk_box_pack_start(GTK_BOX(view->root), view->status, FALSE, FALSE, 2);

    if (view->settings.startFlat)
        archive_tree_model_set_flat(view->model, true);

    gtk_widget_show_all(view->root);
}

void wireScanner(const ViewPtr &view)
{
    ViewWeak weak = view->shared_from_this();
    archiveview::Scanner::Callbacks callbacks;

    callbacks.format = [weak](const std::string &format, const std::string &filters) {
        post(weak, [format, filters](const ViewPtr &v) {
            v->formatText = filters.empty() ? format : format + " (" + filters + ")";
            v->refreshStatus();
        });
    };
    callbacks.comment = [weak](const std::string &comment) {
        post(weak, [comment](const ViewPtr &v) {
            v->summary.comment = comment;
            v->refreshStatus();
        });
    };
    callbacks.entries = [weak](const archiveview::EntryBatch &batch) {
        post(weak, [batch](const ViewPtr &v) {
            const bool flat = archive_tree_model_get_flat(v->model);
            const int before = v->tree.entryCount();
            // In flat mode the per-parent insertions describe rows nobody is
            // showing, so build silently and announce the one run that did
            // appear — the same split the Qt model makes.
            v->tree.addEntries(batch, flat ? nullptr : v.get());
            if (flat) {
                for (int row = before; row < v->tree.entryCount(); ++row)
                    archive_tree_model_rows_inserted(v->model, nullptr, row, 1);
            }
            v->refreshStatus();
        });
    };
    callbacks.progress = [weak](int64_t read, int64_t total) {
        post(weak, [read, total](const ViewPtr &v) {
            v->percent = (total > 0) ? (100 * read / total) : -1;
            v->refreshStatus();
        });
    };
    callbacks.finished = [weak](bool ok, const std::string &error,
                                const archiveview::Summary &summary) {
        post(weak, [ok, error, summary](const ViewPtr &v) {
            v->summary = summary;
            v->percent = -1;
            if (!ok && !error.empty())
                v->formatText = error;
            v->refreshStatus();
        });
    };
    // Passphrase prompting belongs with extraction, which this variant does
    // not do yet. Declining keeps encrypted archives listing (names and sizes
    // live in headers) without a dialog that would have nothing to unlock.
    callbacks.passphraseNeeded = [weak](int) {
        if (ViewPtr v = weak.lock())
            v->scanner.providePassphrase(std::string(), false);
    };

    view->scanner.setCallbacks(std::move(callbacks));
}

void destroyInstance(const ViewPtr &view)
{
    view->dead.store(true, std::memory_order_relaxed);
    // Join before anything the callbacks touch goes away.
    view->scanner.cancelAndWait();
    if (view->model) {
        g_object_unref(view->model);
        view->model = nullptr;
    }
}

} // namespace

#define WLX_EXPORT extern "C" __attribute__((visibility("default")))

WLX_EXPORT void DCPCALL ListSetDefaultParams(ListDefaultParamStruct *dps)
{
    if (!dps || dps->size < static_cast<int>(sizeof(ListDefaultParamStruct)))
        return;
    g_iniPath = dps->DefaultIniName;
}

WLX_EXPORT void DCPCALL ListGetDetectString(char *DetectString, int maxlen)
{
    snprintf(DetectString, maxlen - 1,
        "EXT=\"ZIP\" | EXT=\"ZIPX\" | EXT=\"WAR\" | EXT=\"EAR\" | EXT=\"APK\" | "
        "EXT=\"XPI\" | EXT=\"WHL\" | EXT=\"7Z\" | EXT=\"RAR\" | EXT=\"ACE\" | "
        "EXT=\"TAR\" | EXT=\"GZ\" | EXT=\"TGZ\" | EXT=\"BZ2\" | EXT=\"TBZ\" | "
        "EXT=\"TBZ2\" | EXT=\"XZ\" | EXT=\"TXZ\" | EXT=\"ZST\" | EXT=\"TZST\" | "
        "EXT=\"LZ\" | EXT=\"LZ4\" | EXT=\"LZMA\" | EXT=\"LZO\" | EXT=\"Z\" | "
        "EXT=\"CPIO\" | EXT=\"AR\" | EXT=\"A\" | EXT=\"DEB\" | EXT=\"RPM\" | "
        "EXT=\"ISO\" | EXT=\"CAB\" | EXT=\"XAR\" | EXT=\"PAX\" | EXT=\"LHA\" | "
        "EXT=\"LZH\" | EXT=\"WARC\" | EXT=\"MTREE\"");
}

WLX_EXPORT HWND DCPCALL ListLoad(HWND ParentWin, char *FileToLoad, int ShowFlags)
{
    (void)ShowFlags;
    if (!FileToLoad)
        return nullptr;

    // Answer "is this an archive?" before returning, so DC can fall through
    // to another viewer for anything we cannot show. Bounded — see canRead().
    if (!archiveview::Scanner::canRead(FileToLoad))
        return nullptr;

    auto view = std::make_shared<ArchiveView>();
    view->path = FileToLoad;
    view->settings = archiveview::Settings::load(g_iniPath);
    archiveview::names::setFallbackCodec(view->settings.nameCodec);

    buildUi(view);
    wireScanner(view);

    auto *parent = reinterpret_cast<GtkWidget *>(ParentWin);
    gtk_container_add(GTK_CONTAINER(parent), view->root);
    gtk_widget_show_all(view->root);

    g_instances.push_back(view);
    view->scanner.scan(view->path, view->settings.maxEntries);
    return reinterpret_cast<HWND>(view->root);
}

WLX_EXPORT int DCPCALL ListLoadNext(HWND ParentWin, HWND PluginWin,
                                    char *FileToLoad, int ShowFlags)
{
    (void)ParentWin;
    (void)ShowFlags;
    ViewPtr view = lookup(PluginWin);
    if (!view || !FileToLoad)
        return LISTPLUGIN_ERROR;

    if (!archiveview::Scanner::canRead(FileToLoad))
        return LISTPLUGIN_ERROR;

    // Reuse the widget tree; only the contents change.
    view->scanner.cancelAndWait();
    view->tree.clear();
    view->summary = archiveview::Summary();
    view->formatText.clear();
    view->percent = -1;
    view->path = FileToLoad;

    // The tree was emptied underneath the view, so observers must resync.
    gtk_tree_view_set_model(GTK_TREE_VIEW(view->view), nullptr);
    gtk_tree_view_set_model(GTK_TREE_VIEW(view->view), GTK_TREE_MODEL(view->model));

    view->refreshStatus();
    view->scanner.scan(view->path, view->settings.maxEntries);
    return LISTPLUGIN_OK;
}

WLX_EXPORT void DCPCALL ListCloseWindow(HWND ListWin)
{
    ViewPtr view = lookup(ListWin);
    if (!view)
        return;

    destroyInstance(view);
    if (view->root)
        gtk_widget_destroy(view->root);

    for (auto it = g_instances.begin(); it != g_instances.end(); ++it) {
        if (*it == view) {
            g_instances.erase(it);
            break;
        }
    }
}

WLX_EXPORT int DCPCALL ListSendCommand(HWND ListWin, int Command, int Parameter)
{
    ViewPtr view = lookup(ListWin);
    if (!view)
        return LISTPLUGIN_ERROR;

    switch (Command) {
    case lc_selectall:
        gtk_tree_selection_select_all(
            gtk_tree_view_get_selection(GTK_TREE_VIEW(view->view)));
        break;
    case lc_setpercent: {
        GtkAdjustment *adjustment =
            gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(view->view));
        if (!adjustment)
            return LISTPLUGIN_ERROR;
        const double lower = gtk_adjustment_get_lower(adjustment);
        const double upper = gtk_adjustment_get_upper(adjustment)
                           - gtk_adjustment_get_page_size(adjustment);
        gtk_adjustment_set_value(adjustment,
                                 lower + (upper - lower) * Parameter / 100.0);
        break;
    }
    case lc_newparams:
        // Accept and ignore: returning an error makes DC destroy and recreate
        // the plugin, which restarts the scan from scratch.
        return LISTPLUGIN_OK;
    case lc_focus:
        if (Parameter)
            gtk_widget_grab_focus(view->view);
        break;
    default:
        return LISTPLUGIN_ERROR;
    }
    return LISTPLUGIN_OK;
}

WLX_EXPORT int DCPCALL ListSearchText(HWND ListWin, char *SearchString,
                                      int SearchParameter)
{
    ViewPtr view = lookup(ListWin);
    if (!view || !SearchString || !*SearchString)
        return LISTPLUGIN_ERROR;

    const bool matchCase = (SearchParameter & lcs_matchcase) != 0;
    const bool backwards = (SearchParameter & lcs_backwards) != 0;
    const bool fromStart = (SearchParameter & lcs_findfirst) != 0;

    // Search the flat arrival order regardless of presentation: it visits
    // every member exactly once, including those inside collapsed
    // directories, which a visual walk would skip.
    const auto &flat = view->tree.flat();
    if (flat.empty())
        return LISTPLUGIN_ERROR;

    GtkTreeSelection *selection =
        gtk_tree_view_get_selection(GTK_TREE_VIEW(view->view));
    GtkTreeModel *model = nullptr;
    GtkTreeIter selected;

    int start = 0;
    if (!fromStart && gtk_tree_selection_get_selected(selection, &model, &selected)) {
        const auto *current = archive_tree_model_node(&selected);
        for (size_t row = 0; row < flat.size(); ++row) {
            if (flat[row] == current) {
                start = static_cast<int>(row);
                break;
            }
        }
    }

    const int total = static_cast<int>(flat.size());
    const std::string needle = SearchString;

    for (int step = fromStart ? 0 : 1; step <= total; ++step) {
        const int offset = backwards ? -step : step;
        const int index = ((start + offset) % total + total) % total;
        const std::string &candidate = flat[static_cast<size_t>(index)]->fullPath;

        bool hit = false;
        if (matchCase) {
            hit = candidate.find(needle) != std::string::npos;
        } else {
            gchar *haystack = g_utf8_casefold(candidate.c_str(), -1);
            gchar *pattern = g_utf8_casefold(needle.c_str(), -1);
            hit = haystack && pattern && std::string(haystack).find(pattern)
                                             != std::string::npos;
            g_free(haystack);
            g_free(pattern);
        }
        if (!hit)
            continue;

        GtkTreeIter iter;
        iter.stamp = view->model->stamp;
        iter.user_data = flat[static_cast<size_t>(index)];
        iter.user_data2 = nullptr;
        iter.user_data3 = nullptr;

        GtkTreePath *path = gtk_tree_model_get_path(GTK_TREE_MODEL(view->model), &iter);
        if (path) {
            // Expand ancestors, or a hit inside a collapsed directory cannot
            // be selected.
            gtk_tree_view_expand_to_path(GTK_TREE_VIEW(view->view), path);
            gtk_tree_selection_select_path(selection, path);
            gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(view->view), path,
                                         nullptr, TRUE, 0.5, 0.0);
            gtk_tree_path_free(path);
        }
        return LISTPLUGIN_OK;
    }

    return LISTPLUGIN_ERROR;
}
