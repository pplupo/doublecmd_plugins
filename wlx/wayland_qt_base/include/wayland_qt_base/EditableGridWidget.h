#pragma once

#include <QWidget>
#include <QTableView>
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

/// A QTableView wrapper with full undo/redo, keyboard navigation,
/// drag-to-move columns/rows, context menus, and editing support.
///
/// Accepts a pre-instantiated QTableView* via dependency injection.
/// Because QTableWidget is a subclass of QTableView, you can inject either:
///
///   // Item-based convenience:
///   auto *grid = new EditableGridWidget(new QTableWidget(), fm, this);
///
///   // Model-based flexibility:
///   auto *sqlView = new QTableView();
///   sqlView->setModel(myModel);
///   auto *grid = new EditableGridWidget(sqlView, fm, this);
///
/// All data access goes through QAbstractItemModel — no QTableWidgetItem
/// dependency in the grid's own logic.
class EditableGridWidget : public QWidget {
    Q_OBJECT
public:
    /// Takes ownership of the view and parents it to this widget.
    explicit EditableGridWidget(QTableView *view, FocusManager *fm, QWidget *parent = nullptr);

    /// Access the underlying view (may be QTableView or QTableWidget).
    QTableView *view() const;
    QUndoStack *undoStack() const;

    // --- Data operations (format-agnostic, model-based) ---
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
    void setupView();
    void registerShortcuts();
    void setupDragToMove();
    void showRowContextMenu(const QPoint &pos);
    void showColumnContextMenu(const QPoint &pos);
    void onSortByColumn(int column);
    void onDataChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight,
                       const QList<int> &roles);
    void updateRowNumbers();
    bool isSectionSelected(QHeaderView *header, int logicalIndex) const;

    /// Helper: row/column count via the model
    int rowCount() const;
    int colCount() const;

    FocusManager *m_fm;
    QTableView *m_view;
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
