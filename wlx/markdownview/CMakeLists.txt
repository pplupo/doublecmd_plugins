cmake_minimum_required(VERSION 3.16)
project(markdownview VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(ENABLE_QT6 "Build the Qt6 markdownview plugin" ON)
option(ENABLE_GTK3 "Build the GTK3 markdownview plugin" ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(CAIRO REQUIRED cairo)
pkg_check_modules(RSVG REQUIRED librsvg-2.0)
# Matplot++'s CImg dependency (source/3rd_party/cimg) does its own image
# I/O for formats none of our chart specs ever produce or consume -- these
# are only here because CImg unconditionally references them, not because
# markdownview needs image loading of any kind.
find_package(ZLIB REQUIRED)
find_package(JPEG REQUIRED)
find_package(PNG REQUIRED)
find_package(TIFF REQUIRED)

include(FetchContent)

# Embeds a binary file as a `const unsigned char <SYMBOL>[]` + `const size_t
# <SYMBOL>_size` pair, generated once at configure time (benchmarked: ~0.1-
# 0.8s per file at our sizes, negligible) rather than needing any external
# tool (ld -b binary, a Python script, etc.). Used for the 8 embedded math
# fonts (see src/core/embedded_fonts.h) -- baked directly into the compiled
# .wlx and self-seeded to disk on first use (MicroTeX's FontSrcFile needs a
# real .otf file path; see markdown_engine.cpp's font registry), so a math
# font choice survives the single-file "copy the .wlx" install convention
# every other plugin here already relies on -- no companion folder needed.
function(embed_binary_resource SRC_FILE SYMBOL OUT_CPP_VAR)
    file(READ "${SRC_FILE}" _hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _arr "${_hex}")
    string(LENGTH "${_hex}" _hexlen)
    math(EXPR _size "${_hexlen} / 2")
    set(_out "${CMAKE_CURRENT_BINARY_DIR}/embedded/${SYMBOL}.cpp")
    file(WRITE "${_out}" "#include <cstddef>\nextern const unsigned char ${SYMBOL}[] = {${_arr}};\nextern const size_t ${SYMBOL}_size = ${_size};\n")
    set(${OUT_CPP_VAR} "${_out}" PARENT_SCOPE)
endfunction()

set(EMBEDDED_FONT_SOURCES "")
foreach(_pair
    "3rdparty/MicroTeX/res/lm-math/latinmodern-math.otf:lm_math_otf"
    "3rdparty/MicroTeX/res/lm-math/latinmodern-math.clm1:lm_math_clm"
    "3rdparty/MicroTeX/res/firamath/FiraMath-Regular.otf:fira_math_otf"
    "3rdparty/MicroTeX/res/firamath/FiraMath-Regular.clm1:fira_math_clm"
    "3rdparty/MicroTeX/res/tex-gyre/texgyredejavu-math.otf:dejavu_math_otf"
    "3rdparty/MicroTeX/res/tex-gyre/texgyredejavu-math.clm1:dejavu_math_clm"
    "3rdparty/MicroTeX/res/ibmplexmath/IBMPlexMath-Regular.otf:ibmplex_math_otf"
    "3rdparty/MicroTeX/res/ibmplexmath/IBMPlexMath-Regular.clm1:ibmplex_math_clm"
    "3rdparty/MicroTeX/res/stixtwomath/STIX2Math.otf:stixtwo_math_otf"
    "3rdparty/MicroTeX/res/stixtwomath/STIX2Math.clm1:stixtwo_math_clm"
    "3rdparty/MicroTeX/res/libertinusmath/LibertinusMath-Regular.otf:libertinus_math_otf"
    "3rdparty/MicroTeX/res/libertinusmath/LibertinusMath-Regular.clm1:libertinus_math_clm"
    "3rdparty/MicroTeX/res/texgyrepagellamath/texgyrepagella-math.otf:pagella_math_otf"
    "3rdparty/MicroTeX/res/texgyrepagellamath/texgyrepagella-math.clm1:pagella_math_clm"
    "3rdparty/MicroTeX/res/eulermath/Euler-Math.otf:euler_math_otf"
    "3rdparty/MicroTeX/res/eulermath/Euler-Math.clm1:euler_math_clm"
)
    string(REPLACE ":" ";" _pair_list "${_pair}")
    list(GET _pair_list 0 _src)
    list(GET _pair_list 1 _sym)
    embed_binary_resource("${CMAKE_CURRENT_SOURCE_DIR}/${_src}" "${_sym}" _generated_cpp)
    list(APPEND EMBEDDED_FONT_SOURCES "${_generated_cpp}")
endforeach()

# Same embedding mechanism, one more binary: a minimal (2D-only, no X11/Qt/
# wxWidgets/Lua terminals) prebuilt gnuplot, self-seeded to
# ~/.config/doublecmd/markdownview_gnuplot/ on first chart render that needs
# it (see chart_render.cpp) -- Matplot++'s only real rendering backend is
# gnuplot itself (even its "pngcairo" terminal is gnuplot driving Cairo, not
# a library call), and depending on the user's system already having
# gnuplot installed would make chart fidelity vary by machine. See
# 3rdparty/gnuplot/README.md for exactly how this binary was built.
embed_binary_resource("${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/gnuplot/gnuplot-linux-x86_64" "gnuplot_binary" _gnuplot_generated_cpp)
list(APPEND EMBEDDED_FONT_SOURCES "${_gnuplot_generated_cpp}")

# md4c: a plain-C, toolkit-agnostic CommonMark/GFM parser (replaces md4qt,
# which forced Qt6::Core on both plugin targets via its QString-typed AST
# and turned out to need Qt 6.5+ APIs this project doesn't otherwise
# require). md4c-html is its official HTML-renderer companion, used as-is
# for everything except fenced ```mermaid/plantuml code blocks and
# $...$/$$...$$ LaTeX math spans (rendered via md_html()'s
# MD_FLAG_LATEXMATHSPANS as <x-equation> tags) -- both are post-processed
# out of the resulting HTML in markdown_engine.cpp. See
# https://github.com/mity/md4c.
FetchContent_Declare(md4c
    GIT_REPOSITORY https://github.com/mity/md4c.git
    GIT_TAG        release-0.5.2
    GIT_SHALLOW    TRUE
)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_MD2HTML_EXECUTABLE OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(md4c)

set(tinyxml2_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(tinyxml2_BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory(3rdparty/tinyxml2)

# MicroTeX ("microtex" target below): vendored from upstream's "openmath"
# branch, NOT master -- see the comment on renderMathTag() in
# markdown_engine.cpp for the full story of why. Both toolkit backends
# (microtex-qt, microtex-cairo) are built here so markdownview_core can
# stay toolkit-neutral while each plugin target links only the backend it
# actually needs. HAVE_LOG OFF: the default dumps MicroTeX's entire font/
# symbol tables (thousands of lines) to stdout on every single render --
# fine for its own sample apps, not for a plugin embedded in a file
# manager. BUILD_STATIC: keeps every plugin a single self-contained .wlx
# file, matching every other plugin's "copy the .wlx" install convention
# (platform/cairo's CMakeLists.txt needed a small patch for this -- see
# its own comment).
set(HAVE_LOG OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC ON CACHE BOOL "" FORCE)
set(CAIRO ON CACHE BOOL "" FORCE)
set(QT ON CACHE BOOL "" FORCE)
add_subdirectory(3rdparty/MicroTeX)

# Matplot++ ("matplot" target below): the preferred chart-rendering path
# when the self-seeded vendored gnuplot binary (embedded above) is usable
# at runtime -- falls back to this plugin's own native Cairo chart renderer
# otherwise (see chart_render.cpp's dispatch). Trimmed to what a 2D-only
# report chart actually needs: no examples/tests/installer/package targets,
# no OpenCV (only used by CImg for extra image codecs we never touch), no
# high-resolution world map data (geoplot() is never called by any of our
# 13 mark types).
set(MATPLOTPP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(MATPLOTPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MATPLOTPP_BUILD_INSTALLER OFF CACHE BOOL "" FORCE)
set(MATPLOTPP_BUILD_PACKAGE OFF CACHE BOOL "" FORCE)
set(MATPLOTPP_WITH_OPENCV OFF CACHE BOOL "" FORCE)
set(MATPLOTPP_BUILD_HIGH_RESOLUTION_WORLD_MAP OFF CACHE BOOL "" FORCE)
add_subdirectory(3rdparty/matplotplusplus)

# Core Markdown Rendering Library (Static Library) -- toolkit-neutral:
# md4c (parsing), librsvg+Cairo (SVG rasterization for Mermaid/PlantUML, and
# direct chart drawing for ```chart blocks -- see chart_render.cpp), a curl
# subprocess (fetching those web renders), MicroTeX's core (LaTeX layout,
# backend-agnostic) -- no Qt, no GTK. Each plugin target supplies its own
# renderLatexToPng() implementation (see src/qt6/, src/gtk3/) and links it
# alongside this.
add_library(markdownview_core STATIC
    src/core/markdown_engine.cpp
    src/core/diagram_render.cpp
    src/core/chart_spec.cpp
    src/core/chart_render.cpp
    src/core/chart_render_matplotpp.cpp
    src/core/embedded_fonts.cpp
    ${EMBEDDED_FONT_SOURCES}
)

target_include_directories(markdownview_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../sdk
    ${CAIRO_INCLUDE_DIRS}
    ${RSVG_INCLUDE_DIRS}
    # Explicit rather than relying on md4c-html's PUBLIC usage requirement
    # propagating through target_link_libraries -- observed to not always
    # take effect (environment/CMake-version sensitive), so pinned directly.
    ${md4c_SOURCE_DIR}/src
)

target_link_libraries(markdownview_core PUBLIC
    md4c-html
    microtex
    matplot
    ${CAIRO_LIBRARIES}
    ${RSVG_LIBRARIES}
    ZLIB::ZLIB
    JPEG::JPEG
    PNG::PNG
    TIFF::TIFF
)
target_link_directories(markdownview_core PUBLIC ${CAIRO_LIBRARY_DIRS} ${RSVG_LIBRARY_DIRS})
target_compile_options(markdownview_core PUBLIC ${CAIRO_CFLAGS_OTHER} ${RSVG_CFLAGS_OTHER})

# ---------------------------------------------------------------------
# Qt6 Lister Plugin Target
# ---------------------------------------------------------------------
if(ENABLE_QT6)
    find_package(Qt6 6.2 COMPONENTS Core Gui Widgets PrintSupport)
    if(Qt6_FOUND)
        add_library(markdownview_qt6 MODULE
            src/qt6/plugin_qt6.cpp
            src/qt6/latex_render_qt.cpp
        )
        set_target_properties(markdownview_qt6 PROPERTIES AUTOMOC ON)

        target_link_libraries(markdownview_qt6 PRIVATE
            markdownview_core
            microtex-qt
            Qt6::Core
            Qt6::Gui
            Qt6::Widgets
            Qt6::PrintSupport
        )

        # Statically link standard C++ runtime to prevent GLIBCXX version
        # mismatches across environments.
        target_link_options(markdownview_qt6 PRIVATE
            -static-libstdc++
            -static-libgcc
        )

        set_target_properties(markdownview_qt6 PROPERTIES
            PREFIX ""
            OUTPUT_NAME "markdownview_qt6"
            SUFFIX ".wlx"
        )
    else()
        message(WARNING "markdownview: Qt6 not found, skipping Qt6 build")
    endif()
endif()

# ---------------------------------------------------------------------
# GTK3 Lister Plugin Target
# ---------------------------------------------------------------------
if(ENABLE_GTK3)
    pkg_check_modules(GTK3 gtk+-3.0 webkit2gtk-4.1)
    pkg_check_modules(CairoMM cairomm-1.0)
    pkg_check_modules(PangoMM pangomm-1.4)
    pkg_check_modules(Freetype2 freetype2)
    find_package(Fontconfig)
    if(GTK3_FOUND AND CairoMM_FOUND AND PangoMM_FOUND AND Fontconfig_FOUND AND Freetype2_FOUND)
        add_library(markdownview_gtk3 MODULE
            src/gtk3/plugin_gtk3.cpp
            src/gtk3/latex_render_cairo.cpp
        )
        target_compile_definitions(markdownview_gtk3 PRIVATE BUILD_GTK_TARGET)
        target_include_directories(markdownview_gtk3 PRIVATE
            ${GTK3_INCLUDE_DIRS}
            ${CairoMM_INCLUDE_DIRS}
            ${PangoMM_INCLUDE_DIRS}
            ${Freetype2_INCLUDE_DIRS}
        )
        target_link_directories(markdownview_gtk3 PRIVATE
            ${GTK3_LIBRARY_DIRS} ${CairoMM_LIBRARY_DIRS} ${PangoMM_LIBRARY_DIRS} ${Freetype2_LIBRARY_DIRS}
        )
        target_compile_options(markdownview_gtk3 PRIVATE
            ${GTK3_CFLAGS_OTHER} ${CairoMM_CFLAGS_OTHER} ${PangoMM_CFLAGS_OTHER} ${Freetype2_CFLAGS_OTHER}
        )
        target_link_libraries(markdownview_gtk3 PRIVATE
            markdownview_core
            microtex-cairo
            ${GTK3_LIBRARIES}
            ${CairoMM_LIBRARIES}
            ${PangoMM_LIBRARIES}
            ${Freetype2_LIBRARIES}
            Fontconfig::Fontconfig
        )
        # Deliberately NOT -static-libstdc++/-static-libgcc (unlike the qt6
        # target above). Confirmed via a live gdb session: statically linking
        # libstdc++ gives this .wlx its own private copy of C++ locale/
        # iostream facet templates, separate from the single libstdc++.so
        # every other component in DC's process shares. When two independent
        # copies of that machinery register facet IDs against the same
        # process-wide registry, a subsequent facet lookup can resolve to the
        # WRONG facet's vtable -- reproduced as a char-based
        # regex_traits<char> call landing inside an unrelated
        # codecvt<char16_t,...>::do_unshift, crashing on DC's own main
        # thread. structview, which never statically links libstdc++, never
        # showed this symptom. Sharing the dynamic libstdc++.so (the default;
        # no special flag needed) removes the mechanism outright.
        set_target_properties(markdownview_gtk3 PROPERTIES
            PREFIX ""
            OUTPUT_NAME "markdownview_gtk3"
            SUFFIX ".wlx"
        )
    else()
        message(WARNING "markdownview: GTK3/cairomm-1.0/pangomm-1.4 not found, skipping GTK3 build")
    endif()
endif()
