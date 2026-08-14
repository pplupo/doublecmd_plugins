#include "LogTreeModel.h"
#include <cstring>

namespace {

void log_tree_model_tree_model_init(GtkTreeModelIface *iface);

} // namespace

G_DEFINE_TYPE_WITH_CODE(LogTreeModel, log_tree_model, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE(GTK_TYPE_TREE_MODEL, log_tree_model_tree_model_init))

static void log_tree_model_init(LogTreeModel *self)
{
    self->engine = nullptr;
    self->filter = nullptr;
    self->stamp = g_random_int();
}

static void log_tree_model_class_init(LogTreeModelClass *)
{
    // No properties/signals of our own; GtkTreeModelIface carries all the
    // behavior (installed via log_tree_model_tree_model_init below).
}

LogTreeModel *log_tree_model_new(LogEngine *engine)
{
    auto *model = LOG_TREE_MODEL(g_object_new(LOG_TYPE_TREE_MODEL, nullptr));
    model->engine = engine;
    return model;
}

int log_tree_model_to_engine_row(LogTreeModel *model, int modelRow)
{
    if (model->filter) {
        if (modelRow < 0 || modelRow >= (int)model->filter->size()) return -1;
        return (*model->filter)[modelRow];
    }
    return modelRow;
}

static int modelRowCount(LogTreeModel *model)
{
    if (model->filter) return (int)model->filter->size();
    return model->engine ? model->engine->lineCount() : 0;
}

void log_tree_model_set_filter(LogTreeModel *model, std::vector<int> *filter)
{
    model->filter = filter;
    model->stamp++;
    GtkTreePath *path = gtk_tree_path_new();
    gtk_tree_model_row_deleted(GTK_TREE_MODEL(model), path); // not fully correct per-row, but forces a full redraw
    gtk_tree_path_free(path);
    log_tree_model_rows_changed(model);
}

void log_tree_model_rows_changed(LogTreeModel *model)
{
    // Simplest correct approach for a full reset: emit nothing here and
    // let the caller use gtk_tree_view_set_model() again, OR emit
    // row-inserted for the whole new range. We take the latter: signal a
    // full-range "rows-inserted" so views pick up the new size. Callers
    // that truly reset from scratch should prefer rebuilding the model.
    int count = modelRowCount(model);
    if (count > 0)
        log_tree_model_rows_inserted(model, 0, count - 1);
}

void log_tree_model_rows_inserted(LogTreeModel *model, int firstRow, int lastRow)
{
    for (int r = firstRow; r <= lastRow; ++r) {
        GtkTreePath *path = gtk_tree_path_new_from_indices(r, -1);
        GtkTreeIter iter;
        iter.stamp = model->stamp;
        iter.user_data = GINT_TO_POINTER(r);
        gtk_tree_model_row_inserted(GTK_TREE_MODEL(model), path, &iter);
        gtk_tree_path_free(path);
    }
}

// ── GtkTreeModel vtable ─────────────────────────────────────────────

namespace {

GtkTreeModelFlags tm_get_flags(GtkTreeModel *) { return GTK_TREE_MODEL_LIST_ONLY; }

gint tm_get_n_columns(GtkTreeModel *) { return LOG_TREE_MODEL_N_COLUMNS; }

GType tm_get_column_type(GtkTreeModel *, gint) { return G_TYPE_STRING; }

gboolean tm_get_iter(GtkTreeModel *treeModel, GtkTreeIter *iter, GtkTreePath *path)
{
    auto *model = LOG_TREE_MODEL(treeModel);
    if (gtk_tree_path_get_depth(path) != 1) return FALSE;
    gint row = gtk_tree_path_get_indices(path)[0];
    if (row < 0 || row >= modelRowCount(model)) return FALSE;

    iter->stamp = model->stamp;
    iter->user_data = GINT_TO_POINTER(row);
    return TRUE;
}

GtkTreePath *tm_get_path(GtkTreeModel *treeModel, GtkTreeIter *iter)
{
    auto *model = LOG_TREE_MODEL(treeModel);
    if (iter->stamp != model->stamp) return nullptr;
    return gtk_tree_path_new_from_indices(GPOINTER_TO_INT(iter->user_data), -1);
}

void tm_get_value(GtkTreeModel *treeModel, GtkTreeIter *iter, gint column, GValue *value)
{
    auto *model = LOG_TREE_MODEL(treeModel);
    g_value_init(value, G_TYPE_STRING);
    if (iter->stamp != model->stamp || !model->engine) return;

    int modelRow = GPOINTER_TO_INT(iter->user_data);
    int engineRow = log_tree_model_to_engine_row(model, modelRow);
    if (column == LOG_TREE_MODEL_COL_TEXT && engineRow >= 0) {
        std::string text = model->engine->lineText(engineRow);
        g_value_set_string(value, text.c_str());
    }
}

gboolean tm_iter_next(GtkTreeModel *treeModel, GtkTreeIter *iter)
{
    auto *model = LOG_TREE_MODEL(treeModel);
    if (iter->stamp != model->stamp) return FALSE;
    int row = GPOINTER_TO_INT(iter->user_data) + 1;
    if (row >= modelRowCount(model)) return FALSE;
    iter->user_data = GINT_TO_POINTER(row);
    return TRUE;
}

gboolean tm_iter_children(GtkTreeModel *treeModel, GtkTreeIter *iter, GtkTreeIter *parent)
{
    auto *model = LOG_TREE_MODEL(treeModel);
    if (parent) return FALSE;
    if (modelRowCount(model) == 0) return FALSE;
    iter->stamp = model->stamp;
    iter->user_data = GINT_TO_POINTER(0);
    return TRUE;
}

gboolean tm_iter_has_child(GtkTreeModel *, GtkTreeIter *) { return FALSE; }

gint tm_iter_n_children(GtkTreeModel *treeModel, GtkTreeIter *iter)
{
    auto *model = LOG_TREE_MODEL(treeModel);
    if (iter) return 0;
    return modelRowCount(model);
}

gboolean tm_iter_nth_child(GtkTreeModel *treeModel, GtkTreeIter *iter, GtkTreeIter *parent, gint n)
{
    auto *model = LOG_TREE_MODEL(treeModel);
    if (parent) return FALSE;
    if (n < 0 || n >= modelRowCount(model)) return FALSE;
    iter->stamp = model->stamp;
    iter->user_data = GINT_TO_POINTER(n);
    return TRUE;
}

gboolean tm_iter_parent(GtkTreeModel *, GtkTreeIter *, GtkTreeIter *) { return FALSE; }

void log_tree_model_tree_model_init(GtkTreeModelIface *iface)
{
    iface->get_flags = tm_get_flags;
    iface->get_n_columns = tm_get_n_columns;
    iface->get_column_type = tm_get_column_type;
    iface->get_iter = tm_get_iter;
    iface->get_path = tm_get_path;
    iface->get_value = tm_get_value;
    iface->iter_next = tm_iter_next;
    iface->iter_children = tm_iter_children;
    iface->iter_has_child = tm_iter_has_child;
    iface->iter_n_children = tm_iter_n_children;
    iface->iter_nth_child = tm_iter_nth_child;
    iface->iter_parent = tm_iter_parent;
}

} // namespace
