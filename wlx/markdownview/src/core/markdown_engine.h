#ifndef MARKDOWN_ENGINE_H
#define MARKDOWN_ENGINE_H

#include <string>
#include <vector>

namespace MarkdownEngine {

/**
 * Initialize global sub-engines: MicroTeX (LaTeX math rendering), which on
 * its first call also self-seeds all 8 embedded math fonts to
 * ~/.config/doublecmd/markdownview_fonts/ (see embedded_fonts.h) and scans
 * that same directory for any additional hand-dropped-in font pairs.
 * Safe to call multiple times.
 */
void init();

/**
 * One selectable math font: a display name for UI labels, and the .clm1
 * file path that identifies it -- pass the latter as renderFileToHtml()/
 * renderTextToHtml()'s mathFontClmPath. Using the file path rather than a
 * name as the identifier sidesteps a real problem: MicroTeX registers each
 * font under a name it reads from the font file's own metadata (e.g.
 * "LatinModernMath-Regular"), which frequently isn't the same string as
 * the display name we'd otherwise show in a menu ("Latin Modern Math").
 * The clm path is unambiguous and is also exactly what a user would type
 * into the ini by hand to point at a font of their own.
 */
struct MathFontInfo {
    std::string displayName;
    std::string clmPath;
};

/**
 * Every math font available for selection: the 8 embedded fonts followed
 * by any auto-discovered ones found in the fonts dir (see init()).
 */
std::vector<MathFontInfo> availableMathFonts();

/**
 * Tell the engine which directory DC's DefaultIniName lives in, so the
 * CSS lookup chain's "plugin dir" candidate (markdownview.css) is anchored
 * to the location DC actually handed the plugin, rather than an
 * independently-guessed ~/.config/doublecmd/plugins/wlx path. Call once
 * from ListSetDefaultParams before any render call.
 */
void setPluginConfigDir(const std::string &dir);

/**
 * Enable/disable rendering for one diagram kind -- "mermaid", "plantuml",
 * or "latex". All default to enabled. Disabled means exactly what a
 * render failure already means for that kind: mermaid/plantuml fenced
 * blocks show as plain text (the raw code block); LaTeX math shows as
 * plain, unrendered text. Unrecognized kind names are a no-op. Call
 * before any render* call below; persists until changed again.
 */
void setDiagramEnabled(const std::string &kind, bool enabled);

/**
 * Which backend renders ```chart blocks -- "auto" (default: Matplot++
 * when available, falling back to the native Cairo renderer), "cairo"
 * (always the native Cairo renderer), or "off" (chart blocks show as
 * plain text, same as any other render failure). Unrecognized values
 * fall back to "auto". Call before any render* call below; persists
 * until changed again.
 */
void setChartRendererMode(const std::string &mode);

/**
 * After a render call, returns the CSS file path the engine auto-resolved
 * to when customCssPath was empty or didn't point at an existing file
 * (i.e. it fell through to markdownview.css or ~/.config/markdownpart.css,
 * or just wrote a fresh markdownview.css). Empty if customCssPath itself
 * was used as-is. Callers should persist this back into their ini's
 * theme_file_path setting so the ini reflects what's actually in use.
 */
std::string getLastAutoResolvedCssPath();

/**
 * Parse a markdown file and render it to HTML string with inline LaTeX (MicroTeX)
 * images and embedded diagram SVGs/PNGs.
 *
 * @param filePath Path to the Markdown file.
 * @param darkMode Whether to apply dark mode styling.
 * @param customCssPath Optional path to a custom CSS stylesheet.
 * @param mathFontClmPath Optional path to a .clm1 font file (see
 * availableMathFonts() above, or point this at any .clm1/.otf pair of your
 * own -- e.g. one dropped into markdownview_fonts/ by hand, or anywhere
 * else on disk). Empty uses whichever font init() loaded first (Latin
 * Modern Math, the closest visual match to what MicroTeX rendered before
 * this feature existed). A path that doesn't resolve to a loadable font
 * (missing, not a valid math font, etc.) falls back to that same default
 * rather than failing the render -- exactly as if empty had been passed.
 * @return Formatted HTML string ready for display in QTextBrowser or WebKit.
 *
 * "Save Zoom" is NOT handled here -- confirmed live that a CSS `body {
 * font-size: N%; }` rule has zero effect on Qt's QTextDocument (measured
 * identical rendered text width at 50%/100%/182%). Each toolkit applies its
 * own persisted zoom afterwards via its own native zoom API instead
 * (QTextBrowser::zoomIn/zoomOut on Qt6, webkit_web_view_set_zoom_level on
 * GTK3) -- see reloadContent()/reloadContentNow() in the respective
 * plugin_*.cpp.
 */
std::string renderFileToHtml(const std::string& filePath, bool darkMode = false, const std::string& customCssPath = "", const std::string& mathFontClmPath = "");

/**
 * Render raw markdown string to HTML.
 */
std::string renderTextToHtml(const std::string& markdownText, bool darkMode = false, const std::string& customCssPath = "", const std::string& mathFontClmPath = "");

} // namespace MarkdownEngine

#endif // MARKDOWN_ENGINE_H
