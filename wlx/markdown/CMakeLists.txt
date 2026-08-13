cmake_minimum_required(VERSION 3.16)
project(wlx_markdown VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Find Qt6
find_package(Qt6 6.2 REQUIRED COMPONENTS
    Widgets
    Svg
    Network
    PrintSupport
)

# 3rdparty configurations
set(BUILD_MD4QT_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(3rdparty/md4qt)

set(tinyxml2_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(tinyxml2_BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory(3rdparty/tinyxml2)

set(QT ON CACHE BOOL "" FORCE)
add_subdirectory(3rdparty/MicroTeX)

add_definitions(-DQT_NO_CONTEXTLESS_CONNECT -DMD4QT_QT_SUPPORT)

# Core Markdown Rendering Library
add_library(wlx_markdown_core STATIC
    src/core/markdownvisitor.cpp
    src/core/markdown_engine.cpp
)

target_include_directories(wlx_markdown_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/md4qt/src
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/MicroTeX/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../sdk
)

target_link_libraries(wlx_markdown_core PUBLIC
    md4qt::md4qt
    LaTeX
    Qt6::Widgets
    Qt6::Svg
    Qt6::Network
    Qt6::PrintSupport
)

# Qt6 Lister Plugin Target
add_library(wlx_markdown_qt6 MODULE
    src/qt6/plugin_qt6.cpp
)

target_link_libraries(wlx_markdown_qt6 PRIVATE
    wlx_markdown_core
    Qt6::Widgets
)

set_target_properties(wlx_markdown_qt6 PROPERTIES
    PREFIX ""
    OUTPUT_NAME "wlx_markdown_qt6"
    SUFFIX ".wlx"
)

# Optional GTK3 Target (Disabled by default)
option(BUILD_WLX_GTK "Build GTK3 WLX Target" OFF)
if(BUILD_WLX_GTK)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTK3 gtk+-3.0 webkit2gtk-4.0)
    if(GTK3_FOUND)
        add_library(wlx_markdown_gtk3 MODULE
            src/gtk3/plugin_gtk3.cpp
        )
        target_compile_definitions(wlx_markdown_gtk3 PRIVATE BUILD_GTK_TARGET)
        target_include_directories(wlx_markdown_gtk3 PRIVATE ${GTK3_INCLUDE_DIRS})
        target_link_libraries(wlx_markdown_gtk3 PRIVATE wlx_markdown_core ${GTK3_LIBRARIES})
        set_target_properties(wlx_markdown_gtk3 PROPERTIES
            PREFIX ""
            OUTPUT_NAME "wlx_markdown_gtk3"
            SUFFIX ".wlx"
        )
    endif()
endif()
