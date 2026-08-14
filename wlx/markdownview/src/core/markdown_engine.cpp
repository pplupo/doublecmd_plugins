#include "markdown_engine.h"
#include "markdownvisitor.h"

#include <parser.h>
#include <html.h>
#include "latex.h"

#include <QString>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace {
bool fileExists(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}
std::string readFileUtf8(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
std::string homeDir() {
    const char *h = std::getenv("HOME");
    return h ? h : "";
}
}

namespace MarkdownEngine {

static const char* DEFAULT_LIGHT_CSS = R"(
body {
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
    color: #24292e;
    background-color: #ffffff;
    line-height: 1.6;
    padding: 16px;
}
h1, h2, h3, h4, h5, h6 {
    margin-top: 24px;
    margin-bottom: 16px;
    font-weight: 600;
    line-height: 1.25;
    color: #1b1f23;
}
h1 { font-size: 2em; border-bottom: 1px solid #eaecef; padding-bottom: .3em; }
h2 { font-size: 1.5em; border-bottom: 1px solid #eaecef; padding-bottom: .3em; }
h3 { font-size: 1.25em; }
h4 { font-size: 1em; }
code {
    font-family: SFMono-Regular, Consolas, "Liberation Mono", Menlo, Courier, monospace;
    font-size: 85%;
    background-color: #e4e4e4;
    color: #24292e;
    border-radius: 3px;
    padding: .2em .4em;
}
pre {
    font-family: SFMono-Regular, Consolas, "Liberation Mono", Menlo, Courier, monospace;
    font-size: 85%;
    background-color: #eef1f5;
    color: #24292e;
    border-radius: 6px;
    padding: 0;
    margin: 0;
    overflow: auto;
    line-height: 1.45;
}
pre *, code, code * {
    background-color: transparent !important;
}
table {
    border-collapse: collapse;
    width: 100%;
    margin-top: 0;
    margin-bottom: 16px;
}
table th, table td {
    padding: 6px 13px;
    border: 1px solid #dfe2e5;
}
table tr:nth-child(2n) {
    background-color: #f6f8fa;
}
img {
    max-width: 100%;
    box-sizing: content-box;
}
hr {
    height: .25em;
    padding: 0;
    margin: 24px 0;
    background-color: #e1e4e8;
    border: 0;
}
)";

static const char* DEFAULT_DARK_CSS = R"(
body {
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
    color: #c9d1d9;
    background-color: #0d1117;
    line-height: 1.6;
    padding: 16px;
}
h1, h2, h3, h4, h5, h6 {
    margin-top: 24px;
    margin-bottom: 16px;
    font-weight: 600;
    line-height: 1.25;
    color: #f0f6fc;
}
h1 { font-size: 2em; border-bottom: 1px solid #21262d; padding-bottom: .3em; }
h2 { font-size: 1.5em; border-bottom: 1px solid #21262d; padding-bottom: .3em; }
h3 { font-size: 1.25em; }
h4 { font-size: 1em; }
code {
    font-family: SFMono-Regular, Consolas, "Liberation Mono", Menlo, Courier, monospace;
    font-size: 85%;
    background-color: #2d333b;
    color: #e6edf3;
    border-radius: 3px;
    padding: .2em .4em;
}
pre {
    font-family: SFMono-Regular, Consolas, "Liberation Mono", Menlo, Courier, monospace;
    font-size: 85%;
    background-color: #2d333b;
    color: #e6edf3;
    border-radius: 6px;
    padding: 0;
    margin: 0;
    overflow: auto;
    line-height: 1.45;
}
pre *, code, code * {
    background-color: transparent !important;
}
table {
    border-collapse: collapse;
    width: 100%;
    margin-top: 0;
    margin-bottom: 16px;
}
table th, table td {
    padding: 6px 13px;
    border: 1px solid #30363d;
}
table tr:nth-child(2n) {
    background-color: #161b22;
}
img {
    max-width: 100%;
    box-sizing: content-box;
}
hr {
    height: .25em;
    padding: 0;
    margin: 24px 0;
    background-color: #30363d;
    border: 0;
}
)";

void init() {
    static bool microtex_initialized = false;
    if (!microtex_initialized) {
        tex::LaTeX::init("/usr/share/clatexmath");
        microtex_initialized = true;
    }
}

static QString postProcessHtml(const QString& rawHtml, bool darkMode, const std::string& customCssPath) {
    QString html = rawHtml;

    // Table/blockquote replacements matching identical structure (no outer outline in either theme)
    if (darkMode) {
        // Dark mode blockquote: Vibrant blue left accent bar (#2f81f7), no outer border/outline, italic light text (#c9d1d9)
        html.replace(QStringLiteral("<blockquote>"), 
                     QStringLiteral("<table border=\"0\" class=\"blockquote\" width=\"100%\" cellspacing=\"0\" cellpadding=\"8\" style=\"margin-left: 20px;\"><tr><td width=\"4\" bgcolor=\"#2f81f7\" style=\"padding: 0;\"></td><td width=\"16\" style=\"padding: 0;\"></td><td style=\"font-style: italic; color: #c9d1d9;\">"));
        
        // Wrap <pre> in solid HTML table cell to guarantee 100% continuous #2d333b background with ZERO line gap stripes
        html.replace(QStringLiteral("<pre>"), QStringLiteral("<table border=\"0\" width=\"100%\" cellspacing=\"0\" cellpadding=\"12\" bgcolor=\"#2d333b\" style=\"margin: 12px 0;\"><tr><td bgcolor=\"#2d333b\"><pre style=\"background-color:#2d333b; color:#e6edf3; margin:0; padding:0;\">"));
        html.replace(QStringLiteral("<pre class="), QStringLiteral("<table border=\"0\" width=\"100%\" cellspacing=\"0\" cellpadding=\"12\" bgcolor=\"#2d333b\" style=\"margin: 12px 0;\"><tr><td bgcolor=\"#2d333b\"><pre style=\"background-color:#2d333b; color:#e6edf3; margin:0; padding:0;\" class="));
    } else {
        // Light mode blockquote: Dark navy left accent bar (#1B2B3C), no outer border/outline, italic dark text (#24292e)
        html.replace(QStringLiteral("<blockquote>"), 
                     QStringLiteral("<table border=\"0\" class=\"blockquote\" width=\"100%\" cellspacing=\"0\" cellpadding=\"8\" style=\"margin-left: 20px;\"><tr><td width=\"4\" bgcolor=\"#1B2B3C\" style=\"padding: 0;\"></td><td width=\"16\" style=\"padding: 0;\"></td><td style=\"font-style: italic; color: #24292e;\">"));

        // Wrap <pre> in solid HTML table cell to guarantee 100% continuous #eef1f5 background with ZERO line gap stripes
        html.replace(QStringLiteral("<pre>"), QStringLiteral("<table border=\"0\" width=\"100%\" cellspacing=\"0\" cellpadding=\"12\" bgcolor=\"#eef1f5\" style=\"margin: 12px 0;\"><tr><td bgcolor=\"#eef1f5\"><pre style=\"background-color:#eef1f5; color:#24292e; margin:0; padding:0;\">"));
        html.replace(QStringLiteral("<pre class="), QStringLiteral("<table border=\"0\" width=\"100%\" cellspacing=\"0\" cellpadding=\"12\" bgcolor=\"#eef1f5\" style=\"margin: 12px 0;\"><tr><td bgcolor=\"#eef1f5\"><pre style=\"background-color:#eef1f5; color:#24292e; margin:0; padding:0;\" class="));
    }
    
    html.replace(QStringLiteral("</blockquote>"), QStringLiteral("</td></tr></table>"));
    html.replace(QStringLiteral("</pre>"), QStringLiteral("</pre></td></tr></table>"));

    html.replace(QStringLiteral("<hr />"), darkMode ? QStringLiteral("<hr color=\"#30363d\" size=\"2\" />") : QStringLiteral("<hr color=\"#1A2B3C\" size=\"2\" />"));
    html.replace(QStringLiteral("<hr>"), darkMode ? QStringLiteral("<hr color=\"#30363d\" size=\"2\" />") : QStringLiteral("<hr color=\"#1A2B3C\" size=\"2\" />"));

    // Prepare CSS with strict lookup precedence:
    // 1. theme_file_path (customCssPath argument if provided and file exists)
    // 2. Plugin CSS (~/.config/doublecmd/plugins/wlx/markdownview.css)
    // 3. markdownpart.css (~/.config/doublecmd/plugins/wlx/markdownpart.css or ~/.config/markdownpart.css)
    // 4. Binary String Constants (DEFAULT_LIGHT_CSS / DEFAULT_DARK_CSS)
    std::string cssStr;
    std::string targetCssFile;

    if (!customCssPath.empty() && fileExists(customCssPath)) {
        targetCssFile = customCssPath;
    } else {
        std::string home = homeDir();
        std::string pluginDirCss = home + "/.config/doublecmd/plugins/wlx/markdownview.css";
        std::string pluginPartCss = home + "/.config/doublecmd/plugins/wlx/markdownpart.css";
        std::string userPartCss = home + "/.config/markdownpart.css";

        if (fileExists(pluginDirCss)) targetCssFile = pluginDirCss;
        else if (fileExists(pluginPartCss)) targetCssFile = pluginPartCss;
        else if (fileExists(userPartCss)) targetCssFile = userPartCss;
    }

    if (!targetCssFile.empty())
        cssStr = readFileUtf8(targetCssFile);

    QString cssQStr = cssStr.empty()
        ? (darkMode ? QString::fromUtf8(DEFAULT_DARK_CSS) : QString::fromUtf8(DEFAULT_LIGHT_CSS))
        : QString::fromStdString(cssStr);

    // Wrap in standard HTML template
    QString docHtml = QStringLiteral("<!DOCTYPE html><html><head><style>")
                     + cssQStr
                     + QStringLiteral("</style></head><body>")
                     + html
                     + QStringLiteral("</body></html>");
    return docHtml;
}

std::string renderFileToHtml(const std::string& filePath, bool darkMode, const std::string& customCssPath) {
    init();

    MD::Parser parser;
    auto doc = parser.parse(QString::fromStdString(filePath), true);
    MarkdownVisitor visitor(darkMode);
    QString rawHtml = visitor.toHtml(doc, QString(), false, nullptr);

    return postProcessHtml(rawHtml, darkMode, customCssPath).toStdString();
}

std::string renderTextToHtml(const std::string& markdownText, bool darkMode, const std::string& customCssPath) {
    init();

    MD::Parser parser;
    QString qText = QString::fromStdString(markdownText);
    auto doc = parser.parse(qText, true);
    MarkdownVisitor visitor(darkMode);
    QString rawHtml = visitor.toHtml(doc, QString(), false, nullptr);

    return postProcessHtml(rawHtml, darkMode, customCssPath).toStdString();
}

} // namespace MarkdownEngine
