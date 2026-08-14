#include "DiagramRenderer.h"

#include <dlfcn.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <poll.h>
#include <signal.h>
#include <cstring>
#include <chrono>
#include <fstream>
#include <sstream>
#include <regex>
#include <vector>
#include <map>
#include <cstdio>

extern char **environ;

namespace {

// ── Minimal subprocess runner (replaces QProcess) ──────────────────────
// Captures stdout only; timeout is enforced via poll() on the read end,
// which covers the real failure mode we care about (a hung renderer),
// same as QProcess::waitForFinished(timeoutMs) did.

struct ProcessResult {
    bool started = false;
    int exitCode = -1;
    std::string stdoutData;
};

ProcessResult runProcess(const std::string &exe, const std::vector<std::string> &args,
                          const std::string *stdinData, int timeoutMs)
{
    ProcessResult result;

    int outPipe[2];
    if (pipe(outPipe) != 0) return result;

    int inPipe[2] = {-1, -1};
    if (stdinData && pipe(inPipe) != 0) {
        close(outPipe[0]); close(outPipe[1]);
        return result;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, outPipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, outPipe[0]);
    posix_spawn_file_actions_addclose(&actions, outPipe[1]);
    if (stdinData) {
        posix_spawn_file_actions_adddup2(&actions, inPipe[0], STDIN_FILENO);
        posix_spawn_file_actions_addclose(&actions, inPipe[0]);
        posix_spawn_file_actions_addclose(&actions, inPipe[1]);
    }

    std::vector<char *> argv;
    argv.push_back(const_cast<char *>(exe.c_str()));
    for (auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);

    pid_t pid = -1;
    int rc = posix_spawnp(&pid, exe.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);

    close(outPipe[1]);
    if (stdinData) close(inPipe[0]);

    if (rc != 0) {
        close(outPipe[0]);
        if (stdinData) close(inPipe[1]);
        return result;
    }
    result.started = true;

    if (stdinData) {
        size_t off = 0;
        while (off < stdinData->size()) {
            ssize_t n = write(inPipe[1], stdinData->data() + off, stdinData->size() - off);
            if (n <= 0) break;
            off += static_cast<size_t>(n);
        }
        close(inPipe[1]);
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    bool timedOut = false;
    char buf[8192];
    while (true) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - std::chrono::steady_clock::now())
                              .count();
        if (remaining <= 0) { timedOut = true; break; }

        struct pollfd pfd { outPipe[0], POLLIN, 0 };
        int pr = poll(&pfd, 1, static_cast<int>(remaining));
        if (pr < 0) break;
        if (pr == 0) { timedOut = true; break; }

        if (pfd.revents & (POLLIN | POLLHUP)) {
            ssize_t n = read(outPipe[0], buf, sizeof(buf));
            if (n > 0)
                result.stdoutData.append(buf, static_cast<size_t>(n));
            else
                break; // EOF or error
        } else {
            break;
        }
    }
    close(outPipe[0]);

    if (timedOut)
        kill(pid, SIGKILL);

    int status = 0;
    waitpid(pid, &status, 0);
    if (!timedOut && WIFEXITED(status))
        result.exitCode = WEXITSTATUS(status);
    return result;
}

bool fileExists(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

std::string readFile(const std::string &path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool writeFile(const std::string &path, const std::string &data)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    return f.good();
}

std::string toLower(std::string s)
{
    for (auto &c : s) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string extensionOf(const std::string &path)
{
    auto slash = path.find_last_of('/');
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return {};
    return toLower(path.substr(dot + 1));
}

std::string dirOf(const std::string &path)
{
    auto slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string base64Encode(const std::string &data)
{
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 2 < data.size()) {
        unsigned v = (static_cast<unsigned char>(data[i]) << 16)
                   | (static_cast<unsigned char>(data[i + 1]) << 8)
                   | static_cast<unsigned char>(data[i + 2]);
        out += table[(v >> 18) & 0x3F];
        out += table[(v >> 12) & 0x3F];
        out += table[(v >> 6) & 0x3F];
        out += table[v & 0x3F];
        i += 3;
    }
    size_t rem = data.size() - i;
    if (rem == 1) {
        unsigned v = static_cast<unsigned char>(data[i]) << 16;
        out += table[(v >> 18) & 0x3F];
        out += table[(v >> 12) & 0x3F];
        out += "==";
    } else if (rem == 2) {
        unsigned v = (static_cast<unsigned char>(data[i]) << 16) | (static_cast<unsigned char>(data[i + 1]) << 8);
        out += table[(v >> 18) & 0x3F];
        out += table[(v >> 12) & 0x3F];
        out += table[(v >> 6) & 0x3F];
        out += '=';
    }
    return out;
}

std::string percentEncode(const std::string &data)
{
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(data.size());
    for (unsigned char c : data) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[(c >> 4) & 0xF];
            out += hex[c & 0xF];
        }
    }
    return out;
}

std::string hexEncode(const std::string &data)
{
    static const char *hex = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (unsigned char c : data) {
        out += hex[(c >> 4) & 0xF];
        out += hex[c & 0xF];
    }
    return out;
}

std::string trim(const std::string &s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

} // namespace

namespace DiagramRenderer {

std::string pluginDir()
{
    Dl_info info;
    if (dladdr(reinterpret_cast<void *>(&pluginDir), &info) != 0 && info.dli_fname) {
        std::string path(info.dli_fname);
        auto slash = path.find_last_of('/');
        if (slash != std::string::npos)
            return path.substr(0, slash);
    }
    return {};
}

// ── Settings: minimal "[section]\nkey=value" INI, replacing QSettings ──

void Settings::loadOrInitDefaults(const std::string &iniPath, const std::string &pluginName)
{
    std::ifstream f(iniPath);
    std::map<std::string, std::string> values;
    if (f) {
        std::string line, section;
        while (std::getline(f, line)) {
            line = trim(line);
            if (line.empty() || line[0] == ';' || line[0] == '#') continue;
            if (line.front() == '[' && line.back() == ']') {
                section = line.substr(1, line.size() - 2);
                continue;
            }
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            values[section + "/" + trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
        }
    }

    auto getBool = [&](const std::string &key, bool def) {
        auto it = values.find(pluginName + "/" + key);
        if (it == values.end()) return def;
        return it->second == "true" || it->second == "1";
    };
    auto getStr = [&](const std::string &key, const std::string &def) {
        auto it = values.find(pluginName + "/" + key);
        return it == values.end() ? def : it->second;
    };
    auto getInt = [&](const std::string &key, int def) {
        auto it = values.find(pluginName + "/" + key);
        if (it == values.end()) return def;
        try { return std::stoi(it->second); } catch (...) { return def; }
    };

    autoReloadEnabled = getBool("auto_reload", autoReloadEnabled);
    darkMode = getBool("dark_mode", darkMode);
    useSystemDarkMode = getBool("use_system_dark_mode", useSystemDarkMode);
    renderer = getStr("renderer", renderer);
    mermaidRenderer = getStr("mermaid_renderer", mermaidRenderer);
    plantumlPath = getStr("plantuml_path", plantumlPath);
    javaPath = getStr("java_path", javaPath);
    plantumlServerUrl = getStr("plantuml_server_url", plantumlServerUrl);
    mmdcPath = getStr("mmdc_path", mmdcPath);
    mermaidServerUrl = getStr("mermaid_server_url", mermaidServerUrl);
    timeoutMs = getInt("timeout_ms", timeoutMs);

    save(iniPath, pluginName);
}

void Settings::save(const std::string &iniPath, const std::string &pluginName) const
{
    std::ofstream f(iniPath, std::ios::trunc);
    if (!f) return;
    f << "[" << pluginName << "]\n";
    f << "auto_reload=" << (autoReloadEnabled ? "true" : "false") << "\n";
    f << "dark_mode=" << (darkMode ? "true" : "false") << "\n";
    f << "use_system_dark_mode=" << (useSystemDarkMode ? "true" : "false") << "\n";
    f << "renderer=" << renderer << "\n";
    f << "mermaid_renderer=" << mermaidRenderer << "\n";
    f << "plantuml_path=" << plantumlPath << "\n";
    f << "java_path=" << javaPath << "\n";
    f << "plantuml_server_url=" << plantumlServerUrl << "\n";
    f << "mmdc_path=" << mmdcPath << "\n";
    f << "mermaid_server_url=" << mermaidServerUrl << "\n";
    f << "timeout_ms=" << timeoutMs << "\n";
}

// ── Mermaid ──────────────────────────────────────────────────────────

namespace {

std::string runMermaidWeb(const Settings &s, const std::string &inputPath)
{
    std::string code = readFile(inputPath);
    if (code.empty()) return {};

    std::string url = s.mermaidServerUrl + "/svg/" + percentEncode(base64Encode(code));

    ProcessResult r = runProcess("curl",
        {"-s", "-f", "--max-time", std::to_string(s.timeoutMs / 1000), url},
        nullptr, s.timeoutMs);
    if (r.started && r.exitCode == 0 && !r.stdoutData.empty())
        return r.stdoutData;
    return {};
}

std::string runMermaidLocal(const Settings &s, const std::string &inputPath, bool darkMode)
{
    std::string tempConfigPath = "/tmp/diagramview_mermaid_" + std::to_string(getpid()) + ".json";
    writeFile(tempConfigPath, R"({
  "htmlLabels": false,
  "flowchart": { "htmlLabels": false },
  "sequence": { "htmlLabels": false },
  "gantt": { "htmlLabels": false },
  "journey": { "htmlLabels": false },
  "class": { "htmlLabels": false },
  "state": { "htmlLabels": false },
  "er": { "htmlLabels": false },
  "pie": { "htmlLabels": false },
  "c4": { "htmlLabels": false }
})");

    std::string outputPath = "/tmp/diagramview_mermaid_" + std::to_string(getpid()) + ".svg";

    auto cleanup = [&]() {
        remove(tempConfigPath.c_str());
        remove(outputPath.c_str());
    };

    auto buildArgs = [&](bool withNpxPrefix) {
        std::vector<std::string> args;
        if (withNpxPrefix) { args.push_back("-y"); args.push_back("@mermaid-js/mermaid-cli"); }
        args.push_back("-i"); args.push_back(inputPath);
        args.push_back("-o"); args.push_back(outputPath);
        args.push_back("-e"); args.push_back("svg");
        if (darkMode) { args.push_back("--theme"); args.push_back("dark"); }
        if (fileExists(tempConfigPath)) { args.push_back("-c"); args.push_back(tempConfigPath); }
        return args;
    };

    auto tryExe = [&](const std::string &exe) -> std::string {
        ProcessResult r = runProcess(exe, buildArgs(false), nullptr, s.timeoutMs);
        if (r.started && r.exitCode == 0 && fileExists(outputPath)) {
            std::string svg = readFile(outputPath);
            cleanup();
            return svg;
        }
        return {};
    };

    // 1. configured mmdc_path  2. DC config dir  3. plugin dir  4. plain "mmdc"
    if (auto svg = tryExe(s.mmdcPath); !svg.empty()) return svg;

    std::string configDir = dirOf(s.mmdcPath); // best-effort; real config dir passed by caller via Settings if needed
    std::string pluginMmdc = pluginDir().empty() ? std::string() : pluginDir() + "/mmdc";
    if (!pluginMmdc.empty() && fileExists(pluginMmdc)) {
        if (auto svg = tryExe(pluginMmdc); !svg.empty()) return svg;
    }
    if (s.mmdcPath != "mmdc") {
        if (auto svg = tryExe("mmdc"); !svg.empty()) return svg;
    }

    // 5. npx fallback (3x timeout, matching original)
    ProcessResult r = runProcess("npx", buildArgs(true), nullptr, s.timeoutMs * 3);
    if (r.started && r.exitCode == 0 && fileExists(outputPath)) {
        std::string svg = readFile(outputPath);
        cleanup();
        return svg;
    }

    cleanup();
    return {};
}

} // namespace

std::string renderMermaid(const Settings &s, const std::string &inputPath, bool darkMode)
{
    if (s.mermaidRenderer == "web") {
        if (auto svg = runMermaidWeb(s, inputPath); !svg.empty()) return svg;
    }
    if (auto svg = runMermaidLocal(s, inputPath, darkMode); !svg.empty()) return svg;
    if (s.mermaidRenderer != "web") {
        if (auto svg = runMermaidWeb(s, inputPath); !svg.empty()) return svg;
    }
    return {};
}

// ── PlantUML ─────────────────────────────────────────────────────────

namespace {

std::string runPlantUmlWeb(const Settings &s, const std::string &inputPath)
{
    std::string code = readFile(inputPath);
    if (code.empty()) return {};

    std::string url = s.plantumlServerUrl + "/svg/~h" + hexEncode(code);

    ProcessResult r = runProcess("curl",
        {"-s", "-f", "--max-time", std::to_string(s.timeoutMs / 1000), url},
        nullptr, s.timeoutMs);
    if (r.started && r.exitCode == 0)
        return r.stdoutData;
    return {};
}

std::string runPlantUmlLocal(const Settings &s, const std::string &inputPath, bool darkMode)
{
    std::string code = readFile(inputPath);
    if (code.empty()) return {};

    std::vector<std::string> pumlArgs = {"-tsvg", "-pipe"};
    if (darkMode) pumlArgs.push_back("--dark-mode");

    std::vector<std::string> exeOptions;
    if (toLower(s.plantumlPath).size() >= 4 &&
        toLower(s.plantumlPath).compare(toLower(s.plantumlPath).size() - 4, 4, ".jar") != 0) {
        exeOptions.push_back(s.plantumlPath);
    }
    exeOptions.push_back("plantuml");
    exeOptions.push_back("/usr/bin/plantuml");

    for (const auto &exe : exeOptions) {
        ProcessResult r = runProcess(exe, pumlArgs, &code, s.timeoutMs);
        if (r.started && r.exitCode == 0 && !r.stdoutData.empty())
            return r.stdoutData;
    }

    std::vector<std::string> jarOptions;
    if (toLower(s.plantumlPath).size() >= 4 &&
        toLower(s.plantumlPath).compare(toLower(s.plantumlPath).size() - 4, 4, ".jar") == 0) {
        jarOptions.push_back(s.plantumlPath);
    }
    jarOptions.push_back("/usr/share/java/plantuml/plantuml.jar");
    jarOptions.push_back("/usr/share/plantuml/plantuml.jar");
    if (!pluginDir().empty()) jarOptions.push_back(pluginDir() + "/plantuml.jar");

    for (const auto &jar : jarOptions) {
        if (!fileExists(jar)) continue;
        std::vector<std::string> jarArgs = {"-jar", jar, "-tsvg", "-pipe"};
        if (darkMode) jarArgs.push_back("--dark-mode");

        ProcessResult r = runProcess(s.javaPath, jarArgs, &code, s.timeoutMs);
        if (r.started && r.exitCode == 0 && !r.stdoutData.empty())
            return r.stdoutData;

        if (s.javaPath != "java") {
            ProcessResult r2 = runProcess("java", jarArgs, &code, s.timeoutMs);
            if (r2.started && r2.exitCode == 0 && !r2.stdoutData.empty())
                return r2.stdoutData;
        }
    }
    return {};
}

} // namespace

std::string renderPlantUml(const Settings &s, const std::string &inputPath, bool darkMode)
{
    if (s.renderer == "web") {
        if (auto svg = runPlantUmlWeb(s, inputPath); !svg.empty()) return svg;
        return runPlantUmlLocal(s, inputPath, darkMode);
    }
    if (auto svg = runPlantUmlLocal(s, inputPath, darkMode); !svg.empty()) return svg;
    return runPlantUmlWeb(s, inputPath);
}

// ── SVG text baseline fixup ──────────────────────────────────────────

std::string fixMermaidSvgText(const std::string &svgData)
{
    std::string svg = svgData;

    // Phase 1: fold <text ...><tspan y="..em" dy="..em"> into an absolute
    // pixel y="" on the parent <text>, dropping y/dy from that first tspan.
    static const std::regex textTspanRe(R"(<text\b([^>]*)>\s*<tspan\b([^>]*)>)");
    static const std::regex yRe(R"(\by\s*=\s*"(-?[0-9]*\.?[0-9]+)em")");
    static const std::regex dyRe(R"(\bdy\s*=\s*"(-?[0-9]*\.?[0-9]+)em")");
    static const std::regex textYRe(R"(\by\s*=\s*"[^"]*")");

    // Work on a mutable copy; matches are applied back-to-front so earlier
    // offsets stay valid as the string is edited.
    std::string working = svg;
    std::sregex_iterator it(working.begin(), working.end(), textTspanRe);
    std::sregex_iterator end;
    // Collect matches first (positions), then rebuild string back-to-front
    // to keep earlier offsets valid, mirroring the original Qt code.
    std::vector<std::smatch> matches;
    for (; it != end; ++it) matches.push_back(*it);

    for (auto mit = matches.rbegin(); mit != matches.rend(); ++mit) {
        const std::smatch &match = *mit;
        std::string textAttrs = match[1].str();
        std::string tspanAttrs = match[2].str();

        std::smatch ym, dym;
        if (std::regex_search(tspanAttrs, ym, yRe) && std::regex_search(tspanAttrs, dym, dyRe)) {
            double yEm = std::stod(ym[1].str());
            double dyEm = std::stod(dym[1].str());
            double baselinePx = (yEm + dyEm) * 16.0 - 2.0;

            char buf[64];
            std::snprintf(buf, sizeof(buf), "y=\"%.2f\"", baselinePx);

            std::smatch textYm;
            if (std::regex_search(textAttrs, textYm, textYRe)) {
                textAttrs = textAttrs.substr(0, textYm.position(0)) + buf +
                            textAttrs.substr(textYm.position(0) + textYm.length(0));
            } else {
                textAttrs = std::string(" ") + buf + textAttrs;
            }

            tspanAttrs = std::regex_replace(tspanAttrs, yRe, "");
            tspanAttrs = std::regex_replace(tspanAttrs, dyRe, "");
            // Collapse runs of whitespace (mirrors QString::simplified()).
            tspanAttrs = std::regex_replace(tspanAttrs, std::regex(R"(\s+)"), " ");
            tspanAttrs = trim(tspanAttrs);
            if (!tspanAttrs.empty()) tspanAttrs = " " + tspanAttrs;

            std::string replacement = "<text" + textAttrs + "><tspan" + tspanAttrs + ">";
            working = working.substr(0, match.position(0)) + replacement +
                      working.substr(match.position(0) + match.length(0));
        }
    }

    // Phase 2: convert any remaining y/dy em values (outside the pattern
    // above) to pixel values in place.
    static const std::regex emRe(R"(\b(y|dy)\s*=\s*"(-?[0-9]*\.?[0-9]+)em")");
    std::string final;
    final.reserve(working.size());
    size_t last = 0;
    for (std::sregex_iterator eit(working.begin(), working.end(), emRe), eend; eit != eend; ++eit) {
        const std::smatch &em = *eit;
        final.append(working, last, static_cast<size_t>(em.position(0)) - last);
        double pxValue = std::stod(em[2].str()) * 16.0;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s=\"%.2f\"", em[1].str().c_str(), pxValue);
        final += buf;
        last = static_cast<size_t>(em.position(0) + em.length(0));
    }
    final.append(working, last, std::string::npos);

    return final;
}

} // namespace DiagramRenderer
