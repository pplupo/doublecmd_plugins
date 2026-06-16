cmake_minimum_required(VERSION 3.20)
project(dbview_qt6 CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Allow older cmake_minimum_required in FetchContent dependencies
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

# Optional engine flags
option(ENABLE_ROCKSDB_LEVELDB "Enable RocksDB and LevelDB support using RocksDB (large dependency)" OFF)
if(ENABLE_ROCKSDB_LEVELDB)
    add_compile_options(-include stdint.h)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Sql)

# Ensure the platform static library is built with -fPIC
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Pull in the platform library (builds libwlxbase_wlqt.a)
add_subdirectory(
    ${CMAKE_CURRENT_SOURCE_DIR}/../wlxbase_wlqt
    ${CMAKE_CURRENT_BINARY_DIR}/wlxbase_wlqt
)

# ---------------------------------------------------------------------------
# External dependencies via FetchContent
# ---------------------------------------------------------------------------
include(FetchContent)

# --- DuckDB ---
set(BUILD_SHELL OFF CACHE BOOL "" FORCE)
set(BUILD_UNITTESTS OFF CACHE BOOL "" FORCE)
set(BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(BUILD_EXTENSIONS "parquet" CACHE STRING "" FORCE)
set(EXTENSION_STATIC_BUILD ON CACHE BOOL "" FORCE)
set(ENABLE_EXTENSION_AUTOLOADING OFF CACHE BOOL "" FORCE)
set(ENABLE_EXTENSION_AUTOINSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(duckdb
    GIT_REPOSITORY https://github.com/duckdb/duckdb.git
    GIT_TAG        v1.3.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(duckdb)

# --- RocksDB (optional) ---
if(ENABLE_ROCKSDB_LEVELDB)
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
        PATCH_COMMAND  sed -i "1i #include <cstdint>" db/blob/blob_file_meta.h
    )
    FetchContent_MakeAvailable(rocksdb)
endif()

# --- MS Access static libmdb ---
add_library(mdb STATIC
    src/libmdb/catalog.c
    src/libmdb/file.c
    src/libmdb/table.c
    src/libmdb/data.c
    src/libmdb/dump.c
    src/libmdb/backend.c
    src/libmdb/money.c
    src/libmdb/sargs.c
    src/libmdb/index.c
    src/libmdb/like.c
    src/libmdb/write.c
    src/libmdb/stats.c
    src/libmdb/map.c
    src/libmdb/props.c
    src/libmdb/worktable.c
    src/libmdb/options.c
    src/libmdb/iconv.c
    src/libmdb/version.c
    src/libmdb/rc4.c
    src/libmdb/fakeglib.c
)
target_include_directories(mdb PRIVATE
    include/mdbtools
)
target_compile_definitions(mdb PRIVATE FAKE_GLIB=1 "TLS=__thread" HAVE_ICONV=1 "ICONV_CONST=")

# ---------------------------------------------------------------------------
# Plugin library
# ---------------------------------------------------------------------------
# NOTE: Headers must be listed explicitly so AUTOMOC processes them.
# RocksDbEngine.h is excluded when ENABLE_ROCKSDB is OFF to prevent MOC
# from generating code that references uncompiled RocksDbEngine symbols.
set(DBVIEW_HEADERS
    include/DbEngine.h
    include/DbViewWidget.h
    include/DuckDbEngine.h
    include/DuckDbModel.h
    include/KeyValueModel.h
    include/SqliteEngine.h
    include/FirebirdEngine.h
    include/MdbEngine.h
    include/BdbEngine.h
    include/LmdbEngine.h
)

set(DBVIEW_SOURCES
    ${DBVIEW_HEADERS}
    src/wlx_entry.cpp
    src/DbEngine.cpp
    src/DbViewWidget.cpp
    src/SqliteEngine.cpp
    src/DuckDbEngine.cpp
    src/DuckDbModel.cpp
    src/KeyValueModel.cpp
    src/FirebirdEngine.cpp
    src/MdbEngine.cpp
    src/BdbEngine.cpp
    src/LmdbEngine.cpp
)

if(ENABLE_ROCKSDB_LEVELDB)
    list(APPEND DBVIEW_SOURCES
        include/LevelDbEngine.h src/LevelDbEngine.cpp
        include/RocksDbEngine.h src/RocksDbEngine.cpp
    )
endif()

add_library(dbview_qt6 SHARED ${DBVIEW_SOURCES})

if(ENABLE_ROCKSDB_LEVELDB)
    target_compile_definitions(dbview_qt6 PRIVATE ENABLE_ROCKSDB_LEVELDB)
endif()

set_target_properties(dbview_qt6 PROPERTIES
    PREFIX ""
    SUFFIX ".wlx"
)

target_include_directories(dbview_qt6 PRIVATE
    include
    include/mdbtools
    ${CMAKE_CURRENT_SOURCE_DIR}/../../sdk
    ${duckdb_SOURCE_DIR}/src/include
)

if(ENABLE_ROCKSDB_LEVELDB)
    target_include_directories(dbview_qt6 PRIVATE
        ${rocksdb_SOURCE_DIR}/include
    )
endif()

set(DBVIEW_LINK_LIBS
    wlxbase_wlqt
    Qt6::Widgets
    Qt6::Gui
    Qt6::Core
    Qt6::Sql
    duckdb
    mdb
    lmdb
    db
)

if(ENABLE_ROCKSDB_LEVELDB)
    list(APPEND DBVIEW_LINK_LIBS rocksdb)
endif()

target_link_libraries(dbview_qt6 PRIVATE ${DBVIEW_LINK_LIBS})
