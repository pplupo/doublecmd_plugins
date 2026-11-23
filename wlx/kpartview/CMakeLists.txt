cmake_minimum_required(VERSION 3.16)
project(kpartview_host_qt6 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Double Commander plugin settings
set(CMAKE_SHARED_LIBRARY_PREFIX "")
if(UNIX)
    set(CMAKE_SHARED_LIBRARY_SUFFIX ".wlx")
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
find_package(KF6Parts REQUIRED)
find_package(KF6KIO REQUIRED)
find_package(KF6CoreAddons REQUIRED)
find_package(KF6XmlGui REQUIRED)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/../../sdk
)

add_library(kpartview_host_qt6 SHARED
    src/main.cpp
    src/kpartwidget.cpp
)

target_link_libraries(kpartview_host_qt6
    PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    KF6::Parts
    KF6::KIOFileWidgets
    KF6::CoreAddons
    KF6::XmlGui
)

# Use ECM for standard installation paths and KDE integration
find_package(ECM 5.80.0 REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)

# --- Symbol-export isolation (see sdk/exports/wlx.map) -------------------------
# Export ONLY the WLX "List*" C ABI from each .wlx; keep every internal,
# vendored, and C++-runtime symbol local so none can be interposed against
# Double Commander's own in-process Qt/GTK toolkit stack. if(TARGET) guards each
# so a variant disabled in this configuration is simply skipped.
foreach(_wlx_tgt kpartview_host_qt6)
    if(TARGET ${_wlx_tgt})
        target_link_options(${_wlx_tgt} PRIVATE
            "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/../../sdk/exports/wlx.map")
    endif()
endforeach()
