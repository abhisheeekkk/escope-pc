cmake_minimum_required(VERSION 3.22)

project(EmbeddedScope
    VERSION 0.1.0
    DESCRIPTION "Embedded Debugging Platform"
    LANGUAGES CXX
)

# ── C++ standard ──────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Output directories ────────────────────────────────────────────────────────
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# ── Build type default ────────────────────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# ── Compiler warnings ─────────────────────────────────────────────────────────
add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
)

# ── Qt6 (LGPL, dynamic linking) ───────────────────────────────────────────────
# Qt is linked dynamically to remain LGPL-compliant.
# Do NOT set CMAKE_INTERPROCEDURAL_OPTIMIZATION with Qt static libs.
find_package(Qt6 REQUIRED COMPONENTS
    Core
    Widgets
    OpenGL
    OpenGLWidgets
)
qt_standard_project_setup()

# ── OpenGL ────────────────────────────────────────────────────────────────────
find_package(OpenGL REQUIRED)

# ── Threads ───────────────────────────────────────────────────────────────────
find_package(Threads REQUIRED)

# ── Google Test (fetched at configure time if not found) ──────────────────────
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
# Prevent GTest from overriding our compiler/linker settings on Windows
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()

# ── Subdirectories ────────────────────────────────────────────────────────────
add_subdirectory(core)
add_subdirectory(sim)
add_subdirectory(gui)
add_subdirectory(tools)
add_subdirectory(tests)

# ── Summary ───────────────────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "========================================")
message(STATUS "  EmbeddedScope ${PROJECT_VERSION}")
message(STATUS "  Build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ std    : C++${CMAKE_CXX_STANDARD}")
message(STATUS "  Qt version : ${Qt6_VERSION}")
message(STATUS "  Install to : ${CMAKE_INSTALL_PREFIX}")
message(STATUS "========================================")
message(STATUS "")
