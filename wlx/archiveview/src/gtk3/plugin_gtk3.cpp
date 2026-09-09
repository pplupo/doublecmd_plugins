/// GTK3 variant of the archive lister.
///
/// Shares everything below the UI with the Qt6 variant through
/// archiveview::Scanner and archiveview::EntryTree, so both listings are built
/// from one implementation of the parts where being wrong would mislead:
/// hostile member names shown verbatim, duplicate paths given their own rows,
/// the ZIP central directory's packed sizes and CRCs, encryption indicators.
///
/// Parity with the Qt6 variant: listing, live filter, context menu,
/// extract-to-directory, open-with, and drag-out as text/uri-list.

#include <gtk/gtk.h>
#include <glib/gstdio.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/ArchiveExtractor.h"

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
    GtkWidget *filterBox = nullptr;   ///< GtkEntry
    ArchiveTreeModel *model = nullptr;
    GtkTreeModel *filterModel = nullptr;   ///< GtkTreeModelFilter over `model`

    archiveview::EntryTree tree;
    archiveview::Scanner scanner;
    archiveview::Extractor extractor;

    /// Nodes the current filter admits, plus their ancestors. Recomputed on
    /// each keystroke in one pass so the visible-func stays O(1); asking
    /// "does any descendant match?" per row would be quadratic.
    std::unordered_set<const archiveview::EntryTree::Node *> visible;
    bool filtering = false;

    /// Scratch directory for previewed and dragged-out members, removed when
    /// the view goes away.
    std::string scratch;
    /// True while a nested extraction loop is running; a second request from
    /// a menu click or a drag would otherwise nest two loops over one
    /// extractor. Same guard as the Qt variant.
    bool extracting = false;
    archiveview::Settings settings;
    archiveview::Summary summary;

    std::string path;
    std::string formatText;
    int64_t percent = -1;

    /// Set when the widget is destroyed; idle handlers check it.
    std::atomic<bool> dead{false};

    // Grouped notifications are the Qt model's half of the Listener contract;
    // GTK uses immediate mode instead, where each node is announced the
    // moment it becomes reachable.
    void beforeInsert(const archiveview::EntryTree::Node *, int, int) override {}
    void afterInsert(const archiveview::EntryTree::Node *, int, int) override {}
    void nodeAttached(const archiveview::EntryTree::Node *node) override
    {
        archive_tree_model_row_inserted(model, node);
    }

    void refreshStatus()
    {
        if (dead.load(std::memory_order_relaxed) || !status)
            return;

        std::string text = formatText.empty() ? std::string("scanning…") : formatText;
        if (filtering) {
            text += "   |   " + std::to_string(visible.size()) + " of "
                  + std::to_string(tree.entryCount()) + " members";
        } else {
            text += "   |   " + std::to_string(tree.entryCount()) + " members";
        }
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


/// Normalised in-archive paths of every selected member.
///
/// Selecting a directory means everything under it, which is what every other
/// file manager does.
std::vector<std::string> selectedMembers(const ViewPtr &view)
{
    std::vector<std::string> roots;
    GtkTreeSelection *selection =
        gtk_tree_view_get_selection(GTK_TREE_VIEW(view->view));
    GtkTreeModel *model = nullptr;
    GList *rows = gtk_tree_selection_get_selected_rows(selection, &model);

    for (GList *row = rows; row; row = row->next) {
        auto *path = static_cast<GtkTreePath *>(row->data);
        GtkTreeIter iter;
        if (!gtk_tree_model_get_iter(model, &iter, path))
            continue;

        // The view may be looking through the filter, whose iters are not
        // ours; convert before touching the node.
        GtkTreeIter childIter = iter;
        if (GTK_IS_TREE_MODEL_FILTER(model)) {
            gtk_tree_model_filter_convert_iter_to_child_iter(
                GTK_TREE_MODEL_FILTER(model), &childIter, &iter);
        }

        if (const auto *node = archive_tree_model_node(&childIter))
            roots.push_back(node->fullPath);
    }

    g_list_free_full(rows, reinterpret_cast<GDestroyNotify>(gtk_tree_path_free));

    // Directories expand against the whole tree, not against the rows on
    // screen — so a filtered or flat view still extracts the full subtree.
    return view->tree.membersUnder(roots);
}

/// Extract `members` into `destination`, pumping a nested main loop so the UI
/// stays alive and the Cancel button works. Returns the paths written.
///
/// A nested GMainLoop rather than a `while (...) gtk_main_iteration()` spin,
/// for the same reason the Qt variant uses QEventLoop: a spin has no exit if
/// the terminal callback never arrives, so a failure to report would present
/// as a frozen file manager instead of an error.
std::vector<std::string> extractMembers(const ViewPtr &view,
                                        const std::vector<std::string> &members,
                                        const std::string &destination)
{
    if (view->extracting || members.empty())
        return {};
    view->extracting = true;

    // Reuse a passphrase already accepted for this archive.
    view->extractor.setPassphrase(view->scanner.acceptedPassphrase());

    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "Extracting…", GTK_WINDOW(gtk_widget_get_toplevel(view->view)),
        GTK_DIALOG_DESTROY_WITH_PARENT, "_Cancel", GTK_RESPONSE_CANCEL, nullptr);
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *label = gtk_label_new(view->path.c_str());
    GtkWidget *bar = gtk_progress_bar_new();
    gtk_widget_set_margin_start(label, 8);
    gtk_widget_set_margin_end(label, 8);
    gtk_widget_set_margin_start(bar, 8);
    gtk_widget_set_margin_end(bar, 8);
    gtk_box_pack_start(GTK_BOX(content), label, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(content), bar, FALSE, FALSE, 4);
    gtk_widget_show_all(dialog);

    GMainLoop *loop = g_main_loop_new(nullptr, FALSE);
    bool ok = false;
    std::string error;
    int refused = 0;

    struct Shared {
        GMainLoop *loop;
        GtkWidget *bar;
        GtkWidget *label;
        bool *ok;
        std::string *error;
        int *refused;
        int total;
    } shared{loop, bar, label, &ok, &error, &refused,
             static_cast<int>(members.size())};

    // Cancel closes the loop by cancelling the worker, which then reports.
    g_signal_connect(dialog, "response",
                     G_CALLBACK(+[](GtkDialog *, gint, gpointer data) {
                         static_cast<archiveview::Extractor *>(data)->cancel();
                     }), &view->extractor);

    archiveview::Extractor::Callbacks callbacks;
    ViewWeak weak = view->shared_from_this();
    callbacks.progress = [&shared, weak](int done, int total,
                                         const std::string &member) {
        // Worker thread: hop to the main loop before touching widgets.
        Shared *s = &shared;
        auto *payload = new std::pair<Shared *, std::pair<int, std::string>>(
            s, {total > 0 ? done : 0, member});
        (void)weak;
        g_idle_add([](gpointer data) -> gboolean {
            auto *p = static_cast<std::pair<Shared *, std::pair<int, std::string>> *>(data);
            if (p->first->total > 0) {
                gtk_progress_bar_set_fraction(
                    GTK_PROGRESS_BAR(p->first->bar),
                    double(p->second.first) / double(p->first->total));
            }
            gtk_label_set_text(GTK_LABEL(p->first->label), p->second.second.c_str());
            delete p;
            return G_SOURCE_REMOVE;
        }, payload);
    };
    callbacks.finished = [&shared](bool succeeded, const std::string &message,
                                   int, int refusedCount) {
        *shared.ok = succeeded;
        *shared.error = message;
        *shared.refused = refusedCount;
        // Quitting the loop from the worker thread is safe: g_main_loop_quit
        // is thread-safe, unlike the widget calls above.
        g_main_loop_quit(shared.loop);
    };
    // No prompt here yet: listing never needs one, and a dialog stacked
    // inside this nested loop is the kind of thing that wedges a host.
    callbacks.passphraseNeeded = [&view](int) {
        view->extractor.providePassphrase(std::string(), false);
    };
    view->extractor.setCallbacks(std::move(callbacks));

    view->extractor.extract(view->path, members, destination);
    g_main_loop_run(loop);
    view->extractor.cancelAndWait();
    g_main_loop_unref(loop);

    gtk_widget_destroy(dialog);
    view->extracting = false;

    const std::vector<std::string> written = view->extractor.writtenPaths();

    if (refused > 0) {
        // Worth saying out loud: these are the traversal and absolute-path
        // members the listing shows verbatim.
        GtkWidget *warning = gtk_message_dialog_new(
            GTK_WINDOW(gtk_widget_get_toplevel(view->view)),
            GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK,
            "%d member(s) were not extracted because their stored paths point "
            "outside the destination directory (an absolute path, or one "
            "containing \"..\").", refused);
        gtk_dialog_run(GTK_DIALOG(warning));
        gtk_widget_destroy(warning);
    } else if (!ok && !error.empty() && error != "Cancelled") {
        GtkWidget *warning = gtk_message_dialog_new(
            GTK_WINDOW(gtk_widget_get_toplevel(view->view)),
            GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK,
            "Extraction failed: %s", error.c_str());
        gtk_dialog_run(GTK_DIALOG(warning));
        gtk_widget_destroy(warning);
    }

    return written;
}

const std::string &scratchDir(const ViewPtr &view)
{
    if (view->scratch.empty()) {
        gchar *dir = g_dir_make_tmp("archiveview-XXXXXX", nullptr);
        if (dir) {
            view->scratch = dir;
            g_free(dir);
        }
    }
    return view->scratch;
}

void onExtractSelection(const ViewPtr &view)
{
    const std::vector<std::string> members = selectedMembers(view);
    if (members.empty())
        return;

    GtkWidget *chooser = gtk_file_chooser_dialog_new(
        "Extract selection to", GTK_WINDOW(gtk_widget_get_toplevel(view->view)),
        GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Extract", GTK_RESPONSE_ACCEPT, nullptr);

    gchar *parent = g_path_get_dirname(view->path.c_str());
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(chooser), parent);
    g_free(parent);

    std::string destination;
    if (gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT) {
        gchar *chosen = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
        if (chosen) {
            destination = chosen;
            g_free(chosen);
        }
    }
    gtk_widget_destroy(chooser);

    if (!destination.empty())
        extractMembers(view, members, destination);
}

void onOpenSelection(const ViewPtr &view)
{
    const std::vector<std::string> members = selectedMembers(view);
    if (members.empty() || scratchDir(view).empty())
        return;

    // Only the first: opening thirty files in thirty applications is never
    // what someone means by "open".
    const std::vector<std::string> written =
        extractMembers(view, {members.front()}, view->scratch);
    if (written.empty())
        return;

    gchar *uri = g_filename_to_uri(written.front().c_str(), nullptr, nullptr);
    if (uri) {
        g_app_info_launch_default_for_uri(uri, nullptr, nullptr);
        g_free(uri);
    }
}

void applyFilter(const ViewPtr &view)
{
    const gchar *text = gtk_entry_get_text(GTK_ENTRY(view->filterBox));
    const std::string needle = text ? text : "";

    view->visible.clear();
    view->filtering = !needle.empty();

    if (view->filtering) {
        gchar *folded = g_utf8_casefold(needle.c_str(), -1);
        const std::string pattern = folded ? folded : needle;
        g_free(folded);

        // One pass: a node is visible if it matches, and every ancestor of a
        // visible node is visible too, so matches inside collapsed
        // directories still appear with the path that leads to them.
        for (const auto *node : view->tree.flat()) {
            gchar *haystack = g_utf8_casefold(node->fullPath.c_str(), -1);
            const bool hit = haystack
                          && std::string(haystack).find(pattern) != std::string::npos;
            g_free(haystack);
            if (!hit)
                continue;
            for (const auto *walk = node; walk && walk != view->tree.root();
                 walk = walk->parent) {
                if (!view->visible.insert(walk).second)
                    break;   // ancestors already marked by an earlier match
            }
        }
    }

    // Attach the filter only while it is doing something, and drop back to
    // the raw model when the box is cleared.
    GtkTreeModel *wanted = view->filtering ? view->filterModel
                                           : GTK_TREE_MODEL(view->model);
    if (gtk_tree_view_get_model(GTK_TREE_VIEW(view->view)) != wanted) {
        if (view->filtering)
            gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(view->filterModel));
        gtk_tree_view_set_model(GTK_TREE_VIEW(view->view), wanted);
    } else if (view->filtering) {
        gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(view->filterModel));
    }

    if (view->filtering && view->visible.size() < 500)
        gtk_tree_view_expand_all(GTK_TREE_VIEW(view->view));
    view->refreshStatus();
}

gboolean filterVisible(GtkTreeModel *, GtkTreeIter *iter, gpointer data)
{
    auto *view = static_cast<ArchiveView *>(data);
    if (!view->filtering)
        return TRUE;
    const auto *node = archive_tree_model_node(iter);
    return node && view->visible.count(node) > 0;
}

void showContextMenu(const ViewPtr &view, GdkEvent *event)
{
    const bool hasSelection = !selectedMembers(view).empty();

    GtkWidget *menu = gtk_menu_new();

    GtkWidget *open = gtk_menu_item_new_with_label("Open with default application");
    gtk_widget_set_sensitive(open, hasSelection);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), open);

    GtkWidget *extract = gtk_menu_item_new_with_label("Extract selection to…");
    gtk_widget_set_sensitive(extract, hasSelection);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), extract);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    GtkWidget *toggle = gtk_menu_item_new_with_label(
        archive_tree_model_get_flat(view->model) ? "Show as tree"
                                                 : "Show as flat list");
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), toggle);

    // The view is kept alive by g_instances; the raw pointer is safe for as
    // long as the menu can be clicked, and the handlers re-check.
    auto *raw = new ViewWeak(view->shared_from_this());
    g_object_set_data_full(G_OBJECT(menu), "archiveview",
                           raw, [](gpointer p) { delete static_cast<ViewWeak *>(p); });

    g_signal_connect(open, "activate", G_CALLBACK(+[](GtkMenuItem *item, gpointer) {
        auto *weak = static_cast<ViewWeak *>(g_object_get_data(
            G_OBJECT(gtk_widget_get_parent(GTK_WIDGET(item))), "archiveview"));
        if (ViewPtr v = weak ? weak->lock() : nullptr)
            onOpenSelection(v);
    }), nullptr);
    g_signal_connect(extract, "activate", G_CALLBACK(+[](GtkMenuItem *item, gpointer) {
        auto *weak = static_cast<ViewWeak *>(g_object_get_data(
            G_OBJECT(gtk_widget_get_parent(GTK_WIDGET(item))), "archiveview"));
        if (ViewPtr v = weak ? weak->lock() : nullptr)
            onExtractSelection(v);
    }), nullptr);
    g_signal_connect(toggle, "activate", G_CALLBACK(+[](GtkMenuItem *item, gpointer) {
        auto *weak = static_cast<ViewWeak *>(g_object_get_data(
            G_OBJECT(gtk_widget_get_parent(GTK_WIDGET(item))), "archiveview"));
        ViewPtr v = weak ? weak->lock() : nullptr;
        if (!v)
            return;
        const bool flat = !archive_tree_model_get_flat(v->model);
        // Swapping presentation renumbers every row, so detach the view,
        // change the model, and reattach rather than trying to patch paths.
        gtk_tree_view_set_model(GTK_TREE_VIEW(v->view), nullptr);
        archive_tree_model_set_flat(v->model, flat);
        gtk_tree_view_set_model(GTK_TREE_VIEW(v->view),
                                v->filtering ? v->filterModel
                                             : GTK_TREE_MODEL(v->model));
    }), nullptr);

    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), event);
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

    // GtkTreeModelFilter over our model, matching the Qt variant's
    // QSortFilterProxyModel. The visible-func is O(1) because applyFilter()
    // precomputes the admitted set.
    view->filterModel = gtk_tree_model_filter_new(GTK_TREE_MODEL(view->model), nullptr);
    gtk_tree_model_filter_set_visible_func(GTK_TREE_MODEL_FILTER(view->filterModel),
                                           filterVisible, view.get(), nullptr);

    // The view is attached to the *raw* model until a filter is actually
    // typed. GtkTreeModelFilter reindexes its level on every row-inserted,
    // which is quadratic across a scan: measured at ~1.1 s of overhead for
    // 10k rows, extrapolating past ten minutes for 100k. Filtering is a
    // deliberate, occasional act; paying for it continuously is not.
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

    // Members do not exist on disk, so a drag has to produce them. GTK asks
    // for the data via drag-data-get, which is where the extraction happens —
    // nothing is written merely by selecting rows.
    static const GtkTargetEntry dragTargets[] = {
        { const_cast<gchar *>("text/uri-list"), 0, 0 }
    };
    gtk_tree_view_enable_model_drag_source(treeView, GDK_BUTTON1_MASK,
                                           dragTargets, 1, GDK_ACTION_COPY);
    g_signal_connect(view->view, "drag-data-get",
                     G_CALLBACK(+[](GtkWidget *widget, GdkDragContext *,
                                    GtkSelectionData *data, guint, guint, gpointer) {
        ViewPtr v;
        for (const ViewPtr &candidate : g_instances) {
            if (candidate->view == widget) {
                v = candidate;
                break;
            }
        }
        if (!v || scratchDir(v).empty())
            return;

        const std::vector<std::string> members = selectedMembers(v);
        if (members.empty())
            return;

        const std::vector<std::string> written = extractMembers(v, members, v->scratch);
        if (written.empty())
            return;

        std::vector<gchar *> uris;
        uris.reserve(written.size() + 1);
        for (const std::string &file : written)
            uris.push_back(g_filename_to_uri(file.c_str(), nullptr, nullptr));
        uris.push_back(nullptr);
        gtk_selection_data_set_uris(data, uris.data());
        for (gchar *uri : uris)
            g_free(uri);
    }), nullptr);

    g_signal_connect(view->view, "button-press-event",
                     G_CALLBACK(+[](GtkWidget *widget, GdkEventButton *event,
                                    gpointer) -> gboolean {
        if (event->type != GDK_BUTTON_PRESS || event->button != GDK_BUTTON_SECONDARY)
            return FALSE;
        for (const ViewPtr &candidate : g_instances) {
            if (candidate->view == widget) {
                showContextMenu(candidate, reinterpret_cast<GdkEvent *>(event));
                return TRUE;
            }
        }
        return FALSE;
    }), nullptr);

    view->filterBox = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(view->filterBox), "Filter members…");
    gtk_entry_set_icon_from_icon_name(GTK_ENTRY(view->filterBox),
                                      GTK_ENTRY_ICON_SECONDARY, "edit-clear");
    gtk_widget_set_visible(view->filterBox, view->settings.showFilterBox);
    g_signal_connect(view->filterBox, "changed",
                     G_CALLBACK(+[](GtkEditable *entry, gpointer) {
        for (const ViewPtr &candidate : g_instances) {
            if (candidate->filterBox == GTK_WIDGET(entry)) {
                applyFilter(candidate);
                return;
            }
        }
    }), nullptr);
    g_signal_connect(view->filterBox, "icon-release",
                     G_CALLBACK(+[](GtkEntry *entry, GtkEntryIconPosition, GdkEvent *,
                                    gpointer) { gtk_entry_set_text(entry, ""); }),
                     nullptr);
    gtk_box_pack_start(GTK_BOX(view->root), view->filterBox, FALSE, FALSE, 2);

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

    if (view->settings.startFlat) {
        gtk_tree_view_set_model(GTK_TREE_VIEW(view->view), nullptr);
        archive_tree_model_set_flat(view->model, true);
        gtk_tree_view_set_model(GTK_TREE_VIEW(view->view), GTK_TREE_MODEL(view->model));
    }

    gtk_widget_show_all(view->root);
    // show_all would override the ini setting for the filter box.
    gtk_widget_set_visible(view->filterBox, view->settings.showFilterBox);
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
            // In flat mode the tree's per-node notifications describe rows
            // nobody is showing, so build silently and announce the run that
            // actually appeared in the list.
            v->tree.addEntries(batch, flat ? nullptr : v.get(),
                               archiveview::EntryTree::Mode::Immediate);
            if (flat) {
                for (int row = before; row < v->tree.entryCount(); ++row)
                    archive_tree_model_row_inserted_flat(v->model, row);
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

void removeTree(const std::string &directory)
{
    // Only ever called on a directory this plugin created with g_dir_make_tmp.
    GDir *dir = g_dir_open(directory.c_str(), 0, nullptr);
    if (!dir)
        return;
    while (const gchar *name = g_dir_read_name(dir)) {
        gchar *child = g_build_filename(directory.c_str(), name, nullptr);
        if (g_file_test(child, G_FILE_TEST_IS_DIR)
            && !g_file_test(child, G_FILE_TEST_IS_SYMLINK)) {
            removeTree(child);
        } else {
            g_remove(child);
        }
        g_free(child);
    }
    g_dir_close(dir);
    g_rmdir(directory.c_str());
}

void destroyInstance(const ViewPtr &view)
{
    view->dead.store(true, std::memory_order_relaxed);
    // Join before anything the callbacks touch goes away.
    view->scanner.cancelAndWait();
    view->extractor.cancelAndWait();
    if (!view->scratch.empty()) {
        removeTree(view->scratch);
        view->scratch.clear();
    }
    if (view->filterModel) {
        g_object_unref(view->filterModel);
        view->filterModel = nullptr;
    }
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
        "EXT=\"XPI\" | EXT=\"WHL\" | EXT=\"7Z\" | EXT=\"RAR\" | "
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
    view->visible.clear();
    view->filtering = false;
    if (view->filterBox)
        gtk_entry_set_text(GTK_ENTRY(view->filterBox), "");
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
