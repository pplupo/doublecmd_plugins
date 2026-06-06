#include <wayland_qt_base/PluginToolBar.h>
#include <wayland_qt_base/FocusManager.h>

#include <QAction>
#include <QActionEvent>
#include <QTimer>

namespace QtWlPlugin {

PluginToolBar::PluginToolBar(FocusManager *fm, QWidget *parent)
    : QToolBar(parent)
    , m_fm(fm)
{
    setFocusPolicy(Qt::NoFocus);
    setStyleSheet(
        "QToolBar { spacing: 2px; }"
        "QToolButton { padding: 2px 4px; margin: 1px; }"
    );
    enforceNoFocus();
}

QAction *PluginToolBar::addToolAction(const QString &text, const QKeySequence &shortcut, int ctx)
{
    QAction *action = new QAction(text, this);
    addAction(action);

    if (!shortcut.isEmpty()) {
        action->setToolTip(text + " (" + shortcut.toString(QKeySequence::NativeText) + ")");

        m_fm->registerShortcut(
            shortcut, static_cast<FocusManager::ShortcutContext>(ctx),
            [action]() { action->trigger(); return true; });
    } else {
        action->setToolTip(text);
    }

    // Restore focus to primary view after action trigger
    connect(action, &QAction::triggered, this, [this]() {
        QTimer::singleShot(0, this, [this]() {
            m_fm->restoreViewFocus();
        });
    });

    return action;
}

void PluginToolBar::actionEvent(QActionEvent *event)
{
    QToolBar::actionEvent(event);
    enforceNoFocus();
}

void PluginToolBar::enforceNoFocus()
{
    for (QAction *action : actions()) {
        QWidget *w = widgetForAction(action);
        if (w)
            w->setFocusPolicy(Qt::NoFocus);
    }
}

} // namespace QtWlPlugin
