cmake_minimum_required(VERSION 3.16)
project(pdfview)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_AUTOMOC ON)

# Point to custom dependencies install directory
set(DEPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/scripts/deps_install")

# Include directories
include_directories(
    src/core
    "${DEPS_DIR}/include"
)

# 1. Compile the UI-agnostic engine
add_library(doc_core STATIC
    src/core/document.cpp
    src/core/mupdf_backend.cpp
    src/core/djvu_backend.cpp
)

# Link directories
link_directories("${DEPS_DIR}/lib")

# Static libraries from deps
# MuPDF requires mupdf, mupdf-third.
set(STATIC_LIBS
    -Wl,-Bstatic
    mupdf
    mupdf-third
    djvulibre
    -Wl,-Bdynamic
    pthread
    dl
    m
)

target_link_libraries(doc_core PRIVATE ${STATIC_LIBS})

# 2. Build Qt WLX Plugin
find_package(Qt6 COMPONENTS Widgets PrintSupport REQUIRED)
add_library(pdfview_qt SHARED src/qt/wlx_qt.cpp src/qt/viewer_widget.cpp)
set_target_properties(pdfview_qt PROPERTIES PREFIX "" SUFFIX ".wlx" OUTPUT_NAME "pdfview_qt6")
target_include_directories(pdfview_qt PRIVATE src/qt)
target_link_libraries(pdfview_qt PRIVATE doc_core Qt6::Widgets Qt6::PrintSupport)

# 3. Build GTK WLX Plugin
find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
add_library(pdfview_gtk SHARED src/gtk/wlx_gtk.cpp src/gtk/viewer_widget.cpp)
set_target_properties(pdfview_gtk PROPERTIES PREFIX "" SUFFIX ".wlx" OUTPUT_NAME "pdfview_gtk3")
target_include_directories(pdfview_gtk PRIVATE src/gtk ${GTK3_INCLUDE_DIRS})
target_link_options(pdfview_gtk PRIVATE ${GTK3_LDFLAGS})
target_link_libraries(pdfview_gtk PRIVATE doc_core ${GTK3_LIBRARIES})
