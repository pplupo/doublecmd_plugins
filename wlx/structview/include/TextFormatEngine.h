#pragma once

#include <QString>
#include <QByteArray>
#include <QVariant>
#include <QVector>
#include <QStringList>
#include <QList>
#include <memory>

/// A node in the document tree.
///
/// Each engine builds a tree of DocumentNode objects representing the
/// hierarchical structure of the file. The StructViewWidget displays
/// this tree in a QTreeView (left panel) and shows the selected node's
/// grid data in a QTableView (right panel).
///
/// Grid data (columnNames + rows) represents this node's immediate children
/// that are displayable as a table. Container children become tree nodes
/// instead of grid rows.
class DocumentNode {
public:
    explicit DocumentNode(const QString &name, DocumentNode *parent = nullptr)
        : name(name), parent(parent) {}

    ~DocumentNode() { qDeleteAll(children); }

    QString name;                            ///< Display name in tree
    DocumentNode *parent = nullptr;
    QList<DocumentNode*> children;

    // Grid data for this node
    QStringList columnNames;                 ///< Column headers
    QVector<QVector<QVariant>> rows;         ///< Row data

    bool editable = true;                    ///< Whether grid data can be edited

    /// Add a child node and return pointer to it.
    DocumentNode *addChild(const QString &childName) {
        auto *child = new DocumentNode(childName, this);
        children.append(child);
        return child;
    }

    /// Remove and delete a child by index.
    void removeChild(int index) {
        if (index >= 0 && index < children.size()) {
            delete children.takeAt(index);
        }
    }

    /// Position in parent's children list, or -1 if root.
    int childIndex() const {
        if (!parent) return -1;
        return parent->children.indexOf(const_cast<DocumentNode*>(this));
    }

    /// True if node has no children and no grid data.
    bool isLeaf() const {
        return children.isEmpty() && rows.isEmpty();
    }

    /// True if node has child nodes (not just grid rows).
    bool isContainer() const {
        return !children.isEmpty();
    }
};

/// Abstract base class for structured text format engines.
///
/// Each engine:
///  1. Parses raw file bytes into a DocumentNode tree
///  2. Provides raw text for the read-only Text tab
///  3. Serializes the tree back to bytes for saving
///
/// The factory method createForFile() inspects the file extension and
/// returns the appropriate engine.
class TextFormatEngine {
public:
    virtual ~TextFormatEngine() = default;

    /// Parse raw bytes and build the document tree.
    virtual bool parse(const QByteArray &data) = 0;

    /// Root of the parsed document tree.
    virtual DocumentNode *rootNode() const = 0;

    /// Serialize the entire tree back to bytes for saving.
    /// Must reconstruct from tree structure + any grid edits.
    virtual QByteArray serialize() const = 0;

    /// Pretty-printed raw text for the read-only Text tab.
    virtual QString rawText() const = 0;

    /// Human-readable format name (e.g. "JSON", "XML").
    virtual QString formatName() const = 0;

    /// Factory: detect format from file extension and return the right engine.
    static std::unique_ptr<TextFormatEngine> createForFile(const QString &filepath);

    /// Description of why the last parse() failed (empty if it succeeded).
    QString errorMessage() const { return m_errorMessage; }

    /// 1-based line the failure was reported at, or -1 if the format gave
    /// no position. Column is -1 when only a line is known.
    int errorLine() const { return m_errorLine; }
    int errorColumn() const { return m_errorColumn; }

protected:
    /// Record why parse() failed. Engines that know a position pass it in;
    /// byte offsets are converted to a line/column by offsetToLineColumn().
    void setError(const QString &message, int line = -1, int column = -1) {
        m_errorMessage = message;
        m_errorLine = line;
        m_errorColumn = column;
    }

    /// Convert a byte offset into the 1-based line/column it falls on.
    static void offsetToLineColumn(const QByteArray &data, int offset,
                                   int *line, int *column) {
        int l = 1, c = 1;
        const int end = qMin(offset, static_cast<int>(data.size()));
        for (int i = 0; i < end; ++i) {
            if (data.at(i) == '\n') { ++l; c = 1; } else { ++c; }
        }
        *line = l;
        *column = c;
    }

private:
    QString m_errorMessage;
    int m_errorLine = -1;
    int m_errorColumn = -1;
};
