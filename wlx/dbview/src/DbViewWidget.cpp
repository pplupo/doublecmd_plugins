#include "DbViewWidget.h"
#include "DbEngine.h"
#include "KeyValueModel.h"

#include <wayland_qt_base/FocusManager.h>
#include <wayland_qt_base/PluginToolBar.h>
#include <wayland_qt_base/EditableGridWidget.h>
#include <wayland_qt_base/FindReplacePanel.h>

#include <QVBoxLayout>
#include <QComboBox>
#include <QLabel>
#include <QTableView>
#include <QHeaderView>
#include <QMessageBox>
#include <QAbstractItemModel>
#include <QMenu>
#include <QFileDialog>
#include <QFile>

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

    // Create the QTableView and core components
    m_tableView = new QTableView(this);
    m_tableView->horizontalHeader()->setStretchLastSection(true);
    m_tableView->setAlternatingRowColors(true);

    // Enable context menu for KV binary value operations
    m_tableView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tableView, &QTableView::customContextMenuRequested,
            this, &DbViewWidget::onContextMenu);

    m_fm = new QtWlPlugin::FocusManager(this, m_tableView, this);

    // Build grid first — we need it before the toolbar connects to it
    rebuildGrid(firstTable);

    setupToolbar();
    mainLayout->addWidget(m_toolbar);
    mainLayout->addWidget(m_grid, 1);

    setupFindReplace();
    mainLayout->addWidget(m_findPanel);
    m_findPanel->setVisible(false);
}

void DbViewWidget::setupToolbar()
{
    m_toolbar = new QtWlPlugin::PluginToolBar(m_fm, this);

    // Engine label
    m_engineLabel = new QLabel(m_toolbar);
    m_engineLabel->setText(QStringLiteral(" [%1] ").arg(m_engine->engineName()));
    m_toolbar->addWidget(m_engineLabel);
    m_toolbar->addSeparator();

    // Table selector combo (only for multi-table engines)
    if (m_engine->supportsMultipleTables()) {
        m_tableSelector = new QComboBox(m_toolbar);
        m_tableSelector->setMinimumWidth(150);

        QStringList tables = m_engine->tableNames();
        QStringList views = m_engine->viewNames();

        for (const auto &t : tables)
            m_tableSelector->addItem(QStringLiteral("\xF0\x9F\x93\x8B ") + t, t);
        for (const auto &v : views)
            m_tableSelector->addItem(QStringLiteral("\xF0\x9F\x91\x81 ") + v, v);

        // Select current table
        for (int i = 0; i < m_tableSelector->count(); ++i) {
            if (m_tableSelector->itemData(i).toString() == m_engine->currentTableName()) {
                m_tableSelector->setCurrentIndex(i);
                break;
            }
        }

        connect(m_tableSelector, &QComboBox::currentIndexChanged,
                this, [this](int index) {
            QString tableName = m_tableSelector->itemData(index).toString();
            onTableSelected(tableName);
        });

        m_toolbar->addWidget(m_tableSelector);
        m_toolbar->addSeparator();
    }

    // Submit/Revert (only for engines that support it)
    if (m_engine->supportsSubmitRevert()) {
        m_actSubmit = m_toolbar->addToolAction(
            QStringLiteral("Submit"),
            QKeySequence(Qt::CTRL | Qt::Key_S),
            QtWlPlugin::FocusManager::Always);
        connect(m_actSubmit, &QAction::triggered, this, &DbViewWidget::onSubmitChanges);

        m_actRevert = m_toolbar->addToolAction(
            QStringLiteral("Revert"),
            QKeySequence(Qt::CTRL | Qt::Key_Z),
            QtWlPlugin::FocusManager::WhenNoInput);
        connect(m_actRevert, &QAction::triggered, this, &DbViewWidget::onRevertChanges);
    } else {
        // KV engines: direct writes, show auto-save label
        auto *autoLabel = new QLabel(QStringLiteral(" Auto-save "), m_toolbar);
        m_toolbar->addWidget(autoLabel);
    }

    m_toolbar->addSeparator();

    // Row count label
    m_rowCountLabel = new QLabel(m_toolbar);
    m_toolbar->addWidget(m_rowCountLabel);
    updateRowCount();

    m_toolbar->addSeparator();

    // Find (Ctrl+F)
    auto *actFind = m_toolbar->addToolAction(
        QStringLiteral("Find"),
        QKeySequence(Qt::CTRL | Qt::Key_F),
        QtWlPlugin::FocusManager::Always);
    connect(actFind, &QAction::triggered, this, [this]() {
        bool vis = !m_findPanel->isPanelVisible();
        m_findPanel->showPanel(vis);
    });
}

void DbViewWidget::setupFindReplace()
{
    m_findPanel = new QtWlPlugin::FindReplacePanel(m_fm, this);
    m_findPanel->setReplaceEnabled(false);

    connect(m_findPanel, &QtWlPlugin::FindReplacePanel::findRequested,
            this, &DbViewWidget::onFind);
}

// ---------------------------------------------------------------------------
// Table switching
// ---------------------------------------------------------------------------

void DbViewWidget::rebuildGrid(const QString &tableName)
{
    QAbstractItemModel *model = m_engine->modelForTable(tableName);
    if (!model) return;

    m_tableView->setModel(model);

    // Recreate EditableGridWidget if it already exists (table switch)
    if (m_grid) {
        QLayout *lay = layout();
        if (lay) lay->removeWidget(m_grid);
        delete m_grid;
    }

    m_grid = new QtWlPlugin::EditableGridWidget(
        m_tableView, QtWlPlugin::GridMode::LiveDatabase, m_fm, this);

    updateRowCount();
}

void DbViewWidget::onTableSelected(const QString &tableName)
{
    if (tableName.isEmpty() || tableName == m_engine->currentTableName())
        return;

    rebuildGrid(tableName);

    // Re-insert grid into layout (after toolbar, before find panel)
    auto *lay = qobject_cast<QVBoxLayout*>(layout());
    if (lay) {
        lay->insertWidget(1, m_grid, 1);
    }

    updateRowCount();
}

void DbViewWidget::updateRowCount()
{
    if (!m_rowCountLabel) return;

    int rows = 0;
    if (m_tableView && m_tableView->model())
        rows = m_tableView->model()->rowCount();

    m_rowCountLabel->setText(QStringLiteral("%1 rows").arg(rows));
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
    updateRowCount();
}

void DbViewWidget::onRevertChanges()
{
    if (!m_engine || !m_engine->supportsSubmitRevert()) return;

    m_engine->revertAll();
    updateRowCount();
}

// ---------------------------------------------------------------------------
// Context menu (KV binary operations)
// ---------------------------------------------------------------------------

void DbViewWidget::onContextMenu(const QPoint &pos)
{
    QModelIndex idx = m_tableView->indexAt(pos);
    if (!idx.isValid()) return;

    auto *kvModel = qobject_cast<KeyValueModel*>(m_tableView->model());
    if (!kvModel) return; // Not a KV engine, no special context menu

    int row = idx.row();
    QMenu menu(this);

    // Hex toggle
    bool isBinary = kvModel->isBinaryValue(row);
    if (isBinary) {
        QAction *hexAction = menu.addAction(QStringLiteral("Toggle Hex View"));
        connect(hexAction, &QAction::triggered, this, [kvModel, row]() {
            // Toggle: check current state
            bool currentlyHex = kvModel->data(kvModel->index(row, 1), Qt::DisplayRole)
                                    .toString().contains(QStringLiteral(" "));
            kvModel->setHexMode(row, !currentlyHex);
        });
    }

    // Save value as file
    QAction *saveAction = menu.addAction(QStringLiteral("Save Value as File..."));
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
    QAction *loadAction = menu.addAction(QStringLiteral("Load Value from File..."));
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

    menu.exec(m_tableView->viewport()->mapToGlobal(pos));
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

QtWlPlugin::FocusManager *DbViewWidget::focusManager() const { return m_fm; }
QtWlPlugin::EditableGridWidget *DbViewWidget::grid() const { return m_grid; }

QString DbViewWidget::getSelectionAsText(char sep)
{
    return m_grid ? m_grid->getSelectionAsText(sep) : QString();
}
