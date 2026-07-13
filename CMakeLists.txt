cmake_minimum_required(VERSION 3.24)

project(scb LANGUAGES CXX)

option(SCB_BUILD_TESTS "Build scb tests" ON)

if(SCB_BUILD_TESTS)
    enable_testing()
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Vendor/Catch2/CMakeLists.txt")
    message(FATAL_ERROR
        "Missing Catch2 submodule at Vendor/Catch2.\n"
        "Run: git submodule update --init --recursive"
    )
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Vendor/toml11/CMakeLists.txt")
    message(FATAL_ERROR
        "Missing toml11 submodule at Vendor/toml11.\n"
        "Run: git submodule update --init --recursive"
    )
endif()

set(CATCH_DEVELOPMENT_BUILD OFF CACHE BOOL "" FORCE)
set(CATCH_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(CATCH_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CATCH_BUILD_EXTRA_TESTS OFF CACHE BOOL "" FORCE)
set(CATCH_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(CATCH_BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
set(CATCH_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "" FORCE)

add_subdirectory(Vendor/Catch2)

set(TOML11_PRECOMPILE OFF CACHE BOOL "" FORCE)
set(TOML11_INSTALL OFF CACHE BOOL "" FORCE)
set(TOML11_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(TOML11_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(TOML11_BUILD_TOML_TESTS OFF CACHE BOOL "" FORCE)

add_subdirectory(Vendor/toml11)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/Vendor/Catch2/extras")

add_subdirectory(Modules)
add_subdirectory(Apps)
