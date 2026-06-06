#include "StructViewWidget.h"

#include <wayland_qt_base/FocusManager.h>
#include <wayland_qt_base/PluginToolBar.h>
#include <wayland_qt_base/EditableGridWidget.h>
#include <wayland_qt_base/ScopedFindReplacePanel.h>
#include <wayland_qt_base/EncodingUtils.h>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QApplication>
#include <QAbstractItemModel>

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

StructViewWidget::StructViewWidget(QWidget *parent)
    : QWidget(parent)
    , m_fm(nullptr)
    , m_toolbar(nullptr)
    , m_grid(nullptr)
    , m_findReplace(nullptr)
    , m_table(nullptr)
    , m_splitter(nullptr)
    , m_sectionList(nullptr)
{
}

StructViewWidget::~StructViewWidget() = default;

// ---------------------------------------------------------------------------
// UI setup
// ---------------------------------------------------------------------------

void StructViewWidget::setupUi()
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // Create QTableWidget and core platform components
    m_table = new QTableWidget(this);
    m_fm = new QtWlPlugin::FocusManager(this, m_table, this);
    m_grid = new QtWlPlugin::EditableGridWidget(
        m_table, QtWlPlugin::GridMode::MemoryDocument, m_fm, this);

    setupToolbar();
    mainLayout->addWidget(m_toolbar);

    // Check if engine needs section navigation
    if (m_engine && m_engine->hasSectionNav()) {
        m_splitter = new QSplitter(Qt::Horizontal, this);
        m_sectionList = new QListWidget(this);
        m_sectionList->setMaximumWidth(200);
        m_sectionList->setMinimumWidth(100);

        // Populate section list
        QStringList sections = m_engine->sectionNames();
        m_sectionList->addItems(sections);

        connect(m_sectionList, &QListWidget::currentTextChanged,
                this, &StructViewWidget::onSectionSelected);

        m_splitter->addWidget(m_sectionList);
        m_splitter->addWidget(m_grid);
        m_splitter->setStretchFactor(0, 0);
        m_splitter->setStretchFactor(1, 1);
        mainLayout->addWidget(m_splitter, 1);

        // Register section list as an input widget so focus works
        m_fm->addInputWidget(m_sectionList);

        // Select first section
        if (!sections.isEmpty())
            m_sectionList->setCurrentRow(0);
    } else {
        mainLayout->addWidget(m_grid, 1);
    }

    setupFindReplace();
    mainLayout->addWidget(m_findReplace);
    m_findReplace->setVisible(false);
}

void StructViewWidget::setupToolbar()
{
    m_toolbar = new QtWlPlugin::PluginToolBar(m_fm, this);

    // Format label (read-only)
    if (m_engine) {
        auto *formatLabel = new QAction(m_engine->formatName(), m_toolbar);
        formatLabel->setEnabled(false);
        m_toolbar->addAction(formatLabel);
        m_toolbar->addSeparator();
    }

    // Save
    auto *actSave = m_toolbar->addToolAction(
        QStringLiteral("Save"),
        QKeySequence(Qt::CTRL | Qt::Key_S),
        QtWlPlugin::FocusManager::Always);
    connect(actSave, &QAction::triggered, this, &StructViewWidget::onSave);

    m_toolbar->addSeparator();

    // Word wrap toggle
    auto *actWrap = m_toolbar->addToolAction(
        QStringLiteral("Word Wrap"),
        QKeySequence(), QtWlPlugin::FocusManager::WhenNoInput);
    actWrap->setCheckable(true);
    actWrap->setChecked(false);
    connect(actWrap, &QAction::toggled, m_grid, &QtWlPlugin::EditableGridWidget::setWordWrap);

    // Grid lines toggle
    auto *actGrid = m_toolbar->addToolAction(
        QStringLiteral("Grid Lines"),
        QKeySequence(), QtWlPlugin::FocusManager::WhenNoInput);
    actGrid->setCheckable(true);
    actGrid->setChecked(true);
    connect(actGrid, &QAction::toggled, m_grid, &QtWlPlugin::EditableGridWidget::setShowGrid);

    m_toolbar->addSeparator();

    // Find (Ctrl+F toggles panel)
    auto *actFind = m_toolbar->addToolAction(
        QStringLiteral("Find"),
        QKeySequence(Qt::CTRL | Qt::Key_F),
        QtWlPlugin::FocusManager::Always);
    connect(actFind, &QAction::triggered, this, [this]() {
        bool vis = !m_findReplace->isPanelVisible();
        m_findReplace->showPanel(vis);
    });

    // Dirty indicator
    connect(m_grid, &QtWlPlugin::EditableGridWidget::dirtyChanged,
            this, [this](bool dirty) {
        Q_UNUSED(dirty);
        // Could update window title if parented appropriately
    });
}

void StructViewWidget::setupFindReplace()
{
    m_findReplace = new QtWlPlugin::ScopedFindReplacePanel(m_fm, this);
    m_findReplace->setScopes({
        QStringLiteral("All Cells"),
        QStringLiteral("Current Column"),
        QStringLiteral("Current Row")
    });
    m_findReplace->setReplaceEnabled(true);

    connect(m_findReplace, &QtWlPlugin::FindReplacePanel::findRequested,
            this, &StructViewWidget::onFind);
}

// ---------------------------------------------------------------------------
// File I/O
// ---------------------------------------------------------------------------

bool StructViewWidget::loadFile(const QString &filepath)
{
    m_filepath = filepath;

    // Create engine from file extension
    m_engine = TextFormatEngine::createForFile(filepath);
    if (!m_engine)
        return false;

    // Read raw bytes
    QFile file(filepath);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    QByteArray rawData = file.readAll();
    file.close();

    // Detect encoding and convert to UTF-8 if needed
    QByteArray data = rawData;
    if (!m_engine->hasSectionNav()) {
        // INI uses QSettings which handles encoding internally
        QString detected = QtWlPlugin::EncodingUtils::detectEncoding(rawData);
        if (!detected.isEmpty() && detected.toUpper() != QStringLiteral("UTF-8")) {
            data = QtWlPlugin::EncodingUtils::toUtf8(rawData, detected);
        }
    }

    // Build UI (needs engine to be set first)
    setupUi();

    // Load data into grid
    if (m_engine->hasSectionNav()) {
        // INI: engine was pre-loaded via factory; first section selected by setupUi()
    } else {
        if (!m_engine->loadInto(m_table, data))
            return false;
    }

    // Mark undo stack as clean (initial state)
    m_fm->undoStack()->setClean();

    return true;
}

bool StructViewWidget::saveFile()
{
    return saveFileAs(m_filepath);
}

bool StructViewWidget::saveFileAs(const QString &path)
{
    if (!m_engine) return false;

    // Commit current section for INI
    if (m_engine->hasSectionNav() && !m_currentSection.isEmpty())
        m_engine->commitSection(m_table, m_currentSection);

    QByteArray data = m_engine->serialize(m_table);

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    file.write(data);
    file.close();

    m_filepath = path;
    m_fm->undoStack()->setClean();
    return true;
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

void StructViewWidget::onSave()
{
    if (!saveFile()) {
        QMessageBox::warning(this, QStringLiteral("Save Error"),
            QStringLiteral("Failed to save file: %1").arg(m_filepath));
    }
}

void StructViewWidget::onFind(bool forward)
{
    if (!m_findReplace || !m_table->model()) return;

    QString query = m_findReplace->findText();
    if (query.isEmpty()) return;

    bool caseSensitive = m_findReplace->matchCase();
    QString scope = m_findReplace->currentScope();

    Qt::MatchFlags flags = Qt::MatchContains;
    if (caseSensitive) flags |= Qt::MatchCaseSensitive;

    QAbstractItemModel *model = m_table->model();
    QModelIndex current = m_table->currentIndex();

    // Determine search range
    int startRow = current.isValid() ? current.row() : 0;
    int startCol = current.isValid() ? current.column() : 0;
    int rows = model->rowCount();
    int cols = model->columnCount();

    // Advance past current cell
    if (forward) {
        startCol++;
        if (startCol >= cols) { startCol = 0; startRow++; }
    } else {
        startCol--;
        if (startCol < 0) { startCol = cols - 1; startRow--; }
    }

    // Search loop
    for (int i = 0; i < rows * cols; ++i) {
        int r = startRow, c = startCol;
        if (forward) {
            r = startRow + (startCol + i) / cols;
            c = (startCol + i) % cols;
        } else {
            int total = startRow * cols + startCol;
            int idx = total - i;
            if (idx < 0) idx += rows * cols;
            r = idx / cols;
            c = idx % cols;
        }
        r = r % rows;
        if (r < 0) r += rows;

        // Apply scope filter
        if (scope == QStringLiteral("Current Column") && c != current.column())
            continue;
        if (scope == QStringLiteral("Current Row") && r != current.row())
            continue;

        QModelIndex idx = model->index(r, c);
        QString cellText = model->data(idx, Qt::DisplayRole).toString();

        bool match = caseSensitive
            ? cellText.contains(query, Qt::CaseSensitive)
            : cellText.contains(query, Qt::CaseInsensitive);

        if (match) {
            m_table->setCurrentIndex(idx);
            m_table->scrollTo(idx);
            return;
        }
    }

    QMessageBox::information(this, QString(),
        QStringLiteral("\"%1\" not found.").arg(query));
}

void StructViewWidget::onSectionSelected(const QString &sectionName)
{
    if (!m_engine || sectionName.isEmpty()) return;

    // Commit current section edits before switching
    if (!m_currentSection.isEmpty())
        m_engine->commitSection(m_table, m_currentSection);

    m_currentSection = sectionName;
    m_engine->loadSection(m_table, sectionName);

    // Clear undo stack for section switch (section-level undo is complex)
    m_fm->undoStack()->clear();
}

void StructViewWidget::populateGrid()
{
    // Called for non-section-nav engines after loadInto()
}

// ---------------------------------------------------------------------------
// WLX bridge accessors
// ---------------------------------------------------------------------------

QtWlPlugin::FocusManager *StructViewWidget::focusManager() const { return m_fm; }
QtWlPlugin::EditableGridWidget *StructViewWidget::grid() const { return m_grid; }

QString StructViewWidget::getSelectionAsText(char sep)
{
    return m_grid ? m_grid->getSelectionAsText(sep) : QString();
}
