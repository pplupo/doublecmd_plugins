#pragma once

#include <gtk/gtk.h>

#include "core/EntryTree.h"

/// Custom GtkTreeModel over archiveview::EntryTree — NOT a GtkTreeStore.
///
/// A GtkTreeStore copies every cell of every row into itself, which for a
/// 100k-entry archive means hundreds of thousands of GValues duplicating data
/// the EntryTree already holds. That is the same mistake the predecessor made
/// with QTableWidget, in a different toolkit; this exposes the tree virtually
/// instead, the role ArchiveModel plays on the Qt side.
///
/// Iterators carry an EntryTree::Node* in user_data. EntryTree guarantees
/// node pointers are stable for its lifetime, so GTK_TREE_MODEL_ITERS_PERSIST
/// is honest here.
///
/// Implemented as a classic manual GObject type (GTypeInfo plus a
/// GtkTreeModelIface vtable) rather than via G_DEFINE_TYPE, following
/// logview's LogTreeModel: this header is consumed from C++ and a plain
/// function-based registration keeps that unambiguous.

#define ARCHIVE_TYPE_TREE_MODEL (archive_tree_model_get_type())
#define ARCHIVE_TREE_MODEL(obj) \
    (G_TYPE_CHECK_INSTANCE_CAST((obj), ARCHIVE_TYPE_TREE_MODEL, ArchiveTreeModel))
#define ARCHIVE_IS_TREE_MODEL(obj) \
    (G_TYPE_CHECK_INSTANCE_TYPE((obj), ARCHIVE_TYPE_TREE_MODEL))

/// Column order matches the Qt variant's ArchiveModel::Column so the two
/// listings read identically.
enum {
    ARCHIVE_COL_LOCK = 0,   ///< icon name, or "" when not encrypted
    ARCHIVE_COL_NAME,
    ARCHIVE_COL_SIZE,
    ARCHIVE_COL_PACKED,
    ARCHIVE_COL_RATIO,
    ARCHIVE_COL_CRC,
    ARCHIVE_COL_MODIFIED,
    ARCHIVE_COL_MODE,
    ARCHIVE_COL_OWNER,
    ARCHIVE_COL_LINK,
    ARCHIVE_N_COLUMNS
};

struct ArchiveTreeModel {
    GObject parent_instance;
    archiveview::EntryTree *tree = nullptr;   ///< not owned
    bool flat = false;
    int stamp = 0;   ///< bumped on structural change to invalidate old iters
};

struct ArchiveTreeModelClass {
    GObjectClass parent_class;
};

GType archive_tree_model_get_type();

/// `tree` must outlive the model and is not owned by it.
ArchiveTreeModel *archive_tree_model_new(archiveview::EntryTree *tree);

/// Announce rows [first, first+count) appearing under `parent` (nullptr for
/// the root). Called from EntryTree's listener, on the UI thread.
void archive_tree_model_rows_inserted(ArchiveTreeModel *model,
                                      const archiveview::EntryTree::Node *parent,
                                      int first, int count);

/// Tree vs flat presentation. Resets observers, since every row's path changes.
void archive_tree_model_set_flat(ArchiveTreeModel *model, bool flat);
bool archive_tree_model_get_flat(ArchiveTreeModel *model);

/// The node behind an iter, or nullptr.
const archiveview::EntryTree::Node *archive_tree_model_node(GtkTreeIter *iter);
