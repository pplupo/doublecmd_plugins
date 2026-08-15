cmake_minimum_required(VERSION 3.20)
project(mdk_wlx LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(ENABLE_QT6 "Build Qt6 plugin variant" ON)
# OFF by default: libmdk.so is a dlopen()-at-runtime soft dependency (see
# MdkEngine.cpp), not something this project vendors or links against, and
# it isn't packaged by apt on the target GTK3 test environment (Ubuntu
# 24.04) -- there's nothing to test the GTK3 variant against there. Still
# buildable by anyone who has the vendor lib installed by passing
# -DENABLE_GTK3=ON explicitly.
option(ENABLE_GTK3 "Build GTK3 plugin variant" OFF)

set(MDK_SDK_INCLUDE "$ENV{HOME}/mdk-sdk/include" CACHE PATH "Path to the MDK SDK include directory")

# --- Shared core: dlopen-based libmdk wrapper, zero Qt/GTK dependency ---
# See §2.1 of the GTK porting plan — this must stay Qt-free; do not add
# find_package(Qt6 ...) or Qt6::* to this target.
add_library(mdk_core STATIC
    src/core/MdkEngine.h
    src/core/MdkEngine.cpp
)
target_include_directories(mdk_core PUBLIC
    src/core
    ${MDK_SDK_INCLUDE}
)
target_link_libraries(mdk_core PUBLIC ${CMAKE_DL_LIBS})

# --- Qt6 Variant ---
if(ENABLE_QT6)
    find_package(Qt6 6.2 REQUIRED COMPONENTS Widgets OpenGL OpenGLWidgets)
    set(CMAKE_AUTOMOC ON)

    add_library(mdk_qt6 MODULE
        src/qt6/plugin_qt6.cpp
    )
    target_include_directories(mdk_qt6 PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../sdk
    )
    target_link_libraries(mdk_qt6 PRIVATE
        mdk_core
        Qt6::Widgets
        Qt6::OpenGL
        Qt6::OpenGLWidgets
    )
    target_link_options(mdk_qt6 PRIVATE -static-libstdc++ -static-libgcc)
    set_target_properties(mdk_qt6 PROPERTIES
        PREFIX ""
        OUTPUT_NAME "mdk_qt6"
        SUFFIX ".wlx"
        AUTOMOC ON
    )
endif()

# --- GTK3 Variant ---
if(ENABLE_GTK3)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTK3 gtk+-3.0 epoxy)
    if(GTK3_FOUND)
        add_library(mdk_gtk3 MODULE
            src/gtk3/plugin_gtk3.cpp
        )
        target_include_directories(mdk_gtk3 PRIVATE
            ${GTK3_INCLUDE_DIRS}
            ${CMAKE_CURRENT_SOURCE_DIR}/../../sdk
        )
        target_link_libraries(mdk_gtk3 PRIVATE mdk_core ${GTK3_LIBRARIES})
        target_link_options(mdk_gtk3 PRIVATE -static-libstdc++ -static-libgcc)
        set_target_properties(mdk_gtk3 PROPERTIES
            PREFIX ""
            OUTPUT_NAME "mdk_gtk3"
            SUFFIX ".wlx"
        )
    else()
        message(STATUS "mdk: gtk+-3.0/epoxy not found — GTK3 variant skipped for this build")
    endif()
endif()
