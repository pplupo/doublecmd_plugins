#pragma once

#include <QWidget>
#include <QString>

namespace QtWlPlugin {

/// Shared dark/light theme management for WLX plugins.
///
/// Detects the system or Double Commander theme by inspecting the
/// application palette and applies a comprehensive Qt stylesheet.
/// Falls back to dark mode when detection is inconclusive.
class ThemeManager {
public:
    enum Theme { Light, Dark };

    /// Detect system/DC dark vs light theme from the application palette.
    /// Falls back to Dark when undetermined.
    static Theme detectSystemTheme();

    static void applyTheme(QWidget *root, Theme theme);
    static bool isDark();

private:
    static QString darkStylesheet();
    static Theme s_current;
};

} // namespace QtWlPlugin
