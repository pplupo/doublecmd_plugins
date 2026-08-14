cmake_minimum_required(VERSION 3.20)
project(mdk_wlx LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(ENABLE_QT6 "Build Qt6 plugin variant" ON)
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

# --- GTK3 Variant (not yet implemented — see plan §3.7 / §9 checklist) ---
if(ENABLE_GTK3)
    message(FATAL_ERROR "mdk: GTK3 variant not implemented yet")
endif()
