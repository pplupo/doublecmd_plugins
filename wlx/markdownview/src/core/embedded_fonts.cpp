#include "embedded_fonts.h"

// Each of these is generated at CMake configure time (see
// embed_binary_resource() in CMakeLists.txt) from the real .otf/.clm1
// files vendored under 3rdparty/MicroTeX/res/. Declared here rather than
// in a generated header since the symbol list itself doesn't change
// between configures, only their contents.
#define DECLARE_EMBEDDED(sym) \
    extern const unsigned char sym[]; \
    extern const size_t sym##_size;

DECLARE_EMBEDDED(lm_math_otf)
DECLARE_EMBEDDED(lm_math_clm)
DECLARE_EMBEDDED(fira_math_otf)
DECLARE_EMBEDDED(fira_math_clm)
DECLARE_EMBEDDED(dejavu_math_otf)
DECLARE_EMBEDDED(dejavu_math_clm)
DECLARE_EMBEDDED(ibmplex_math_otf)
DECLARE_EMBEDDED(ibmplex_math_clm)
DECLARE_EMBEDDED(stixtwo_math_otf)
DECLARE_EMBEDDED(stixtwo_math_clm)
DECLARE_EMBEDDED(libertinus_math_otf)
DECLARE_EMBEDDED(libertinus_math_clm)
DECLARE_EMBEDDED(pagella_math_otf)
DECLARE_EMBEDDED(pagella_math_clm)
DECLARE_EMBEDDED(euler_math_otf)
DECLARE_EMBEDDED(euler_math_clm)

#undef DECLARE_EMBEDDED

namespace MarkdownEngine {

const std::vector<EmbeddedFont> &embeddedFonts() {
    // Verified live, individually, before being embedded here: each font
    // was downloaded, converted via MicroTeX's own prebuilt/otf2clm.py
    // (needs FontForge), and test-rendered through MicroTeX's actual
    // parser/layout/draw pipeline (not just "the file exists") -- see
    // RELEASE_AUDIT.md-style verification notes in the commit that added
    // this. "newpxmath" specifically does NOT use the font of that name:
    // the actual newpx CTAN package's own OpenType math font
    // (TeXGyrePagellaX-Regular.otf) was rejected outright by MicroTeX
    // ("is not a math font!" -- its MATH table exists per FontForge but
    // isn't complete/valid enough for MicroTeX's own validation). TeX
    // Gyre Pagella Math (a separate, unrelated project, same family as
    // the DejaVu Math TeX Gyre entry below) renders correctly and serves
    // the same "modern Palatino-styled math" goal, so it's substituted
    // in instead.
    static const std::vector<EmbeddedFont> fonts = {
        {"Latin Modern Math", lm_math_otf, lm_math_otf_size, lm_math_clm, lm_math_clm_size},
        {"IBM Plex Math", ibmplex_math_otf, ibmplex_math_otf_size, ibmplex_math_clm, ibmplex_math_clm_size},
        {"STIX Two Math", stixtwo_math_otf, stixtwo_math_otf_size, stixtwo_math_clm, stixtwo_math_clm_size},
        {"Libertinus Math", libertinus_math_otf, libertinus_math_otf_size, libertinus_math_clm, libertinus_math_clm_size},
        {"Fira Math", fira_math_otf, fira_math_otf_size, fira_math_clm, fira_math_clm_size},
        {"DejaVu Math TeX Gyre", dejavu_math_otf, dejavu_math_otf_size, dejavu_math_clm, dejavu_math_clm_size},
        {"TeX Gyre Pagella Math", pagella_math_otf, pagella_math_otf_size, pagella_math_clm, pagella_math_clm_size},
        {"Euler Math", euler_math_otf, euler_math_otf_size, euler_math_clm, euler_math_clm_size},
    };
    return fonts;
}

} // namespace MarkdownEngine
