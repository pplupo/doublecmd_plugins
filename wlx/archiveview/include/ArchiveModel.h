#pragma once

#include <QAbstractItemModel>
#include <QHash>
#include <QIcon>

#include "core/ArchiveEntry.h"
#include "core/EntryTree.h"

/// Qt view onto archiveview::EntryTree.
///
/// Replaces the QTableWidget approach outright: seven heap QTableWidgetItems
/// per member does not survive a 100k-entry archive, and a flat list of
/// "a/b/c/d/file.txt" strings is unusable at that size regardless of how it
/// is stored.
///
/// The hierarchy itself lives in the core so the GTK3 variant builds exactly
/// the same one — synthesised directories, separate rows for duplicate paths,
/// absolute paths left absolute. Those are decisions about what the user is
/// told about an archive, not presentation details, and must not differ
/// between toolkits.
class ArchiveModel : public QAbstractItemModel,
                     private archiveview::EntryTree::Listener {
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
    void appendEntries(const archiveview::EntryBatch &batch);
    void clearEntries();

    bool isFlat() const { return m_flat; }
    /// Switching representation resets the model — the row identity of every
    /// index changes, so there is no incremental form of this.
    void setFlat(bool flat);

    /// The entry behind an index, or nullptr for a synthesised directory.
    const archiveview::Entry *entryAt(const QModelIndex &index) const;
    /// Full in-archive path for an index, empty if invalid.
    QString pathAt(const QModelIndex &index) const;

    /// An index onto the `row`th entry in arrival order, independent of
    /// whether the model is currently presenting a tree or a flat list.
    /// Used for printing and export, which are always flat.
    QModelIndex flatIndex(int row, int column) const;

    int entryCount() const { return m_tree.entryCount(); }
    /// Members whose path was already claimed by an earlier member.
    int duplicateCount() const { return m_tree.duplicateCount(); }

    // --- QAbstractItemModel ---
    QModelIndex index(int row, int column,
                      const QModelIndex &parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

private:
    using Node = archiveview::EntryTree::Node;

    // --- EntryTree::Listener: core insertions become Qt notifications ------
    void beforeInsert(const Node *parent, int first, int count) override;
    void afterInsert(const Node *parent, int first, int count) override;

    Node *nodeFor(const QModelIndex &index) const;
    QModelIndex indexForNode(const Node *node, int column = 0) const;

    QVariant displayText(const Node *node, int column) const;
    QIcon iconFor(const Node *node) const;

    archiveview::EntryTree m_tree;
    mutable QHash<QString, QIcon> m_iconCache;
    bool m_flat = false;
};
