#ifndef MARKDOWN_ENGINE_H
#define MARKDOWN_ENGINE_H

#include <string>

namespace MarkdownEngine {

/**
 * Initialize global sub-engines (e.g. MicroTeX LaTeX parser).
 * Safe to call multiple times.
 */
void init();

/**
 * Parse a markdown file and render it to HTML string with inline LaTeX (MicroTeX)
 * images and embedded diagram SVGs/PNGs.
 * 
 * @param filePath Path to the Markdown file.
 * @param darkMode Whether to apply dark mode styling.
 * @param customCssPath Optional path to a custom CSS stylesheet.
 * @return Formatted HTML string ready for display in QTextBrowser or WebKit.
 */
std::string renderFileToHtml(const std::string& filePath, bool darkMode = false, const std::string& customCssPath = "");

/**
 * Render raw markdown string to HTML.
 */
std::string renderTextToHtml(const std::string& markdownText, bool darkMode = false, const std::string& customCssPath = "");

} // namespace MarkdownEngine

#endif // MARKDOWN_ENGINE_H
