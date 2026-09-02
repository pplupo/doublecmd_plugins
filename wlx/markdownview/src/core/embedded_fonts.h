#pragma once

#include <string>
#include <vector>

namespace MarkdownEngine {

/// One math font baked directly into the compiled plugin binary.
struct EmbeddedFont {
    std::string name;         // display name, and the ini's math_font value
    const unsigned char *otfData;
    size_t otfSize;
    const unsigned char *clmData;
    size_t clmSize;
};

/// All 8 embedded math fonts, in the order they should appear in the
/// "Math Font" menu. embeddedFonts()[0] ("Latin Modern Math") is loaded
/// first into MicroTeX's context and becomes its default -- the closest
/// visual match to what MicroTeX rendered before this feature existed, so
/// an unset math_font ini value (the UI's own separate "Default" menu
/// entry, not one of these 8) looks unchanged from before.
const std::vector<EmbeddedFont> &embeddedFonts();

} // namespace MarkdownEngine
