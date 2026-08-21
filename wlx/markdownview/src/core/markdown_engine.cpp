#include "markdown_engine.h"
#include "diagram_render.h"
#include "latex_render.h"

#include <md4c-html.h>
#include "latex.h"

#include <cstdlib>
#include <cstdio>
#include <cstdarg>
#include <cctype>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace MarkdownEngine {

namespace {

// ── Diagnostics ──────────────────────────────────────────────────────────
// Logs to the project scratch dir (survives redeploys, unlike the deployed
// config dir) so a live crash can be correlated with the last thing this
// plugin did. Found via a debug build + GDB harness that the field crash was
// a SIGSEGV inside pangomm (missing Pango::init(), fixed in
// gtk3/latex_render_cairo.cpp) -- a hardware fault, not a thrown exception,
// so it could never have been caught by a try/catch here. This logging is
// the general-purpose safety net for whatever's next: the last line written
// before a crash is the diagnosis.
#define MV_LOG_PATH "/home/pplupo/repos/plugins/scratch/markdownview_debug.log"

void mvLog(const char *fmt, ...) {
    FILE *f = fopen(MV_LOG_PATH, "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

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

// md4c's MD_FLAG_LATEXMATHSPANS pairs `$...$` naively left-to-right with no
// notion of what makes a *plausible* math span, so a literal currency
// dollar (e.g. "$1.00") gets paired with the next unrelated dollar sign
// anywhere later in the text (e.g. "$4B"), swallowing everything in
// between as one bogus math span. Confirmed live: "Testing if 2 monetary
// values such as $1.00 and $4B work." rendered with "1.00 and " eaten as
// math and "$4B work." left dangling outside it.
//
// Since md4c's own pairing can't be reached from outside the library, this
// pre-processing pass runs before md_html() and backslash-escapes any `$`
// that Pandoc's tex_math_dollars rules (the closest thing to a documented,
// widely-used disambiguation heuristic for this exact ambiguity) would
// reject as a math delimiter, so md4c never sees it as a candidate in the
// first place. Per those rules, a `$` can only open math if the next
// character is non-space, and can only close math if the previous
// character is non-space AND the next character is not a digit -- that
// last clause is exactly what rules out "$4B" as a closer for "$1.00"
// (the char after it, '4', is a digit), leaving both dollar signs literal.
// Genuine math like "$x^2$" is unaffected: 'x' isn't a digit-adjacent
// closer, so its closing "$" still qualifies. `$$...$$` display-math
// delimiters are left untouched entirely (rare enough alongside literal
// currency to not need the same treatment, and doubling the heuristic
// above for a 2-char delimiter isn't worth the added risk here).
//
// Backtick code spans and fenced code blocks are skipped verbatim -- `$`
// inside a shell snippet like `$HOME` must never be touched.
std::string escapeStrayDollars(const std::string &md) {
    std::string out;
    out.reserve(md.size());

    auto isFenceLine = [&](size_t lineStart, size_t lineEnd, char &fenceChar, size_t &fenceLen) -> bool {
        size_t p = lineStart;
        int leadingSpaces = 0;
        while (p < lineEnd && md[p] == ' ' && leadingSpaces < 3) { ++p; ++leadingSpaces; }
        if (p >= lineEnd || (md[p] != '`' && md[p] != '~')) return false;
        char c = md[p];
        size_t start = p;
        while (p < lineEnd && md[p] == c) ++p;
        if (p - start < 3) return false;
        fenceChar = c;
        fenceLen = p - start;
        return true;
    };

    bool inFence = false;
    char fenceChar = '`';
    size_t fenceLen = 0;

    // Single monotonically-advancing index over the whole document -- NOT
    // a per-line outer loop wrapping a per-line inner scan. A valid math
    // span is allowed to cross a single '\n' (only a blank line ends a
    // paragraph), so the inner scan below can legitimately consume past the
    // end of the "current" line; a per-line outer loop that re-derives
    // lineEnd and resumes from lineEnd+1 afterwards would then re-walk
    // (and re-append) whatever the inner scan already consumed on the next
    // line. Checking for a fence line only at each actual line start avoids
    // that entirely.
    size_t i = 0;
    while (i < md.size()) {
        bool atLineStart = (i == 0 || md[i - 1] == '\n');
        if (atLineStart) {
            size_t lineEnd = md.find('\n', i);
            if (lineEnd == std::string::npos) lineEnd = md.size();
            char lineFenceChar; size_t lineFenceLen;
            if (isFenceLine(i, lineEnd, lineFenceChar, lineFenceLen)) {
                if (!inFence) {
                    inFence = true; fenceChar = lineFenceChar; fenceLen = lineFenceLen;
                } else if (lineFenceChar == fenceChar && lineFenceLen >= fenceLen) {
                    inFence = false;
                }
                out.append(md, i, lineEnd - i);
                if (lineEnd < md.size()) { out += '\n'; lineEnd += 1; }
                i = lineEnd;
                continue;
            }
        }

        if (inFence) {
            size_t lineEnd = md.find('\n', i);
            if (lineEnd == std::string::npos) lineEnd = md.size();
            out.append(md, i, lineEnd - i);
            if (lineEnd < md.size()) { out += '\n'; lineEnd += 1; }
            i = lineEnd;
            continue;
        }

        char c = md[i];
        if (c == '\n') { out += c; ++i; continue; }
        if (c == '\\' && i + 1 < md.size()) { out += c; out += md[i + 1]; i += 2; continue; }
        if (c == '`') {
            size_t start = i;
            while (i < md.size() && md[i] == '`') ++i;
            size_t tickLen = i - start;
            std::string ticks(tickLen, '`');
            size_t close = md.find(ticks, i);
            // Don't let a code span's closing run of backticks belong to a
            // longer run (CommonMark requires an exact-length match); scan
            // forward if this one falls short.
            while (close != std::string::npos) {
                size_t after = close + tickLen;
                if (after >= md.size() || md[after] != '`') break;
                close = md.find(ticks, after + 1);
            }
            if (close != std::string::npos) {
                out.append(md, start, close + tickLen - start);
                i = close + tickLen;
            } else {
                out.append(md, start, i - start);
            }
            continue;
        }
        if (c == '$') {
            if (i + 1 < md.size() && md[i + 1] == '$') { out += "$$"; i += 2; continue; }
            if (i + 1 >= md.size() || isspace((unsigned char)md[i + 1])) {
                out += "\\$"; ++i; continue;
            }
            // Candidate opener; look for a valid closer up to the next
            // blank line (math can't span a paragraph break).
            size_t paraEnd = md.find("\n\n", i);
            if (paraEnd == std::string::npos) paraEnd = md.size();
            size_t k = i + 1;
            bool found = false;
            while (k < paraEnd) {
                if (md[k] == '\\') { k += 2; continue; }
                if (md[k] == '$' && !(k + 1 < md.size() && md[k + 1] == '$')) {
                    bool prevNonSpace = !isspace((unsigned char)md[k - 1]);
                    bool nextNonDigit = !(k + 1 < md.size() && isdigit((unsigned char)md[k + 1]));
                    if (prevNonSpace && nextNonDigit) { found = true; break; }
                }
                ++k;
            }
            if (found) {
                out.append(md, i, k + 1 - i);
                i = k + 1;
            } else {
                out += "\\$";
                ++i;
            }
            continue;
        }
        out += c;
        ++i;
    }
    return out;
}

std::string parseMarkdownToHtml(const std::string &markdown) {
    std::string html;
    unsigned parserFlags = MD_DIALECT_GITHUB | MD_FLAG_LATEXMATHSPANS;
    std::string preprocessed = escapeStrayDollars(markdown);
    md_html(preprocessed.data(), (MD_SIZE)preprocessed.size(), mdHtmlOutputCallback, &html, parserFlags, 0);
    return html;
}

// --- Fenced code block post-processing: mermaid/plantuml -> rendered image ---

std::string renderDiagramImgTag(const std::string &lang, const std::string &code, bool darkMode) {
    mvLog("[renderDiagramImgTag] ENTER lang='%s' code(%zu chars)", lang.c_str(), code.size());
    std::string svg;
    if (lang == "mermaid") {
        svg = DiagramRender::renderMermaidWeb(code, darkMode);
        mvLog("[renderDiagramImgTag] renderMermaidWeb -> %zu bytes", svg.size());
        if (!svg.empty()) svg = DiagramRender::fixMermaidSvgText(svg, darkMode);
    } else { // plantuml / puml
        svg = DiagramRender::renderPlantUmlWeb(code, darkMode);
        mvLog("[renderDiagramImgTag] renderPlantUmlWeb -> %zu bytes", svg.size());
        if (!svg.empty() && darkMode) svg = DiagramRender::fixPlantUmlSvgDark(svg);
    }
    if (svg.empty()) { mvLog("[renderDiagramImgTag] empty svg, returning empty tag"); return {}; }

    int w = 0, h = 0;
    mvLog("[renderDiagramImgTag] calling svgToHighDpiPng...");
    std::vector<uint8_t> png = DiagramRender::svgToHighDpiPng(svg, 2.0f, darkMode, w, h);
    mvLog("[renderDiagramImgTag] svgToHighDpiPng -> %zu bytes (w=%d h=%d)", png.size(), w, h);
    if (png.empty()) return {};

    std::string b64 = base64Encode(png);
    std::string tag = "<p align=\"center\">\n<img src=\"data:image/png;base64," + b64 + "\"";
    if (w > 0 && h > 0) tag += " width=\"" + std::to_string(w) + "\" height=\"" + std::to_string(h) + "\"";
    tag += " />\n</p>\n";
    return tag;
}

// Hand-written scanner replacing what used to be a `static const std::regex
// codeBlockRe(R"RX(<pre><code class="language-(mermaid|plantuml|puml)">
// ([\s\S]*?)</code></pre>\n?)RX")`. Confirmed via a live gdb session against
// the actual deployed plugin (full symbols, real crash): constructing this
// std::regex crashed with SIGSEGV inside std::codecvt::do_unshift, called
// from deep within libstdc++'s regex compiler's locale setup -- reproducible
// even after isolating the call onto its own thread with a 64MB stack (so
// this was NOT simply "not enough stack" as first suspected; libstdc++'s
// std::regex touches C++ locale facets on first construction, and a facet
// race/uninitialized-state issue on a freshly spawned thread in a process
// that also does its own C setlocale() -- DC does, per its own startup log
// -- is a known class of libstdc++ instability). The pattern itself is
// simple enough that a fixed-string scan removes the dependency on
// std::regex here entirely, rather than continuing to tune around an
// unreliable library facility.
struct CodeBlockMatch { size_t start, end; std::string lang, code; };

bool findNextCodeBlock(const std::string &html, size_t from, CodeBlockMatch &out) {
    static const std::string openPrefix = "<pre><code class=\"language-";
    while (true) {
        size_t openPos = html.find(openPrefix, from);
        if (openPos == std::string::npos) return false;
        size_t langStart = openPos + openPrefix.size();
        size_t langEnd = html.find('"', langStart);
        if (langEnd == std::string::npos) return false;
        std::string lang = html.substr(langStart, langEnd - langStart);
        // Must be immediately followed by `>` (matches the original
        // regex's literal `">` right after the captured language group).
        if (langEnd + 1 >= html.size() || html[langEnd + 1] != '>') { from = langEnd + 1; continue; }
        if (lang != "mermaid" && lang != "plantuml" && lang != "puml") { from = langEnd + 1; continue; }

        size_t codeStart = langEnd + 2;
        static const std::string closeTag = "</code></pre>";
        size_t closeTagPos = html.find(closeTag, codeStart);
        if (closeTagPos == std::string::npos) return false; // unterminated block, same as the regex simply not matching

        size_t blockEnd = closeTagPos + closeTag.size();
        if (blockEnd < html.size() && html[blockEnd] == '\n') ++blockEnd; // optional trailing \n, matching `\n?`

        out.start = openPos;
        out.end = blockEnd;
        out.lang = std::move(lang);
        out.code = html.substr(codeStart, closeTagPos - codeStart);
        return true;
    }
}

std::string replaceDiagramBlocks(const std::string &htmlIn, bool darkMode) {
    std::string out;
    size_t lastEnd = 0;
    size_t searchFrom = 0;
    CodeBlockMatch m;
    while (findNextCodeBlock(htmlIn, searchFrom, m)) {
        std::string code = htmlUnescape(m.code);
        std::string rendered = renderDiagramImgTag(m.lang, code, darkMode);
        out.append(htmlIn, lastEnd, m.start - lastEnd);
        out += rendered.empty() ? htmlIn.substr(m.start, m.end - m.start) : rendered; // fall back to the plain code block on failure
        lastEnd = m.end;
        searchFrom = m.end;
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
    mvLog("[renderMathTag] ENTER isDisplay=%d tex(%zu chars)='%.100s'", (int)isDisplay, tex.size(), tex.c_str());
    int w = 0, h = 0;
    std::vector<uint8_t> png = renderLatexToPng(tex, darkMode, w, h);
    mvLog("[renderMathTag] renderLatexToPng returned %zu bytes (w=%d h=%d)", png.size(), w, h);
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

// Hand-written scanner replacing what used to be
// `static const std::regex mathRe(R"(<x-equation( type="display")?>
// ([\s\S]*?)</x-equation>)")`. This is the THIRD static const std::regex in
// this plugin, and the one that was missed in the first two rounds of the
// same fix (replaceDiagramBlocks's codeBlockRe and fixMermaidSvgText's
// foreignObjRe/textTspanRe/etc, both in diagram_render.cpp) -- explaining
// why the crash persisted even after those were removed: this file has real
// LaTeX content ($$...$$), so replaceMathTags runs on every test and hit
// this regex every time regardless of what else was fixed. Confirmed root
// cause (see diagram_render.cpp's fixMermaidSvgText for the full writeup):
// std::regex construction in this process crashes inside
// std::codecvt::do_unshift via regex_traits<char>'s locale/collate setup --
// not a stack or threading issue, and not something regex-pattern-specific.
bool findXEquationTag(const std::string &s, size_t from, size_t &matchStart, size_t &matchEnd,
                      bool &isDisplay, std::string &inner)
{
    static const std::string openTag = "<x-equation";
    static const std::string closeTag = "</x-equation>";
    size_t pos = from;
    while (true) {
        pos = s.find(openTag, pos);
        if (pos == std::string::npos) return false;
        size_t p = pos + openTag.size();
        bool display = false;
        static const std::string typeDisplay = " type=\"display\"";
        if (s.compare(p, typeDisplay.size(), typeDisplay) == 0) {
            display = true;
            p += typeDisplay.size();
        }
        if (p >= s.size() || s[p] != '>') { pos += openTag.size(); continue; }
        size_t contentStart = p + 1;
        size_t closePos = s.find(closeTag, contentStart);
        if (closePos == std::string::npos) return false; // unterminated, same as no regex match
        matchStart = pos;
        matchEnd = closePos + closeTag.size();
        isDisplay = display;
        inner = s.substr(contentStart, closePos - contentStart);
        return true;
    }
}

std::string replaceMathTags(const std::string &htmlIn, bool darkMode) {
    std::string out;
    size_t lastEnd = 0, searchFrom = 0;
    size_t ms, me; bool isDisplay; std::string rawTexEscaped;
    while (findXEquationTag(htmlIn, searchFrom, ms, me, isDisplay, rawTexEscaped)) {
        std::string rawTex = htmlUnescape(rawTexEscaped);
        out.append(htmlIn, lastEnd, ms - lastEnd);
        out += renderMathTag(rawTex, isDisplay, darkMode);
        lastEnd = me;
        searchFrom = me;
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
    mvLog("[renderMarkdown] ENTER markdown.size()=%zu darkMode=%d", markdown.size(), (int)darkMode);
    std::string html = parseMarkdownToHtml(markdown);
    mvLog("[renderMarkdown] parseMarkdownToHtml -> %zu bytes", html.size());
    html = replaceDiagramBlocks(html, darkMode);
    mvLog("[renderMarkdown] replaceDiagramBlocks -> %zu bytes", html.size());
    html = replaceMathTags(html, darkMode);
    mvLog("[renderMarkdown] replaceMathTags -> %zu bytes", html.size());
    std::string result = postProcessHtml(html, darkMode, customCssPath);
    mvLog("[renderMarkdown] EXIT postProcessHtml -> %zu bytes", result.size());
    return result;
}

} // namespace

void init() {
    static bool microtex_initialized = false;
    if (!microtex_initialized) {
        mvLog("[init] calling tex::LaTeX::init(\"/usr/share/clatexmath\")...");
        tex::LaTeX::init("/usr/share/clatexmath");
        mvLog("[init] tex::LaTeX::init done, RES_BASE='%s'", tex::RES_BASE.c_str());
        microtex_initialized = true;
    }
}

std::string renderFileToHtml(const std::string &filePath, bool darkMode, const std::string &customCssPath) {
    mvLog("\n==== renderFileToHtml('%s') ====", filePath.c_str());
    init();
    std::string content = readFileUtf8(filePath);
    mvLog("[renderFileToHtml] read %zu bytes from file", content.size());
    std::string result = renderMarkdown(content, darkMode, customCssPath);
    mvLog("[renderFileToHtml] EXIT OK, %zu bytes of HTML", result.size());
    return result;
}

std::string renderTextToHtml(const std::string &markdownText, bool darkMode, const std::string &customCssPath) {
    init();
    return renderMarkdown(markdownText, darkMode, customCssPath);
}

} // namespace MarkdownEngine
