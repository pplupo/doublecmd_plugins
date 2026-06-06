#include "DbViewWidget.h"
#include "SqliteBackend.h"

#include <wayland_qt_base/FocusManager.h>
#include <wayland_qt_base/PluginToolBar.h>
#include <wayland_qt_base/EditableGridWidget.h>
#include <wayland_qt_base/FindReplacePanel.h>

#include <QVBoxLayout>
#include <QComboBox>
#include <QLabel>
#include <QTableView>
#include <QHeaderView>
#include <QSqlTableModel>
#include <QSqlError>
#include <QMessageBox>
#include <QAbstractItemModel>

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

DbViewWidget::DbViewWidget(QWidget *parent)
    : QWidget(parent)
    , m_backend(new SqliteBackend(this))
    , m_fm(nullptr)
    , m_toolbar(nullptr)
    , m_grid(nullptr)
    , m_findPanel(nullptr)
    , m_tableView(nullptr)
    , m_tableSelector(nullptr)
    , m_rowCountLabel(nullptr)
{
}

DbViewWidget::~DbViewWidget() = default;

// ---------------------------------------------------------------------------
// File loading
// ---------------------------------------------------------------------------

bool DbViewWidget::loadFile(const QString &filepath)
{
    if (!m_backend->openDatabase(filepath))
        return false;

    QStringList tables = m_backend->tableNames();
    QStringList views = m_backend->viewNames();

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

    // Table selector combo
    m_tableSelector = new QComboBox(m_toolbar);
    m_tableSelector->setMinimumWidth(150);

    QStringList tables = m_backend->tableNames();
    QStringList views = m_backend->viewNames();

    for (const auto &t : tables)
        m_tableSelector->addItem(QStringLiteral("📋 ") + t, t);
    for (const auto &v : views)
        m_tableSelector->addItem(QStringLiteral("👁 ") + v, v);

    // Select current table
    for (int i = 0; i < m_tableSelector->count(); ++i) {
        if (m_tableSelector->itemData(i).toString() == m_backend->currentTableName()) {
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

    // Submit changes (Ctrl+S)
    auto *actSubmit = m_toolbar->addToolAction(
        QStringLiteral("Submit"),
        QKeySequence(Qt::CTRL | Qt::Key_S),
        QtWlPlugin::FocusManager::Always);
    connect(actSubmit, &QAction::triggered, this, &DbViewWidget::onSubmitChanges);

    // Revert changes (Ctrl+Z — revert all pending, not per-cell undo)
    auto *actRevert = m_toolbar->addToolAction(
        QStringLiteral("Revert"),
        QKeySequence(Qt::CTRL | Qt::Key_Z),
        QtWlPlugin::FocusManager::WhenNoInput);
    connect(actRevert, &QAction::triggered, this, &DbViewWidget::onRevertChanges);

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
    QSqlTableModel *model = m_backend->modelForTable(tableName);
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
    if (tableName.isEmpty() || tableName == m_backend->currentTableName())
        return;

    rebuildGrid(tableName);

    // Re-insert grid into layout (after toolbar, before find panel)
    auto *lay = qobject_cast<QVBoxLayout*>(layout());
    if (lay) {
        // Index 0 = toolbar, we insert at 1
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
    auto *model = qobject_cast<QSqlTableModel*>(m_tableView->model());
    if (!model) return;

    if (!model->submitAll()) {
        QMessageBox::warning(this, QStringLiteral("Submit Error"),
            QStringLiteral("Failed to submit changes:\n%1").arg(model->lastError().text()));
        model->revertAll();
    }
    updateRowCount();
}

void DbViewWidget::onRevertChanges()
{
    auto *model = qobject_cast<QSqlTableModel*>(m_tableView->model());
    if (!model) return;

    model->revertAll();
    updateRowCount();
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
