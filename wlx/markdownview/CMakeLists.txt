cmake_minimum_required(VERSION 3.16)
project(markdownview VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Find Qt6. markdownview_core (below) only needs Core (QString, the AST type
# in md4qt's MD4QT_QT_SUPPORT build mode) and Gui (QPainter/QPixmap/QImage,
# used narrowly by MicroTeX's Qt Graphics2D backend for LaTeX math
# rendering -- guarded at runtime by a qApp-null check, see markdownvisitor.
# cpp's onMath()). Widgets is only needed by the Qt6 plugin's own UI layer.
find_package(Qt6 6.2 REQUIRED COMPONENTS
    Core
    Gui
    Widgets
)

find_package(PkgConfig REQUIRED)
pkg_check_modules(CAIRO REQUIRED cairo)
pkg_check_modules(RSVG REQUIRED librsvg-2.0)

# 3rdparty configurations - enforce static libraries for maximum portability across environments
set(BUILD_MD4QT_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(3rdparty/md4qt)

set(tinyxml2_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(tinyxml2_BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory(3rdparty/tinyxml2)

set(QT ON CACHE BOOL "" FORCE)
add_subdirectory(3rdparty/MicroTeX)

add_definitions(-DQT_NO_CONTEXTLESS_CONNECT -DMD4QT_QT_SUPPORT)

# Core Markdown Rendering Library (Static Library)
add_library(markdownview_core STATIC
    src/core/markdownvisitor.cpp
    src/core/markdown_engine.cpp
)

target_include_directories(markdownview_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/md4qt/src
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/MicroTeX/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../sdk
    ${CAIRO_INCLUDE_DIRS}
    ${RSVG_INCLUDE_DIRS}
)

target_link_libraries(markdownview_core PUBLIC
    md4qt::md4qt
    LaTeX
    Qt6::Core
    Qt6::Gui
    ${CAIRO_LIBRARIES}
    ${RSVG_LIBRARIES}
)
target_link_directories(markdownview_core PUBLIC ${CAIRO_LIBRARY_DIRS} ${RSVG_LIBRARY_DIRS})
target_compile_options(markdownview_core PUBLIC ${CAIRO_CFLAGS_OTHER} ${RSVG_CFLAGS_OTHER})

# Qt6 Lister Plugin Target
add_library(markdownview_qt6 MODULE
    src/qt6/plugin_qt6.cpp
)

target_link_libraries(markdownview_qt6 PRIVATE
    markdownview_core
    Qt6::Widgets
)

# Statically link standard C++ runtime to prevent GLIBCXX version mismatches across environments
target_link_options(markdownview_qt6 PRIVATE
    -static-libstdc++
    -static-libgcc
)

set_target_properties(markdownview_qt6 PROPERTIES
    PREFIX ""
    OUTPUT_NAME "markdownview_qt6"
    SUFFIX ".wlx"
)

# Optional GTK3 Target
# NOTE: markdownview_core (linked below) still pulls in Qt6::Core/Gui --
# down from Core/Gui/Widgets/Svg/Network/PrintSupport. QtNetwork (Mermaid/
# PlantUML web rendering) was replaced with a curl subprocess; QtSvg and the
# rest of QtGui's painting API (SVG rasterization) were replaced with
# librsvg+Cairo. What's left: md4qt itself is built in MD4QT_QT_SUPPORT mode
# (QString-based parser/AST, requires Qt6::Core), and MicroTeX's LaTeX math
# rendering uses its Qt Graphics2D backend (requires Qt6::Gui for QPainter/
# QPixmap) -- guarded at runtime by a qApp-null check in onMath(), so it
# degrades to plain-text math rendering rather than crashing when no
# QGuiApplication exists (true for this GTK3 plugin's host process). Fully
# eliminating Qt would mean flipping md4qt to its ICU/STL string mode and
# building MicroTeX's Cairo backend (needs cairomm/pangomm) instead --
# tracked as further work, not done here.
option(BUILD_WLX_GTK "Build GTK3 WLX Target" ON)
if(BUILD_WLX_GTK)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTK3 gtk+-3.0 webkit2gtk-4.1)
    if(GTK3_FOUND)
        add_library(markdownview_gtk3 MODULE
            src/gtk3/plugin_gtk3.cpp
        )
        target_compile_definitions(markdownview_gtk3 PRIVATE BUILD_GTK_TARGET)
        target_include_directories(markdownview_gtk3 PRIVATE ${GTK3_INCLUDE_DIRS})
        target_link_libraries(markdownview_gtk3 PRIVATE markdownview_core ${GTK3_LIBRARIES})
        target_link_options(markdownview_gtk3 PRIVATE -static-libstdc++ -static-libgcc)
        set_target_properties(markdownview_gtk3 PROPERTIES
            PREFIX ""
            OUTPUT_NAME "markdownview_gtk3"
            SUFFIX ".wlx"
        )
    endif()
endif()
