cmake_minimum_required(VERSION 3.20)
project(dbview_qt6 CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Optional engine flags
option(ENABLE_ROCKSDB "Enable RocksDB support (large dependency)" OFF)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Sql)

# Ensure the platform static library is built with -fPIC
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Pull in the platform library (builds libwayland_qt_base.a)
add_subdirectory(
    ${CMAKE_CURRENT_SOURCE_DIR}/../../plugin-platform/wlx/wayland_qt_base
    ${CMAKE_CURRENT_BINARY_DIR}/wayland_qt_base
)

# ---------------------------------------------------------------------------
# External dependencies via FetchContent
# ---------------------------------------------------------------------------
include(FetchContent)

# --- DuckDB ---
set(BUILD_SHELL OFF CACHE BOOL "" FORCE)
set(BUILD_UNITTESTS OFF CACHE BOOL "" FORCE)
set(BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(BUILD_EXTENSIONS "" CACHE STRING "" FORCE)
set(ENABLE_EXTENSION_AUTOLOADING OFF CACHE BOOL "" FORCE)
set(ENABLE_EXTENSION_AUTOINSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(duckdb
    GIT_REPOSITORY https://github.com/duckdb/duckdb.git
    GIT_TAG        v1.3.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(duckdb)

# --- LevelDB ---
set(LEVELDB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LEVELDB_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(leveldb
    GIT_REPOSITORY https://github.com/google/leveldb.git
    GIT_TAG        1.23
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(leveldb)

# --- RocksDB (optional) ---
if(ENABLE_ROCKSDB)
    set(WITH_TESTS OFF CACHE BOOL "" FORCE)
    set(WITH_BENCHMARK_TOOLS OFF CACHE BOOL "" FORCE)
    set(WITH_TOOLS OFF CACHE BOOL "" FORCE)
    set(WITH_GFLAGS OFF CACHE BOOL "" FORCE)
    set(ROCKSDB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(FAIL_ON_WARNINGS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(rocksdb
        GIT_REPOSITORY https://github.com/facebook/rocksdb.git
        GIT_TAG        v9.11.2
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(rocksdb)
endif()

# ---------------------------------------------------------------------------
# Plugin library
# ---------------------------------------------------------------------------
set(DBVIEW_SOURCES
    src/wlx_entry.cpp
    src/DbEngine.cpp
    src/DbViewWidget.cpp
    src/SqliteEngine.cpp
    src/DuckDbEngine.cpp
    src/DuckDbModel.cpp
    src/LevelDbEngine.cpp
    src/KeyValueModel.cpp
)

if(ENABLE_ROCKSDB)
    list(APPEND DBVIEW_SOURCES src/RocksDbEngine.cpp)
endif()

add_library(dbview_qt6 SHARED ${DBVIEW_SOURCES})

if(ENABLE_ROCKSDB)
    target_compile_definitions(dbview_qt6 PRIVATE ENABLE_ROCKSDB)
endif()

set_target_properties(dbview_qt6 PROPERTIES
    PREFIX ""
    SUFFIX ".wlx"
)

target_include_directories(dbview_qt6 PRIVATE
    include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../sdk
    ${duckdb_SOURCE_DIR}/src/include
    ${leveldb_SOURCE_DIR}/include
)

if(ENABLE_ROCKSDB)
    target_include_directories(dbview_qt6 PRIVATE
        ${rocksdb_SOURCE_DIR}/include
    )
endif()

set(DBVIEW_LINK_LIBS
    wayland_qt_base
    Qt6::Widgets
    Qt6::Gui
    Qt6::Core
    Qt6::Sql
    duckdb
    leveldb
)

if(ENABLE_ROCKSDB)
    list(APPEND DBVIEW_LINK_LIBS rocksdb)
endif()

target_link_libraries(dbview_qt6 PRIVATE ${DBVIEW_LINK_LIBS})
