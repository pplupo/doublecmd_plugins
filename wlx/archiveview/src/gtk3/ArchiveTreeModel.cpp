#include "ArchiveTreeModel.h"

#include "core/EntryFormat.h"

using archiveview::EntryTree;

namespace {

EntryTree::Node *nodeOf(GtkTreeIter *iter)
{
    return static_cast<EntryTree::Node *>(iter->user_data);
}

/// The children of `parent`, honouring flat mode.
///
/// In flat mode the model is a list: the root's "children" are every
/// entry-bearing node in arrival order, and nothing has children of its own.
const std::vector<EntryTree::Node *> *childrenOf(ArchiveTreeModel *model,
                                                 const EntryTree::Node *parent)
{
    if (model->flat) {
        static const std::vector<EntryTree::Node *> none;
        return parent ? &none : &model->tree->flat();
    }
    return &(parent ? parent : model->tree->root())->children;
}

GtkTreeModelFlags getFlags(GtkTreeModel *)
{
    // Not LIST_ONLY: this is a real tree in the default presentation.
    // ITERS_PERSIST is honest because EntryTree never moves a node.
    return GTK_TREE_MODEL_ITERS_PERSIST;
}

gint getNColumns(GtkTreeModel *)
{
    return ARCHIVE_N_COLUMNS;
}

GType getColumnType(GtkTreeModel *, gint)
{
    return G_TYPE_STRING;
}

gboolean getIter(GtkTreeModel *treeModel, GtkTreeIter *iter, GtkTreePath *path)
{
    auto *model = ARCHIVE_TREE_MODEL(treeModel);
    if (!model->tree)
        return FALSE;

    const gint depth = gtk_tree_path_get_depth(path);
    const gint *indices = gtk_tree_path_get_indices(path);
    if (depth <= 0)
        return FALSE;

    const EntryTree::Node *node = nullptr;
    for (gint level = 0; level < depth; ++level) {
        const auto *siblings = childrenOf(model, node);
        const gint row = indices[level];
        if (row < 0 || row >= static_cast<gint>(siblings->size()))
            return FALSE;
        node = (*siblings)[static_cast<size_t>(row)];
    }

    iter->stamp = model->stamp;
    iter->user_data = const_cast<EntryTree::Node *>(node);
    iter->user_data2 = nullptr;
    iter->user_data3 = nullptr;
    return TRUE;
}

GtkTreePath *getPath(GtkTreeModel *treeModel, GtkTreeIter *iter)
{
    auto *model = ARCHIVE_TREE_MODEL(treeModel);
    const EntryTree::Node *node = nodeOf(iter);
    if (!node)
        return nullptr;

    GtkTreePath *path = gtk_tree_path_new();
    if (model->flat) {
        // Flat rows are indexed by arrival order, which the node does not
        // know; the row field is its position among its tree siblings.
        const auto &flat = model->tree->flat();
        for (size_t row = 0; row < flat.size(); ++row) {
            if (flat[row] == node) {
                gtk_tree_path_prepend_index(path, static_cast<gint>(row));
                break;
            }
        }
        return path;
    }

    for (const EntryTree::Node *walk = node;
         walk && walk != model->tree->root();
         walk = walk->parent) {
        gtk_tree_path_prepend_index(path, walk->row);
    }
    return path;
}

void getValue(GtkTreeModel *treeModel, GtkTreeIter *iter, gint column, GValue *value)
{
    auto *model = ARCHIVE_TREE_MODEL(treeModel);
    g_value_init(value, G_TYPE_STRING);

    const EntryTree::Node *node = nodeOf(iter);
    if (!node) {
        g_value_set_string(value, "");
        return;
    }

    // A synthesised directory has no entry record; every column but the name
    // is genuinely unknown for it, and blank beats a fabricated zero.
    const bool known = node->hasEntry;
    const archiveview::Entry &entry = node->entry;
    std::string text;

    switch (column) {
    case ARCHIVE_COL_LOCK:
        if (known && (entry.encrypted || entry.metadataEncrypted))
            text = "changes-prevent";   // icon name for the cell renderer
        break;
    case ARCHIVE_COL_NAME:
        text = model->flat ? node->fullPath : node->name;
        break;
    case ARCHIVE_COL_SIZE:
        if (known && !entry.isDir())
            text = archiveview::format::size(entry.size);
        break;
    case ARCHIVE_COL_PACKED:
        if (known && !entry.isDir())
            text = archiveview::format::size(entry.compressedSize);
        break;
    case ARCHIVE_COL_RATIO:
        if (known && !entry.isDir())
            text = archiveview::format::ratio(entry.compressedSize, entry.size);
        break;
    case ARCHIVE_COL_CRC:
        if (known && !entry.isDir())
            text = archiveview::format::crc(entry.crc32, entry.hasCrc);
        break;
    case ARCHIVE_COL_MODIFIED:
        if (known)
            text = archiveview::format::modified(entry.modified, entry.hasModified);
        break;
    case ARCHIVE_COL_MODE:
        if (known && entry.mode)
            text = archiveview::format::mode(entry.mode, entry.type);
        break;
    case ARCHIVE_COL_OWNER:
        if (known)
            text = archiveview::format::owner(entry);
        break;
    case ARCHIVE_COL_LINK:
        if (known)
            text = entry.linkTarget;
        break;
    default:
        break;
    }

    g_value_set_string(value, text.c_str());
}

gboolean iterNext(GtkTreeModel *treeModel, GtkTreeIter *iter)
{
    auto *model = ARCHIVE_TREE_MODEL(treeModel);
    const EntryTree::Node *node = nodeOf(iter);
    if (!node)
        return FALSE;

    if (model->flat) {
        const auto &flat = model->tree->flat();
        for (size_t row = 0; row + 1 < flat.size(); ++row) {
            if (flat[row] == node) {
                iter->user_data = flat[row + 1];
                return TRUE;
            }
        }
        return FALSE;
    }

    const EntryTree::Node *parent = node->parent;
    const auto *siblings = childrenOf(model, parent == model->tree->root()
                                                 ? nullptr : parent);
    const int next = node->row + 1;
    if (next >= static_cast<int>(siblings->size()))
        return FALSE;
    iter->user_data = (*siblings)[static_cast<size_t>(next)];
    return TRUE;
}

gboolean iterNthChild(GtkTreeModel *treeModel, GtkTreeIter *iter,
                      GtkTreeIter *parent, gint n)
{
    auto *model = ARCHIVE_TREE_MODEL(treeModel);
    if (!model->tree)
        return FALSE;

    const EntryTree::Node *parentNode = parent ? nodeOf(parent) : nullptr;
    const auto *children = childrenOf(model, parentNode);
    if (n < 0 || n >= static_cast<gint>(children->size()))
        return FALSE;

    iter->stamp = model->stamp;
    iter->user_data = (*children)[static_cast<size_t>(n)];
    iter->user_data2 = nullptr;
    iter->user_data3 = nullptr;
    return TRUE;
}

gboolean iterChildren(GtkTreeModel *treeModel, GtkTreeIter *iter, GtkTreeIter *parent)
{
    return iterNthChild(treeModel, iter, parent, 0);
}

gboolean iterHasChild(GtkTreeModel *treeModel, GtkTreeIter *iter)
{
    auto *model = ARCHIVE_TREE_MODEL(treeModel);
    if (model->flat)
        return FALSE;
    const EntryTree::Node *node = nodeOf(iter);
    return node && !node->children.empty();
}

gint iterNChildren(GtkTreeModel *treeModel, GtkTreeIter *iter)
{
    auto *model = ARCHIVE_TREE_MODEL(treeModel);
    if (!model->tree)
        return 0;
    const EntryTree::Node *node = iter ? nodeOf(iter) : nullptr;
    return static_cast<gint>(childrenOf(model, node)->size());
}

gboolean iterParent(GtkTreeModel *treeModel, GtkTreeIter *iter, GtkTreeIter *child)
{
    auto *model = ARCHIVE_TREE_MODEL(treeModel);
    if (model->flat)
        return FALSE;

    const EntryTree::Node *node = nodeOf(child);
    if (!node || !node->parent || node->parent == model->tree->root())
        return FALSE;

    iter->stamp = model->stamp;
    iter->user_data = node->parent;
    iter->user_data2 = nullptr;
    iter->user_data3 = nullptr;
    return TRUE;
}

// These take GObject's own signatures rather than the tidier typed ones, so
// no function-pointer casts are needed. Casting between incompatible function
// types is undefined behaviour that happens to work, and -Wcast-function-type
// is right to complain about it.
void treeModelInit(void *ifacePtr, void *)
{
    auto *iface = static_cast<GtkTreeModelIface *>(ifacePtr);
    iface->get_flags = getFlags;
    iface->get_n_columns = getNColumns;
    iface->get_column_type = getColumnType;
    iface->get_iter = getIter;
    iface->get_path = getPath;
    iface->get_value = getValue;
    iface->iter_next = iterNext;
    iface->iter_children = iterChildren;
    iface->iter_has_child = iterHasChild;
    iface->iter_n_children = iterNChildren;
    iface->iter_nth_child = iterNthChild;
    iface->iter_parent = iterParent;
}

void instanceInit(GTypeInstance *instance, void *)
{
    auto *model = reinterpret_cast<ArchiveTreeModel *>(instance);
    model->tree = nullptr;
    model->flat = false;
    model->stamp = g_random_int();
}

void classInit(void *, void *)
{
}

} // namespace

GType archive_tree_model_get_type()
{
    static GType type = 0;
    if (type == 0) {
        static const GTypeInfo info = {
            sizeof(ArchiveTreeModelClass), nullptr, nullptr,
            classInit, nullptr, nullptr,
            sizeof(ArchiveTreeModel), 0,
            instanceInit, nullptr
        };
        static const GInterfaceInfo treeModelInfo = {
            treeModelInit, nullptr, nullptr
        };
        type = g_type_register_static(G_TYPE_OBJECT, "ArchiveTreeModel", &info,
                                      GTypeFlags(0));
        g_type_add_interface_static(type, GTK_TYPE_TREE_MODEL, &treeModelInfo);
    }
    return type;
}

ArchiveTreeModel *archive_tree_model_new(archiveview::EntryTree *tree)
{
    auto *model = ARCHIVE_TREE_MODEL(g_object_new(ARCHIVE_TYPE_TREE_MODEL, nullptr));
    model->tree = tree;
    return model;
}

void archive_tree_model_row_inserted(ArchiveTreeModel *model,
                                     const archiveview::EntryTree::Node *node)
{
    if (!model || !model->tree || !node || model->flat)
        return;

    GtkTreeIter iter;
    iter.stamp = model->stamp;
    iter.user_data = const_cast<archiveview::EntryTree::Node *>(node);
    iter.user_data2 = nullptr;
    iter.user_data3 = nullptr;

    GtkTreePath *path = getPath(GTK_TREE_MODEL(model), &iter);
    if (!path)
        return;
    gtk_tree_model_row_inserted(GTK_TREE_MODEL(model), path, &iter);
    gtk_tree_path_free(path);

    // A directory that just gained its first child has to be re-announced as
    // expandable, or GtkTreeView will never draw an expander for it.
    const archiveview::EntryTree::Node *parent = node->parent;
    if (parent && parent != model->tree->root() && parent->children.size() == 1) {
        GtkTreeIter parentIter;
        parentIter.stamp = model->stamp;
        parentIter.user_data = const_cast<archiveview::EntryTree::Node *>(parent);
        parentIter.user_data2 = nullptr;
        parentIter.user_data3 = nullptr;
        GtkTreePath *parentPath = getPath(GTK_TREE_MODEL(model), &parentIter);
        if (parentPath) {
            gtk_tree_model_row_has_child_toggled(GTK_TREE_MODEL(model),
                                                 parentPath, &parentIter);
            gtk_tree_path_free(parentPath);
        }
    }
}

void archive_tree_model_row_inserted_flat(ArchiveTreeModel *model, int index)
{
    if (!model || !model->tree || !model->flat)
        return;
    const auto &flat = model->tree->flat();
    if (index < 0 || index >= static_cast<int>(flat.size()))
        return;

    GtkTreeIter iter;
    iter.stamp = model->stamp;
    iter.user_data = flat[static_cast<size_t>(index)];
    iter.user_data2 = nullptr;
    iter.user_data3 = nullptr;

    GtkTreePath *path = gtk_tree_path_new_from_indices(index, -1);
    gtk_tree_model_row_inserted(GTK_TREE_MODEL(model), path, &iter);
    gtk_tree_path_free(path);
}

void archive_tree_model_set_flat(ArchiveTreeModel *model, bool flat)
{
    if (!model || model->flat == flat)
        return;
    model->flat = flat;
    // Every row's path changes, so old iterators must not be reused.
    model->stamp = g_random_int();
}

bool archive_tree_model_get_flat(ArchiveTreeModel *model)
{
    return model && model->flat;
}

const archiveview::EntryTree::Node *archive_tree_model_node(GtkTreeIter *iter)
{
    return iter ? nodeOf(iter) : nullptr;
}
