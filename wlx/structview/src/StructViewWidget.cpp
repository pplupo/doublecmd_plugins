#include "StructViewWidget.h"

#include <wlxbase_wlqt/FocusManager.h>
#include <wlxbase_wlqt/PluginToolBar.h>
#include <wlxbase_wlqt/EditableGridWidget.h>
#include <wlxbase_wlqt/ScopedFindReplacePanel.h>
#include <wlxbase_wlqt/FilterableHeaderView.h>
#include <wlxbase_wlqt/PluginStatusBar.h>
#include <wlxbase_wlqt/PluginSplitView.h>
#include <wlxbase_wlqt/ThemeManager.h>
#include <wlxbase_wlqt/EncodingUtils.h>
#include <wlxbase_wlqt/SequentialRowProxyModel.h>

#include <QVBoxLayout>
#include <QHeaderView>
#include <QTableView>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QClipboard>
#include <QMenu>
#include <QApplication>
#include <QLabel>

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
    // Neutralize FocusManager FIRST — it has a global event filter on qApp
    // and a connection to qApp::focusChanged. If these fire during child
    // destruction (when focus shifts as widgets die), they access
    // half-destroyed objects and crash.
    if (m_fm) {
        if (qApp) {
            qApp->removeEventFilter(m_fm);
            disconnect(qApp, nullptr, m_fm, nullptr);
        }
    }

    // Block signals so that Qt's arbitrary child destruction order cannot
    // trigger dataChanged/selectionChanged callbacks on dead objects.
    if (m_gridView)
        m_gridView->blockSignals(true);
    if (m_gridModel)
        m_gridModel->blockSignals(true);
    if (m_filterProxy)
        m_filterProxy->blockSignals(true);
    if (m_grid && m_grid->undoStack())
        m_grid->undoStack()->blockSignals(true);
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
    m_filterProxy = new SequentialRowProxyModel(this);
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
    m_gridView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);

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

    m_grid->setExtraContextMenuCallback([this](QMenu *menu, const QModelIndex &idx) {
        if (!m_currentNode || !idx.isValid()) return;

        QModelIndex sourceIdx = m_filterProxy->mapToSource(idx);
        int r = sourceIdx.row();
        int c = sourceIdx.column();

        QString pathLabel = (m_engine && m_engine->formatName() == QStringLiteral("XML"))
            ? QStringLiteral("Copy XPath")
            : QStringLiteral("Copy JSONPath");

        QAction *actCopyPath = menu->addAction(pathLabel);
        connect(actCopyPath, &QAction::triggered, this, [this, r, c]() {
            QApplication::clipboard()->setText(getCellPath(m_currentNode, r, c));
        });

        QAction *actCopyKeyValue = nullptr;
        bool isTabular = !m_currentNode->columnNames.isEmpty() && m_currentNode->columnNames.size() > 2;
        if (isTabular) {
            actCopyKeyValue = menu->addAction(QStringLiteral("Copy Subtree"));
        } else {
            actCopyKeyValue = menu->addAction(QStringLiteral("Copy Key:Value"));
        }

        connect(actCopyKeyValue, &QAction::triggered, this, [this, r]() {
            if (m_engine && m_engine->formatName() == QStringLiteral("XML")) {
                if (m_currentNode->columnNames.size() == 2 && m_currentNode->columnNames[0] == QStringLiteral("Name")) {
                    QString name = m_currentNode->rows[r][0].toString();
                    QString val = m_currentNode->rows[r][1].toString();
                    if (name.startsWith('@')) {
                        QApplication::clipboard()->setText(QStringLiteral("%1=\"%2\"").arg(name.mid(1)).arg(val));
                    } else {
                        QApplication::clipboard()->setText(QStringLiteral("<%1>%2</%1>").arg(name).arg(val));
                    }
                } else if (!m_currentNode->columnNames.isEmpty()) {
                    QString rowTagName = QStringLiteral("item");
                    for (const auto *child : m_currentNode->children) {
                        if (child->name.contains('[')) {
                            rowTagName = child->name.section('[', 0, 0);
                            break;
                        }
                    }
                    DocumentNode temp(rowTagName, m_currentNode->parent);
                    temp.columnNames = m_currentNode->columnNames;
                    temp.rows.append(m_currentNode->rows[r]);
                    QByteArray data = m_engine->serializeSubtree(&temp);
                    QApplication::clipboard()->setText(QString::fromUtf8(data).trimmed());
                }
            } else {
                if (m_currentNode->columnNames.size() == 2 && m_currentNode->columnNames[0] == QStringLiteral("Key")) {
                    QString key = m_currentNode->rows[r][0].toString();
                    QString val = m_currentNode->rows[r][1].toString();
                    bool ok;
                    val.toDouble(&ok);
                    if (val == QStringLiteral("null") || val == QStringLiteral("true") || val == QStringLiteral("false") || ok) {
                        QApplication::clipboard()->setText(QStringLiteral("\"%1\": %2").arg(key).arg(val));
                    } else {
                        QString escapedVal = val;
                        escapedVal.replace(QStringLiteral("\""), QStringLiteral("\\\""));
                        QApplication::clipboard()->setText(QStringLiteral("\"%1\": \"%2\"").arg(key).arg(escapedVal));
                    }
                } else if (m_currentNode->columnNames.size() == 1 && m_currentNode->columnNames[0] == QStringLiteral("Value")) {
                    QApplication::clipboard()->setText(m_currentNode->rows[r][0].toString());
                } else if (!m_currentNode->columnNames.isEmpty()) {
                    DocumentNode temp(m_currentNode->name, m_currentNode->parent);
                    temp.columnNames = m_currentNode->columnNames;
                    temp.rows.append(m_currentNode->rows[r]);
                    QByteArray data = m_engine->serializeSubtree(&temp);
                    QString text = QString::fromUtf8(data).trimmed();
                    if (m_engine->formatName() == QStringLiteral("JSON") || m_engine->formatName() == QStringLiteral("CBOR")) {
                        if (text.startsWith('[') && text.endsWith(']')) {
                            text = text.mid(1, text.length() - 2).trimmed();
                        }
                    }
                    QApplication::clipboard()->setText(text);
                }
            }
        });
    });

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
    m_treeView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_treeView, &QTreeView::customContextMenuRequested, this,
            &StructViewWidget::showTreeContextMenu);

    // --- Right panel: Tabs ---
    m_tabWidget = new QTabWidget;

    // Grid tab
    auto *gridContainer = new QWidget;
    auto *gridLayout = new QVBoxLayout(gridContainer);
    gridLayout->setContentsMargins(0, 0, 0, 0);
    gridLayout->setSpacing(0);
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

    m_dirtyIndicator = new QLabel(QStringLiteral("✓"), this);
    m_dirtyIndicator->setContentsMargins(4, 0, 4, 0);
    m_toolbar->addWidget(m_dirtyIndicator);

    if (m_grid && m_grid->undoStack()) {
        connect(m_grid->undoStack(), &QUndoStack::cleanChanged, this, [this](bool clean) {
            if (m_dirtyIndicator) {
                m_dirtyIndicator->setText(clean ? QStringLiteral("✓") : QStringLiteral("✱"));
            }
        });
    }

    auto *actSave = m_toolbar->addToolAction(
        QStringLiteral("Save"), QKeySequence::Save, 0, QStringLiteral("document-save"));
    connect(actSave, &QAction::triggered, this, &StructViewWidget::onSave);

    auto *actWrap = m_toolbar->addToolAction(
        QStringLiteral("Word Wrap"), QKeySequence(), 0, QStringLiteral("format-text-direction-ltr"));
    actWrap->setCheckable(true);
    connect(actWrap, &QAction::toggled, this, [this](bool on) {
        m_grid->setWordWrap(on);
    });

    auto *actGrid = m_toolbar->addToolAction(
        QStringLiteral("Grid Lines"), QKeySequence(), 0, QStringLiteral("border-all"));
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
        QStringLiteral("Find"), QKeySequence::Find, 0, QStringLiteral("edit-find"));
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

    if (m_grid && m_grid->undoStack()) {
        m_grid->undoStack()->clear();
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

    // Hide header if virtual (e.g. Key/Value, Name/Value, or Value only)
    bool isVirtual = false;
    if (node->columnNames.isEmpty()) {
        isVirtual = true;
    } else if (node->columnNames.size() == 1 && node->columnNames[0] == QStringLiteral("Value")) {
        isVirtual = true;
    } else if (node->columnNames.size() == 2 &&
               (node->columnNames[0] == QStringLiteral("Key") || node->columnNames[0] == QStringLiteral("Name")) &&
               node->columnNames[1] == QStringLiteral("Value")) {
        isVirtual = true;
    }

    m_gridView->horizontalHeader()->show();
    m_filterHeader->setHeaderVisible(!isVirtual);

    // Resize columns
    m_gridView->horizontalHeader()->setStretchLastSection(true);
    m_gridView->resizeColumnsToContents();
    m_gridView->resizeRowsToContents();
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

    if (m_grid && m_grid->undoStack()) {
        m_grid->undoStack()->setClean();
    }

    return true;
}

void StructViewWidget::onSave()
{
    FocusManager::expectReloadFocus();
    if (!saveFile()) {
        QMessageBox::warning(nullptr, QStringLiteral("Save Error"),
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

QString StructViewWidget::getJsonPath(const DocumentNode *node) const
{
    if (!node) return QString();
    QString path;
    const DocumentNode *curr = node;
    while (curr) {
        QString name = curr->name;
        if (curr->parent) {
            if (name.startsWith('[')) {
                path = name + path;
            } else {
                path = "." + name + path;
            }
        } else {
            path = name + path;
        }
        curr = curr->parent;
    }
    return path;
}

QString StructViewWidget::getXmlPath(const DocumentNode *node) const
{
    if (!node) return QString();
    QString path;
    const DocumentNode *curr = node;
    while (curr) {
        QString name = curr->name;
        if (name.contains('[')) {
            int openBracket = name.indexOf('[');
            int closeBracket = name.indexOf(']');
            if (openBracket != -1 && closeBracket != -1) {
                QString tag = name.left(openBracket);
                int idx = name.mid(openBracket + 1, closeBracket - openBracket - 1).toInt();
                name = QStringLiteral("%1[%2]").arg(tag).arg(idx + 1);
            }
        }
        path = "/" + name + path;
        curr = curr->parent;
    }
    return path;
}

QString StructViewWidget::getCellPath(const DocumentNode *node, int r, int c) const
{
    if (!node) return QString();
    if (m_engine && m_engine->formatName() == QStringLiteral("XML")) {
        QString basePath = getXmlPath(node);
        if (node->columnNames.size() == 2 && node->columnNames[0] == QStringLiteral("Name")) {
            if (r >= 0 && r < node->rows.size()) {
                QString name = node->rows[r][0].toString();
                return basePath + "/" + name;
            }
        } else if (!node->columnNames.isEmpty()) {
            QString tag = "*";
            for (const auto *child : node->children) {
                if (child->name.contains('[')) {
                    tag = child->name.section('[', 0, 0);
                    break;
                }
            }
            if (c >= 0 && c < node->columnNames.size()) {
                QString colName = node->columnNames[c];
                return basePath + QStringLiteral("/%1[%2]/%3").arg(tag).arg(r + 1).arg(colName);
            }
        }
        return basePath;
    } else {
        QString basePath = getJsonPath(node);
        if (node->columnNames.size() == 2 && node->columnNames[0] == QStringLiteral("Key")) {
            if (r >= 0 && r < node->rows.size()) {
                QString key = node->rows[r][0].toString();
                return basePath + "." + key;
            }
        } else if (node->columnNames.size() == 1 && node->columnNames[0] == QStringLiteral("Value")) {
            return basePath + QStringLiteral("[%1]").arg(r);
        } else if (!node->columnNames.isEmpty()) {
            if (c >= 0 && c < node->columnNames.size()) {
                QString colName = node->columnNames[c];
                return basePath + QStringLiteral("[%1].%2").arg(r).arg(colName);
            }
        }
        return basePath;
    }
}

void StructViewWidget::showTreeContextMenu(const QPoint &pos)
{
    QModelIndex idx = m_treeView->indexAt(pos);
    if (!idx.isValid()) return;

    auto ptr = idx.data(Qt::UserRole).value<quintptr>();
    auto *node = reinterpret_cast<DocumentNode*>(ptr);
    if (!node) return;

    QMenu menu(this);
    
    QString pathLabel = (m_engine && m_engine->formatName() == QStringLiteral("XML"))
        ? QStringLiteral("Copy XPath")
        : QStringLiteral("Copy JSONPath");
    QAction *actCopyPath = menu.addAction(pathLabel);

    bool isComplex = node->isContainer();
    QAction *actCopySubtree = nullptr;
    QAction *actCopyKeyValue = nullptr;

    if (isComplex) {
        actCopySubtree = menu.addAction(QStringLiteral("Copy Subtree"));
    } else {
        actCopyKeyValue = menu.addAction(QStringLiteral("Copy Key:Value"));
    }

    QAction *actCopyValue = menu.addAction(QStringLiteral("Copy Value"));

    QAction *res = menu.exec(m_treeView->viewport()->mapToGlobal(pos));
    if (!res) return;

    if (res == actCopyPath) {
        QString path = (m_engine && m_engine->formatName() == QStringLiteral("XML"))
            ? getXmlPath(node)
            : getJsonPath(node);
        QApplication::clipboard()->setText(path);
    } else if (actCopySubtree && res == actCopySubtree) {
        if (m_engine) {
            QByteArray data = m_engine->serializeSubtree(node);
            QApplication::clipboard()->setText(QString::fromUtf8(data).trimmed());
        }
    } else if (actCopyKeyValue && res == actCopyKeyValue) {
        if (m_engine) {
            QByteArray data = m_engine->serializeSubtree(node);
            QApplication::clipboard()->setText(QString::fromUtf8(data).trimmed());
        }
    } else if (res == actCopyValue) {
        if (m_engine) {
            QByteArray data = m_engine->serializeSubtree(node);
            QApplication::clipboard()->setText(QString::fromUtf8(data).trimmed());
        }
    }
}

