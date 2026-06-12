#include "DbViewWidget.h"
#include "DbEngine.h"
#include "KeyValueModel.h"
#include "DuckDbModel.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QPlainTextEdit>
#include <QLabel>
#include <QJsonDocument>
#include <QJsonValue>
#include <QJsonObject>
#include <QJsonArray>

#include <wlxbase_wlqt/FocusManager.h>
#include <wlxbase_wlqt/PluginToolBar.h>
#include <wlxbase_wlqt/EditableGridWidget.h>
#include <wlxbase_wlqt/FindReplacePanel.h>
#include <wlxbase_wlqt/FilterableHeaderView.h>
#include <wlxbase_wlqt/PluginStatusBar.h>
#include <wlxbase_wlqt/PluginSplitView.h>
#include <wlxbase_wlqt/ThemeManager.h>

#include <QVBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QAbstractItemModel>
#include <QMenu>
#include <QFileDialog>
#include <QFile>
#include <QSortFilterProxyModel>
#include <QUndoStack>
#include <QIcon>
#include <QStyle>
#include <QUndoCommand>
#include <QStyledItemDelegate>
#include <QSqlRecord>
#include <QSqlField>
#include <QSqlIndex>
#include <QSqlTableModel>
#include <iostream>

using namespace QtWlPlugin;

namespace {
class ValueInspectorDialog : public QDialog {
public:
    ValueInspectorDialog(const QString &text, bool readOnly, QWidget *parent = nullptr)
        : QDialog(parent)
        , m_wasJson(false)
    {
        setWindowTitle(readOnly ? QStringLiteral("Inspect Value (Read-Only)") : QStringLiteral("Inspect / Edit Value"));
        resize(600, 450);

        auto *layout = new QVBoxLayout(this);

        // Detect if JSON
        QString displayText = text;
        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &err);
        if (err.error == QJsonParseError::NoError && (doc.isObject() || doc.isArray())) {
            m_wasJson = true;
            displayText = QString::fromUtf8(doc.toJson(QJsonDocument::Indented));
        }

        auto *infoLabel = new QLabel(m_wasJson ? QStringLiteral("Format: JSON (Pretty-printed)") : QStringLiteral("Format: Plain Text"), this);
        layout->addWidget(infoLabel);

        m_editor = new QPlainTextEdit(this);
        m_editor->setPlainText(displayText);
        m_editor->setReadOnly(readOnly);
        m_editor->setFont(QFont(QStringLiteral("monospace"), 10));
        layout->addWidget(m_editor);

        auto *buttonBox = new QDialogButtonBox(
            readOnly ? QDialogButtonBox::Close : (QDialogButtonBox::Save | QDialogButtonBox::Cancel),
            this);
        connect(buttonBox, &QDialogButtonBox::accepted, this, &ValueInspectorDialog::onAccepted);
        connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
        layout->addWidget(buttonBox);
    }

    QString value() const {
        QString txt = m_editor->toPlainText();
        if (m_wasJson) {
            QJsonParseError err;
            QJsonDocument doc = QJsonDocument::fromJson(txt.toUtf8(), &err);
            if (err.error == QJsonParseError::NoError && (doc.isObject() || doc.isArray())) {
                return QString::fromUtf8(doc.toJson(QJsonDocument::Compact));
            }
        }
        return txt;
    }

private:
    void onAccepted() {
        if (!m_editor->isReadOnly() && m_wasJson) {
            QString txt = m_editor->toPlainText();
            QJsonParseError err;
            QJsonDocument doc = QJsonDocument::fromJson(txt.toUtf8(), &err);
            if (err.error != QJsonParseError::NoError || (!doc.isObject() && !doc.isArray())) {
                auto res = QMessageBox::warning(this, QStringLiteral("Invalid JSON"),
                    QStringLiteral("The edited text is not valid JSON. Save as plain text anyway?"),
                    QMessageBox::Yes | QMessageBox::No);
                if (res != QMessageBox::Yes) {
                    return; // Don't accept
                }
            }
        }
        accept();
    }

    QPlainTextEdit *m_editor;
    bool m_wasJson;
};

class DbSortFilterProxyModel : public QSortFilterProxyModel {
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override {
        if (auto *duckModel = qobject_cast<DuckDbModel*>(sourceModel())) {
            duckModel->sort(column, order);
        } else {
            QSortFilterProxyModel::sort(column, order);
        }
    }
};

QVariant getRowIdentifier(QAbstractItemModel *model, int row) {
    if (auto *duckModel = qobject_cast<DuckDbModel*>(model)) {
        return QVariant(static_cast<qlonglong>(duckModel->rowId(row)));
    } else if (auto *kvModel = qobject_cast<KeyValueModel*>(model)) {
        return kvModel->rawKey(row);
    } else if (auto *sqlModel = qobject_cast<QSqlTableModel*>(model)) {
        QSqlIndex pk = sqlModel->primaryKey();
        if (!pk.isEmpty()) {
            QVariantMap pkValues;
            for (int i = 0; i < pk.count(); ++i) {
                QString name = pk.field(i).name();
                int col = sqlModel->fieldIndex(name);
                QModelIndex idx = sqlModel->index(row, col);
                pkValues[name] = sqlModel->data(idx, Qt::EditRole);
            }
            return pkValues;
        }
    }
    return row; // Fallback to visual row index
}

int findRowByIdentifier(QAbstractItemModel *model, const QVariant &id) {
    if (auto *duckModel = qobject_cast<DuckDbModel*>(model)) {
        int64_t targetId = id.toLongLong();
        for (int r = 0; r < duckModel->rowCount(); ++r) {
            if (duckModel->rowId(r) == targetId) return r;
        }
    } else if (auto *kvModel = qobject_cast<KeyValueModel*>(model)) {
        QByteArray targetKey = id.toByteArray();
        for (int r = 0; r < kvModel->rowCount(); ++r) {
            if (kvModel->rawKey(r) == targetKey) return r;
        }
    } else if (auto *sqlModel = qobject_cast<QSqlTableModel*>(model)) {
        if (id.typeId() == QMetaType::QVariantMap) {
            QVariantMap pkValues = id.toMap();
            for (int r = 0; r < sqlModel->rowCount(); ++r) {
                bool match = true;
                for (auto it = pkValues.begin(); it != pkValues.end(); ++it) {
                    int col = sqlModel->fieldIndex(it.key());
                    if (sqlModel->data(sqlModel->index(r, col), Qt::EditRole) != it.value()) {
                        match = false;
                        break;
                    }
                }
                if (match) return r;
            }
        }
    }
    if (id.typeId() == QMetaType::Int) {
        return id.toInt();
    }
    return -1;
}

int findColumnByName(QAbstractItemModel *model, const QString &name) {
    for (int c = 0; c < model->columnCount(); ++c) {
        if (model->headerData(c, Qt::Horizontal, Qt::DisplayRole).toString() == name) {
            return c;
        }
    }
    return -1;
}

class DbEditCommand : public QUndoCommand {
public:
    DbEditCommand(QAbstractItemModel *model, 
                  const QVariant &rowId, 
                  const QString &colName, 
                  const QVariant &oldVal, 
                  const QVariant &newVal)
        : m_model(model), m_rowId(rowId), m_colName(colName)
        , m_oldVal(oldVal), m_newVal(newVal)
    {
        setText(QStringLiteral("Edit cell"));
    }

    void undo() override {
        applyValue(m_oldVal);
    }

    void redo() override {
        applyValue(m_newVal);
    }

private:
    void applyValue(const QVariant &value) {
        int visualRow = findRowByIdentifier(m_model, m_rowId);
        int visualCol = findColumnByName(m_model, m_colName);
        if (visualRow >= 0 && visualCol >= 0) {
            QModelIndex idx = m_model->index(visualRow, visualCol);
            m_model->setData(idx, value, Qt::EditRole);
        }
    }

    QAbstractItemModel *m_model;
    QVariant m_rowId;
    QString m_colName;
    QVariant m_oldVal;
    QVariant m_newVal;
};

class DbEditDelegate : public QStyledItemDelegate {
public:
    DbEditDelegate(QUndoStack *undoStack, QObject *parent = nullptr)
        : QStyledItemDelegate(parent), m_undoStack(undoStack) {}

    void setModelData(QWidget *editor, QAbstractItemModel *model, const QModelIndex &index) const override {
        QModelIndex srcIndex = index;
        QAbstractItemModel *srcModel = model;
        if (auto *proxy = qobject_cast<const QAbstractProxyModel*>(model)) {
            srcIndex = proxy->mapToSource(index);
            srcModel = proxy->sourceModel();
        }

        QVariant oldValue = srcModel->data(srcIndex, Qt::EditRole);

        // Let default delegate set the model data
        QStyledItemDelegate::setModelData(editor, model, index);

        QVariant newValue = srcModel->data(srcIndex, Qt::EditRole);

        if (oldValue != newValue) {
            QVariant rowId = getRowIdentifier(srcModel, srcIndex.row());
            QString colName = srcModel->headerData(srcIndex.column(), Qt::Horizontal, Qt::DisplayRole).toString();

            // Revert temporarily so push can apply it cleanly
            srcModel->setData(srcIndex, oldValue, Qt::EditRole);

            m_undoStack->push(new DbEditCommand(srcModel, rowId, colName, oldValue, newValue));
        }
    }

private:
    QUndoStack *m_undoStack;
};
}


// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

DbViewWidget::DbViewWidget(QWidget *parent)
    : QWidget(parent)
{
    m_undoStack = new QUndoStack(this);
}

DbViewWidget::~DbViewWidget()
{
    // Disconnect all signals BEFORE child widgets start being destroyed.
    // Without this, Qt's arbitrary child destruction order can trigger
    // callbacks (e.g. selectionChanged, dataChanged) on half-destroyed objects,
    // causing crashes when the user navigates away with unsaved edits.
    if (m_tableList)
        disconnect(m_tableList, nullptr, this, nullptr);
    if (m_filterProxy)
        disconnect(m_filterProxy, nullptr, this, nullptr);
    if (m_fm)
        m_fm->setActive(false);

    // Detach model chain BEFORE m_engine (unique_ptr) is destroyed.
    // m_engine's destructor deletes the QSqlTableModel, so the view
    // and proxy must not reference it at that point.
    if (m_tableView)
        m_tableView->setModel(nullptr);
    if (m_filterProxy)
        m_filterProxy->setSourceModel(nullptr);
}

// ---------------------------------------------------------------------------
// File loading
// ---------------------------------------------------------------------------

bool DbViewWidget::loadFile(const QString &filepath)
{
    m_engine = DbEngine::createForFile(filepath);
    if (!m_engine)
        return false;

    m_filepath = filepath;

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

    // --- Create the primary view and FocusManager FIRST ---
    // (setupToolbar needs m_fm for shortcut registration)
    m_tableView = new QTableView;

    // Install filterable header (must be before setModel)
    m_filterHeader = new FilterableHeaderView(Qt::Horizontal, m_tableView);
    m_filterHeader->setFilterEnabled(true);
    m_filterHeader->setStretchLastSection(true);
    m_tableView->setHorizontalHeader(m_filterHeader);

    m_tableView->setAlternatingRowColors(true);
    m_tableView->setSortingEnabled(true);
    m_tableView->setSelectionBehavior(QAbstractItemView::SelectItems);
    m_tableView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tableView->setItemDelegate(new DbEditDelegate(m_undoStack, this));
    m_tableView->setEditTriggers(QAbstractItemView::EditKeyPressed | QAbstractItemView::AnyKeyPressed);
    connect(m_tableView, &QTableView::doubleClicked, this, &DbViewWidget::openValueInspector);

    m_fm = new FocusManager(this, m_tableView, this);

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

    // Load the first table's model into the view
    rebuildGrid(firstTable);

    // Create the grid widget (wraps m_tableView — only done once)
    m_grid = new EditableGridWidget(
        m_tableView, GridMode::LiveDatabase, m_fm, this);

    m_grid->setThemeToggleEnabled(true);

    connect(m_filterHeader, &FilterableHeaderView::filterChanged, this,
            [this](int column, const QString &text) {
        if (m_filterProxy) {
            m_filterProxy->setFilterKeyColumn(column);
            m_filterProxy->setFilterFixedString(text);
            updateStatusBar();
        }
    });

    // Setup Grid context menu integration
    setupGridContextMenu();

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

    // Commit (only for engines that support it)
    m_actSubmit = m_toolbar->addToolAction(
        QStringLiteral("Commit"),
        QKeySequence(Qt::CTRL | Qt::Key_S),
        FocusManager::Always,
        QStringLiteral("document-save"));
    m_actSubmit->setEnabled(m_engine && m_engine->supportsSubmitRevert());
    connect(m_actSubmit, &QAction::triggered, this, &DbViewWidget::onSubmitChanges);

    // Undo
    auto *actUndo = m_toolbar->addToolAction(
        QStringLiteral("Undo"),
        QKeySequence::Undo,
        FocusManager::Always,
        QStringLiteral("edit-undo"));
    connect(actUndo, &QAction::triggered, m_undoStack, &QUndoStack::undo);

    // Redo
    auto *actRedo = m_toolbar->addToolAction(
        QStringLiteral("Redo"),
        QKeySequence::Redo,
        FocusManager::Always,
        QStringLiteral("edit-redo"));
    actRedo->setShortcuts({QKeySequence::Redo, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z)});
    connect(actRedo, &QAction::triggered, m_undoStack, &QUndoStack::redo);

    // Reload
    auto *actReload = m_toolbar->addToolAction(
        QStringLiteral("Reload"),
        QKeySequence(Qt::Key_F5),
        FocusManager::Always,
        QStringLiteral("view-refresh"));
    connect(actReload, &QAction::triggered, this, [this]() {
        if (!m_filepath.isEmpty()) {
            if (m_engine && m_engine->supportsSubmitRevert()) {
                m_engine->revertAll();
            }
            QString table = m_engine ? m_engine->currentTableName() : QString();
            if (loadFile(m_filepath)) {
                m_undoStack->clear();
                if (!table.isEmpty()) {
                    rebuildGrid(table);
                    for (int i = 0; i < m_tableList->count(); ++i) {
                        if (m_tableList->item(i)->data(Qt::UserRole).toString() == table) {
                            m_tableList->setCurrentRow(i);
                            break;
                        }
                    }
                }
            }
        }
    });

    // Word wrap toggle
    auto *actWrap = m_toolbar->addToolAction(
        QStringLiteral("Word Wrap"),
        QKeySequence(),
        0,
        QStringLiteral("format-text-direction-ltr"));
    actWrap->setCheckable(true);
    connect(actWrap, &QAction::toggled, this, [this](bool on) {
        if (m_grid) m_grid->setWordWrap(on);
    });

    // Find
    auto *actFind = m_toolbar->addToolAction(
        QStringLiteral("Find"),
        QKeySequence(Qt::CTRL | Qt::Key_F),
        FocusManager::Always,
        QStringLiteral("edit-find"));
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

void DbViewWidget::setupGridContextMenu()
{
    m_grid->setExtraContextMenuCallback(
        [this](QMenu *menu, const QModelIndex &idx) {
        QAbstractItemModel *model = m_tableView->model();
        QModelIndex srcIdx = idx;
        if (auto *proxy = qobject_cast<QSortFilterProxyModel*>(model)) {
            srcIdx = proxy->mapToSource(idx);
            model = proxy->sourceModel();
        }
        if (!srcIdx.isValid()) return;

        // Generic Cell Inspector/Editor
        bool isBinary = false;
        auto *kvModel = qobject_cast<KeyValueModel*>(model);
        if (kvModel) {
            isBinary = kvModel->isBinaryValue(srcIdx.row());
        }

        if (!isBinary) {
            bool readOnly = !(model->flags(srcIdx) & Qt::ItemIsEditable);
            QString actionText = readOnly ? QStringLiteral("Inspect Value (Read-Only)...") : QStringLiteral("Inspect / Edit Value...");
            QAction *inspectAction = menu->addAction(actionText);
            connect(inspectAction, &QAction::triggered, this, [this, idx]() {
                openValueInspector(idx);
            });
            menu->addSeparator();
        }

        // KV Model specific actions
        if (kvModel) {
            int row = srcIdx.row();
            // Hex toggle
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
        }
    });
}

void DbViewWidget::openValueInspector(const QModelIndex &index)
{
    QAbstractItemModel *model = m_tableView->model();
    QModelIndex srcIdx = index;
    if (auto *proxy = qobject_cast<QSortFilterProxyModel*>(model)) {
        srcIdx = proxy->mapToSource(index);
        model = proxy->sourceModel();
    }
    if (!srcIdx.isValid()) return;

    // Check if binary
    bool isBinary = false;
    auto *kvModel = qobject_cast<KeyValueModel*>(model);
    if (kvModel) {
        isBinary = kvModel->isBinaryValue(srcIdx.row());
    }

    if (!isBinary) {
        bool readOnly = !(model->flags(srcIdx) & Qt::ItemIsEditable);
        QVariant val = model->data(srcIdx, Qt::EditRole);
        ValueInspectorDialog dialog(val.toString(), readOnly, this);
        if (dialog.exec() == QDialog::Accepted) {
            QString newVal = dialog.value();
            QVariant oldValue = model->data(srcIdx, Qt::EditRole);
            if (oldValue != newVal) {
                QVariant rowId = getRowIdentifier(model, srcIdx.row());
                QString colName = model->headerData(srcIdx.column(), Qt::Horizontal, Qt::DisplayRole).toString();
                m_undoStack->push(new DbEditCommand(model, rowId, colName, oldValue, newVal));
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Table switching
// ---------------------------------------------------------------------------

void DbViewWidget::rebuildGrid(const QString &tableName)
{
    // Detach the old model from the view BEFORE the engine deletes it.
    // modelForTable() destroys the previous QSqlTableModel internally,
    // so the view and proxy must not reference it during that window.
    //
    // NOTE: Do NOT delete m_grid here. EditableGridWidget reparents the
    // QTableView to itself, so deleting the grid would destroy the view too.
    m_tableView->setModel(nullptr);
    if (m_filterProxy)
        m_filterProxy->setSourceModel(nullptr);

    if (m_undoStack)
        m_undoStack->clear();

    // Now safe to request new model (engine deletes old one internally)
    QAbstractItemModel *model = m_engine->modelForTable(tableName);
    if (!model) return;

    // Set up new model chain
    if (!m_filterProxy) {
        m_filterProxy = new DbSortFilterProxyModel(this);
        m_filterProxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    }
    m_filterProxy->setSourceModel(model);
    m_tableView->setModel(m_filterProxy);

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
    if (m_filterHeader)
        m_filterHeader->clearFilters();

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
    m_undoStack->clear();
    updateStatusBar();
}

void DbViewWidget::onRevertChanges()
{
    if (!m_engine || !m_engine->supportsSubmitRevert()) return;

    m_engine->revertAll();
    m_undoStack->clear();
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
