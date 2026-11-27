# Kiln: a content-hashed build pipeline with Luau/cluau manifests
#
# NOTE (do not delete): this CMake build is the PERMANENT external-build bootstrap
# ("escape hatch") for Kiln's self-hosting chain. Kiln builds itself via
# bootstrap.sh: external-build = this CMake build, self-build =
# external-build/kiln building the workspace manifests, proof-build =
# self-build/kiln building them again. The self-build/proof-build binaries
# must be byte-identical (convergence check).
# CI must keep this job green forever. See "Self-hosting" in docs/decisions.md.

cmake_minimum_required(VERSION 3.20)
project(kiln LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(KILN_BUILD_TESTS "Build the test suite" ON)
option(KILN_SANITIZERS "Enable ASan+UBSan (dev preset)" OFF)

# ---- third_party: Luau -----------------------------------------------------
set(LUAU_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_WEB OFF CACHE BOOL "" FORCE)
add_subdirectory(plugins/luau/luau EXCLUDE_FROM_ALL)

# ---- third_party: BLAKE3 (portable C) --------------------------------------
add_library(blake3 STATIC
  plugins/blake3/c/blake3.c
  plugins/blake3/c/blake3_dispatch.c
  plugins/blake3/c/blake3_portable.c)
target_include_directories(blake3 PUBLIC plugins/blake3/c)
target_compile_definitions(blake3 PRIVATE
  BLAKE3_NO_SSE2 BLAKE3_NO_SSE41 BLAKE3_NO_AVX2 BLAKE3_NO_AVX512)

# ---- third_party: SQLite amalgamation --------------------------------------
add_library(sqlite3 STATIC plugins/sqlite/sqlite3.c)
target_include_directories(sqlite3 PUBLIC plugins/sqlite)
target_compile_definitions(sqlite3 PRIVATE SQLITE_THREADSAFE=1 SQLITE_DQS=0)

# ---- cluau: the.cluau manifest translator (no deps) -------
file(GLOB CLUAU_SOURCES CONFIGURE_DEPENDS plugins/cluau/cluau/src/translate/*.cpp)
add_library(cluau STATIC ${CLUAU_SOURCES})
target_include_directories(cluau PUBLIC plugins/cluau/cluau/include
  PRIVATE plugins/cluau/cluau/src/translate)
target_compile_features(cluau PRIVATE cxx_std_17)
# cluau_check: the strict rules over Luau's analysis (`kiln check`).
file(GLOB CLUAU_CHECK_SOURCES CONFIGURE_DEPENDS plugins/cluau/cluau/src/check/*.cpp)
add_library(cluau_check STATIC ${CLUAU_CHECK_SOURCES})
target_include_directories(cluau_check PUBLIC plugins/cluau/cluau/include)
target_link_libraries(cluau_check PUBLIC Luau.Analysis)
target_compile_features(cluau_check PRIVATE cxx_std_17)

# ---- third_party: header-only ----------------------------------------------
add_library(kiln_headers INTERFACE)
target_include_directories(kiln_headers INTERFACE
  plugins/cli11 plugins/json)

# ---- kiln_core -------------------------------------------------------------
file(GLOB_RECURSE KILN_CORE_SOURCES CONFIGURE_DEPENDS
  plugins/kiln/src/core/*.cpp plugins/kiln/src/luau/*.cpp
  plugins/kiln/src/rules/*.cpp plugins/kiln/src/workspace/*.cpp
  plugins/kiln/src/console/*.cpp)
add_library(kiln_core STATIC ${KILN_CORE_SOURCES})
target_include_directories(kiln_core PUBLIC plugins/kiln/src)
target_link_libraries(kiln_core PUBLIC
  blake3 sqlite3 kiln_headers cluau cluau_check
  Luau.Ast Luau.Compiler Luau.VM Luau.Analysis Luau.Config Luau.EqSat)
find_package(Threads REQUIRED)
target_link_libraries(kiln_core PUBLIC Threads::Threads)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(kiln_core PUBLIC dl)
endif()

# ---- kiln executable ---------------------------------------------------------
file(GLOB KILN_CLI_SOURCES CONFIGURE_DEPENDS plugins/kiln/src/cli/*.cpp)
add_executable(kiln ${KILN_CLI_SOURCES})
target_link_libraries(kiln PRIVATE kiln_core)


if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(kiln_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# ---- tests --------------------------------------------------------------------
if(KILN_BUILD_TESTS)
  enable_testing()
  file(GLOB KILN_TEST_SOURCES CONFIGURE_DEPENDS plugins/kiln_tests/*.cpp)
  add_executable(kiln_tests ${KILN_TEST_SOURCES})
  target_link_libraries(kiln_tests PRIVATE kiln_core)
  # hamr's C++ test library (header-only), from the hamr submodule.
  target_include_directories(kiln_tests PRIVATE plugins/hamr/hamr/include)
  target_compile_definitions(kiln_tests PRIVATE
    KILN_BINARY_PATH="$<TARGET_FILE:kiln>"
    KILN_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(kiln_tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
  endif()
  add_test(NAME kiln_tests COMMAND kiln_tests)
endif()
