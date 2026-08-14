#include "diagram_render.h"

#include <cairo.h>
#include <librsvg/rsvg.h>

#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>
#include <poll.h>
#include <signal.h>
#include <chrono>
#include <regex>
#include <cstring>
#include <cstdio>
#include <algorithm>

extern char **environ;

namespace {

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

cairo_status_t writeToString(void *closure, const unsigned char *data, unsigned int length) {
    auto *out = static_cast<std::string *>(closure);
    out->append(reinterpret_cast<const char *>(data), length);
    return CAIRO_STATUS_SUCCESS;
}

} // namespace

namespace DiagramRender {

std::string renderMermaidWeb(const std::string &code, bool darkMode)
{
    std::string theme = darkMode ? "\"dark\"" : "\"default\"";
    std::string config = "%%{init: {\"theme\": " + theme +
        ", \"flowchart\": {\"htmlLabels\": false}, \"sequence\": {\"htmlLabels\": false}, "
        "\"gantt\": {\"htmlLabels\": false}, \"journey\": {\"htmlLabels\": false}, "
        "\"class\": {\"htmlLabels\": false}, \"state\": {\"htmlLabels\": false}, "
        "\"er\": {\"htmlLabels\": false}, \"pie\": {\"htmlLabels\": false}, "
        "\"c4\": {\"htmlLabels\": false}, \"themeVariables\": {\"background\": \"transparent\"}}}%%\n";
    std::string url = "https://mermaid.ink/svg/" + base64UrlEncode(config + code);
    return httpGet(url);
}

std::string renderPlantUmlWeb(const std::string &code, bool darkMode)
{
    std::string modified = code;
    size_t startIdx = modified.find("@startuml");
    std::string skin = darkMode
        ? "\nskinparam backgroundColor transparent\n"
          "skinparam defaultFontColor #f0f6fc\n"
          "skinparam ParticipantBackgroundColor #21262d\n"
          "skinparam ParticipantBorderColor #58a6ff\n"
          "skinparam ParticipantFontColor #f0f6fc\n"
          "skinparam ActorBackgroundColor #21262d\n"
          "skinparam ActorBorderColor #58a6ff\n"
          "skinparam ActorFontColor #f0f6fc\n"
          "skinparam SequenceGroupBackgroundColor #161b22\n"
          "skinparam SequenceGroupBorderColor #8b949e\n"
          "skinparam SequenceGroupHeaderFontColor #f0f6fc\n"
          "skinparam SequenceLifeLineBorderColor #58a6ff\n"
          "skinparam SequenceLifeLineBackgroundColor #161b22\n"
          "skinparam ArrowColor #58a6ff\n"
          "skinparam ActivityBackgroundColor #21262d\n"
          "skinparam ActivityBorderColor #58a6ff\n"
          "skinparam ActivityFontColor #f0f6fc\n"
          "skinparam ClassBackgroundColor #21262d\n"
          "skinparam ClassHeaderBackgroundColor #161b22\n"
          "skinparam ClassBorderColor #58a6ff\n"
          "skinparam ClassFontColor #f0f6fc\n"
          "skinparam NoteBackgroundColor #21262d\n"
          "skinparam NoteBorderColor #58a6ff\n"
          "skinparam NoteFontColor #f0f6fc\n"
        : "\nskinparam backgroundColor transparent\n";

    if (startIdx != std::string::npos) modified.insert(startIdx + 9, skin);
    else modified = skin + modified;

    std::string url = "http://www.plantuml.com/plantuml/svg/~h" + toHex(modified);
    return httpGet(url);
}

std::string fixMermaidSvgText(const std::string &svgIn, bool darkMode)
{
    std::string svg = svgIn;

    // <foreignObject> HTML-label workaround -> plain <text> librsvg can render.
    static const std::regex foreignObjRe(
        R"RX(<foreignObject\s+width="([^"]+)"\s+height="([^"]+)"[^>]*>.*?<span[^>]*>(?:<p>)?(.*?)(?:</p>)?</span>.*?</foreignObject>)RX");
    std::string textColor = darkMode ? "#c9d1d9" : "#333333";
    {
        std::string out;
        auto begin = std::sregex_iterator(svg.begin(), svg.end(), foreignObjRe);
        auto end = std::sregex_iterator();
        size_t lastEnd = 0;
        for (auto it = begin; it != end; ++it) {
            auto &m = *it;
            out.append(svg, lastEnd, m.position(0) - lastEnd);
            double w = std::stod(m[1].str());
            double h = std::stod(m[2].str());
            char buf[256];
            snprintf(buf, sizeof(buf),
                     "<text x=\"%g\" y=\"%g\" dominant-baseline=\"middle\" text-anchor=\"middle\" "
                     "font-family=\"sans-serif\" font-size=\"14px\" fill=\"%s\">",
                     w / 2.0, h / 2.0 + 2.0, textColor.c_str());
            out += buf;
            out += m[3].str();
            out += "</text>";
            lastEnd = m.position(0) + m.length(0);
        }
        out.append(svg, lastEnd, std::string::npos);
        svg = out;
    }

    // Combine <tspan y="..em" dy="..em"> into an absolute pixel y on <text>.
    static const std::regex textTspanRe(R"RX(<text\b([^>]*)>\s*<tspan\b([^>]*)>)RX");
    static const std::regex yRe(R"RX(\by\s*=\s*"(-?[0-9]*\.?[0-9]+)em")RX");
    static const std::regex dyRe(R"RX(\bdy\s*=\s*"(-?[0-9]*\.?[0-9]+)em")RX");
    static const std::regex textYRe(R"RX(\by\s*=\s*"[^"]*")RX");
    {
        std::string out;
        auto begin = std::sregex_iterator(svg.begin(), svg.end(), textTspanRe);
        auto end = std::sregex_iterator();
        size_t lastEnd = 0;
        for (auto it = begin; it != end; ++it) {
            auto &m = *it;
            std::string textAttrs = m[1].str();
            std::string tspanAttrs = m[2].str();
            std::smatch ym, dym;
            if (std::regex_search(tspanAttrs, ym, yRe) && std::regex_search(tspanAttrs, dym, dyRe)) {
                double yEm = std::stod(ym[1].str());
                double dyEm = std::stod(dym[1].str());
                double baselinePx = (yEm + dyEm) * 16.0 - 2.0;
                char yAttr[64];
                snprintf(yAttr, sizeof(yAttr), "y=\"%.2f\"", baselinePx);
                std::smatch textY;
                if (std::regex_search(textAttrs, textY, textYRe))
                    textAttrs = std::regex_replace(textAttrs, textYRe, yAttr);
                else
                    textAttrs = std::string(" ") + yAttr + textAttrs;
                tspanAttrs = std::regex_replace(tspanAttrs, yRe, "");
                tspanAttrs = std::regex_replace(tspanAttrs, dyRe, "");
                // collapse whitespace
                tspanAttrs = std::regex_replace(tspanAttrs, std::regex(R"RX(\s+)RX"), " ");
                if (!tspanAttrs.empty() && tspanAttrs[0] != ' ') tspanAttrs = " " + tspanAttrs;

                out.append(svg, lastEnd, m.position(0) - lastEnd);
                out += "<text" + textAttrs + "><tspan" + tspanAttrs + ">";
                lastEnd = m.position(0) + m.length(0);
            }
        }
        out.append(svg, lastEnd, std::string::npos);
        svg = out;
    }

    // Remaining bare em -> px conversions.
    static const std::regex emRe(R"RX(\b(y|dy)\s*=\s*"(-?[0-9]*\.?[0-9]+)em")RX");
    {
        std::string out;
        auto begin = std::sregex_iterator(svg.begin(), svg.end(), emRe);
        auto end = std::sregex_iterator();
        size_t lastEnd = 0;
        for (auto it = begin; it != end; ++it) {
            auto &m = *it;
            out.append(svg, lastEnd, m.position(0) - lastEnd);
            double px = std::stod(m[2].str()) * 16.0;
            char buf[64];
            snprintf(buf, sizeof(buf), "%s=\"%.2f\"", m[1].str().c_str(), px);
            out += buf;
            lastEnd = m.position(0) + m.length(0);
        }
        out.append(svg, lastEnd, std::string::npos);
        svg = out;
    }

    return svg;
}

std::string fixPlantUmlSvgDark(const std::string &svgIn)
{
    std::string svg = svgIn;
    svg = std::regex_replace(svg, std::regex(R"RX(stroke="(#181818|#000000|#333333|#000|black)")RX"), "stroke=\"#58a6ff\"");
    svg = std::regex_replace(svg, std::regex(R"RX(stroke\s*:\s*(#181818|#000000|#333333|#000|black))RX"), "stroke:#58a6ff");
    svg = std::regex_replace(svg, std::regex(R"RX(stroke-width:0\.[0-9]+)RX"), "stroke-width:1.0");
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
