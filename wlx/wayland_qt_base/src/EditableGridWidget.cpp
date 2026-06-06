#include <wayland_qt_base/EditableGridWidget.h>
#include <wayland_qt_base/FocusManager.h>

#include <QApplication>
#include <QClipboard>
#include <QVBoxLayout>
#include <QMenu>
#include <QPainter>
#include <QTextOption>
#include <QTextDocument>
#include <QAbstractTextDocumentLayout>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QAbstractItemDelegate>
#include <QRegularExpression>
#include <QMessageBox>
#include <algorithm>

namespace QtWlPlugin {

// ---------------------------------------------------------------------------
// Undo Commands (internal, format-agnostic — no CSV quoting)
// ---------------------------------------------------------------------------

class EditCellCommand : public QUndoCommand {
public:
    EditCellCommand(QTableWidget *view, int row, int col,
                    const QString &oldText, const QString &newText,
                    QUndoCommand *parent = nullptr)
        : QUndoCommand(parent), m_view(view), m_row(row), m_col(col),
          m_oldText(oldText), m_newText(newText) {
        setText(QString("Edit cell (%1, %2)").arg(row).arg(col));
    }
    void undo() override {
        m_view->blockSignals(true);
        if (auto *item = m_view->item(m_row, m_col)) item->setText(m_oldText);
        m_view->blockSignals(false);
    }
    void redo() override {
        m_view->blockSignals(true);
        if (auto *item = m_view->item(m_row, m_col)) item->setText(m_newText);
        m_view->blockSignals(false);
    }
private:
    QTableWidget *m_view;
    int m_row, m_col;
    QString m_oldText, m_newText;
};

class RowColCommand : public QUndoCommand {
public:
    RowColCommand(QTableWidget *view, int index, int count, bool isRow, bool isInsert,
                  QUndoCommand *parent = nullptr)
        : QUndoCommand(parent), m_view(view), m_index(index), m_count(count),
          m_isRow(isRow), m_isInsert(isInsert) {
        setText(QString("%1 %2 %3(s)").arg(isInsert ? "Insert" : "Delete")
                .arg(count).arg(isRow ? "row" : "col"));
        if (!isInsert) {
            for (int i = 0; i < count; ++i) {
                QStringList list;
                int limit = isRow ? view->columnCount() : view->rowCount();
                for (int j = 0; j < limit; ++j) {
                    auto *item = isRow ? view->item(index + i, j) : view->item(j, index + i);
                    list << (item ? item->text() : "");
                }
                m_data << list;
            }
        } else {
            for (int i = 0; i < count; ++i) {
                QStringList list;
                int limit = isRow ? view->columnCount() : view->rowCount();
                for (int j = 0; j < limit; ++j) list << "";
                m_data << list;
            }
        }
    }
    void undo() override { if (m_isInsert) applyDelete(); else applyInsert(); }
    void redo() override { if (m_isInsert) applyInsert(); else applyDelete(); }

private:
    void applyInsert() {
        m_view->blockSignals(true);
        for (int i = 0; i < m_count; ++i) {
            if (m_isRow) m_view->insertRow(m_index + i);
            else m_view->insertColumn(m_index + i);
            QStringList list = i < m_data.size() ? m_data[i] : QStringList();
            int limit = m_isRow ? m_view->columnCount() : m_view->rowCount();
            for (int j = 0; j < limit; ++j) {
                QString text = j < list.size() ? list[j] : "";
                auto *item = new QTableWidgetItem(text);
                item->setToolTip(text);
                item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsEditable);
                if (m_isRow) m_view->setItem(m_index + i, j, item);
                else m_view->setItem(j, m_index + i, item);
            }
        }
        m_view->blockSignals(false);
    }
    void applyDelete() {
        m_view->blockSignals(true);
        for (int i = m_count - 1; i >= 0; --i) {
            if (m_isRow) m_view->removeRow(m_index + i);
            else m_view->removeColumn(m_index + i);
        }
        m_view->blockSignals(false);
    }
    QTableWidget *m_view;
    int m_index, m_count;
    QList<QStringList> m_data;
    bool m_isRow, m_isInsert;
};

class DataSnapshotCommand : public QUndoCommand {
public:
    DataSnapshotCommand(QTableWidget *view, const QList<QStringList> &before,
                        const QList<QStringList> &after, const QString &text)
        : m_view(view), m_before(before), m_after(after), m_first(true) { setText(text); }
    void undo() override { restore(m_before); }
    void redo() override { if (m_first) { m_first = false; return; } restore(m_after); }
private:
    void restore(const QList<QStringList> &data) {
        m_view->blockSignals(true);
        for (int r = 0; r < data.size() && r < m_view->rowCount(); ++r)
            for (int c = 0; c < data[r].size() && c < m_view->columnCount(); ++c)
                if (auto *item = m_view->item(r, c)) item->setText(data[r][c]);
        m_view->blockSignals(false);
    }
    QTableWidget *m_view;
    QList<QStringList> m_before, m_after;
    bool m_first;
};

class SectionMoveCommand : public QUndoCommand {
public:
    SectionMoveCommand(QHeaderView *header, const QList<int> &beforeOrder,
                       const QList<int> &afterOrder, const QString &text)
        : m_header(header), m_before(beforeOrder), m_after(afterOrder), m_first(true) { setText(text); }
    void undo() override { restore(m_before); }
    void redo() override { if (m_first) { m_first = false; return; } restore(m_after); }
private:
    void restore(const QList<int> &order) {
        for (int target = 0; target < order.size(); ++target) {
            int logical = order[target];
            int currentVisual = m_header->visualIndex(logical);
            if (currentVisual != target)
                m_header->moveSection(currentVisual, target);
        }
    }
    QHeaderView *m_header;
    QList<int> m_before, m_after;
    bool m_first;
};

// ---------------------------------------------------------------------------
// WrapAnywhereDelegate
// ---------------------------------------------------------------------------

void WrapAnywhereDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                                  const QModelIndex &index) const {
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    QString text = opt.text;
    opt.text.clear();
    QApplication::style()->drawControl(QStyle::CE_ItemViewItem, &opt, painter);

    if (!text.isEmpty()) {
        painter->save();
        QRect textRect = QApplication::style()->subElementRect(QStyle::SE_ItemViewItemText, &opt);
        painter->setClipRect(textRect);
        painter->setFont(opt.font);
        QTextOption textOption;
        textOption.setWrapMode(m_wrap ? QTextOption::WrapAnywhere : QTextOption::NoWrap);
        textOption.setAlignment(opt.displayAlignment);
        if (opt.state & QStyle::State_Selected)
            painter->setPen(opt.palette.color(QPalette::HighlightedText));
        else
            painter->setPen(opt.palette.color(QPalette::Text));
        painter->drawText(textRect, text, textOption);
        painter->restore();
    }
}

QSize WrapAnywhereDelegate::sizeHint(const QStyleOptionViewItem &option,
                                      const QModelIndex &index) const {
    if (!m_wrap) return QStyledItemDelegate::sizeHint(option, index);
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    QRect textRect = QApplication::style()->subElementRect(QStyle::SE_ItemViewItemText, &opt);
    int width = textRect.width();
    if (width <= 0) width = opt.rect.width();
    QTextDocument doc;
    doc.setDefaultFont(opt.font);
    QTextOption textOption;
    textOption.setWrapMode(QTextOption::WrapAnywhere);
    doc.setDefaultTextOption(textOption);
    doc.setTextWidth(width);
    doc.setPlainText(opt.text);
    return QSize(width, qMax((int)doc.size().height(), opt.fontMetrics.height()));
}

void WrapAnywhereDelegate::setWrapAnywhere(bool wrap) { m_wrap = wrap; }
bool WrapAnywhereDelegate::wrapAnywhere() const { return m_wrap; }

// ---------------------------------------------------------------------------
// EditableGridWidget
// ---------------------------------------------------------------------------

EditableGridWidget::EditableGridWidget(FocusManager *fm, QWidget *parent)
    : QWidget(parent)
    , m_fm(fm)
    , m_isProgrammaticChange(false)
    , m_lastSortColumn(-1)
    , m_lastSortOrder(Qt::AscendingOrder)
    , m_dragHeader(nullptr)
    , m_dragLogicalIndex(-1)
    , m_isDraggingSection(false)
{
    setFocusPolicy(Qt::NoFocus);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    setupTable();
    layout->addWidget(m_view);

    m_undoStack = new QUndoStack(this);
    fm->setUndoStack(m_undoStack);

    connect(m_undoStack, &QUndoStack::cleanChanged, this, [this](bool clean) {
        emit dirtyChanged(!clean);
    });
    connect(m_undoStack, &QUndoStack::indexChanged, this, [this]() { updateRowNumbers(); });
    connect(m_view, &QTableWidget::itemChanged, this, &EditableGridWidget::onItemChanged);

    // Stash old text when entering a cell editor
    connect(fm, &FocusManager::inputWidgetEntered, this, [this](QWidget *) {
        if (auto *item = m_view->currentItem()) {
            if (!item->data(Qt::UserRole).isValid()) {
                m_isProgrammaticChange = true;
                item->setData(Qt::UserRole, item->text());
                m_isProgrammaticChange = false;
            }
        }
    });

    registerShortcuts();
    setupDragToMove();
}

void EditableGridWidget::setupTable()
{
    m_view = new QTableWidget(this);
    m_view->setFocusPolicy(Qt::ClickFocus);
    m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);

    m_view->horizontalHeader()->setSectionsClickable(true);
    m_view->horizontalHeader()->setHighlightSections(true);
    m_view->horizontalHeader()->setSectionsMovable(false);
    m_view->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
    m_view->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);

    m_view->verticalHeader()->setSectionsClickable(true);
    m_view->verticalHeader()->setHighlightSections(true);
    m_view->verticalHeader()->setSectionsMovable(false);
    m_view->verticalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
    m_view->verticalHeader()->setSectionResizeMode(QHeaderView::Interactive);

    m_view->setSortingEnabled(false);

    m_wrapDelegate = new WrapAnywhereDelegate(m_view);
    m_view->setItemDelegate(m_wrapDelegate);

    m_view->setContextMenuPolicy(Qt::CustomContextMenu);

    connect(m_view, &QWidget::customContextMenuRequested, this,
            [this](const QPoint &pos) { showRowContextMenu(pos); });
    connect(m_view->verticalHeader(), &QWidget::customContextMenuRequested, this,
            [this](const QPoint &pos) { showRowContextMenu(pos); });
    connect(m_view->horizontalHeader(), &QWidget::customContextMenuRequested, this,
            [this](const QPoint &pos) { showColumnContextMenu(pos); });
    connect(m_view->horizontalHeader(), &QHeaderView::sectionClicked, this,
            &EditableGridWidget::onSortByColumn);

    // Install event filter on header viewports for drag-to-move
    m_view->horizontalHeader()->viewport()->installEventFilter(this);
    m_view->verticalHeader()->viewport()->installEventFilter(this);
    // Install on view itself for in-editor key handling
    m_view->installEventFilter(this);
}

void EditableGridWidget::registerShortcuts()
{
    // Ctrl+C → copy
    m_fm->registerShortcut(QKeySequence(Qt::CTRL | Qt::Key_C), FocusManager::WhenNoInput,
        [this]() { copySelection('\t'); return true; });

    // Ctrl+V → paste
    m_fm->registerShortcut(QKeySequence(Qt::CTRL | Qt::Key_V), FocusManager::WhenNoInput,
        [this]() { pasteSelection(); return true; });

    // Delete → delete selected rows
    m_fm->registerShortcut(QKeySequence(Qt::Key_Delete), FocusManager::WhenNoInput,
        [this]() { deleteSelectedRows(); return true; });

    // Enter/Return → edit current cell
    m_fm->registerShortcut(QKeySequence(Qt::Key_Return), FocusManager::WhenNoInput,
        [this]() {
            if (m_view->currentItem()) { m_view->editItem(m_view->currentItem()); return true; }
            return false;
        });
    m_fm->registerShortcut(QKeySequence(Qt::Key_Enter), FocusManager::WhenNoInput,
        [this]() {
            if (m_view->currentItem()) { m_view->editItem(m_view->currentItem()); return true; }
            return false;
        });

    // Arrow keys with right-wrap
    auto arrowHandler = [this](int key) -> bool {
        int visualCol = m_view->horizontalHeader()->visualIndex(m_view->currentIndex().column());
        int visualRow = m_view->verticalHeader()->visualIndex(m_view->currentIndex().row());
        if (key == Qt::Key_Up) visualRow--;
        if (key == Qt::Key_Down) visualRow++;
        if (key == Qt::Key_Left) {
            visualCol--;
            if (visualCol < 0 && visualRow > 0) {
                visualCol = m_view->columnCount() - 1;
                visualRow--;
            }
        }
        if (key == Qt::Key_Right) {
            visualCol++;
            if (visualCol >= m_view->columnCount() && visualRow < m_view->rowCount() - 1) {
                visualCol = 0;
                visualRow++;
            }
        }
        visualRow = qBound(0, visualRow, m_view->rowCount() - 1);
        visualCol = qBound(0, visualCol, m_view->columnCount() - 1);
        int r = m_view->verticalHeader()->logicalIndex(visualRow);
        int c = m_view->horizontalHeader()->logicalIndex(visualCol);
        m_view->setCurrentCell(r, c);
        return true;
    };

    m_fm->registerShortcut(QKeySequence(Qt::Key_Up), FocusManager::WhenNoInput,
        [=]() { return arrowHandler(Qt::Key_Up); });
    m_fm->registerShortcut(QKeySequence(Qt::Key_Down), FocusManager::WhenNoInput,
        [=]() { return arrowHandler(Qt::Key_Down); });
    m_fm->registerShortcut(QKeySequence(Qt::Key_Left), FocusManager::WhenNoInput,
        [=]() { return arrowHandler(Qt::Key_Left); });
    m_fm->registerShortcut(QKeySequence(Qt::Key_Right), FocusManager::WhenNoInput,
        [=]() { return arrowHandler(Qt::Key_Right); });
}

void EditableGridWidget::setupDragToMove()
{
    m_moveDebounceTimer = new QTimer(this);
    m_moveDebounceTimer->setSingleShot(true);
    m_moveDebounceTimer->setInterval(0);

    connect(m_moveDebounceTimer, &QTimer::timeout, this, [this]() {
        if (!m_isDraggingSection || !m_dragHeader) return;

        int newVisual = m_dragHeader->visualIndex(m_dragLogicalIndex);
        bool anyMoved = (newVisual != m_dragBeforeOrder.indexOf(m_dragLogicalIndex));

        if (anyMoved) {
            bool isHorizontal = (m_dragHeader == m_view->horizontalHeader());
            QList<int> currentOrder;
            for (int v = 0; v < m_dragHeader->count(); ++v)
                currentOrder.append(m_dragHeader->logicalIndex(v));

            QList<int> nonSelected;
            for (int li : currentOrder)
                if (!m_dragSelectedSections.contains(li)) nonSelected.append(li);

            QList<int> selectedInOrder;
            for (int li : m_dragBeforeOrder)
                if (m_dragSelectedSections.contains(li)) selectedInOrder.append(li);

            int insertIdx = qBound(0, newVisual, nonSelected.size());
            QList<int> targetOrder;
            for (int i = 0; i < insertIdx; ++i) targetOrder.append(nonSelected[i]);
            for (int li : selectedInOrder) targetOrder.append(li);
            for (int i = insertIdx; i < nonSelected.size(); ++i) targetOrder.append(nonSelected[i]);

            for (int v = 0; v < targetOrder.size(); ++v) {
                int logical = targetOrder[v];
                int curVisual = m_dragHeader->visualIndex(logical);
                if (curVisual != v) m_dragHeader->moveSection(curVisual, v);
            }

            QList<int> afterOrder;
            for (int v = 0; v < m_dragHeader->count(); ++v)
                afterOrder.append(m_dragHeader->logicalIndex(v));
            m_undoStack->push(new SectionMoveCommand(m_dragHeader, m_dragBeforeOrder, afterOrder,
                isHorizontal ? "Move columns" : "Move rows"));
            updateRowNumbers();
        }

        m_isDraggingSection = false;
        m_dragHeader->setSectionsMovable(false);
        m_dragHeader = nullptr;
    });

    auto connectMoveDebounce = [this](QHeaderView *header) {
        connect(header, &QHeaderView::sectionMoved, this, [this](int, int, int) {
            if (m_isDraggingSection) m_moveDebounceTimer->start();
        });
    };
    connectMoveDebounce(m_view->horizontalHeader());
    connectMoveDebounce(m_view->verticalHeader());
}

bool EditableGridWidget::eventFilter(QObject *obj, QEvent *event)
{
    QHeaderView *hHeader = m_view->horizontalHeader();
    QHeaderView *vHeader = m_view->verticalHeader();

    // --- Header viewport: drag-to-move ---
    if (event->type() == QEvent::MouseButtonPress) {
        if (obj == hHeader->viewport() || obj == vHeader->viewport()) {
            QHeaderView *header = (obj == hHeader->viewport()) ? hHeader : vHeader;
            auto *me = static_cast<QMouseEvent *>(event);
            int logicalIndex = header->logicalIndexAt(me->pos());
            if (logicalIndex >= 0 && isSectionSelected(header, logicalIndex)) {
                header->setSectionsMovable(true);
                m_isDraggingSection = true;
                m_dragHeader = header;
                m_dragLogicalIndex = logicalIndex;
                m_dragBeforeOrder.clear();
                for (int v = 0; v < header->count(); ++v)
                    m_dragBeforeOrder.append(header->logicalIndex(v));

                bool isHorizontal = (header == hHeader);
                m_dragSelectedSections.clear();
                QModelIndexList sel = m_view->selectionModel()->selectedIndexes();
                for (const QModelIndex &idx : sel) {
                    int li = isHorizontal ? idx.column() : idx.row();
                    m_dragSelectedSections.insert(li);
                }

                QItemSelection savedSel = m_view->selectionModel()->selection();
                QTimer::singleShot(0, this, [this, savedSel]() {
                    if (m_isDraggingSection)
                        m_view->selectionModel()->select(savedSel, QItemSelectionModel::ClearAndSelect);
                });
            } else {
                header->setSectionsMovable(false);
            }
        }
    }

    // --- In-editor key handling ---
    if (event->type() == QEvent::KeyPress && obj == m_view && m_fm->activeInput()) {
        auto *ke = static_cast<QKeyEvent *>(event);

        if (ke->key() == Qt::Key_Escape) {
            if (auto *item = m_view->currentItem()) {
                QVariant oldData = item->data(Qt::UserRole);
                if (oldData.isValid()) {
                    m_isProgrammaticChange = true;
                    item->setText(oldData.toString());
                    item->setData(Qt::UserRole, QVariant());
                    m_isProgrammaticChange = false;
                }
            }
            m_view->closePersistentEditor(m_view->currentItem());
            return true;
        }

        if (ke->key() == Qt::Key_Up || ke->key() == Qt::Key_Down) {
            QModelIndex current = m_view->currentIndex();
            int r = current.row(), c = current.column();
            if (ke->key() == Qt::Key_Up) r--;
            if (ke->key() == Qt::Key_Down) r++;
            r = qBound(0, r, m_view->rowCount() - 1);

            // Cancel current edit without saving
            if (auto *item = m_view->currentItem()) {
                QVariant oldData = item->data(Qt::UserRole);
                if (oldData.isValid()) {
                    m_isProgrammaticChange = true;
                    item->setText(oldData.toString());
                    item->setData(Qt::UserRole, QVariant());
                    m_isProgrammaticChange = false;
                }
            }
            m_view->closePersistentEditor(m_view->currentItem());
            m_view->setCurrentCell(r, c);
            m_view->editItem(m_view->item(r, c));
            return true;
        }

        if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
            QModelIndex current = m_view->currentIndex();
            int r = current.row(), c = current.column();

            // Commit via delegate
            QAbstractItemDelegate *delegate = m_view->itemDelegateForIndex(current);
            QWidget *editor = m_fm->activeInput();
            if (delegate && editor)
                delegate->setModelData(editor, m_view->model(), current);

            // Navigate right (wrap to next row)
            int visualCol = m_view->horizontalHeader()->visualIndex(c);
            int visualRow = m_view->verticalHeader()->visualIndex(r);
            visualCol++;
            if (visualCol >= m_view->columnCount()) {
                visualCol = 0;
                visualRow++;
            }
            if (visualRow < m_view->rowCount()) {
                int nr = m_view->verticalHeader()->logicalIndex(visualRow);
                int nc = m_view->horizontalHeader()->logicalIndex(visualCol);
                QTimer::singleShot(0, this, [this, nr, nc]() {
                    m_view->setCurrentCell(nr, nc);
                });
            }
            return true;
        }
    }

    return QWidget::eventFilter(obj, event);
}

QTableWidget *EditableGridWidget::tableWidget() const { return m_view; }
QUndoStack *EditableGridWidget::undoStack() const { return m_undoStack; }
bool EditableGridWidget::isDirty() const { return !m_undoStack->isClean(); }

void EditableGridWidget::setWordWrap(bool wrap)
{
    m_wrapDelegate->setWrapAnywhere(wrap);
    m_view->setWordWrap(wrap);
    if (wrap) {
        m_view->resizeRowsToContents();
    } else {
        m_view->verticalHeader()->setDefaultSectionSize(m_view->fontMetrics().height() + 8);
        m_view->resizeRowsToContents();
    }
}

bool EditableGridWidget::wordWrap() const { return m_wrapDelegate->wrapAnywhere(); }
void EditableGridWidget::setShowGrid(bool show) { m_view->setShowGrid(show); }

void EditableGridWidget::onItemChanged(QTableWidgetItem *item)
{
    if (m_isProgrammaticChange || !item) return;

    QVariant oldData = item->data(Qt::UserRole);
    if (!oldData.isValid()) return;

    QString oldText = oldData.toString();
    QString newText = item->text();

    m_isProgrammaticChange = true;
    item->setData(Qt::UserRole, QVariant());
    m_isProgrammaticChange = false;

    if (oldText != newText) {
        m_isProgrammaticChange = true;
        item->setText(oldText);
        m_isProgrammaticChange = false;
        m_undoStack->push(new EditCellCommand(m_view, item->row(), item->column(), oldText, newText));
    }
}

void EditableGridWidget::onSortByColumn(int column)
{
    if (column != m_lastSortColumn) {
        m_lastSortColumn = column;
        m_lastSortOrder = Qt::AscendingOrder;
        m_view->horizontalHeader()->setSortIndicator(-1, Qt::AscendingOrder);
        return;
    }

    QList<QStringList> beforeData;
    for (int r = 0; r < m_view->rowCount(); ++r) {
        QStringList row;
        for (int c = 0; c < m_view->columnCount(); ++c)
            row << (m_view->item(r, c) ? m_view->item(r, c)->text() : "");
        beforeData << row;
    }

    m_view->blockSignals(true);
    m_view->sortItems(column, m_lastSortOrder);
    m_view->blockSignals(false);

    QList<QStringList> afterData;
    for (int r = 0; r < m_view->rowCount(); ++r) {
        QStringList row;
        for (int c = 0; c < m_view->columnCount(); ++c)
            row << (m_view->item(r, c) ? m_view->item(r, c)->text() : "");
        afterData << row;
    }

    m_undoStack->push(new DataSnapshotCommand(m_view, beforeData, afterData, "Sort"));
    m_view->horizontalHeader()->setSortIndicatorShown(true);
    m_view->horizontalHeader()->setSortIndicator(column, m_lastSortOrder);
    m_lastSortOrder = (m_lastSortOrder == Qt::AscendingOrder) ? Qt::DescendingOrder : Qt::AscendingOrder;
}

void EditableGridWidget::updateRowNumbers()
{
    QHeaderView *vh = m_view->verticalHeader();
    m_view->blockSignals(true);
    for (int v = 0; v < m_view->rowCount(); ++v) {
        int logical = vh->logicalIndex(v);
        auto *item = m_view->verticalHeaderItem(logical);
        if (!item) {
            item = new QTableWidgetItem();
            m_view->setVerticalHeaderItem(logical, item);
        }
        item->setText(QString::number(v + 1));
    }
    m_view->blockSignals(false);
}

bool EditableGridWidget::isSectionSelected(QHeaderView *header, int logicalIndex) const
{
    QItemSelectionModel *sel = m_view->selectionModel();
    if (!sel) return false;
    bool isHorizontal = (header == m_view->horizontalHeader());
    if (isHorizontal) {
        for (int r = 0; r < m_view->rowCount(); ++r)
            if (!sel->isSelected(m_view->model()->index(r, logicalIndex))) return false;
        return m_view->rowCount() > 0;
    } else {
        for (int c = 0; c < m_view->columnCount(); ++c)
            if (!sel->isSelected(m_view->model()->index(logicalIndex, c))) return false;
        return m_view->columnCount() > 0;
    }
}

// ---------------------------------------------------------------------------
// Copy / Paste / Insert / Delete — Format-agnostic
// ---------------------------------------------------------------------------

void EditableGridWidget::copySelection(char separator)
{
    QString text = getSelectionAsText(separator);
    if (!text.isEmpty())
        QApplication::clipboard()->setText(text);
}

QString EditableGridWidget::getSelectionAsText(char separator)
{
    QModelIndexList sel = m_view->selectionModel()->selectedIndexes();
    if (sel.isEmpty()) return {};

    int minVRow = m_view->rowCount(), maxVRow = -1;
    int minVCol = m_view->columnCount(), maxVCol = -1;
    for (const auto &index : sel) {
        int vr = m_view->verticalHeader()->visualIndex(index.row());
        int vc = m_view->horizontalHeader()->visualIndex(index.column());
        if (vr < minVRow) minVRow = vr; if (vr > maxVRow) maxVRow = vr;
        if (vc < minVCol) minVCol = vc; if (vc > maxVCol) maxVCol = vc;
    }

    QString outText;
    for (int vr = minVRow; vr <= maxVRow; ++vr) {
        int r = m_view->verticalHeader()->logicalIndex(vr);
        QStringList rowItems;
        for (int vc = minVCol; vc <= maxVCol; ++vc) {
            int c = m_view->horizontalHeader()->logicalIndex(vc);
            QModelIndex idx = m_view->model()->index(r, c);
            QString cellText;
            if (m_view->selectionModel()->isSelected(idx)) {
                auto *item = m_view->item(r, c);
                cellText = item ? item->text() : "";
            }
            rowItems << cellText;
        }
        outText += rowItems.join(separator) + "\n";
    }
    return outText;
}

void EditableGridWidget::pasteSelection()
{
    int targetVisualRow = m_view->rowCount();
    QModelIndexList sel = m_view->selectionModel()->selectedIndexes();
    if (!sel.isEmpty()) {
        int minVRow = m_view->rowCount();
        for (const auto &index : sel) {
            int vr = m_view->verticalHeader()->visualIndex(index.row());
            if (vr < minVRow) minVRow = vr;
        }
        targetVisualRow = minVRow;
    }

    int endRow = m_view->rowCount();
    bool needsMove = (targetVisualRow < endRow);

    QList<int> beforeOrder;
    QHeaderView *vh = m_view->verticalHeader();
    if (needsMove) {
        for (int v = 0; v < vh->count(); ++v)
            beforeOrder.append(vh->logicalIndex(v));
    }

    if (needsMove) m_undoStack->beginMacro("Paste rows");
    pasteSelectionAt(endRow);

    int rowsInserted = m_view->rowCount() - endRow;
    if (rowsInserted > 0 && needsMove) {
        QList<int> midOrder;
        for (int v = 0; v < vh->count(); ++v) midOrder.append(vh->logicalIndex(v));

        for (int i = 0; i < rowsInserted; ++i) {
            int logicalRow = endRow + i;
            int curVisual = vh->visualIndex(logicalRow);
            vh->moveSection(curVisual, targetVisualRow + i);
        }

        QList<int> afterOrder;
        for (int v = 0; v < vh->count(); ++v) afterOrder.append(vh->logicalIndex(v));
        m_undoStack->push(new SectionMoveCommand(vh, midOrder, afterOrder, "Move pasted rows"));
        updateRowNumbers();
    }
    if (needsMove) m_undoStack->endMacro();
}

void EditableGridWidget::pasteSelectionAt(int atRow)
{
    int targetCols = m_view->columnCount();
    if (targetCols <= 0) return;

    QString text = QApplication::clipboard()->text();
    if (text.isEmpty()) return;

    QStringList lines = text.split(QRegularExpression("\r?\n"));
    if (!lines.isEmpty() && lines.last().isEmpty()) lines.removeLast();
    if (lines.isEmpty()) return;

    // Detect separator: try tab first, then comma
    char sep = '\t';
    QStringList testList = lines.first().split(QLatin1Char('\t'));
    if (testList.size() != targetCols) {
        QStringList commaTest = lines.first().split(QLatin1Char(','));
        if (commaTest.size() == targetCols) sep = ',';
        else if (testList.size() != targetCols) return;
    }

    int rowsToInsert = lines.size();
    m_isProgrammaticChange = true;
    m_undoStack->push(new RowColCommand(m_view, atRow, rowsToInsert, true, true));
    m_isProgrammaticChange = false;

    for (int i = 0; i < rowsToInsert; ++i) {
        QStringList list = lines.at(i).split(QLatin1Char(sep));
        for (int vc = 0; vc < targetCols; ++vc) {
            int c = m_view->horizontalHeader()->logicalIndex(vc);
            QString cellText = vc < list.size() ? list.at(vc).trimmed() : "";
            if (auto *item = m_view->item(atRow + i, c)) {
                m_isProgrammaticChange = true;
                item->setText(cellText);
                m_isProgrammaticChange = false;
            }
        }
    }
}

void EditableGridWidget::insertRows(int count, int atRow)
{
    if (m_view->columnCount() <= 0 || count <= 0) return;
    m_undoStack->push(new RowColCommand(m_view, atRow, count, true, true));
}

void EditableGridWidget::deleteSelectedRows()
{
    QModelIndexList sel = m_view->selectionModel()->selectedIndexes();
    if (sel.isEmpty()) return;

    QSet<int> rowsSet;
    for (const auto &index : sel) rowsSet.insert(index.row());
    QList<int> rowsToDelete = rowsSet.values();
    std::sort(rowsToDelete.begin(), rowsToDelete.end(), std::greater<int>());

    m_undoStack->beginMacro("Delete rows");
    for (int r : rowsToDelete)
        m_undoStack->push(new RowColCommand(m_view, r, 1, true, false));
    m_undoStack->endMacro();
}

void EditableGridWidget::copyColumnSelection(char separator)
{
    QModelIndexList sel = m_view->selectionModel()->selectedIndexes();
    if (sel.isEmpty()) return;

    int minCol = m_view->columnCount(), maxCol = -1;
    for (const auto &index : sel) {
        if (index.column() < minCol) minCol = index.column();
        if (index.column() > maxCol) maxCol = index.column();
    }

    QString outText;
    for (int r = 0; r < m_view->rowCount(); ++r) {
        QStringList rowItems;
        for (int c = minCol; c <= maxCol; ++c) {
            QString cellText = m_view->item(r, c) ? m_view->item(r, c)->text() : "";
            rowItems << cellText;
        }
        outText += rowItems.join(separator) + "\n";
    }
    QApplication::clipboard()->setText(outText);
}

void EditableGridWidget::pasteColumnSelectionAt(int atCol)
{
    QString text = QApplication::clipboard()->text();
    if (text.isEmpty()) return;

    QStringList lines = text.split(QRegularExpression("\r?\n"));
    if (!lines.isEmpty() && lines.last().isEmpty()) lines.removeLast();
    if (lines.isEmpty()) return;

    if (lines.size() != m_view->rowCount()) {
        QMessageBox::warning(this, "Paste Error",
            QString("Clipboard contains %1 rows, but table has %2.")
            .arg(lines.size()).arg(m_view->rowCount()));
        return;
    }

    char sep = '\t';
    int colsToInsert = lines.first().split(QLatin1Char('\t')).size();
    if (colsToInsert <= 1) {
        sep = ',';
        colsToInsert = lines.first().split(QLatin1Char(',')).size();
    }

    m_isProgrammaticChange = true;
    m_undoStack->push(new RowColCommand(m_view, atCol, colsToInsert, false, true));
    m_isProgrammaticChange = false;

    for (int r = 0; r < m_view->rowCount(); ++r) {
        QStringList list = lines.at(r).split(QLatin1Char(sep));
        for (int c = 0; c < colsToInsert; ++c) {
            QString cellText = c < list.size() ? list.at(c).trimmed() : "";
            if (auto *item = m_view->item(r, atCol + c)) {
                m_isProgrammaticChange = true;
                item->setText(cellText);
                m_isProgrammaticChange = false;
            }
        }
    }
}

void EditableGridWidget::insertColumns(int count, int atCol)
{
    if (m_view->rowCount() <= 0 || count <= 0) return;
    m_undoStack->push(new RowColCommand(m_view, atCol, count, false, true));
}

void EditableGridWidget::deleteSelectedColumns()
{
    QModelIndexList sel = m_view->selectionModel()->selectedIndexes();
    if (sel.isEmpty()) return;

    QSet<int> colsSet;
    for (const auto &index : sel) colsSet.insert(index.column());
    QList<int> colsToDelete = colsSet.values();
    std::sort(colsToDelete.begin(), colsToDelete.end(), std::greater<int>());

    m_undoStack->beginMacro("Delete cols");
    for (int c : colsToDelete)
        m_undoStack->push(new RowColCommand(m_view, c, 1, false, false));
    m_undoStack->endMacro();
}

// ---------------------------------------------------------------------------
// Context Menus
// ---------------------------------------------------------------------------

void EditableGridWidget::showRowContextMenu(const QPoint &pos)
{
    QMenu menu(this);
    QAction *actCopyTSV = nullptr, *actCopyCSV = nullptr, *actDelete = nullptr;
    QAction *actInsertAbove = nullptr, *actInsertBelow = nullptr;
    QAction *actPasteAbove = nullptr, *actPasteBelow = nullptr;

    int minRow = m_view->rowCount(), maxRow = -1, numRows = 0;
    QModelIndexList sel = m_view->selectionModel()->selectedIndexes();
    if (!sel.isEmpty()) {
        QSet<int> rows;
        for (const auto &index : sel) {
            rows.insert(index.row());
            if (index.row() < minRow) minRow = index.row();
            if (index.row() > maxRow) maxRow = index.row();
        }
        numRows = rows.size();
        actCopyTSV = menu.addAction("Copy Selection as TSV");
        actCopyCSV = menu.addAction("Copy Selection as CSV");
        menu.addSeparator();
        actDelete = menu.addAction("Delete Selected Rows");
    } else {
        int clickedRow = m_view->rowAt(pos.y());
        if (clickedRow >= 0) { minRow = maxRow = clickedRow; numRows = 1; }
    }

    if (numRows > 0) {
        menu.addSeparator();
        QString rowStr = (numRows == 1) ? "1 row" : QString("%1 rows").arg(numRows);
        actInsertAbove = menu.addAction(QString("Insert %1 above").arg(rowStr));
        actInsertBelow = menu.addAction(QString("Insert %1 below").arg(rowStr));
        if (!QApplication::clipboard()->text().isEmpty()) {
            menu.addSeparator();
            actPasteAbove = menu.addAction("Insert from Clipboard above");
            actPasteBelow = menu.addAction("Insert from Clipboard below");
        }
    }

    QAction *res = menu.exec(m_view->viewport()->mapToGlobal(pos));
    QTimer::singleShot(0, this, [this]() { m_fm->restoreViewFocus(); });
    if (!res) return;

    if (res == actCopyTSV) copySelection('\t');
    else if (res == actCopyCSV) copySelection(',');
    else if (res == actDelete) deleteSelectedRows();
    else if (res == actInsertAbove) insertRows(numRows, minRow);
    else if (res == actInsertBelow) insertRows(numRows, maxRow + 1);
    else if (res == actPasteAbove) pasteSelectionAt(minRow);
    else if (res == actPasteBelow) pasteSelectionAt(maxRow + 1);
}

void EditableGridWidget::showColumnContextMenu(const QPoint &pos)
{
    QMenu menu(this);
    QAction *actCopy = nullptr, *actDelete = nullptr;
    QAction *actInsertLeft = nullptr, *actInsertRight = nullptr;
    QAction *actPasteLeft = nullptr, *actPasteRight = nullptr;

    int minCol = m_view->columnCount(), maxCol = -1, numCols = 0;
    QModelIndexList sel = m_view->selectionModel()->selectedIndexes();
    if (!sel.isEmpty()) {
        QSet<int> cols;
        for (const auto &index : sel) {
            cols.insert(index.column());
            if (index.column() < minCol) minCol = index.column();
            if (index.column() > maxCol) maxCol = index.column();
        }
        numCols = cols.size();
        actCopy = menu.addAction("Copy Columns");
        menu.addSeparator();
        actDelete = menu.addAction("Delete Selected Columns");
    } else {
        int clickedCol = m_view->columnAt(pos.x());
        if (clickedCol >= 0) { minCol = maxCol = clickedCol; numCols = 1; }
    }

    if (numCols > 0) {
        menu.addSeparator();
        QString colStr = (numCols == 1) ? "1 col" : QString("%1 cols").arg(numCols);
        actInsertLeft = menu.addAction(QString("Insert %1 left").arg(colStr));
        actInsertRight = menu.addAction(QString("Insert %1 right").arg(colStr));
        if (!QApplication::clipboard()->text().isEmpty()) {
            menu.addSeparator();
            actPasteLeft = menu.addAction("Insert from Clipboard left");
            actPasteRight = menu.addAction("Insert from Clipboard right");
        }
    }

    QAction *res = menu.exec(m_view->horizontalHeader()->viewport()->mapToGlobal(pos));
    QTimer::singleShot(0, this, [this]() { m_fm->restoreViewFocus(); });
    if (!res) return;

    if (res == actCopy) copyColumnSelection('\t');
    else if (res == actDelete) deleteSelectedColumns();
    else if (res == actInsertLeft) insertColumns(numCols, minCol);
    else if (res == actInsertRight) insertColumns(numCols, maxCol + 1);
    else if (res == actPasteLeft) pasteColumnSelectionAt(minCol);
    else if (res == actPasteRight) pasteColumnSelectionAt(maxCol + 1);
}

} // namespace QtWlPlugin
