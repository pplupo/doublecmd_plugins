cmake_minimum_required(VERSION 3.16)
project(svgview CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(ENABLE_QT6 "Build Qt6 plugin variant" ON)
option(ENABLE_GTK3 "Build GTK3 plugin variant" ON)

find_package(PkgConfig REQUIRED)

# --- Shared core: SVG parsing, rendering, PNG export ---
pkg_check_modules(RSVG REQUIRED librsvg-2.0>=2.52)
pkg_check_modules(CAIRO REQUIRED cairo)

add_library(svgview_core STATIC
    src/svg_core.h
    src/svg_core.cpp
)

target_include_directories(svgview_core PUBLIC src ${RSVG_INCLUDE_DIRS} ${CAIRO_INCLUDE_DIRS})
target_link_libraries(svgview_core PUBLIC ${RSVG_LIBRARIES} ${CAIRO_LIBRARIES})

# --- Qt6 Variant ---
if(ENABLE_QT6)
    find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
    set(CMAKE_AUTOMOC ON)

    add_library(svgview_qt6 SHARED
        src/wlx_qt.cpp
    )

    set_target_properties(svgview_qt6 PROPERTIES
        PREFIX ""
        SUFFIX ".wlx"
        AUTOMOC ON
    )

    target_include_directories(svgview_qt6 PRIVATE
        src
        ${CMAKE_CURRENT_SOURCE_DIR}/../../sdk
    )

    target_link_libraries(svgview_qt6 PRIVATE
        svgview_core
        Qt6::Widgets
        Qt6::Gui
        Qt6::Core
    )
endif()

# --- GTK3 Variant ---
if(ENABLE_GTK3)
    pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
    
    add_library(svgview_gtk3 MODULE
        src/wlx_gtk.cpp
    )
    
    target_include_directories(svgview_gtk3 PRIVATE
        ${GTK3_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../sdk
    )
    
    target_link_libraries(svgview_gtk3 PRIVATE 
        svgview_core 
        ${GTK3_LIBRARIES}
    )
    
    # Static link libstdc++ and libgcc for portability
    target_link_options(svgview_gtk3 PRIVATE -static-libstdc++ -static-libgcc)
    
    set_target_properties(svgview_gtk3 PROPERTIES
        PREFIX ""
        OUTPUT_NAME "svgview_gtk"
        SUFFIX ".wlx"
    )
endif()
