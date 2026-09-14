// Compiled instead of chart_render_vegalite.cpp when CMakeLists.txt's
// ENABLE_VLCONVERT option is OFF -- keeps markdown_engine.cpp's figure
// path free of any #ifdef by always providing this namespace's functions,
// just with vl-convert unconditionally reported as absent. isCompiledIn()
// returning false is what routes ```vegalite blocks to Kroki instead (see
// renderVegaLiteImgTag).
#include "chart_render_vegalite.h"

namespace VegaLite {

bool isAvailable() { return false; }

bool isCompiledIn() { return false; }

std::vector<uint8_t> renderVegaLiteSpecPng(const std::string &, bool, const std::string &, int &, int &) { return {}; }

void setDisplayScale(double) {}

} // namespace VegaLite
