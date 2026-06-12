cmake_minimum_required(VERSION 3.16)
project(csvview_qt6 CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets PrintSupport)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Pull in the base library (builds libwlxbase_wlqt.a)
add_subdirectory(
    ${CMAKE_CURRENT_SOURCE_DIR}/../wlxbase_wlqt
    ${CMAKE_CURRENT_BINARY_DIR}/wlxbase_wlqt
)

add_library(csvview_qt6 SHARED
    src/plugin.cpp
)

set_target_properties(csvview_qt6 PROPERTIES
    PREFIX ""
    SUFFIX ".wlx"
)

target_include_directories(csvview_qt6 PRIVATE
    src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../sdk
)

target_link_libraries(csvview_qt6 PRIVATE
    wlxbase_wlqt
    Qt6::Widgets
    Qt6::Gui
    Qt6::Core
    Qt6::PrintSupport
)
