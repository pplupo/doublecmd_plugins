cmake_minimum_required(VERSION 3.20)
project(structview_qt6 CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Allow older cmake_minimum_required in FetchContent dependencies
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Xml)
# Ensure the platform static library is built with -fPIC
# (required because we link it into a shared .wlx library)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(FetchContent)

# ---------------------------------------------------------------------------
# yaml-cpp — YAML 1.2 parser (using system package)
# ---------------------------------------------------------------------------
find_package(yaml-cpp REQUIRED)

# ---------------------------------------------------------------------------
# toml++ — header-only TOML parser (v3.4.0)
# ---------------------------------------------------------------------------
FetchContent_Declare(tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG        v3.4.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(tomlplusplus)

# Pull in the platform library (builds libwayland_qt_base.a)
add_subdirectory(
    ${CMAKE_CURRENT_SOURCE_DIR}/../wayland_qt_base
    ${CMAKE_CURRENT_BINARY_DIR}/wayland_qt_base
)

# NOTE: Headers with Q_OBJECT must be listed explicitly so AUTOMOC processes them.
add_library(structview_qt6 SHARED
    include/StructViewWidget.h
    include/TextFormatEngine.h
    src/wlx_entry.cpp
    src/StructViewWidget.cpp
    src/JsonEngine.cpp
    src/XmlEngine.cpp
    src/IniEngine.cpp
    src/CborEngine.cpp
    src/YamlEngine.cpp
    src/TomlEngine.cpp
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
    yaml-cpp
    tomlplusplus::tomlplusplus
)
