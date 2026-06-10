#include <wayland_qt_base/FilterRowWidget.h>

#include <QLineEdit>
#include <QHBoxLayout>
#include <QTableView>
#include <QHeaderView>
#include <QAbstractItemModel>
#include <QEvent>
#include <QSpacerItem>

namespace QtWlPlugin {

FilterRowWidget::FilterRowWidget(QTableView *view, QWidget *parent)
    : QWidget(parent)
    , m_view(view)
    , m_layout(new QHBoxLayout(this))
    , m_verticalSpacer(nullptr)
{
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(0);
    setFixedHeight(24);

    // Track header geometry changes to sync widths
    if (m_view->horizontalHeader())
        m_view->horizontalHeader()->installEventFilter(this);
    if (m_view->verticalHeader())
        m_view->verticalHeader()->installEventFilter(this);

    rebuildInputs();
}

void FilterRowWidget::rebuildInputs()
{
    // Clear existing
    for (auto *input : m_inputs)
        delete input;
    m_inputs.clear();

    // Remove old vertical header spacer
    if (m_verticalSpacer) {
        m_layout->removeItem(m_verticalSpacer);
        delete m_verticalSpacer;
        m_verticalSpacer = nullptr;
    }

    QAbstractItemModel *model = m_view->model();
    if (!model) return;

    // Add spacer for the vertical header (row number column)
    int vhWidth = 0;
    if (m_view->verticalHeader() && m_view->verticalHeader()->isVisible())
        vhWidth = m_view->verticalHeader()->width();
    m_verticalSpacer = new QSpacerItem(vhWidth, 0, QSizePolicy::Fixed, QSizePolicy::Minimum);
    m_layout->addItem(m_verticalSpacer);

    int cols = model->columnCount();
    for (int c = 0; c < cols; ++c) {
        auto *input = new QLineEdit(this);
        input->setPlaceholderText(QStringLiteral("Filter..."));
        input->setMaximumHeight(22);
        input->setFrame(true);
        input->setClearButtonEnabled(true);

        int col = c;
        connect(input, &QLineEdit::textChanged, this, [this, col](const QString &text) {
            emit filterChanged(col, text);
        });

        m_layout->addWidget(input);
        m_inputs.append(input);
    }

    syncWidths();
}

void FilterRowWidget::syncWidths()
{
    QHeaderView *header = m_view->horizontalHeader();
    if (!header) return;

    // Update vertical header spacer width
    if (m_verticalSpacer) {
        int vhWidth = 0;
        if (m_view->verticalHeader() && m_view->verticalHeader()->isVisible())
            vhWidth = m_view->verticalHeader()->width();
        m_verticalSpacer->changeSize(vhWidth, 0, QSizePolicy::Fixed, QSizePolicy::Minimum);
    }

    for (int c = 0; c < m_inputs.size(); ++c) {
        int w = header->sectionSize(c);
        m_inputs[c]->setFixedWidth(w);
    }

    m_layout->invalidate();
}

void FilterRowWidget::setFilterVisible(bool visible)
{
    m_visible = visible;
    setVisible(visible);
}

bool FilterRowWidget::isFilterVisible() const
{
    return m_visible;
}

void FilterRowWidget::clearFilters()
{
    for (auto *input : m_inputs)
        input->clear();
}

void FilterRowWidget::syncToModel()
{
    rebuildInputs();
}

bool FilterRowWidget::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == m_view->horizontalHeader() || obj == m_view->verticalHeader()) {
        if (event->type() == QEvent::Resize
            || event->type() == QEvent::LayoutRequest) {
            syncWidths();
        }
    }
    return QWidget::eventFilter(obj, event);
}

} // namespace QtWlPlugin
