cmake_minimum_required(VERSION 3.16)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

project(
    microtex
    VERSION 1.0.0
    DESCRIPTION "A micro, fast, cross-platform, and embeddable LaTeX rendering library"
)

# The API (and ABI) is compatible when MICROTEX_API_VERSION is the same.
set(MICROTEX_API_VERSION 1)

# Trimmed-down vendoring of upstream's own CMakeLists.txt (the "openmath"
# branch -- see the comment on renderMathTag() in markdown_engine.cpp for
# why this repo is on that branch instead of upstream's master). Upstream's
# original root CMakeLists.txt also wires up wasm/flutter/skia/gdi_win/gtk
# (gtkmm) platforms and several example apps; none of those directories are
# vendored here at all since this project only ever needs the Qt6 and
# Cairo backends.
option(CAIRO "Build with CAIRO" OFF)
option(QT "Build with Qt" OFF)
option(HAVE_LOG "If enable log" ON)
if (HAVE_LOG)
    add_definitions(-DHAVE_LOG)
endif ()
option(BUILD_STATIC "If build for static library" OFF)
if (BUILD_STATIC)
    set(_BUILD_STATIC ON)
endif ()

include(MicroTeXInstall.cmake)

add_subdirectory(lib)

if (CAIRO)
    add_subdirectory(platform/cairo)
endif ()

if (QT)
    add_subdirectory(platform/qt)
endif ()
