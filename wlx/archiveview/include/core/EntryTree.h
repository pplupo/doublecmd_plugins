#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/ArchiveEntry.h"

namespace archiveview {

/// The member hierarchy, built incrementally from scanner batches.
///
/// Shared by both UI variants rather than written twice. The rules encoded
/// here are not presentation details — they are decisions about what the user
/// is told about an archive:
///
///  * Directories are synthesised from member paths, so an archive that
///    stores no explicit directory entries (common for zip) still gets a
///    tree.
///  * A member whose path is already taken gets its **own row** rather than
///    being folded into the first. A duplicate path is a known trick for
///    showing one file and extracting another; hiding it would be wrong.
///  * An absolute member path keeps its leading slash and sits at the top
///    level, so it stays visibly absolute instead of being quietly rehomed.
///
/// Node pointers are stable for the lifetime of the tree: nodes are owned by
/// a deque-like vector of unique_ptr, never reallocated in place. Both the Qt
/// model (QModelIndex::internalPointer) and the GTK model (GtkTreeIter's
/// user_data) rely on that.
class EntryTree {
public:
    struct Node {
        std::string name;      ///< last path component
        std::string fullPath;
        Node *parent = nullptr;
        std::vector<Node *> children;
        int row = 0;           ///< index within parent->children
        bool isDir = false;
        bool attached = false;    ///< already announced to the UI
        bool hasEntry = false;    ///< false for a synthesised directory
        bool isDuplicate = false; ///< another member claimed the same path
        Entry entry;
    };

    /// Bracket around each group of sibling insertions.
    ///
    /// Qt requires beginInsertRows() *before* the model mutates; GTK emits
    /// row-inserted *after*. Bracketing satisfies both from one traversal,
    /// which is why this is a listener rather than a list of insertions
    /// returned after the fact.
    class Listener {
    public:
        virtual ~Listener() = default;
        /// `parent` is nullptr for the root. Rows [first, first+count) are
        /// about to appear under it. Grouped mode only.
        virtual void beforeInsert(const Node *parent, int first, int count) = 0;
        virtual void afterInsert(const Node *parent, int first, int count) = 0;
        /// Called immediately after a single node becomes reachable.
        /// Immediate mode only.
        virtual void nodeAttached(const Node *node) { (void)node; }
    };

    /// How insertions are announced.
    ///
    /// Grouped is what Qt wants: build the batch's new nodes detached, then
    /// attach each sibling run inside one beginInsertRows/endInsertRows pair,
    /// so a 512-entry batch costs a handful of notifications.
    ///
    /// Immediate is what GTK requires. A GtkTreeModel must not expose a row
    /// before it has announced it — GtkTreeModelFilter builds its level cache
    /// from row-inserted signals and independently enumerates what the model
    /// already contains, so any row that exists before its signal is counted
    /// twice. Measured on a fixture with 100 nested directories: 110 real
    /// rows reported as 217. Immediate mode attaches and announces one node
    /// at a time, parent before child, so the two views never disagree.
    enum class Mode { Grouped, Immediate };

    EntryTree();

    /// Add a batch, announcing each sibling group through `listener`
    /// (which may be null).
    ///
    /// New nodes whose parent is already visible are built detached and
    /// attached in groups, so a 512-entry batch produces a handful of
    /// notifications rather than 512. Nodes created underneath a node that is
    /// itself new ride along inside their ancestor's insertion and are never
    /// announced separately.
    void addEntries(const EntryBatch &batch, Listener *listener = nullptr,
                    Mode mode = Mode::Grouped);

    void clear();

    const Node *root() const { return &m_root; }
    Node *root() { return &m_root; }

    /// Entry-bearing nodes in arrival order — the flat view, and the order
    /// printing and export use.
    const std::vector<Node *> &flat() const { return m_flat; }

    /// Every entry-bearing member at `roots` or beneath them.
    ///
    /// Selecting a directory means everything under it, which is what every
    /// other file manager does. Resolved here, against the whole tree, rather
    /// than by walking the view's rows — a view walk gets three things wrong:
    /// it descends only column-0 indexes (Qt gives selections in the name
    /// column), it sees nothing beneath a row in flat mode, and it silently
    /// skips children a live filter is hiding. Extraction should not depend
    /// on what happens to be on screen.
    std::vector<std::string> membersUnder(const std::vector<std::string> &roots) const;

    int entryCount() const { return static_cast<int>(m_flat.size()); }
    int duplicateCount() const { return m_duplicateCount; }

private:
    using Pending = std::unordered_map<Node *, std::vector<Node *>>;

    Node *makeNode(const std::string &name, const std::string &fullPath,
                   Node *parent, bool registerPath);
    struct Sink {
        Pending *pending = nullptr;    ///< null in immediate mode
        Listener *listener = nullptr;
    };

    Node *ensureNode(const std::string &path, bool isDir, Sink *sink);
    Node *addDuplicate(Node *original, Sink *sink);
    void hold(Node *node, Node *parent, Sink *sink);
    void flush(Sink *sink);

    std::vector<std::unique_ptr<Node>> m_storage;
    Node m_root;
    std::unordered_map<std::string, Node *> m_byPath;
    std::vector<Node *> m_flat;
    int m_duplicateCount = 0;
};

} // namespace archiveview
