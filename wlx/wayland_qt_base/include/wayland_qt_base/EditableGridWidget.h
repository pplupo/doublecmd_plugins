#pragma once

#include <QWidget>
#include <QTableWidget>
#include <QUndoStack>
#include <QHeaderView>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QPointer>
#include <QSet>

namespace QtWlPlugin {

class FocusManager;

/// Custom delegate that wraps text at any character (not just word boundaries).
class WrapAnywhereDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override;
    void setWrapAnywhere(bool wrap);
    bool wrapAnywhere() const;

private:
    bool m_wrap = false;
};

/// A QTableWidget wrapper with full undo/redo, keyboard navigation,
/// drag-to-move columns/rows, context menus, and editing support.
///
/// This is format-agnostic — it provides no file I/O, no parsing, no
/// encoding detection. The consumer plugin loads data into the table
/// and uses the grid's operations for editing.
class EditableGridWidget : public QWidget {
    Q_OBJECT
public:
    explicit EditableGridWidget(FocusManager *fm, QWidget *parent = nullptr);

    QTableWidget *tableWidget() const;
    QUndoStack *undoStack() const;

    // --- Data operations (format-agnostic) ---
    void copySelection(char separator = '\t');
    QString getSelectionAsText(char separator = '\t');
    void pasteSelection();
    void pasteSelectionAt(int atRow);
    void insertRows(int count, int atRow);
    void deleteSelectedRows();

    void copyColumnSelection(char separator = '\t');
    void pasteColumnSelectionAt(int atCol);
    void insertColumns(int count, int atCol);
    void deleteSelectedColumns();

    // --- Appearance ---
    void setWordWrap(bool wrap);
    bool wordWrap() const;
    void setShowGrid(bool show);

    // --- State ---
    bool isDirty() const;

signals:
    void dirtyChanged(bool dirty);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    void setupTable();
    void registerShortcuts();
    void setupDragToMove();
    void showRowContextMenu(const QPoint &pos);
    void showColumnContextMenu(const QPoint &pos);
    void onSortByColumn(int column);
    void onItemChanged(QTableWidgetItem *item);
    void updateRowNumbers();
    bool isSectionSelected(QHeaderView *header, int logicalIndex) const;

    FocusManager *m_fm;
    QTableWidget *m_view;
    QUndoStack *m_undoStack;
    WrapAnywhereDelegate *m_wrapDelegate;
    bool m_isProgrammaticChange;

    // Sort state
    int m_lastSortColumn;
    Qt::SortOrder m_lastSortOrder;

    // Drag-to-move state
    QHeaderView *m_dragHeader;
    int m_dragLogicalIndex;
    QList<int> m_dragBeforeOrder;
    QSet<int> m_dragSelectedSections;
    bool m_isDraggingSection;
    QTimer *m_moveDebounceTimer;
};

} // namespace QtWlPlugin
