find_package(ZLIB REQUIRED)
find_package(BZip2 REQUIRED)
find_package(LibLZMA REQUIRED)
find_package(zstd CONFIG REQUIRED)

add_library(digzip_engine
    src/version.cpp)
add_library(DigZip::engine ALIAS digzip_engine)

set_target_properties(digzip_engine PROPERTIES
    OUTPUT_NAME digzip
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

include(GenerateExportHeader)
generate_export_header(digzip_engine
    BASE_NAME digzip
    EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/include/digzip/export.hpp")

target_include_directories(digzip_engine
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_BINARY_DIR}/include")

target_compile_features(digzip_engine PUBLIC cxx_std_17)
target_compile_definitions(digzip_engine PRIVATE
    DIGZIP_VERSION_STRING="${PROJECT_VERSION}"
    DIGZIP_API_VERSION=${DIGZIP_API_VERSION})

if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(digzip_engine PUBLIC DIGZIP_STATIC_DEFINE)
endif()

target_link_libraries(digzip_engine PRIVATE
    ZLIB::ZLIB
    BZip2::BZip2
    LibLZMA::LibLZMA
    zstd::libzstd)
digzip_set_warnings(digzip_engine)
