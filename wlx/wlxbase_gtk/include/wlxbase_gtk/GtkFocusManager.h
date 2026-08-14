#pragma once

#include <gtk/gtk.h>
#include <functional>
#include <vector>
#include <string>
#include <memory>

namespace GtkWlPlugin {

/// GTK counterpart to QtWlPlugin::FocusManager. Manages a shortcut
/// registry (keyval+modifiers -> handler, gated by whether an input
/// widget currently has focus) and an undo/redo command stack, mirroring
/// the Qt version's API shape closely enough that porting a plugin's
/// shortcut-registration code is close to mechanical.
///
/// Key handling: GTK3 delivers key-press-event to whichever widget has
/// focus, then lets it bubble up through parent containers if
/// unhandled — so shortcuts are caught by connecting "key-press-event" on
/// pluginRoot itself (the same principle as Qt's qApp-level eventFilter,
/// just using GTK's native propagation instead of a global filter).
class GtkFocusManager {
public:
    enum ShortcutContext { WhenNoInput, Always };
    using ShortcutId = int;

    GtkFocusManager(GtkWidget *pluginRoot, GtkWidget *primaryView);
    ~GtkFocusManager();

    GtkFocusManager(const GtkFocusManager &) = delete;
    GtkFocusManager &operator=(const GtkFocusManager &) = delete;

    // --- Input widget tracking ---
    void addInputWidget(GtkWidget *w);
    void removeInputWidget(GtkWidget *w);
    bool isInputWidget(GtkWidget *w) const;
    bool anyInputFocused() const;

    // --- Shortcut registration ---
    ShortcutId registerShortcut(guint keyval, GdkModifierType mods, ShortcutContext ctx,
                                 std::function<bool()> handler);
    void unregisterShortcut(ShortcutId id);

    // --- Undo/redo ---
    struct UndoCommand {
        std::string text;
        std::function<void()> undo;
        std::function<void()> redo;
    };
    /// Pushes the command and immediately calls its redo() (matching
    /// QUndoStack::push semantics: pushing == doing).
    void pushUndo(UndoCommand cmd);
    void undo();
    void redo();
    bool canUndo() const;
    bool canRedo() const;
    void clearUndoStack();
    /// Auto-registers Ctrl+Z (undo), Ctrl+Shift+Z / Ctrl+Y (redo) — call
    /// once after construction if the consumer wants undo shortcuts.
    void enableUndoShortcuts();

    GtkWidget *pluginRoot() const { return m_pluginRoot; }
    GtkWidget *primaryView() const { return m_primaryView; }

private:
    static gboolean onKeyPress(GtkWidget *widget, GdkEventKey *event, gpointer userData);
    bool handleKeyPress(GdkEventKey *event);

    GtkWidget *m_pluginRoot;
    GtkWidget *m_primaryView;
    std::vector<GtkWidget *> m_inputWidgets;

    struct RegisteredShortcut {
        ShortcutId id;
        guint keyval;
        GdkModifierType mods;
        ShortcutContext ctx;
        std::function<bool()> handler;
    };
    std::vector<RegisteredShortcut> m_shortcuts;
    ShortcutId m_nextShortcutId = 1;

    std::vector<UndoCommand> m_undoStack;
    size_t m_undoIndex = 0; // number of commands currently "done"; redo stack is [m_undoIndex, size())
};

} // namespace GtkWlPlugin
