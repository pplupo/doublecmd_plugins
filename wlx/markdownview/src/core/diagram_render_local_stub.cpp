// Compiled instead of diagram_render_local.cpp when CMakeLists.txt's
// ENABLE_LOCAL_DIAGRAMS option is OFF -- keeps DiagramRender's dispatch
// free of any #ifdef by always providing this namespace's functions, just
// with the Rust renderers unconditionally reported as absent, which routes
// Mermaid and PlantUML to their web services instead.
#include "diagram_render_local.h"

namespace LocalDiagram {

bool isCompiledIn() { return false; }

std::string renderMermaid(const std::string &) { return {}; }

std::string renderPlantUml(const std::string &) { return {}; }

} // namespace LocalDiagram
