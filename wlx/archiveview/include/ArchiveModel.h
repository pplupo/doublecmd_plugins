#pragma once

#include <QAbstractItemModel>
#include <QHash>
#include <QIcon>
#include <QVector>

#include <memory>
#include <vector>

#include "ArchiveEntry.h"

/// Tree/flat model over archive members, fed incrementally by ArchiveScanner.
///
/// Replaces the QTableWidget approach outright: seven heap QTableWidgetItems
/// per member does not survive a 100k-entry archive, and a flat list of
/// "a/b/c/d/file.txt" strings is unusable at that size regardless of how it
/// is stored.
///
/// Directories are synthesised from member paths, so an archive that stores
/// no explicit directory entries (common for zip) still gets a tree.
class ArchiveModel : public QAbstractItemModel {
    Q_OBJECT
public:
    enum Column {
        LockColumn,      ///< encryption indicator; narrow, leftmost
        NameColumn,
        SizeColumn,
        PackedColumn,
        RatioColumn,
        CrcColumn,
        ModifiedColumn,
        ModeColumn,
        OwnerColumn,
        LinkColumn,
        ColumnCount
    };

    explicit ArchiveModel(QObject *parent = nullptr);
    ~ArchiveModel() override;

    /// Insert a batch of entries produced by the scanner.
    void appendEntries(const ArchiveEntryBatch &batch);
    void clearEntries();

    bool isFlat() const { return m_flat; }
    /// Switching representation resets the model — the row identity of every
    /// index changes, so there is no incremental form of this.
    void setFlat(bool flat);

    /// The entry behind an index, or nullptr for a synthesised directory.
    const ArchiveEntry *entryAt(const QModelIndex &index) const;
    /// Full in-archive path for an index, empty if invalid.
    QString pathAt(const QModelIndex &index) const;

    /// An index onto the `row`th entry in arrival order, independent of
    /// whether the model is currently presenting a tree or a flat list.
    /// Used for printing and export, which are always flat.
    QModelIndex flatIndex(int row, int column) const;

    int entryCount() const { return static_cast<int>(m_flatNodes.size()); }
    /// Members whose path was already claimed by an earlier member.
    int duplicateCount() const { return m_duplicateCount; }

    // --- QAbstractItemModel ---
    QModelIndex index(int row, int column,
                      const QModelIndex &parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role) const override;

private:
    struct Node {
        QString name;      ///< last path component
        QString fullPath;
        Node *parent = nullptr;
        QVector<Node *> children;
        int row = 0;
        bool isDir = false;
        bool hasEntry = false;    ///< false for a synthesised directory
        bool attached = false;    ///< already visible to the view
        bool isDuplicate = false; ///< another member claimed the same path
        ArchiveEntry entry;
    };

    Node *nodeFor(const QModelIndex &index) const;
    QModelIndex indexForNode(Node *node, int column = 0) const;
    Node *makeNode(const QString &name, const QString &fullPath, Node *parent,
                   bool registerPath);
    void attachNode(Node *node, Node *parent,
                    QHash<Node *, QVector<Node *>> *pending);
    /// Give a repeated path its own row rather than folding it into the first.
    Node *addDuplicate(Node *original, QHash<Node *, QVector<Node *>> *pending);
    /// Find or create the node for `path`, creating missing ancestors.
    /// New nodes whose parent is already attached are left detached and
    /// recorded in `pending` so the caller can announce them in one go.
    Node *ensureNode(const QString &path, bool isDir,
                     QHash<Node *, QVector<Node *>> *pending);
    void attachPending(QHash<Node *, QVector<Node *>> *pending);

    QVariant displayText(const Node *node, int column) const;
    QIcon iconFor(const Node *node) const;

    std::vector<std::unique_ptr<Node>> m_storage;
    Node m_root;
    QHash<QString, Node *> m_byPath;
    std::vector<Node *> m_flatNodes;   ///< entry-bearing nodes, arrival order
    mutable QHash<QString, QIcon> m_iconCache;
    int m_duplicateCount = 0;
    bool m_flat = false;
};
