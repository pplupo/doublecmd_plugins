cmake_minimum_required(VERSION 3.20)
project(structview_qt6 CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Xml)
# Ensure the platform static library is built with -fPIC
# (required because we link it into a shared .wlx library)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Pull in the platform library (builds libwayland_qt_base.a)
add_subdirectory(
    ${CMAKE_CURRENT_SOURCE_DIR}/../../plugin-platform/wlx/wayland_qt_base
    ${CMAKE_CURRENT_BINARY_DIR}/wayland_qt_base
)

add_library(structview_qt6 SHARED
    src/wlx_entry.cpp
    src/StructViewWidget.cpp
    src/JsonEngine.cpp
    src/XmlEngine.cpp
    src/IniEngine.cpp
    src/CborEngine.cpp
)

set_target_properties(structview_qt6 PROPERTIES
    PREFIX ""
    SUFFIX ".wlx"
)

target_include_directories(structview_qt6 PRIVATE
    include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../sdk
)

target_link_libraries(structview_qt6 PRIVATE
    wayland_qt_base
    Qt6::Widgets
    Qt6::Gui
    Qt6::Core
    Qt6::Xml
)
