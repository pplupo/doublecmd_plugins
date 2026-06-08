#include "DbViewWidget.h"
#include "DbEngine.h"
#include "KeyValueModel.h"

#include <wayland_qt_base/FocusManager.h>
#include <wayland_qt_base/PluginToolBar.h>
#include <wayland_qt_base/EditableGridWidget.h>
#include <wayland_qt_base/FindReplacePanel.h>
#include <wayland_qt_base/FilterRowWidget.h>
#include <wayland_qt_base/PluginStatusBar.h>
#include <wayland_qt_base/PluginSplitView.h>
#include <wayland_qt_base/ThemeManager.h>

#include <QVBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QAbstractItemModel>
#include <QMenu>
#include <QFileDialog>
#include <QFile>
#include <QSortFilterProxyModel>

using namespace QtWlPlugin;

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

DbViewWidget::DbViewWidget(QWidget *parent)
    : QWidget(parent)
{
}

DbViewWidget::~DbViewWidget() = default;

// ---------------------------------------------------------------------------
// File loading
// ---------------------------------------------------------------------------

bool DbViewWidget::loadFile(const QString &filepath)
{
    m_engine = DbEngine::createForFile(filepath);
    if (!m_engine)
        return false;

    QStringList tables = m_engine->tableNames();
    QStringList views = m_engine->viewNames();

    if (tables.isEmpty() && views.isEmpty())
        return false;

    QString firstTable = tables.isEmpty() ? views.first() : tables.first();
    setupUi(firstTable);

    // Apply saved theme
    ThemeManager::applyTheme(this, ThemeManager::currentTheme());

    return true;
}

// ---------------------------------------------------------------------------
// UI setup
// ---------------------------------------------------------------------------

void DbViewWidget::setupUi(const QString &firstTable)
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // --- Toolbar ---
    setupToolbar();
    mainLayout->addWidget(m_toolbar);

    // --- Left panel: Table list ---
    m_tableList = new QListWidget;
    m_tableList->setMinimumWidth(120);
    m_tableList->setMaximumWidth(250);
    populateTableList();

    connect(m_tableList, &QListWidget::currentItemChanged,
            this, &DbViewWidget::onTableSelected);

    // --- Right panel: Grid ---
    auto *rightContainer = new QWidget;
    auto *rightLayout = new QVBoxLayout(rightContainer);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);

    m_tableView = new QTableView;
    m_tableView->horizontalHeader()->setStretchLastSection(true);
    m_tableView->setAlternatingRowColors(true);
    m_tableView->setSortingEnabled(true);
    m_tableView->setSelectionBehavior(QAbstractItemView::SelectItems);
    m_tableView->setSelectionMode(QAbstractItemView::ExtendedSelection);

    m_fm = new FocusManager(this, m_tableView, this);

    // Build grid
    rebuildGrid(firstTable);

    m_filterRow = new FilterRowWidget(m_tableView, this);
    m_grid->setFilterRow(m_filterRow);
    m_grid->setThemeToggleEnabled(true);

    connect(m_filterRow, &FilterRowWidget::filterChanged, this,
            [this](int column, const QString &text) {
        if (m_filterProxy) {
            m_filterProxy->setFilterKeyColumn(column);
            m_filterProxy->setFilterFixedString(text);
            updateStatusBar();
        }
    });

    // Setup KV context menu integration
    setupKvContextMenu();

    rightLayout->addWidget(m_filterRow);
    rightLayout->addWidget(m_grid, 1);

    // --- Split view (only show list if multi-table) ---
    if (m_engine->supportsMultipleTables()) {
        m_splitView = new PluginSplitView(m_tableList, rightContainer, this);
        mainLayout->addWidget(m_splitView, 1);
    } else {
        m_tableList->hide();
        mainLayout->addWidget(rightContainer, 1);
    }

    // --- Find/replace ---
    setupFindReplace();
    mainLayout->addWidget(m_findPanel);
    m_findPanel->setVisible(false);

    // --- Status bar ---
    m_statusBar = new PluginStatusBar(this);
    m_statusBar->setFormatInfo(m_engine->engineName());
    mainLayout->addWidget(m_statusBar);

    updateStatusBar();

    // Select first table in list
    if (m_tableList->count() > 0)
        m_tableList->setCurrentRow(0);
}

void DbViewWidget::populateTableList()
{
    m_tableList->clear();

    QStringList tables = m_engine->tableNames();
    QStringList views = m_engine->viewNames();

    for (const auto &t : tables) {
        auto *item = new QListWidgetItem(
            style()->standardIcon(QStyle::SP_FileIcon), t, m_tableList);
        item->setData(Qt::UserRole, t);
    }
    for (const auto &v : views) {
        auto *item = new QListWidgetItem(
            style()->standardIcon(QStyle::SP_FileDialogInfoView), v, m_tableList);
        item->setData(Qt::UserRole, v);
    }
}

void DbViewWidget::setupToolbar()
{
    m_toolbar = new PluginToolBar(m_fm, this);

    // Submit/Revert (only for engines that support it)
    if (m_engine->supportsSubmitRevert()) {
        m_actSubmit = m_toolbar->addToolAction(
            QStringLiteral("Submit"),
            QKeySequence(Qt::CTRL | Qt::Key_S),
            FocusManager::Always);
        connect(m_actSubmit, &QAction::triggered, this, &DbViewWidget::onSubmitChanges);

        m_actRevert = m_toolbar->addToolAction(
            QStringLiteral("Revert"),
            QKeySequence(Qt::CTRL | Qt::Key_Z),
            FocusManager::WhenNoInput);
        connect(m_actRevert, &QAction::triggered, this, &DbViewWidget::onRevertChanges);
    }

    // Word wrap toggle
    auto *actWrap = m_toolbar->addToolAction(
        QStringLiteral("Word Wrap"), QKeySequence(), 0);
    actWrap->setCheckable(true);
    connect(actWrap, &QAction::toggled, this, [this](bool on) {
        if (m_grid) m_grid->setWordWrap(on);
    });

    // Grid lines toggle
    auto *actGrid = m_toolbar->addToolAction(
        QStringLiteral("Grid Lines"), QKeySequence(), 0);
    actGrid->setCheckable(true);
    actGrid->setChecked(true);
    connect(actGrid, &QAction::toggled, this, [this](bool on) {
        if (m_grid) m_grid->setShowGrid(on);
    });

    // Find
    auto *actFind = m_toolbar->addToolAction(
        QStringLiteral("Find"),
        QKeySequence(Qt::CTRL | Qt::Key_F),
        FocusManager::Always);
    connect(actFind, &QAction::triggered, this, [this]() {
        if (m_findPanel)
            m_findPanel->showPanel(!m_findPanel->isPanelVisible());
    });
}

void DbViewWidget::setupFindReplace()
{
    m_findPanel = new FindReplacePanel(m_fm, this);
    m_findPanel->setReplaceEnabled(false);

    connect(m_findPanel, &FindReplacePanel::findRequested,
            this, &DbViewWidget::onFind);
}

void DbViewWidget::setupKvContextMenu()
{
    m_grid->setExtraContextMenuCallback(
        [this](QMenu *menu, const QModelIndex &idx) {
        QAbstractItemModel *model = m_tableView->model();
        auto *kvModel = qobject_cast<KeyValueModel*>(model);
        QModelIndex srcIdx = idx;
        if (auto *proxy = qobject_cast<QSortFilterProxyModel*>(model)) {
            kvModel = qobject_cast<KeyValueModel*>(proxy->sourceModel());
            srcIdx = proxy->mapToSource(idx);
        }
        if (!kvModel || !srcIdx.isValid()) return;

        int row = srcIdx.row();

        // Hex toggle
        bool isBinary = kvModel->isBinaryValue(row);
        if (isBinary) {
            QAction *hexAction = menu->addAction(QStringLiteral("Toggle Hex View"));
            connect(hexAction, &QAction::triggered, this, [kvModel, row]() {
                bool currentlyHex = kvModel->data(kvModel->index(row, 1), Qt::DisplayRole)
                                        .toString().contains(QStringLiteral(" "));
                kvModel->setHexMode(row, !currentlyHex);
            });
        }

        // Save value as file
        QAction *saveAction = menu->addAction(QStringLiteral("Save Value as File..."));
        connect(saveAction, &QAction::triggered, this, [this, kvModel, row]() {
            QByteArray data = kvModel->rawValue(row);
            QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save Value"));
            if (path.isEmpty()) return;
            QFile f(path);
            if (f.open(QIODevice::WriteOnly)) {
                f.write(data);
                f.close();
            }
        });

        // Load value from file
        QAction *loadAction = menu->addAction(QStringLiteral("Load Value from File..."));
        connect(loadAction, &QAction::triggered, this, [this, kvModel, row]() {
            QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Load Value"));
            if (path.isEmpty()) return;
            QFile f(path);
            if (f.open(QIODevice::ReadOnly)) {
                QByteArray fileData = f.readAll();
                f.close();
                kvModel->loadValueFromFile(row, fileData);
            }
        });
    });
}

// ---------------------------------------------------------------------------
// Table switching
// ---------------------------------------------------------------------------

void DbViewWidget::rebuildGrid(const QString &tableName)
{
    QAbstractItemModel *model = m_engine->modelForTable(tableName);
    if (!model) return;

    if (!m_filterProxy) {
        m_filterProxy = new QSortFilterProxyModel(this);
        m_filterProxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    }
    m_filterProxy->setSourceModel(model);
    m_tableView->setModel(m_filterProxy);

    // Recreate EditableGridWidget if it already exists (table switch)
    if (m_grid) {
        QLayout *lay = layout();
        if (lay) lay->removeWidget(m_grid);
        delete m_grid;
    }

    m_grid = new EditableGridWidget(
        m_tableView, GridMode::LiveDatabase, m_fm, this);

    updateStatusBar();
}

void DbViewWidget::onTableSelected(QListWidgetItem *current,
                                    QListWidgetItem * /*previous*/)
{
    if (!current) return;

    QString tableName = current->data(Qt::UserRole).toString();
    if (tableName.isEmpty() || tableName == m_engine->currentTableName())
        return;

    rebuildGrid(tableName);

    // Re-insert grid into the right-side layout
    if (m_filterRow) {
        m_filterRow->syncToModel();
        m_filterRow->clearFilters();
    }

    updateStatusBar();
}

void DbViewWidget::updateStatusBar()
{
    if (!m_statusBar) return;

    int total = 0;
    int filtered = 0;
    if (m_tableView && m_tableView->model()) {
        filtered = m_tableView->model()->rowCount();
        if (m_filterProxy && m_filterProxy->sourceModel()) {
            total = m_filterProxy->sourceModel()->rowCount();
        } else {
            total = filtered;
        }
    }

    m_statusBar->setRowCount(filtered, total);

    if (m_engine)
        m_statusBar->setEncoding(m_engine->currentTableName());
}

// ---------------------------------------------------------------------------
// Data operations
// ---------------------------------------------------------------------------

void DbViewWidget::onSubmitChanges()
{
    if (!m_engine || !m_engine->supportsSubmitRevert()) return;

    if (!m_engine->submitAll()) {
        QMessageBox::warning(this, QStringLiteral("Submit Error"),
            QStringLiteral("Failed to submit changes:\n%1").arg(m_engine->lastError()));
        m_engine->revertAll();
    }
    updateStatusBar();
}

void DbViewWidget::onRevertChanges()
{
    if (!m_engine || !m_engine->supportsSubmitRevert()) return;

    m_engine->revertAll();
    updateStatusBar();
}

// ---------------------------------------------------------------------------
// Find
// ---------------------------------------------------------------------------

void DbViewWidget::onFind(bool forward)
{
    if (!m_findPanel || !m_tableView || !m_tableView->model())
        return;

    QString query = m_findPanel->findText();
    if (query.isEmpty()) return;

    bool caseSensitive = m_findPanel->matchCase();
    QAbstractItemModel *model = m_tableView->model();
    QModelIndex current = m_tableView->currentIndex();

    int startRow = current.isValid() ? current.row() : 0;
    int startCol = current.isValid() ? current.column() : 0;
    int rows = model->rowCount();
    int cols = model->columnCount();
    int total = rows * cols;

    // Advance past current cell
    if (forward) {
        startCol++;
        if (startCol >= cols) { startCol = 0; startRow++; }
    } else {
        startCol--;
        if (startCol < 0) { startCol = cols - 1; startRow--; }
    }

    for (int i = 0; i < total; ++i) {
        int offset = forward ? i : -i;
        int linearStart = startRow * cols + startCol;
        int linearIdx = (linearStart + offset + total) % total;
        int r = linearIdx / cols;
        int c = linearIdx % cols;

        QModelIndex idx = model->index(r, c);
        QString cellText = model->data(idx, Qt::DisplayRole).toString();

        bool match = caseSensitive
            ? cellText.contains(query, Qt::CaseSensitive)
            : cellText.contains(query, Qt::CaseInsensitive);

        if (match) {
            m_tableView->setCurrentIndex(idx);
            m_tableView->scrollTo(idx);
            m_findPanel->setStatusText(QStringLiteral("Found at row %1, col %2").arg(r + 1).arg(c + 1));
            return;
        }
    }

    m_findPanel->setStatusText(QStringLiteral("\"%1\" not found").arg(query));
}

// ---------------------------------------------------------------------------
// WLX bridge accessors
// ---------------------------------------------------------------------------

FocusManager *DbViewWidget::focusManager() const { return m_fm; }
EditableGridWidget *DbViewWidget::grid() const { return m_grid; }

QString DbViewWidget::getSelectionAsText(char sep)
{
    return m_grid ? m_grid->getSelectionAsText(sep) : QString();
}
