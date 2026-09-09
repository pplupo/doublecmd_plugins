cmake_minimum_required(VERSION 3.16)
project(htmlview VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_AUTOMOC ON)

option(ENABLE_QT6 "Build the Qt6/QtWebEngine htmlview plugin" ON)

# -Wformat=2 pulls in -Wformat-nonliteral and -Wformat-security, the exact
# class of defect that lets a mismatched .mo file in a translation
# directory turn a printf-family call into a stack read. Nothing here
# passes a runtime-sourced string as a format, and this keeps it that way.
add_compile_options(-Wall -Wextra -Wformat=2)

# Toolkit-agnostic probe (file sniffing, charset resolution, transcode).
# Split out from the Qt6 backend so a GTK3/WebKitGTK sibling can share it
# unchanged, matching markdownview's core/qt6/gtk3 layout.
add_library(htmlview_core STATIC
    src/core/html_probe.cpp
)
target_include_directories(htmlview_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../sdk
)

# ---------------------------------------------------------------------
# Qt6 Lister Plugin Target
# ---------------------------------------------------------------------
if(ENABLE_QT6)
    find_package(Qt6 6.2 COMPONENTS Core Gui Widgets PrintSupport WebEngineWidgets)
    if(Qt6_FOUND AND TARGET Qt6::WebEngineWidgets)
        add_library(htmlview_qt6 MODULE
            src/qt6/plugin_qt6.cpp
        )
        set_target_properties(htmlview_qt6 PROPERTIES AUTOMOC ON)

        target_link_libraries(htmlview_qt6 PRIVATE
            htmlview_core
            Qt6::Core
            Qt6::Gui
            Qt6::Widgets
            Qt6::PrintSupport
            Qt6::WebEngineWidgets
        )

        # Deliberately NOT -static-libstdc++ (unlike markdownview's Qt6
        # target): this plugin shares a C++ ABI boundary with QtWebEngine,
        # which is itself built against the system libstdc++. A private
        # copy here would mean two std::string implementations meeting
        # across the Qt6::WebEngineWidgets interface.
        set_target_properties(htmlview_qt6 PROPERTIES
            PREFIX ""
            OUTPUT_NAME "htmlview_qt6"
            SUFFIX ".wlx"
        )
    else()
        message(WARNING "htmlview: Qt6 WebEngineWidgets not found, skipping Qt6 build")
    endif()
endif()

# ---------------------------------------------------------------------
# Probe test harness (not installed)
# ---------------------------------------------------------------------
option(ENABLE_TESTS "Build the html_probe test harness" ON)
if(ENABLE_TESTS)
    add_executable(test_html_probe tests/test_html_probe.cpp)
    target_link_libraries(test_html_probe PRIVATE htmlview_core)
endif()
