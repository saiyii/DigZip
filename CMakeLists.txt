cmake_minimum_required(VERSION 3.21)

option(DIGZIP_BUILD_TESTS "Build the test suite" ON)
option(DIGZIP_BUILD_CLI "Build the digzip command-line tool" ON)
option(DIGZIP_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

if(DIGZIP_BUILD_TESTS)
    list(APPEND VCPKG_MANIFEST_FEATURES "tests")
endif()

project(DigZip
    VERSION 0.1.0
    DESCRIPTION "Fast, multi-format, open source archive extractor"
    LANGUAGES CXX)

# Bumped to 1 at v1.0, when the C API becomes stable.
set(DIGZIP_API_VERSION 0)

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")

include(cmake/Warnings.cmake)

add_subdirectory(engine)

if(DIGZIP_BUILD_CLI)
    add_subdirectory(cli)
endif()

if(DIGZIP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
