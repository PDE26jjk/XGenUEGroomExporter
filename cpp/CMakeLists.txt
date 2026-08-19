cmake_minimum_required(VERSION 3.24)

project(xgcore VERSION 0.1.0 LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(XGCORE_BUILD_TOOLS "Build the xgcore_test CLI" ON)
option(XGCORE_BUILD_TESTS "Build unit tests" ON)
option(XGCORE_ENABLE_ASAN "Enable AddressSanitizer (MSVC only)" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MSVC: static CRT (/MT, /MTd) so the final DLL has no runtime DLL
# dependencies beyond the OS. Applies to xgcore and all vendored deps.
if(MSVC)
  cmake_policy(SET CMP0091 NEW)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# All third-party dependencies are linked statically.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static third-party libraries" FORCE)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "xgcore requires a 64-bit build (Maya is 64-bit only).")
endif()

# ---------------------------------------------------------------------------
# Third-party dependencies
#
# Local source checkouts can be used instead of network fetches by passing
# -DALEMBIC_SOURCE_DIR=... / -DIMATH_SOURCE_DIR=... / -DZLIB_SOURCE_DIR=...
# ---------------------------------------------------------------------------
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# --- zlib (used for XGen group inflate) ---
set(XGCORE_ZLIB_VERSION "1.3.1" CACHE STRING "zlib version tag to fetch")
if(DEFINED ZLIB_SOURCE_DIR)
  set(FETCHCONTENT_SOURCE_DIR_ZLIB "${ZLIB_SOURCE_DIR}")
endif()
FetchContent_Declare(zlib
  GIT_REPOSITORY https://github.com/madler/zlib.git
  GIT_TAG        "v${XGCORE_ZLIB_VERSION}"
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(zlib)
if(TARGET zlibstatic)
  set(XGCORE_ZLIB_TARGET zlibstatic)
elseif(TARGET zlib)
  set(XGCORE_ZLIB_TARGET zlib)
else()
  message(FATAL_ERROR "zlib build did not produce a usable library target")
endif()

# --- Imath (required by Alembic) ---
set(XGCORE_IMATH_VERSION "3.1.12" CACHE STRING "Imath version tag to fetch")
if(DEFINED IMATH_SOURCE_DIR)
  set(FETCHCONTENT_SOURCE_DIR_IMATH "${IMATH_SOURCE_DIR}")
endif()
FetchContent_Declare(imath
  GIT_REPOSITORY https://github.com/AcademySoftwareFoundation/Imath.git
  GIT_TAG        "v${XGCORE_IMATH_VERSION}"
  GIT_SHALLOW    TRUE
)
set(IMATH_TEST OFF CACHE BOOL "Imath tests" FORCE)
set(IMATH_DOCS OFF CACHE BOOL "Imath docs" FORCE)
FetchContent_MakeAvailable(imath)
if(NOT TARGET Imath::Imath)
  message(FATAL_ERROR "Imath build did not produce Imath::Imath")
endif()
# Imath's BUILD_INTERFACE exposes only src/Imath, but Alembic (and xgcore)
# include headers as <Imath/...>. Add the parent directory so both spellings
# resolve when Imath is consumed as a subproject.
if(TARGET Imath)
  target_include_directories(
    Imath INTERFACE "$<BUILD_INTERFACE:${imath_SOURCE_DIR}/src>")
endif()

# --- Alembic (static, Ogawa only) ---
set(XGCORE_ALEMBIC_VERSION "1.8.12" CACHE STRING "Alembic version tag to fetch")
if(DEFINED ALEMBIC_SOURCE_DIR)
  set(FETCHCONTENT_SOURCE_DIR_ALEMBIC "${ALEMBIC_SOURCE_DIR}")
endif()
FetchContent_Declare(alembic
  GIT_REPOSITORY https://github.com/alembic/alembic.git
  GIT_TAG        "${XGCORE_ALEMBIC_VERSION}"
  GIT_SHALLOW    TRUE
)
set(ALEMBIC_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(USE_HDF5 OFF CACHE BOOL "" FORCE)
set(USE_PYALEMBIC OFF CACHE BOOL "" FORCE)
set(USE_MAYA OFF CACHE BOOL "" FORCE)
set(USE_ARNOLD OFF CACHE BOOL "" FORCE)
set(USE_PRMAN OFF CACHE BOOL "" FORCE)
set(USE_HOUDINI OFF CACHE BOOL "" FORCE)
set(USE_BINARIES OFF CACHE BOOL "" FORCE)
set(USE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(USE_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(alembic)
if(NOT TARGET Alembic::Alembic)
  message(FATAL_ERROR "Alembic build did not produce Alembic::Alembic")
endif()

# ---------------------------------------------------------------------------
# xgcore library
# ---------------------------------------------------------------------------
add_library(xgcore SHARED
  src/xgcore_api.cpp
  src/error.cpp
  src/json.cpp
  src/xgen_parse.cpp
  src/abc_writer.cpp
)
add_library(xgcore::xgcore ALIAS xgcore)

target_include_directories(xgcore
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(xgcore
  PRIVATE
    Alembic::Alembic
    Imath::Imath
    ${XGCORE_ZLIB_TARGET}
)

target_compile_definitions(xgcore PRIVATE XGCORE_BUILDING_DLL)
set_target_properties(xgcore PROPERTIES
  OUTPUT_NAME xgcore
  PREFIX ""
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES
)

if(XGCORE_ENABLE_ASAN AND MSVC)
  target_compile_options(xgcore PRIVATE /fsanitize=address)
  target_link_options(xgcore PRIVATE /fsanitize=address)
endif()

# ---------------------------------------------------------------------------
# CLI tool
# ---------------------------------------------------------------------------
if(XGCORE_BUILD_TOOLS)
  add_executable(xgcore_test tools/xgcore_test.cpp)
  target_link_libraries(xgcore_test PRIVATE xgcore::xgcore Alembic::Alembic
                                                   Imath::Imath)
endif()

# ---------------------------------------------------------------------------
# Unit tests
# ---------------------------------------------------------------------------
if(XGCORE_BUILD_TESTS)
  enable_testing()
  add_executable(xgcore_tests tests/test_main.cpp src/json.cpp)
  target_include_directories(xgcore_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(xgcore_tests PRIVATE xgcore::xgcore ${XGCORE_ZLIB_TARGET})
  add_test(NAME xgcore_tests COMMAND xgcore_tests)
endif()
