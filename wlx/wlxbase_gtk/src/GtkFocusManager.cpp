#include "wlxbase_gtk/GtkFocusManager.h"

#include <algorithm>

namespace GtkWlPlugin {

GtkFocusManager::GtkFocusManager(GtkWidget *pluginRoot, GtkWidget *primaryView)
    : m_pluginRoot(pluginRoot)
    , m_primaryView(primaryView)
{
    gtk_widget_add_events(m_pluginRoot, GDK_KEY_PRESS_MASK);
    g_signal_connect(m_pluginRoot, "key-press-event", G_CALLBACK(onKeyPress), this);
}

GtkFocusManager::~GtkFocusManager() = default;

void GtkFocusManager::addInputWidget(GtkWidget *w)
{
    if (std::find(m_inputWidgets.begin(), m_inputWidgets.end(), w) == m_inputWidgets.end())
        m_inputWidgets.push_back(w);
}

void GtkFocusManager::removeInputWidget(GtkWidget *w)
{
    m_inputWidgets.erase(std::remove(m_inputWidgets.begin(), m_inputWidgets.end(), w), m_inputWidgets.end());
}

bool GtkFocusManager::isInputWidget(GtkWidget *w) const
{
    return std::find(m_inputWidgets.begin(), m_inputWidgets.end(), w) != m_inputWidgets.end();
}

bool GtkFocusManager::anyInputFocused() const
{
    for (GtkWidget *w : m_inputWidgets)
        if (gtk_widget_has_focus(w)) return true;
    return false;
}

GtkFocusManager::ShortcutId GtkFocusManager::registerShortcut(
    guint keyval, GdkModifierType mods, ShortcutContext ctx, std::function<bool()> handler)
{
    ShortcutId id = m_nextShortcutId++;
    m_shortcuts.push_back({id, keyval, mods, ctx, std::move(handler)});
    return id;
}

void GtkFocusManager::unregisterShortcut(ShortcutId id)
{
    m_shortcuts.erase(std::remove_if(m_shortcuts.begin(), m_shortcuts.end(),
                                      [id](const RegisteredShortcut &s) { return s.id == id; }),
                       m_shortcuts.end());
}

gboolean GtkFocusManager::onKeyPress(GtkWidget *, GdkEventKey *event, gpointer userData)
{
    auto *self = static_cast<GtkFocusManager *>(userData);
    return self->handleKeyPress(event) ? TRUE : FALSE;
}

bool GtkFocusManager::handleKeyPress(GdkEventKey *event)
{
    // Normalize to the "consumed" modifier set (ignore Lock/NumLock etc.)
    GdkModifierType relevant = static_cast<GdkModifierType>(
        event->state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK | GDK_MOD1_MASK | GDK_SUPER_MASK));

    bool inputFocused = anyInputFocused();

    for (const auto &s : m_shortcuts) {
        if (s.ctx == WhenNoInput && inputFocused) continue;
        if (s.keyval != event->keyval) continue;
        if (s.mods != relevant) continue;
        if (s.handler && s.handler())
            return true;
    }
    return false;
}

void GtkFocusManager::pushUndo(UndoCommand cmd)
{
    // Discard any redo history beyond the current point (matches
    // QUndoStack::push semantics).
    if (m_undoIndex < m_undoStack.size())
        m_undoStack.erase(m_undoStack.begin() + m_undoIndex, m_undoStack.end());

    if (cmd.redo) cmd.redo();
    m_undoStack.push_back(std::move(cmd));
    m_undoIndex = m_undoStack.size();
}

void GtkFocusManager::undo()
{
    if (m_undoIndex == 0) return;
    --m_undoIndex;
    if (m_undoStack[m_undoIndex].undo)
        m_undoStack[m_undoIndex].undo();
}

void GtkFocusManager::redo()
{
    if (m_undoIndex >= m_undoStack.size()) return;
    if (m_undoStack[m_undoIndex].redo)
        m_undoStack[m_undoIndex].redo();
    ++m_undoIndex;
}

bool GtkFocusManager::canUndo() const { return m_undoIndex > 0; }
bool GtkFocusManager::canRedo() const { return m_undoIndex < m_undoStack.size(); }

void GtkFocusManager::clearUndoStack()
{
    m_undoStack.clear();
    m_undoIndex = 0;
}

void GtkFocusManager::enableUndoShortcuts()
{
    registerShortcut(GDK_KEY_z, GDK_CONTROL_MASK, Always, [this]() { undo(); return true; });
    registerShortcut(GDK_KEY_z, static_cast<GdkModifierType>(GDK_CONTROL_MASK | GDK_SHIFT_MASK), Always,
                      [this]() { redo(); return true; });
    registerShortcut(GDK_KEY_y, GDK_CONTROL_MASK, Always, [this]() { redo(); return true; });
}

} // namespace GtkWlPlugin
