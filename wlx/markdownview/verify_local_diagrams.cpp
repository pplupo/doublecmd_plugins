// Proves the ENABLE_LOCAL_DIAGRAMS build renders Mermaid and PlantUML
// without touching the network.
//
// The method is the point: every service base URL is pointed at an
// unresolvable host FIRST. If a diagram still comes back, nothing went out
// -- there is nowhere for it to have gone. Any accidental web fallback
// shows up here as an empty result, not as a silent pass.
//
// Build against a core compiled with -DENABLE_LOCAL_DIAGRAMS=ON:
//
//   g++ -std=c++17 -O1 -o verify_local_diagrams verify_local_diagrams.cpp \
//     -Isrc/core $(pkg-config --cflags cairo librsvg-2.0) \
//     build_offline/libmarkdownview_core.a 3rdparty/supramark/libmarkdownview_diagrams.a \
//     $(pkg-config --libs cairo librsvg-2.0) -lz -lexpat -lstdc++ -lm -lpthread -ldl
#include "diagram_render.h"
#include "diagram_render_local.h"

#include <cstdio>
#include <string>

static int failures = 0;

static void report(const char *what, const std::string &svg, bool mermaid)
{
    std::string patched = mermaid ? DiagramRender::fixMermaidSvgText(svg, false) : svg;
    int w = 0, h = 0;
    bool ok = !patched.empty() &&
              !DiagramRender::svgToHighDpiPng(patched, 2.0f, false, w, h).empty() && w > 0 && h > 0;
    printf("  %-14s %s  %6zu bytes  %dx%d px\n", what, ok ? "PASS" : "FAIL", patched.size(), w, h);
    if (!ok) ++failures;
}

int main()
{
    if (!LocalDiagram::isCompiledIn()) {
        printf("built without ENABLE_LOCAL_DIAGRAMS -- nothing to verify here\n");
        return 2;
    }

    DiagramRender::setMermaidBaseUrl("https://mermaid.invalid.example");
    DiagramRender::setPlantUmlBaseUrl("https://plantuml.invalid.example");
    DiagramRender::setKrokiBaseUrl("https://kroki.invalid.example");
    printf("all service URLs pointed at unresolvable hosts\n\n");

    printf("-- mermaid, rendered in-process\n");
    report("flowchart", DiagramRender::renderMermaid(
        "graph TD\n  A[Start] --> B{Choice}\n  B -->|yes| C[Do it]\n  B -->|no| D[Skip]\n", false), true);
    report("sequence", DiagramRender::renderMermaid(
        "sequenceDiagram\n  participant Client\n  participant API\n"
        "  Client->>API: POST /render\n  API-->>Client: 200 svg\n", false), true);
    report("state", DiagramRender::renderMermaid(
        "stateDiagram-v2\n  [*] --> Idle\n  Idle --> Rendering: open\n  Rendering --> [*]\n", false), true);
    report("dark mode", DiagramRender::renderMermaid("graph LR\n  A --> B\n", true), true);

    printf("\n-- plantuml, rendered in-process\n");
    report("sequence", DiagramRender::renderPlantUml(
        "@startuml\nAlice -> Bob: Hello\nBob --> Alice: Hi\n@enduml\n", false), false);
    report("class", DiagramRender::renderPlantUml(
        "@startuml\nclass Renderer {\n  +render(spec): Svg\n}\n@enduml\n", false), false);
    report("activity", DiagramRender::renderPlantUml(
        "@startuml\nstart\n:read file;\nif (diagram?) then (yes)\n  :render;\nelse (no)\n"
        "  :plain text;\nendif\nstop\n@enduml\n", false), false);
    report("dark mode", DiagramRender::renderPlantUml(
        "@startuml\nAlice -> Bob: Hello\n@enduml\n", true), false);

    printf("\n-- the web renderers, same unreachable hosts: these MUST fail\n");
    bool mermaidWebEmpty = DiagramRender::renderMermaidWeb("graph TD\n A-->B\n", false).empty();
    bool plantUmlWebEmpty = DiagramRender::renderPlantUmlWeb("@startuml\nA->B:x\n@enduml\n", false).empty();
    printf("  %-14s %s\n", "mermaid web", mermaidWebEmpty ? "PASS (empty)" : "FAIL (got a response?!)");
    printf("  %-14s %s\n", "plantuml web", plantUmlWebEmpty ? "PASS (empty)" : "FAIL (got a response?!)");
    if (!mermaidWebEmpty) ++failures;
    if (!plantUmlWebEmpty) ++failures;

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASS", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
