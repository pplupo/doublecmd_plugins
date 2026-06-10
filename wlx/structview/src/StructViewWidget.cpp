#include "StructViewWidget.h"

#include <wayland_qt_base/FocusManager.h>
#include <wayland_qt_base/PluginToolBar.h>
#include <wayland_qt_base/EditableGridWidget.h>
#include <wayland_qt_base/ScopedFindReplacePanel.h>
#include <wayland_qt_base/FilterableHeaderView.h>
#include <wayland_qt_base/PluginStatusBar.h>
#include <wayland_qt_base/PluginSplitView.h>
#include <wayland_qt_base/ThemeManager.h>
#include <wayland_qt_base/EncodingUtils.h>

#include <QVBoxLayout>
#include <QHeaderView>
#include <QTableView>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>

using namespace QtWlPlugin;

StructViewWidget::StructViewWidget(QWidget *parent)
    : QWidget(parent)
    , m_fm(nullptr)
{
    setupUi();

    // Apply saved theme
    ThemeManager::applyTheme(this, ThemeManager::currentTheme());
}

StructViewWidget::~StructViewWidget()
{
    // Disconnect all signals BEFORE child widgets start being destroyed.
    // Without this, Qt's arbitrary child destruction order can trigger
    // callbacks (e.g. selectionChanged, dataChanged) on half-destroyed objects,
    // causing crashes when the user navigates away with unsaved edits.
    if (m_treeView && m_treeView->selectionModel())
        disconnect(m_treeView->selectionModel(), nullptr, this, nullptr);
    if (m_gridModel)
        disconnect(m_gridModel, nullptr, this, nullptr);
    if (m_filterProxy)
        disconnect(m_filterProxy, nullptr, this, nullptr);
    if (m_fm)
        m_fm->setActive(false);
}

void StructViewWidget::setupUi()
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // --- Create the primary view and FocusManager FIRST ---
    // (setupToolbar needs m_fm for shortcut registration)
    m_gridView = new QTableView;
    m_gridModel = new QStandardItemModel(this);
    m_filterProxy = new QSortFilterProxyModel(this);
    m_filterProxy->setSourceModel(m_gridModel);
    m_filterProxy->setFilterCaseSensitivity(Qt::CaseInsensitive);

    // Install filterable header (must be before setModel)
    m_filterHeader = new FilterableHeaderView(Qt::Horizontal, m_gridView);
    m_filterHeader->setFilterEnabled(true);
    m_filterHeader->setStretchLastSection(true);
    m_gridView->setHorizontalHeader(m_filterHeader);

    m_gridView->setModel(m_filterProxy);
    m_gridView->setSortingEnabled(true);
    m_gridView->setAlternatingRowColors(true);
    m_gridView->setSelectionBehavior(QAbstractItemView::SelectItems);
    m_gridView->setSelectionMode(QAbstractItemView::ExtendedSelection);

    connect(m_filterHeader, &FilterableHeaderView::filterChanged, this,
            [this](int column, const QString &text) {
        if (m_filterProxy) {
            m_filterProxy->setFilterKeyColumn(column);
            m_filterProxy->setFilterFixedString(text);
            updateStatusBar();
        }
    });

    m_fm = new FocusManager(this, m_gridView, this);
    m_grid = new EditableGridWidget(m_gridView, GridMode::MemoryDocument, m_fm, this);

    // --- Toolbar ---
    setupToolbar();
    mainLayout->addWidget(m_toolbar);

    // --- Left panel: Tree view ---
    m_treeView = new QTreeView;
    m_treeModel = new QStandardItemModel(this);
    m_treeView->setModel(m_treeModel);
    m_treeView->setHeaderHidden(true);
    m_treeView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_treeView->setMinimumWidth(120);

    // --- Right panel: Tabs ---
    m_tabWidget = new QTabWidget;

    // Grid tab
    auto *gridContainer = new QWidget;
    auto *gridLayout = new QVBoxLayout(gridContainer);
    gridLayout->setContentsMargins(0, 0, 0, 0);
    gridLayout->setSpacing(0);

    m_grid->setThemeToggleEnabled(true);

    gridLayout->addWidget(m_grid);

    m_tabWidget->addTab(gridContainer, QStringLiteral("Grid"));

    // Text tab (read-only)
    m_textView = new QPlainTextEdit;
    m_textView->setReadOnly(true);
    m_textView->setFont(QFont(QStringLiteral("monospace"), 10));
    m_textView->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_tabWidget->addTab(m_textView, QStringLiteral("Text"));

    // --- Split view ---
    m_splitView = new PluginSplitView(m_treeView, m_tabWidget, this);
    mainLayout->addWidget(m_splitView, 1);

    // --- Find/replace ---
    setupFindReplace();
    mainLayout->addWidget(m_findReplace);

    // --- Status bar ---
    m_statusBar = new PluginStatusBar(this);
    mainLayout->addWidget(m_statusBar);

    // --- Connect tree selection ---
    connect(m_treeView->selectionModel(), &QItemSelectionModel::currentChanged,
            this, &StructViewWidget::onTreeNodeSelected);

}

void StructViewWidget::setupToolbar()
{
    m_toolbar = new PluginToolBar(m_fm, this);

    auto *actSave = m_toolbar->addToolAction(
        QStringLiteral("Save"), QKeySequence::Save, 0);
    connect(actSave, &QAction::triggered, this, &StructViewWidget::onSave);

    auto *actWrap = m_toolbar->addToolAction(
        QStringLiteral("Word Wrap"), QKeySequence(), 0);
    actWrap->setCheckable(true);
    connect(actWrap, &QAction::toggled, this, [this](bool on) {
        m_grid->setWordWrap(on);
    });

    auto *actGrid = m_toolbar->addToolAction(
        QStringLiteral("Grid Lines"), QKeySequence(), 0);
    actGrid->setCheckable(true);
    actGrid->setChecked(true);
    connect(actGrid, &QAction::toggled, this, [this](bool on) {
        m_grid->setShowGrid(on);
    });
}

void StructViewWidget::setupFindReplace()
{
    m_findReplace = new ScopedFindReplacePanel(m_fm, this);
    auto *actFind = m_toolbar->addToolAction(
        QStringLiteral("Find"), QKeySequence::Find, 0);
    connect(actFind, &QAction::triggered, m_findReplace, [this]() {
        m_findReplace->showPanel(!m_findReplace->isPanelVisible());
    });
    connect(m_findReplace, &ScopedFindReplacePanel::findRequested, this,
            &StructViewWidget::onFind);
}

bool StructViewWidget::loadFile(const QString &filepath)
{
    QFile file(filepath);
    if (!file.open(QIODevice::ReadOnly))
        return false;

    QByteArray data = file.readAll();
    file.close();

    m_engine = TextFormatEngine::createForFile(filepath);
    if (!m_engine) return false;

    if (!m_engine->parse(data))
        return false;

    m_filepath = filepath;

    // Populate tree
    populateTree();

    // Set text tab
    m_textView->setPlainText(m_engine->rawText());

    // Status bar
    QFileInfo fi(filepath);
    m_statusBar->setFormatInfo(m_engine->formatName());
    m_statusBar->setEncoding(EncodingUtils::detectEncoding(data));

    // Select root node
    if (m_treeModel->rowCount() > 0) {
        m_treeView->setCurrentIndex(m_treeModel->index(0, 0));
        m_treeView->expandAll();
    }

    return true;
}

void StructViewWidget::populateTree()
{
    m_treeModel->clear();
    DocumentNode *root = m_engine->rootNode();
    if (!root) return;

    auto *rootItem = new QStandardItem(root->name);
    rootItem->setData(QVariant::fromValue(reinterpret_cast<quintptr>(root)),
                      Qt::UserRole);
    m_treeModel->appendRow(rootItem);

    populateTreeNode(rootItem, root);
}

void StructViewWidget::populateTreeNode(QStandardItem *parentItem, DocumentNode *node)
{
    for (auto *child : node->children) {
        auto *item = new QStandardItem(child->name);
        item->setData(QVariant::fromValue(reinterpret_cast<quintptr>(child)),
                      Qt::UserRole);
        item->setEditable(false);

        // Icon: folder for containers, document for leaves
        if (child->isContainer()) {
            item->setIcon(style()->standardIcon(QStyle::SP_DirIcon));
        } else {
            item->setIcon(style()->standardIcon(QStyle::SP_FileIcon));
        }

        parentItem->appendRow(item);
        populateTreeNode(item, child);
    }
}

void StructViewWidget::onTreeNodeSelected(const QModelIndex &current,
                                           const QModelIndex & /*previous*/)
{
    if (!current.isValid()) return;

    auto ptr = current.data(Qt::UserRole).value<quintptr>();
    auto *node = reinterpret_cast<DocumentNode*>(ptr);
    if (!node) return;

    showNodeData(node);
}

void StructViewWidget::showNodeData(DocumentNode *node)
{
    m_currentNode = node;

    m_gridModel->clear();

    if (node->columnNames.isEmpty() && node->rows.isEmpty()) {
        // No grid data — show message
        m_gridModel->setHorizontalHeaderLabels({QStringLiteral("Info")});
        auto *item = new QStandardItem(
            QStringLiteral("Select a child node to view data."));
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        m_gridModel->appendRow(item);
    } else {
        // Set column headers
        m_gridModel->setHorizontalHeaderLabels(node->columnNames);

        // Populate rows
        for (const auto &row : node->rows) {
            QList<QStandardItem*> items;
            for (const auto &val : row) {
                auto *item = new QStandardItem(val.toString());
                if (!node->editable)
                    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
                items.append(item);
            }
            m_gridModel->appendRow(items);
        }
    }

    m_filterHeader->clearFilters();

    updateStatusBar();

    // Resize columns
    m_gridView->horizontalHeader()->setStretchLastSection(true);
    m_gridView->resizeColumnsToContents();
}

void StructViewWidget::updateStatusBar()
{
    int total = m_gridModel->rowCount();
    int filtered = m_filterProxy->rowCount();
    m_statusBar->setRowCount(filtered, total);
}

bool StructViewWidget::saveFile()
{
    return saveFileAs(m_filepath);
}

bool StructViewWidget::saveFileAs(const QString &path)
{
    if (!m_engine) return false;

    // Sync current grid data back to the node
    if (m_currentNode && !m_currentNode->columnNames.isEmpty()) {
        m_currentNode->rows.clear();
        for (int r = 0; r < m_gridModel->rowCount(); ++r) {
            QVector<QVariant> row;
            for (int c = 0; c < m_gridModel->columnCount(); ++c) {
                auto *item = m_gridModel->item(r, c);
                row.append(item ? item->data(Qt::DisplayRole) : QVariant());
            }
            m_currentNode->rows.append(std::move(row));
        }
    }

    QByteArray output = m_engine->serialize();
    if (output.isEmpty()) return false;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;

    file.write(output);
    file.close();
    return true;
}

void StructViewWidget::onSave()
{
    if (!saveFile()) {
        QMessageBox::warning(this, QStringLiteral("Save Error"),
                             QStringLiteral("Could not save file."));
    }
}

void StructViewWidget::onFind(bool forward)
{
    // Delegate to grid search
    if (!m_gridView->model()) return;

    QString searchText = m_findReplace->findText();
    if (searchText.isEmpty()) return;

    QModelIndex start = m_gridView->currentIndex();
    if (!start.isValid())
        start = m_gridView->model()->index(0, 0);

    QAbstractItemModel *model = m_gridView->model();
    int rows = model->rowCount();
    int cols = model->columnCount();
    int startRow = start.row();
    int startCol = start.column();

    // Search from current position
    for (int i = 0; i < rows * cols; ++i) {
        int offset = forward ? (i + 1) : (rows * cols - i - 1);
        int r = (startRow + offset / cols) % rows;
        int c = (startCol + offset % cols) % cols;
        QModelIndex idx = model->index(r, c);
        QString text = model->data(idx, Qt::DisplayRole).toString();
        if (text.contains(searchText, Qt::CaseInsensitive)) {
            m_gridView->setCurrentIndex(idx);
            m_gridView->scrollTo(idx);
            return;
        }
    }
}

FocusManager *StructViewWidget::focusManager() const { return m_fm; }
EditableGridWidget *StructViewWidget::grid() const { return m_grid; }

QString StructViewWidget::getSelectionAsText(char sep)
{
    return m_grid->getSelectionAsText(sep);
}
