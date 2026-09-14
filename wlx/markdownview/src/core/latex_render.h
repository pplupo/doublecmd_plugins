#pragma once

#include <string>
#include <vector>
#include <cstdint>

/// One available math font: a display name plus the .otf/.clm file pair
/// MicroTeX's FontSrcFile needs to load it. Built by markdown_engine.cpp's
/// font registry (embedded fonts self-seeded to disk, auto-discovered
/// fonts, and the ini's custom path) -- toolkit-neutral, since it's just
/// file paths.
struct LatexFontEntry {
    std::string name;
    std::string otfPath;
    std::string clmPath;
    // Filled in by initLatexFonts()/addLatexFont() below: the name MicroTeX
    // itself registered the font under (FontMeta::name, read from the font
    // file's own metadata), which is what MicroTeX::parse()'s mathFontName
    // parameter actually matches against internally. This is NOT always
    // equal to `name` above -- e.g. our own display name "Latin Modern
    // Math" vs. the font's internal "LatinModernMath-Regular". Passing an
    // unresolved name straight through doesn't fail gracefully:
    // MicroTeX::parse() silently resolves to a null font pointer and
    // segfaults deep inside env.upem(), rather than throwing or falling
    // back to the default font -- which is why markdown_engine.cpp never
    // passes anything to MicroTeX::parse() except a canonicalName it
    // already resolved itself (via its g_clmPathToCanonical map), or empty.
    std::string canonicalName;
};

/// One-time setup: registers the toolkit's own Graphics2D backend with
/// MicroTeX and loads every given font into its context (fonts.front()
/// becomes the default used whenever a render doesn't name one). Fills in
/// each entry's canonicalName as a side effect (see the field's comment
/// above). Safe to call more than once; only takes effect the first time.
/// Implemented twice, once per UI target, for the same reason
/// renderLatexToPng() below is:
///   - src/qt6/latex_render_qt.cpp: MicroTeX's Qt Graphics2D backend
///   - src/gtk3/latex_render_cairo.cpp: MicroTeX's Cairo backend
void initLatexFonts(std::vector<LatexFontEntry> &fonts);

/// Loads one additional font after initLatexFonts() has already run --
/// e.g. an ini-supplied clm path markdown_engine.cpp didn't already know
/// about at startup. Returns the font's canonicalName (see LatexFontEntry
/// above) on success, or empty on any failure (missing file, not a valid
/// math font per MicroTeX's own validation, etc.) -- callers must treat
/// empty as "use the default font", never as a name to pass to
/// renderLatexToPng().
std::string addLatexFont(const std::string &clmPath, const std::string &otfPath);

/// Renders a LaTeX math expression to PNG bytes, using the named math font
/// (empty string = whichever font initLatexFonts() was given first).
///
/// Returns empty on failure (unparseable LaTeX, unknown font name, etc.)
/// -- the caller falls back to plain-text rendering of the source
/// expression.
std::vector<uint8_t> renderLatexToPng(const std::string &tex, bool darkMode,
                                       const std::string &mathFontName,
                                       int &logicalWidth, int &logicalHeight);
