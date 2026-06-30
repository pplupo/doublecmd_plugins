cmake_minimum_required(VERSION 3.16)
project(wlx-log-viewer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Required for Q_OBJECT macro — generates moc files for vtable/signal-slot metadata
set(CMAKE_AUTOMOC ON)

# Visibility hidden by default as requested in spec
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN 1)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
include(FetchContent)
FetchContent_Declare(
  re2
  GIT_REPOSITORY https://github.com/google/re2.git
  GIT_TAG 2024-07-02
)
set(RE2_BUILD_TESTING OFF CACHE BOOL "" FORCE)
# Ensure re2 is built as a static library with PIC
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
FetchContent_MakeAvailable(re2)

# Need Qt6::WaylandClient? If not just Widgets and Core
# For inotify, it's native linux.

# Build the shared library (WLX plugin)
add_library(logviewer SHARED
    src/wlx_plugin.cpp
    src/LogViewerWidget.cpp
    src/LogModel.cpp
)

set_target_properties(logviewer PROPERTIES
    PREFIX ""
    SUFFIX ".wlx"
    OUTPUT_NAME "logviewer"
)

target_link_libraries(logviewer PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    re2::re2
)

# Only export specific functions
# This is usually handled by `extern "C" __attribute__((visibility("default")))` in the code,
# combined with the hidden preset.
