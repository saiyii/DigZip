cmake_minimum_required(VERSION 3.21)

option(DIGZIP_BUILD_TESTS "Build the test suite" ON)
option(DIGZIP_BUILD_CLI "Build the digzip command-line tool" ON)
option(DIGZIP_BUILD_GUI "Build the digzip-gui graphical interface (needs Qt 6)" OFF)
option(DIGZIP_WITH_RAR "Read RAR 5 archives (DigZip's own decoder)" ON)
option(DIGZIP_BUILD_FUZZERS "Build the fuzz targets (replay drivers unless DIGZIP_LIBFUZZER is ON)" OFF)
option(DIGZIP_LIBFUZZER "Link the fuzz targets with libFuzzer (Clang)" OFF)
option(DIGZIP_ENABLE_SANITIZERS "Build with AddressSanitizer (and UndefinedBehaviorSanitizer outside MSVC)" OFF)
option(DIGZIP_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

if(DIGZIP_BUILD_TESTS)
    list(APPEND VCPKG_MANIFEST_FEATURES "tests")
endif()

project(DigZip
    VERSION 0.2.0
    DESCRIPTION "Fast, multi-format, open source archive extractor"
    LANGUAGES CXX)

# Bumped to 1 at v1.0, when the C API becomes stable.
set(DIGZIP_API_VERSION 0)

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")

if(DIGZIP_ENABLE_SANITIZERS)
    if(MSVC)
        add_compile_options(/fsanitize=address)
    else()
        add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

include(cmake/Warnings.cmake)

add_subdirectory(engine)

if(DIGZIP_BUILD_CLI)
    add_subdirectory(cli)
endif()

if(DIGZIP_BUILD_GUI)
    add_subdirectory(gui)
endif()

if(DIGZIP_BUILD_TESTS OR DIGZIP_BUILD_FUZZERS)
    enable_testing()
endif()

if(DIGZIP_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(DIGZIP_BUILD_FUZZERS)
    add_subdirectory(fuzz)
endif()
