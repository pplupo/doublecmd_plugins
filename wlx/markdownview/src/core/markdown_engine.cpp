#include "markdown_engine.h"
#include "diagram_render.h"
#include "latex_render.h"

#include <md4c-html.h>
#include "latex.h"

#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <regex>
#include <sys/stat.h>

namespace MarkdownEngine {

namespace {

bool fileExists(const std::string &path) { struct stat st; return stat(path.c_str(), &st) == 0; }

std::string readFileUtf8(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss; ss << f.rdbuf();
    return ss.str();
}

std::string homeDir() {
    const char *h = std::getenv("HOME");
    return h ? h : "";
}

std::string htmlUnescape(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ) {
        if (s.compare(i, 5, "&amp;") == 0) { out += '&'; i += 5; }
        else if (s.compare(i, 4, "&lt;") == 0) { out += '<'; i += 4; }
        else if (s.compare(i, 4, "&gt;") == 0) { out += '>'; i += 4; }
        else if (s.compare(i, 6, "&quot;") == 0) { out += '"'; i += 6; }
        else if (s.compare(i, 5, "&#39;") == 0) { out += '\''; i += 5; }
        else { out += s[i]; i += 1; }
    }
    return out;
}

std::string base64Encode(const std::vector<uint8_t> &data) {
    static const char *tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        unsigned v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out += tbl[(v >> 18) & 0x3F]; out += tbl[(v >> 12) & 0x3F];
        out += tbl[(v >> 6) & 0x3F]; out += tbl[v & 0x3F];
    }
    if (i + 1 == data.size()) {
        unsigned v = data[i] << 16;
        out += tbl[(v >> 18) & 0x3F]; out += tbl[(v >> 12) & 0x3F]; out += "==";
    } else if (i + 2 == data.size()) {
        unsigned v = (data[i] << 16) | (data[i + 1] << 8);
        out += tbl[(v >> 18) & 0x3F]; out += tbl[(v >> 12) & 0x3F]; out += tbl[(v >> 6) & 0x3F]; out += "=";
    }
    return out;
}

void replaceAll(std::string &s, const std::string &from, const std::string &to) {
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) { s.replace(pos, from.size(), to); pos += to.size(); }
}

std::string htmlEscape(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            default: out += c;
        }
    }
    return out;
}

// --- md_html() driver ---

void mdHtmlOutputCallback(const MD_CHAR *text, MD_SIZE size, void *userdata) {
    static_cast<std::string *>(userdata)->append(text, size);
}

std::string parseMarkdownToHtml(const std::string &markdown) {
    std::string html;
    unsigned parserFlags = MD_DIALECT_GITHUB | MD_FLAG_LATEXMATHSPANS;
    md_html(markdown.data(), (MD_SIZE)markdown.size(), mdHtmlOutputCallback, &html, parserFlags, 0);
    return html;
}

// --- Fenced code block post-processing: mermaid/plantuml -> rendered image ---

std::string renderDiagramImgTag(const std::string &lang, const std::string &code, bool darkMode) {
    std::string svg;
    if (lang == "mermaid") {
        svg = DiagramRender::renderMermaidWeb(code, darkMode);
        if (!svg.empty()) svg = DiagramRender::fixMermaidSvgText(svg, darkMode);
    } else { // plantuml / puml
        svg = DiagramRender::renderPlantUmlWeb(code, darkMode);
        if (!svg.empty() && darkMode) svg = DiagramRender::fixPlantUmlSvgDark(svg);
    }
    if (svg.empty()) return {};

    int w = 0, h = 0;
    std::vector<uint8_t> png = DiagramRender::svgToHighDpiPng(svg, 2.0f, darkMode, w, h);
    if (png.empty()) return {};

    std::string b64 = base64Encode(png);
    std::string tag = "<p align=\"center\">\n<img src=\"data:image/png;base64," + b64 + "\"";
    if (w > 0 && h > 0) tag += " width=\"" + std::to_string(w) + "\" height=\"" + std::to_string(h) + "\"";
    tag += " />\n</p>\n";
    return tag;
}

std::string replaceDiagramBlocks(const std::string &htmlIn, bool darkMode) {
    static const std::regex codeBlockRe(
        R"RX(<pre><code class="language-(mermaid|plantuml|puml)">([\s\S]*?)</code></pre>\n?)RX");
    std::string out;
    auto begin = std::sregex_iterator(htmlIn.begin(), htmlIn.end(), codeBlockRe);
    auto end = std::sregex_iterator();
    size_t lastEnd = 0;
    for (auto it = begin; it != end; ++it) {
        auto &m = *it;
        std::string lang = m[1].str();
        std::string code = htmlUnescape(m[2].str());
        std::string rendered = renderDiagramImgTag(lang, code, darkMode);
        out.append(htmlIn, lastEnd, m.position(0) - lastEnd);
        out += rendered.empty() ? m.str(0) : rendered; // fall back to the plain code block on failure
        lastEnd = m.position(0) + m.length(0);
    }
    out.append(htmlIn, lastEnd, std::string::npos);
    return out;
}

// --- <x-equation> post-processing: LaTeX math -> rendered image ---
// md4c-html renders MD_FLAG_LATEXMATHSPANS spans as <x-equation>...</x-equation>
// (inline) / <x-equation type="display">...</x-equation> (display) -- a
// non-standard tag by design (HTML has no native math element), meant to be
// post-processed by the consumer. That's us.

std::string renderMathTag(const std::string &tex, bool isDisplay, bool darkMode) {
    int w = 0, h = 0;
    std::vector<uint8_t> png = renderLatexToPng(tex, darkMode, w, h);
    if (png.empty()) {
        // Fallback: plain text, same shape as the original md4qt-based code's
        // non-LaTeX/parse-failure fallback. `tex` is the raw (unescaped)
        // source at this point, so it needs re-escaping for safe embedding.
        std::string escaped = htmlEscape(tex);
        return isDisplay ? ("<pre class=\"math block\"><code>" + escaped + "</code></pre>\n")
                          : ("<span class=\"math inline\">$" + escaped + "$</span>");
    }
    std::string b64 = base64Encode(png);
    std::string imgTag = "<img src=\"data:image/png;base64," + b64 + "\" />";
    return isDisplay ? ("<p align=\"center\">" + imgTag + "</p>") : ("<span class=\"math inline\">" + imgTag + "</span>");
}

std::string replaceMathTags(const std::string &htmlIn, bool darkMode) {
    static const std::regex mathRe(R"(<x-equation( type="display")?>([\s\S]*?)</x-equation>)");
    std::string out;
    auto begin = std::sregex_iterator(htmlIn.begin(), htmlIn.end(), mathRe);
    auto end = std::sregex_iterator();
    size_t lastEnd = 0;
    for (auto it = begin; it != end; ++it) {
        auto &m = *it;
        bool isDisplay = m[1].matched;
        std::string rawTex = htmlUnescape(m[2].str());
        out.append(htmlIn, lastEnd, m.position(0) - lastEnd);
        out += renderMathTag(rawTex, isDisplay, darkMode);
        lastEnd = m.position(0) + m.length(0);
    }
    out.append(htmlIn, lastEnd, std::string::npos);
    return out;
}

// --- Theming + final document wrap ---

const char *DEFAULT_LIGHT_CSS = R"(
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

const char *DEFAULT_DARK_CSS = R"(
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

std::string postProcessHtml(const std::string &rawHtml, bool darkMode, const std::string &customCssPath) {
    std::string html = rawHtml;

    if (darkMode) {
        replaceAll(html, "<blockquote>",
            "<table border=\"0\" class=\"blockquote\" width=\"100%\" cellspacing=\"0\" cellpadding=\"8\" style=\"margin-left: 20px;\"><tr><td width=\"4\" bgcolor=\"#2f81f7\" style=\"padding: 0;\"></td><td width=\"16\" style=\"padding: 0;\"></td><td style=\"font-style: italic; color: #c9d1d9;\">");
        replaceAll(html, "<pre>",
            "<table border=\"0\" width=\"100%\" cellspacing=\"0\" cellpadding=\"12\" bgcolor=\"#2d333b\" style=\"margin: 12px 0;\"><tr><td bgcolor=\"#2d333b\"><pre style=\"background-color:#2d333b; color:#e6edf3; margin:0; padding:0;\">");
        replaceAll(html, "<pre class=",
            "<table border=\"0\" width=\"100%\" cellspacing=\"0\" cellpadding=\"12\" bgcolor=\"#2d333b\" style=\"margin: 12px 0;\"><tr><td bgcolor=\"#2d333b\"><pre style=\"background-color:#2d333b; color:#e6edf3; margin:0; padding:0;\" class=");
    } else {
        replaceAll(html, "<blockquote>",
            "<table border=\"0\" class=\"blockquote\" width=\"100%\" cellspacing=\"0\" cellpadding=\"8\" style=\"margin-left: 20px;\"><tr><td width=\"4\" bgcolor=\"#1B2B3C\" style=\"padding: 0;\"></td><td width=\"16\" style=\"padding: 0;\"></td><td style=\"font-style: italic; color: #24292e;\">");
        replaceAll(html, "<pre>",
            "<table border=\"0\" width=\"100%\" cellspacing=\"0\" cellpadding=\"12\" bgcolor=\"#eef1f5\" style=\"margin: 12px 0;\"><tr><td bgcolor=\"#eef1f5\"><pre style=\"background-color:#eef1f5; color:#24292e; margin:0; padding:0;\">");
        replaceAll(html, "<pre class=",
            "<table border=\"0\" width=\"100%\" cellspacing=\"0\" cellpadding=\"12\" bgcolor=\"#eef1f5\" style=\"margin: 12px 0;\"><tr><td bgcolor=\"#eef1f5\"><pre style=\"background-color:#eef1f5; color:#24292e; margin:0; padding:0;\" class=");
    }

    replaceAll(html, "</blockquote>", "</td></tr></table>");
    replaceAll(html, "</pre>", "</pre></td></tr></table>");

    std::string hrReplacement = darkMode ? "<hr color=\"#30363d\" size=\"2\" />" : "<hr color=\"#1A2B3C\" size=\"2\" />";
    replaceAll(html, "<hr />", hrReplacement);
    replaceAll(html, "<hr>", hrReplacement);

    // CSS lookup precedence: customCssPath -> plugin CSS -> markdownpart.css -> built-in default.
    std::string cssStr, targetCssFile;
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
    if (!targetCssFile.empty()) cssStr = readFileUtf8(targetCssFile);
    if (cssStr.empty()) cssStr = darkMode ? DEFAULT_DARK_CSS : DEFAULT_LIGHT_CSS;

    return "<!DOCTYPE html><html><head><style>" + cssStr + "</style></head><body>" + html + "</body></html>";
}

std::string renderMarkdown(const std::string &markdown, bool darkMode, const std::string &customCssPath) {
    std::string html = parseMarkdownToHtml(markdown);
    html = replaceDiagramBlocks(html, darkMode);
    html = replaceMathTags(html, darkMode);
    return postProcessHtml(html, darkMode, customCssPath);
}

} // namespace

void init() {
    static bool microtex_initialized = false;
    if (!microtex_initialized) {
        tex::LaTeX::init("/usr/share/clatexmath");
        microtex_initialized = true;
    }
}

std::string renderFileToHtml(const std::string &filePath, bool darkMode, const std::string &customCssPath) {
    init();
    return renderMarkdown(readFileUtf8(filePath), darkMode, customCssPath);
}

std::string renderTextToHtml(const std::string &markdownText, bool darkMode, const std::string &customCssPath) {
    init();
    return renderMarkdown(markdownText, darkMode, customCssPath);
}

} // namespace MarkdownEngine
