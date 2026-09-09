cmake_minimum_required(VERSION 3.20)
project(archiveview_qt6 CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets PrintSupport)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBARCHIVE REQUIRED IMPORTED_TARGET libarchive)

# Pull in the platform library (builds libwlxbase_wlqt.a)
add_subdirectory(
    ${CMAKE_CURRENT_SOURCE_DIR}/../wlxbase_wlqt
    ${CMAKE_CURRENT_BINARY_DIR}/wlxbase_wlqt
)

# Toolkit-free core, shared by the Qt6 and (future) GTK3 variants. Nothing in
# here may reference Qt or GTK: the ZIP parser and the extraction path checks
# are security-relevant, and two copies would become two behaviours.
set(ARCHIVEVIEW_CORE_SOURCES
    src/core/ArchiveNames.cpp
    src/core/ZipCentralDirectory.cpp
    src/core/PassphraseBroker.cpp
    src/core/ArchiveScanner.cpp
    src/core/ArchiveExtractor.cpp
    src/core/ArchiveSettings.cpp
    src/core/EntryTree.cpp
    src/core/EntryFormat.cpp
    include/core/ArchiveEntry.h
    include/core/EntryTree.h
    include/core/EntryFormat.h
    include/core/ArchiveNames.h
    include/core/ZipCentralDirectory.h
    include/core/PassphraseBroker.h
    include/core/ArchiveScanner.h
    include/core/ArchiveExtractor.h
    include/core/ArchiveSettings.h
)

add_library(archiveview_core STATIC ${ARCHIVEVIEW_CORE_SOURCES})
target_include_directories(archiveview_core PUBLIC include)
target_compile_options(archiveview_core PRIVATE -Wall -Wextra)
target_compile_definitions(archiveview_core PUBLIC _FILE_OFFSET_BITS=64)
target_link_libraries(archiveview_core PUBLIC PkgConfig::LIBARCHIVE)
find_package(Threads REQUIRED)
target_link_libraries(archiveview_core PUBLIC Threads::Threads)

set(ARCHIVEVIEW_HEADERS
    include/ArchiveModel.h
    include/ArchiveScanner.h
    include/ArchiveViewWidget.h
    include/ArchiveExtractor.h
    include/ArchiveTreeView.h
)

add_library(archiveview_qt6 SHARED
    ${ARCHIVEVIEW_HEADERS}
    src/wlx_entry.cpp
    src/ArchiveModel.cpp
    src/ArchiveScanner.cpp
    src/ArchiveViewWidget.cpp
    src/ArchiveExtractor.cpp
    src/ArchiveTreeView.cpp
)

set_target_properties(archiveview_qt6 PROPERTIES
    PREFIX ""
    SUFFIX ".wlx"
)

target_include_directories(archiveview_qt6 PRIVATE
    include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../sdk
)

# -D_FILE_OFFSET_BITS=64: archives larger than 2GB must not be truncated by a
# 32-bit off_t on a 32-bit build.
# -fvisibility=hidden: only the List* entry points are exported (see the
# WLX_EXPORT macro in wlx_entry.cpp).
target_compile_options(archiveview_qt6 PRIVATE
    -Wall -Wextra
    -fvisibility=hidden
    -fvisibility-inlines-hidden
)
target_compile_definitions(archiveview_qt6 PRIVATE _FILE_OFFSET_BITS=64)

# -fvisibility=hidden only governs this target's own objects; symbols coming in
# from the statically linked wlxbase_wlqt (and enca inside it) would otherwise
# still be exported from the .wlx and be interposable by whatever the host
# process already loaded of the same library.
target_link_options(archiveview_qt6 PRIVATE
    -Wl,--exclude-libs,ALL
    -Wl,-Bsymbolic
    -Wl,-Bsymbolic-functions
)

target_link_libraries(archiveview_qt6 PRIVATE
    archiveview_core
    wlxbase_wlqt
    Qt6::Widgets
    Qt6::Gui
    Qt6::Core
    Qt6::PrintSupport
    PkgConfig::LIBARCHIVE
)

# The lister shells out to nothing. The predecessor this plugin replaces built
# a /bin/sh command line containing the previewed file's name, which made
# opening a file called `x$(...).zip` arbitrary code execution. Guard the
# property that makes that impossible rather than trusting review to catch a
# reintroduction.
add_custom_target(archiveview_no_subprocess ALL
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/src
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckNoSubprocess.cmake
    COMMENT "archiveview: verifying no subprocess/shell usage"
)
add_dependencies(archiveview_qt6 archiveview_no_subprocess)

# ---------------------------------------------------------------------------
# GTK3 variant — same core, different toolkit. Built only when GTK3 is
# present, following the convention of the other dual-toolkit plugins here.
# ---------------------------------------------------------------------------
option(ENABLE_TESTS "Build the archiveview test harnesses" ON)
option(ENABLE_GTK3 "Build the GTK3 archiveview plugin" ON)

if(ENABLE_GTK3)
    pkg_check_modules(GTK3 gtk+-3.0)
    if(GTK3_FOUND)
        add_library(archiveview_gtk3 SHARED
            src/gtk3/plugin_gtk3.cpp
            src/gtk3/ArchiveTreeModel.cpp
            src/gtk3/ArchiveTreeModel.h
        )
        set_target_properties(archiveview_gtk3 PROPERTIES
            PREFIX ""
            SUFFIX ".wlx"
        )
        target_include_directories(archiveview_gtk3 PRIVATE
            include
            src/gtk3
            ${CMAKE_CURRENT_SOURCE_DIR}/../../sdk
            ${GTK3_INCLUDE_DIRS}
        )
        target_compile_options(archiveview_gtk3 PRIVATE
            -Wall -Wextra
            -fvisibility=hidden
            -fvisibility-inlines-hidden
            ${GTK3_CFLAGS_OTHER}
        )
        target_link_directories(archiveview_gtk3 PRIVATE ${GTK3_LIBRARY_DIRS})
        target_link_libraries(archiveview_gtk3 PRIVATE
            archiveview_core
            ${GTK3_LIBRARIES}
        )
        # Same reasoning as the Qt6 variant: only the List* entry points may
        # be exported, so nothing here can be interposed by, or collide with,
        # what the host already loaded.
        target_link_options(archiveview_gtk3 PRIVATE
            -Wl,--exclude-libs,ALL
            -Wl,-Bsymbolic
            -Wl,-Bsymbolic-functions
        )
        add_dependencies(archiveview_gtk3 archiveview_no_subprocess)

        if(ENABLE_TESTS)
            # GTK counterpart to wlx_host: dlopen()s the built .wlx and walks
            # the custom tree model through the public GtkTreeModel API.
            add_executable(gtk_host tests/gtk_host.cpp)
            target_include_directories(gtk_host PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/../../sdk
                ${GTK3_INCLUDE_DIRS}
            )
            target_compile_options(gtk_host PRIVATE -Wall -Wextra ${GTK3_CFLAGS_OTHER})
            target_link_directories(gtk_host PRIVATE ${GTK3_LIBRARY_DIRS})
            target_link_libraries(gtk_host PRIVATE ${GTK3_LIBRARIES} ${CMAKE_DL_LIBS})
            add_dependencies(gtk_host archiveview_gtk3)
        endif()
    else()
        message(STATUS "archiveview: gtk+-3.0 not found — GTK3 variant skipped")
    endif()
endif()

# ---------------------------------------------------------------------------
# Headless harness (see tests/scan_smoke.cpp)
# ---------------------------------------------------------------------------
if(ENABLE_TESTS)
    add_executable(scan_smoke
        tests/scan_smoke.cpp
        src/ArchiveModel.cpp
        src/ArchiveScanner.cpp
        # Only the headers this target actually links — listing
        # ArchiveViewWidget.h here would have AUTOMOC generate code for a
        # class whose implementation is not part of this executable.
        include/ArchiveModel.h
        include/ArchiveScanner.h
    )
    target_include_directories(scan_smoke PRIVATE include)
    target_compile_options(scan_smoke PRIVATE -Wall -Wextra)
    target_compile_definitions(scan_smoke PRIVATE _FILE_OFFSET_BITS=64)
    target_link_libraries(scan_smoke PRIVATE
        archiveview_core
        Qt6::Widgets
        Qt6::Gui
        Qt6::Core
        PkgConfig::LIBARCHIVE
    )

    add_executable(extract_smoke tests/extract_smoke.cpp)
    target_include_directories(extract_smoke PRIVATE include)
    target_compile_options(extract_smoke PRIVATE -Wall -Wextra)
    target_compile_definitions(extract_smoke PRIVATE _FILE_OFFSET_BITS=64)
    target_link_libraries(extract_smoke PRIVATE
        archiveview_core
        Qt6::Core
        PkgConfig::LIBARCHIVE
    )

    # Stand-in host that dlopen()s the built .wlx and drives its C ABI.
    add_executable(wlx_host tests/wlx_host.cpp)
    target_include_directories(wlx_host PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../sdk
    )
    target_compile_options(wlx_host PRIVATE -Wall -Wextra)
    target_link_libraries(wlx_host PRIVATE Qt6::Widgets Qt6::Gui Qt6::Core ${CMAKE_DL_LIBS})
    add_dependencies(wlx_host archiveview_qt6)
endif()

