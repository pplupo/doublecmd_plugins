#include "diagram_render.h"

#include "diagram_render_local.h"
#include "vegalite_spec.h"

#include "../../3rdparty/nlohmann_json/json.hpp"

#include <cairo.h>
#include <librsvg/rsvg.h>

#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>
#include <poll.h>
#include <signal.h>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <cctype>
#include <algorithm>

extern char **environ;

namespace {

// std::stod() throws std::invalid_argument/std::out_of_range on
// non-numeric or overflowing input, and every call site here feeds it
// text captured by a regex group that's only constrained to "not a
// quote character" (e.g. foreignObjRe's width="([^"]+)"), not to
// actually being numeric -- real mermaid/plantuml SVG output that
// doesn't match the common case (a plain number, optionally with a "px"
// unit stod itself already tolerates by stopping at the first
// non-numeric character) would throw here, uncaught, crashing the
// process. Never confirmed as the exact cause of the "markdownview
// crashes on add/use" reports, but a real, unguarded exception risk on
// this exact code path regardless.
double safeStod(const std::string &s, double fallback = 0.0) {
    try {
        return std::stod(s);
    } catch (const std::exception &) {
        return fallback;
    }
}

struct ProcessResult { bool started = false; int exitCode = -1; std::string stdoutData; };

// Same posix_spawn+poll subprocess runner used elsewhere in this project
// (e.g. diagramview's DiagramRenderer) -- no Qt event loop needed.
ProcessResult runProcess(const std::string &exe, const std::vector<std::string> &args, int timeoutMs)
{
    ProcessResult result;
    int outPipe[2];
    if (pipe(outPipe) != 0) return result;

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, outPipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, outPipe[0]);
    posix_spawn_file_actions_addclose(&actions, outPipe[1]);

    std::vector<char *> argv;
    argv.push_back(const_cast<char *>(exe.c_str()));
    for (auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);

    pid_t pid = -1;
    int rc = posix_spawnp(&pid, exe.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(outPipe[1]);
    if (rc != 0) { close(outPipe[0]); return result; }
    result.started = true;

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    bool timedOut = false;
    char buf[8192];
    while (true) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) { timedOut = true; break; }
        struct pollfd pfd { outPipe[0], POLLIN, 0 };
        int pr = poll(&pfd, 1, (int)remaining);
        if (pr < 0) break;
        if (pr == 0) { timedOut = true; break; }
        if (pfd.revents & (POLLIN | POLLHUP)) {
            ssize_t n = read(outPipe[0], buf, sizeof(buf));
            if (n > 0) result.stdoutData.append(buf, (size_t)n);
            else break;
        } else break;
    }
    close(outPipe[0]);
    if (timedOut) kill(pid, SIGKILL);
    int status = 0;
    waitpid(pid, &status, 0);
    if (!timedOut && WIFEXITED(status)) result.exitCode = WEXITSTATUS(status);
    return result;
}

std::string httpGet(const std::string &url, int timeoutMs = 15000) {
    ProcessResult r = runProcess("curl", {"-s", "-L", "--max-time", std::to_string(timeoutMs / 1000), url}, timeoutMs + 2000);
    if (r.exitCode != 0) return {};
    return r.stdoutData;
}

std::string base64UrlEncode(const std::string &data) {
    static const char *tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        unsigned v = ((unsigned char)data[i] << 16) | ((unsigned char)data[i + 1] << 8) | (unsigned char)data[i + 2];
        out += tbl[(v >> 18) & 0x3F]; out += tbl[(v >> 12) & 0x3F];
        out += tbl[(v >> 6) & 0x3F]; out += tbl[v & 0x3F];
    }
    if (i + 1 == data.size()) {
        unsigned v = (unsigned char)data[i] << 16;
        out += tbl[(v >> 18) & 0x3F]; out += tbl[(v >> 12) & 0x3F];
    } else if (i + 2 == data.size()) {
        unsigned v = ((unsigned char)data[i] << 16) | ((unsigned char)data[i + 1] << 8);
        out += tbl[(v >> 18) & 0x3F]; out += tbl[(v >> 12) & 0x3F]; out += tbl[(v >> 6) & 0x3F];
    }
    return out; // no padding, matches Qt's OmitTrailingEquals
}

std::string toHex(const std::string &data) {
    static const char *hex = "0123456789abcdef";
    std::string out;
    for (unsigned char c : data) { out += hex[c >> 4]; out += hex[c & 0xF]; }
    return out;
}

// Service bases, each overridable from the ini (see
// MarkdownEngine::setDiagramServiceUrl). Mermaid and PlantUML keep their
// own dedicated services; Kroki is used for Vega-Lite alone, which has no
// comparable single-purpose service.
//
// Routing all three through Kroki was tried and reverted. Measured
// 2026-09-08, be precise about which half of that holds: Kroki's mermaid
// backend was returning HTTP 500 after ~30s for every request, including
// `graph TD\nA-->B`, while mermaid.ink answered the same diagram in ~2.4s
// -- so for Mermaid it was not slower, it was unusable. For PlantUML the
// two were comparable (Kroki ~0.18s warm vs plantuml.com ~0.22s), so that
// half is a consistency choice, not a latency one. Each of these accepts a
// self-hosted instance at the same URL shape, so the configurability that
// motivated Kroki survives the revert.
std::string g_mermaidBaseUrl = "https://mermaid.ink";
std::string g_plantUmlBaseUrl = "http://www.plantuml.com/plantuml";
std::string g_krokiBaseUrl = "https://kroki.io";

// Trailing slashes are trimmed so each caller below can splice a path on
// without doubling the separator.
std::string trimTrailingSlashes(const std::string &url) {
    std::string trimmed = url;
    while (!trimmed.empty() && trimmed.back() == '/') trimmed.pop_back();
    return trimmed;
}

// POSTs one diagram to the configured Kroki endpoint and returns the SVG
// it renders back, or empty on any failure. Only ```vegalite uses this;
// Mermaid and PlantUML go to their own faster services above.
//
// POST rather than Kroki's GET-with-deflated-path form: the GET URL grows
// with the diagram, and a real figure spec (data rows and all) runs well
// past what is safe to put in a URL -- which is also the part of a request
// intermediaries log. Verified live against https://kroki.io (2026-09-08).
// 15s matches what the old httpGet() allowed, deliberately: these renders
// run synchronously on the UI thread (see the comment in
// MarkdownViewerWidget::reloadContent), so the timeout is also how long a
// document full of diagrams can freeze Double Commander for. Worth knowing
// that a struggling endpoint spends the whole budget -- kroki.io's mermaid
// backend was observed 500ing at ~30s, which this abandons at 15.
std::string krokiRenderSvg(const std::string &diagramType, const std::string &source, int timeoutMs = 15000)
{
    if (g_krokiBaseUrl.empty() || source.empty()) return {};
    std::string payload = nlohmann::json{
        {"diagram_source", source},
        {"diagram_type", diagramType},
        {"output_format", "svg"},
    }.dump();

    // The body goes through a 0600 temp file rather than curl's argv.
    // Every process on the machine can read another's argv via /proc, and
    // the body here is the diagram's full source text -- precisely the
    // content this whole feature is opt-in about.
    char bodyPath[] = "/tmp/markdownview_kroki_XXXXXX";
    int fd = mkstemp(bodyPath);
    if (fd < 0) return {};
    bool written = write(fd, payload.data(), payload.size()) == (ssize_t)payload.size();
    close(fd);
    if (!written) { ::unlink(bodyPath); return {}; }

    // -f makes a non-2xx reply exit non-zero rather than handing back
    // Kroki's error page as if it were an SVG -- that is what makes the
    // "fall back to the block's plain text" contract actually hold for a
    // diagram the endpoint rejects.
    // --post30x keeps a redirect a POST. Without them curl silently turns a
    // followed 301/302/303 into a GET, which Kroki answers with its landing
    // page -- the failure a self-hosted `http://kroki.internal` that
    // redirects to https would otherwise hit, and an unobvious one, since
    // the request "succeeds".
    ProcessResult r = runProcess("curl", {"-s", "-f", "-L",
                                          "--post301", "--post302", "--post303",
                                          "--max-time", std::to_string(timeoutMs / 1000),
                                          "-H", "Content-Type: application/json",
                                          "--data-binary", std::string("@") + bodyPath,
                                          g_krokiBaseUrl},
                                 timeoutMs + 2000);
    ::unlink(bodyPath);
    if (r.exitCode != 0) return {};
    return r.stdoutData;
}

cairo_status_t writeToString(void *closure, const unsigned char *data, unsigned int length) {
    auto *out = static_cast<std::string *>(closure);
    out->append(reinterpret_cast<const char *>(data), length);
    return CAIRO_STATUS_SUCCESS;
}

} // namespace

namespace DiagramRender {

// Single accent color used everywhere a diagram needs a line/border/arrow
// color, matching PlantUML's own accent below and the blockquote left-bar
// color in the theme CSS -- previously mermaid used its own generic
// "dark"/"default" built-in theme with no accent override at all, so its
// lines never matched PlantUML's explicit blue skinparam accents in the
// same document.
constexpr const char *kAccentColor = "#58a6ff";

void setMermaidBaseUrl(const std::string &url)
{
    std::string trimmed = trimTrailingSlashes(url);
    if (!trimmed.empty()) g_mermaidBaseUrl = trimmed;
}

void setPlantUmlBaseUrl(const std::string &url)
{
    std::string trimmed = trimTrailingSlashes(url);
    if (!trimmed.empty()) g_plantUmlBaseUrl = trimmed;
}

void setKrokiBaseUrl(const std::string &url)
{
    std::string trimmed = trimTrailingSlashes(url);
    if (!trimmed.empty()) g_krokiBaseUrl = trimmed;
}

// The theme block rides along INSIDE the source as a `%%{init}%%`
// directive rather than being a request parameter, which is why the same
// decorated string works unchanged against mermaid.ink and against a
// locally linked mermaid-little -- both hand it to the same mermaid
// dialect. Split out of renderMermaidWeb so the local path gets identical
// styling instead of quietly rendering unthemed.
std::string mermaidWithTheme(const std::string &code, bool darkMode)
{
    std::string theme = darkMode ? "\"dark\"" : "\"default\"";
    std::string config = "%%{init: {\"theme\": " + theme +
        ", \"flowchart\": {\"htmlLabels\": false}, \"sequence\": {\"htmlLabels\": false}, "
        "\"gantt\": {\"htmlLabels\": false}, \"journey\": {\"htmlLabels\": false}, "
        "\"class\": {\"htmlLabels\": false}, \"state\": {\"htmlLabels\": false}, "
        "\"er\": {\"htmlLabels\": false}, \"pie\": {\"htmlLabels\": false}, "
        "\"c4\": {\"htmlLabels\": false}, \"themeVariables\": {\"background\": \"transparent\", "
        "\"lineColor\": \"" + std::string(kAccentColor) + "\", "
        "\"primaryBorderColor\": \"" + std::string(kAccentColor) + "\", "
        "\"secondaryBorderColor\": \"" + std::string(kAccentColor) + "\", "
        "\"tertiaryBorderColor\": \"" + std::string(kAccentColor) + "\", "
        "\"nodeBorder\": \"" + std::string(kAccentColor) + "\", "
        "\"clusterBorder\": \"" + std::string(kAccentColor) + "\", "
        "\"defaultLinkColor\": \"" + std::string(kAccentColor) + "\", "
        "\"actorBorder\": \"" + std::string(kAccentColor) + "\", "
        "\"activationBorderColor\": \"" + std::string(kAccentColor) + "\", "
        "\"labelBoxBorderColor\": \"" + std::string(kAccentColor) + "\", "
        "\"signalColor\": \"" + std::string(kAccentColor) + "\"}}}%%\n";
    return config + code;
}

// Same idea as mermaidWithTheme: the skinparams live in the source, so one
// decorated string serves both the web and the local renderer.
std::string plantUmlWithTheme(const std::string &code, bool darkMode)
{
    std::string modified = code;
    size_t startIdx = modified.find("@startuml");
    std::string skin = darkMode
        ? "\nskinparam backgroundColor transparent\n"
          "skinparam defaultFontColor #f0f6fc\n"
          "skinparam ParticipantBackgroundColor #21262d\n"
          "skinparam ParticipantBorderColor " + std::string(kAccentColor) + "\n"
          "skinparam ParticipantFontColor #f0f6fc\n"
          "skinparam ActorBackgroundColor #21262d\n"
          "skinparam ActorBorderColor " + std::string(kAccentColor) + "\n"
          "skinparam ActorFontColor #f0f6fc\n"
          "skinparam SequenceGroupBackgroundColor #161b22\n"
          "skinparam SequenceGroupBorderColor #8b949e\n"
          "skinparam SequenceGroupHeaderFontColor #f0f6fc\n"
          "skinparam SequenceLifeLineBorderColor " + std::string(kAccentColor) + "\n"
          "skinparam SequenceLifeLineBackgroundColor #161b22\n"
          "skinparam ArrowColor " + std::string(kAccentColor) + "\n"
          "skinparam ActivityBackgroundColor #21262d\n"
          "skinparam ActivityBorderColor " + std::string(kAccentColor) + "\n"
          "skinparam ActivityFontColor #f0f6fc\n"
          "skinparam ClassBackgroundColor #21262d\n"
          "skinparam ClassHeaderBackgroundColor #161b22\n"
          "skinparam ClassBorderColor " + std::string(kAccentColor) + "\n"
          "skinparam ClassFontColor #f0f6fc\n"
          "skinparam NoteBackgroundColor #21262d\n"
          "skinparam NoteBorderColor " + std::string(kAccentColor) + "\n"
          "skinparam NoteFontColor #f0f6fc\n"
        : "\nskinparam backgroundColor transparent\n"
          "skinparam ParticipantBorderColor " + std::string(kAccentColor) + "\n"
          "skinparam ActorBorderColor " + std::string(kAccentColor) + "\n"
          "skinparam SequenceLifeLineBorderColor " + std::string(kAccentColor) + "\n"
          "skinparam ArrowColor " + std::string(kAccentColor) + "\n"
          "skinparam ActivityBorderColor " + std::string(kAccentColor) + "\n"
          "skinparam ClassBorderColor " + std::string(kAccentColor) + "\n"
          "skinparam NoteBorderColor " + std::string(kAccentColor) + "\n";

    if (startIdx != std::string::npos) modified.insert(startIdx + 9, skin);
    else modified = skin + modified;
    return modified;
}

std::string renderMermaidWeb(const std::string &code, bool darkMode)
{
    return httpGet(g_mermaidBaseUrl + "/svg/" + base64UrlEncode(mermaidWithTheme(code, darkMode)));
}

std::string renderPlantUmlWeb(const std::string &code, bool darkMode)
{
    return httpGet(g_plantUmlBaseUrl + "/svg/~h" + toHex(plantUmlWithTheme(code, darkMode)));
}

std::string renderMermaid(const std::string &code, bool darkMode)
{
    if (LocalDiagram::isCompiledIn())
        return LocalDiagram::renderMermaid(mermaidWithTheme(code, darkMode));
    return renderMermaidWeb(code, darkMode);
}

std::string renderPlantUml(const std::string &code, bool darkMode)
{
    if (LocalDiagram::isCompiledIn())
        return LocalDiagram::renderPlantUml(plantUmlWithTheme(code, darkMode));
    return renderPlantUmlWeb(code, darkMode);
}

std::string renderVegaLiteWeb(const std::string &vegaLiteJson, bool darkMode)
{
    // "sans-serif" rather than the document's own CSS body family: the
    // endpoint lays the text out with ITS fonts, and asking for a family it
    // does not have buys nothing. It is also what makes the returned SVG
    // rasterize predictably here, since librsvg resolves generic
    // sans-serif locally. The spec's own config.font, if it sets one, still
    // wins -- normalizeSpec only fills defaults in underneath.
    //
    // Note the layout is still approximate across the two editions: Kroki
    // measures text with its fonts and librsvg draws it with local ones, so
    // where the two disagree a long label can shift or collide.
    std::string spec = VegaLiteSpec::normalizeSpec(vegaLiteJson, darkMode, "sans-serif");
    if (spec.empty()) return {};
    return krokiRenderSvg("vegalite", spec);
}

// ── Hand-written replacements for what used to be static const std::regex
// objects in this function (foreignObjRe, textTspanRe, yRe, dyRe, textYRe,
// emRe) plus fixPlantUmlSvgDark's three std::regex_replace calls. Confirmed
// via a live gdb session against the real deployed plugin: constructing ANY
// of these std::regex objects crashes with SIGSEGV, on DC's own main thread,
// with a full symbolized backtrace showing regex_traits<char>::transform ->
// collate<char>::transform dispatching into
// codecvt<char16_t,char,__mbstate_t>::do_unshift -- a char-collate call
// landing in a completely unrelated char16_t codecvt facet. That is libstdc++
// locale facet-ID corruption/mismatch, not a stack or threading problem (both
// were tried first, in this exact order, and neither changed the crash).
// Every pattern in this file is simple enough to hand-write; doing so removes
// the dependency on libstdc++'s <regex>/<locale> machinery here entirely,
// rather than continuing to chase an unreliable library facility. ──

// Regex `\bNAME\s*=\s*"[^"]*"` generalized to any attribute name: finds the
// next `name="value"` (tolerant of whitespace around `=`) starting at `from`,
// requiring a non-word character (or start of string) immediately before
// `name` -- the hand-rolled equivalent of \b. Returns false if not found.
bool findQuotedAttr(const std::string &s, const std::string &name, size_t from,
                    size_t &matchStart, size_t &matchEnd, std::string &value)
{
    size_t pos = from;
    while (true) {
        pos = s.find(name, pos);
        if (pos == std::string::npos) return false;
        if (pos > 0) {
            char prev = s[pos - 1];
            if (isalnum((unsigned char)prev) || prev == '_') { ++pos; continue; }
        }
        size_t p = pos + name.size();
        while (p < s.size() && isspace((unsigned char)s[p])) ++p;
        if (p >= s.size() || s[p] != '=') { ++pos; continue; }
        ++p;
        while (p < s.size() && isspace((unsigned char)s[p])) ++p;
        if (p >= s.size() || s[p] != '"') { ++pos; continue; }
        size_t valStart = p + 1;
        size_t valEnd = s.find('"', valStart);
        if (valEnd == std::string::npos) return false;
        matchStart = pos;
        matchEnd = valEnd + 1;
        value = s.substr(valStart, valEnd - valStart);
        return true;
    }
}

// Validates `-?[0-9]*\.?[0-9]+` starting at `start` (at least one digit
// somewhere, at most one '.', optional leading '-'). Sets numEnd to just
// past the numeric text on success.
bool isValidEmNumber(const std::string &s, size_t start, size_t &numEnd)
{
    size_t i = start;
    if (i < s.size() && s[i] == '-') ++i;
    size_t digitCount = 0;
    bool sawDot = false;
    while (i < s.size()) {
        if (isdigit((unsigned char)s[i])) { ++i; ++digitCount; }
        else if (s[i] == '.' && !sawDot) { sawDot = true; ++i; }
        else break;
    }
    if (digitCount == 0) return false;
    numEnd = i;
    return true;
}

// Regex `\bNAME\s*=\s*"(-?[0-9]*\.?[0-9]+)em"`: like findQuotedAttr, but the
// quoted value must be exactly a number immediately followed by "em".
bool findNumericEmAttr(const std::string &s, const std::string &name, size_t from,
                       size_t &matchStart, size_t &matchEnd, double &emValue)
{
    size_t pos = from;
    while (true) {
        pos = s.find(name, pos);
        if (pos == std::string::npos) return false;
        if (pos > 0) {
            char prev = s[pos - 1];
            if (isalnum((unsigned char)prev) || prev == '_') { ++pos; continue; }
        }
        size_t p = pos + name.size();
        while (p < s.size() && isspace((unsigned char)s[p])) ++p;
        if (p >= s.size() || s[p] != '=') { ++pos; continue; }
        ++p;
        while (p < s.size() && isspace((unsigned char)s[p])) ++p;
        if (p >= s.size() || s[p] != '"') { ++pos; continue; }
        size_t valStart = p + 1;
        size_t numEnd;
        if (!isValidEmNumber(s, valStart, numEnd) || s.compare(numEnd, 2, "em") != 0
            || numEnd + 2 >= s.size() || s[numEnd + 2] != '"') { ++pos; continue; }
        matchStart = pos;
        matchEnd = numEnd + 3; // "em" + closing quote
        emValue = safeStod(s.substr(valStart, numEnd - valStart));
        return true;
    }
}

// Regex `\b(y|dy)\s*=\s*"(-?[0-9]*\.?[0-9]+)em"`: earliest of either "y=" or
// "dy=" (proper \b handling already keeps these from colliding -- "dy=" is
// never mistaken for a standalone "y=" since 'd' immediately precedes it).
bool findYOrDyEmAttr(const std::string &s, size_t from, size_t &matchStart, size_t &matchEnd,
                     std::string &attrName, double &emValue)
{
    size_t yStart, yEnd; double yVal;
    size_t dyStart, dyEnd; double dyVal;
    bool hasY = findNumericEmAttr(s, "y", from, yStart, yEnd, yVal);
    bool hasDy = findNumericEmAttr(s, "dy", from, dyStart, dyEnd, dyVal);
    if (!hasY && !hasDy) return false;
    if (hasY && (!hasDy || yStart <= dyStart)) {
        matchStart = yStart; matchEnd = yEnd; attrName = "y"; emValue = yVal;
    } else {
        matchStart = dyStart; matchEnd = dyEnd; attrName = "dy"; emValue = dyVal;
    }
    return true;
}

// Regex `<text\b([^>]*)>\s*<tspan\b([^>]*)>`.
bool findTextTspanPair(const std::string &s, size_t from, size_t &matchStart, size_t &matchEnd,
                       std::string &textAttrs, std::string &tspanAttrs)
{
    size_t pos = from;
    while (true) {
        pos = s.find("<text", pos);
        if (pos == std::string::npos) return false;
        size_t afterTag = pos + 5;
        if (afterTag < s.size() && (isalnum((unsigned char)s[afterTag]) || s[afterTag] == '_')) { pos += 5; continue; }
        size_t textAttrsEnd = s.find('>', afterTag);
        if (textAttrsEnd == std::string::npos) return false;
        std::string tAttrs = s.substr(afterTag, textAttrsEnd - afterTag);
        size_t p = textAttrsEnd + 1;
        while (p < s.size() && isspace((unsigned char)s[p])) ++p;
        if (s.compare(p, 6, "<tspan") != 0) { pos += 5; continue; }
        size_t tspanAfter = p + 6;
        if (tspanAfter < s.size() && (isalnum((unsigned char)s[tspanAfter]) || s[tspanAfter] == '_')) { pos += 5; continue; }
        size_t tspanAttrsEnd = s.find('>', tspanAfter);
        if (tspanAttrsEnd == std::string::npos) return false;
        textAttrs = tAttrs;
        tspanAttrs = s.substr(tspanAfter, tspanAttrsEnd - tspanAfter);
        matchStart = pos;
        matchEnd = tspanAttrsEnd + 1;
        return true;
    }
}

// Regex `\s+` collapse-to-single-space, mirroring QString::simplified()'s
// intent (used on tspanAttrs after stripping y/dy).
std::string collapseWhitespace(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    bool inSpace = false;
    for (char c : s) {
        if (isspace((unsigned char)c)) {
            if (!inSpace) out += ' ';
            inSpace = true;
        } else {
            out += c;
            inSpace = false;
        }
    }
    return out;
}

// Removes one findNumericEmAttr-style match from a string, if present.
std::string removeNumericEmAttr(const std::string &s, const std::string &name)
{
    size_t ms, me; double v;
    if (!findNumericEmAttr(s, name, 0, ms, me, v)) return s;
    return s.substr(0, ms) + s.substr(me);
}

// Regex `<foreignObject\s+width="([^"]+)"\s+height="([^"]+)"[^>]*>.*?
// <span[^>]*>(?:<p>)?(.*?)(?:</p>)?</span>.*?</foreignObject>`. The lazy
// `.*?` quantifiers are naturally reproduced by std::string::find() picking
// the FIRST occurrence of what follows -- the standard equivalence between
// non-greedy regex matching and a linear "find the next occurrence" scan.
// One simplification from the original: `.` in ECMAScript regex mode doesn't
// match newlines by default (not set here), while find() does span them;
// immaterial for real mermaid/plantuml output, where these spans are always
// single-line.
bool findForeignObject(const std::string &s, size_t from, size_t &matchStart, size_t &matchEnd,
                       std::string &width, std::string &height, std::string &innerText)
{
    size_t pos = from;
    while (true) {
        pos = s.find("<foreignObject", pos);
        if (pos == std::string::npos) return false;
        size_t p = pos + 14;
        if (p >= s.size() || !isspace((unsigned char)s[p])) { pos += 14; continue; }
        while (p < s.size() && isspace((unsigned char)s[p])) ++p;
        if (s.compare(p, 7, "width=\"") != 0) { pos += 14; continue; }
        p += 7;
        size_t wEnd = s.find('"', p);
        if (wEnd == std::string::npos) return false;
        if (wEnd == p) { pos += 14; continue; } // width="" doesn't match [^"]+ (one or more)
        std::string w = s.substr(p, wEnd - p);
        p = wEnd + 1;
        if (p >= s.size() || !isspace((unsigned char)s[p])) { pos += 14; continue; }
        while (p < s.size() && isspace((unsigned char)s[p])) ++p;
        if (s.compare(p, 8, "height=\"") != 0) { pos += 14; continue; }
        p += 8;
        size_t hEnd = s.find('"', p);
        if (hEnd == std::string::npos) return false;
        if (hEnd == p) { pos += 14; continue; }
        std::string h = s.substr(p, hEnd - p);
        p = hEnd + 1;
        size_t tagEnd = s.find('>', p);
        if (tagEnd == std::string::npos) return false;
        p = tagEnd + 1;
        size_t spanPos = s.find("<span", p);
        if (spanPos == std::string::npos) return false;
        size_t spanTagEnd = s.find('>', spanPos);
        if (spanTagEnd == std::string::npos) return false;
        size_t contentStart = spanTagEnd + 1;
        if (s.compare(contentStart, 3, "<p>") == 0) contentStart += 3;
        size_t spanClose = s.find("</span>", contentStart);
        if (spanClose == std::string::npos) return false;
        size_t contentEnd = spanClose;
        static const std::string pClose = "</p>";
        if (contentEnd >= contentStart + pClose.size() &&
            s.compare(contentEnd - pClose.size(), pClose.size(), pClose) == 0)
            contentEnd -= pClose.size();
        std::string inner = s.substr(contentStart, contentEnd - contentStart);
        size_t afterSpan = spanClose + 7;
        size_t fEnd = s.find("</foreignObject>", afterSpan);
        if (fEnd == std::string::npos) return false;
        matchStart = pos;
        matchEnd = fEnd + 16;
        width = w; height = h; innerText = inner;
        return true;
    }
}

std::string fixMermaidSvgText(const std::string &svgIn, bool darkMode)
{
    std::string svg = svgIn;

    // <foreignObject> HTML-label workaround -> plain <text> librsvg can render.
    std::string textColor = darkMode ? "#c9d1d9" : "#333333";
    {
        std::string out;
        size_t lastEnd = 0, searchFrom = 0;
        size_t ms, me; std::string wStr, hStr, inner;
        while (findForeignObject(svg, searchFrom, ms, me, wStr, hStr, inner)) {
            out.append(svg, lastEnd, ms - lastEnd);
            double w = safeStod(wStr);
            double h = safeStod(hStr);
            char buf[256];
            snprintf(buf, sizeof(buf),
                     "<text x=\"%g\" y=\"%g\" dominant-baseline=\"middle\" text-anchor=\"middle\" "
                     "font-family=\"sans-serif\" font-size=\"14px\" fill=\"%s\">",
                     w / 2.0, h / 2.0 + 2.0, textColor.c_str());
            out += buf;
            out += inner;
            out += "</text>";
            lastEnd = me;
            searchFrom = me;
        }
        out.append(svg, lastEnd, std::string::npos);
        svg = out;
    }

    // Combine <tspan y="..em" dy="..em"> into an absolute pixel y on <text>.
    {
        std::string out;
        size_t lastEnd = 0, searchFrom = 0;
        size_t ms, me; std::string textAttrs, tspanAttrs;
        while (findTextTspanPair(svg, searchFrom, ms, me, textAttrs, tspanAttrs)) {
            size_t yMs, yMe, dyMs, dyMe; double yEm, dyEm;
            bool hasY = findNumericEmAttr(tspanAttrs, "y", 0, yMs, yMe, yEm);
            bool hasDy = findNumericEmAttr(tspanAttrs, "dy", 0, dyMs, dyMe, dyEm);
            if (hasY && hasDy) {
                double baselinePx = (yEm + dyEm) * 16.0 - 2.0;
                char yAttr[64];
                snprintf(yAttr, sizeof(yAttr), "y=\"%.2f\"", baselinePx);

                size_t textYMs, textYMe; std::string dummy;
                if (findQuotedAttr(textAttrs, "y", 0, textYMs, textYMe, dummy))
                    textAttrs = textAttrs.substr(0, textYMs) + yAttr + textAttrs.substr(textYMe);
                else
                    textAttrs = std::string(" ") + yAttr + textAttrs;

                tspanAttrs = removeNumericEmAttr(tspanAttrs, "y");
                tspanAttrs = removeNumericEmAttr(tspanAttrs, "dy");
                tspanAttrs = collapseWhitespace(tspanAttrs);
                if (!tspanAttrs.empty() && tspanAttrs[0] != ' ') tspanAttrs = " " + tspanAttrs;

                out.append(svg, lastEnd, ms - lastEnd);
                out += "<text" + textAttrs + "><tspan" + tspanAttrs + ">";
                lastEnd = me;
            }
            searchFrom = me;
        }
        out.append(svg, lastEnd, std::string::npos);
        svg = out;
    }

    // Remaining bare em -> px conversions.
    {
        std::string out;
        size_t lastEnd = 0, searchFrom = 0;
        size_t ms, me; std::string attrName; double emVal;
        while (findYOrDyEmAttr(svg, searchFrom, ms, me, attrName, emVal)) {
            out.append(svg, lastEnd, ms - lastEnd);
            double px = emVal * 16.0;
            char buf[64];
            snprintf(buf, sizeof(buf), "%s=\"%.2f\"", attrName.c_str(), px);
            out += buf;
            lastEnd = me;
            searchFrom = me;
        }
        out.append(svg, lastEnd, std::string::npos);
        svg = out;
    }

    return svg;
}

// Replaces every occurrence of any of `needles` with `replacement`.
std::string replaceAllOf(const std::string &s, const std::vector<std::string> &needles,
                          const std::string &replacement)
{
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        bool matched = false;
        for (const auto &needle : needles) {
            if (s.compare(i, needle.size(), needle) == 0) {
                out += replacement;
                i += needle.size();
                matched = true;
                break;
            }
        }
        if (!matched) out += s[i++];
    }
    return out;
}

std::string fixPlantUmlSvgDark(const std::string &svgIn)
{
    std::string svg = svgIn;
    // Regex `stroke="(#181818|#000000|#333333|#000|black)"`.
    svg = replaceAllOf(svg, {"stroke=\"#181818\"", "stroke=\"#000000\"", "stroke=\"#333333\"",
                             "stroke=\"#000\"", "stroke=\"black\""}, "stroke=\"#58a6ff\"");
    // Regex `stroke\s*:\s*(#181818|#000000|#333333|#000|black)` -- real
    // PlantUML/mermaid output never puts whitespace around this colon, so a
    // plain substring match covers it without reproducing \s* generically.
    svg = replaceAllOf(svg, {"stroke:#181818", "stroke:#000000", "stroke:#333333",
                             "stroke:#000", "stroke:black"}, "stroke:#58a6ff");
    // Regex `stroke-width:0\.[0-9]+` -> "stroke-width:1.0".
    {
        std::string out;
        size_t i = 0;
        static const std::string prefix = "stroke-width:0.";
        while (i < svg.size()) {
            if (svg.compare(i, prefix.size(), prefix) == 0) {
                size_t j = i + prefix.size();
                size_t digitsEnd = j;
                while (digitsEnd < svg.size() && isdigit((unsigned char)svg[digitsEnd])) ++digitsEnd;
                if (digitsEnd > j) { // at least one digit, matching [0-9]+
                    out += "stroke-width:1.0";
                    i = digitsEnd;
                    continue;
                }
            }
            out += svg[i++];
        }
        svg = out;
    }
    return svg;
}

std::vector<uint8_t> svgToHighDpiPng(const std::string &svgData, float scale, bool darkMode,
                                      int &logicalWidth, int &logicalHeight)
{
    GError *error = nullptr;
    RsvgHandle *handle = rsvg_handle_new_from_data(
        reinterpret_cast<const guint8 *>(svgData.data()), (gsize)svgData.size(), &error);
    if (!handle) { if (error) g_error_free(error); return {}; }

    gdouble w = 0, h = 0;
    gboolean hasSize = rsvg_handle_get_intrinsic_size_in_pixels(handle, &w, &h);
    if (!hasSize || w <= 0 || h <= 0) {
        RsvgRectangle vb{};
        gboolean hasViewbox = FALSE, dW = FALSE, dH = FALSE;
        rsvg_handle_get_intrinsic_dimensions(handle, &dW, nullptr, &dH, nullptr, &hasViewbox, &vb);
        if (hasViewbox && vb.width > 0 && vb.height > 0) { w = vb.width; h = vb.height; }
        else { w = 800; h = 600; }
    }
    // librsvg's reported size is trusted verbatim below with no bound at
    // all -- a percentage-based viewBox or other malformed SVG can make w/h
    // non-finite or absurdly large. cairo_image_surface_create() with a
    // pathological size can itself abort/crash (Cairo's own docs note
    // behavior is undefined beyond its internal size limits), and even a
    // "successful" multi-gigabyte allocation attempt is a crash risk on its
    // own. Same bug class as diagramview_gtk3's fitToView() zoom, which
    // needed the same kind of clamp for the same underlying reason
    // (librsvg/Cairo dimensions from renderer output, trusted unchecked).
    if (!std::isfinite(w) || !std::isfinite(h) || w <= 0 || h <= 0) {
        w = 800; h = 600;
    }
    constexpr double kMaxDim = 8000.0; // generous; well beyond any real diagram, far below a crash-risk allocation
    if (w > kMaxDim || h > kMaxDim) {
        double capScale = kMaxDim / std::max(w, h);
        w *= capScale; h *= capScale;
    }
    logicalWidth = (int)w;
    logicalHeight = (int)h;

    int pixelWidth = std::max(1, (int)(w * scale));
    int pixelHeight = std::max(1, (int)(h * scale));

    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pixelWidth, pixelHeight);
    cairo_t *cr = cairo_create(surface);

    cairo_set_source_rgba(cr, darkMode ? 0x0d / 255.0 : 0xFA / 255.0,
                              darkMode ? 0x11 / 255.0 : 0xFA / 255.0,
                              darkMode ? 0x17 / 255.0 : 0xFA / 255.0, 0.0);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_scale(cr, (double)pixelWidth / w, (double)pixelHeight / h);

    RsvgRectangle viewport{0, 0, w, h};
    rsvg_handle_render_document(handle, cr, &viewport, &error);
    if (error) { g_error_free(error); error = nullptr; }

    std::string pngBytes;
    cairo_surface_write_to_png_stream(surface, writeToString, &pngBytes);

    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    g_object_unref(handle);

    return std::vector<uint8_t>(pngBytes.begin(), pngBytes.end());
}

} // namespace DiagramRender
