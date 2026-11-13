find_package(ZLIB REQUIRED)
find_package(BZip2 REQUIRED)
find_package(LibLZMA REQUIRED)
find_package(zstd CONFIG REQUIRED)

# All engine code is compiled once into an object library, shared by the public library and by
# the tests, which also exercise internal components.
add_library(digzip_objects OBJECT
    src/archive.cpp
    src/limits.cpp
    src/names.cpp
    src/text.cpp
    src/version.cpp
    src/codec/bcj2.cpp
    src/codec/bzip2.cpp
    src/codec/deflate.cpp
    src/codec/gzip.cpp
    src/codec/inflate.cpp
    src/codec/lzma_raw.cpp
    src/codec/xz.cpp
    src/codec/zstd.cpp
    src/extract/extractor.cpp
    src/extract/output_file.cpp
    src/extract/path_rules.cpp
    src/format/common.cpp
    src/format/open.cpp
    src/format/sevenzip.cpp
    src/format/tar.cpp
    src/format/zip.cpp
    src/io/file_source.cpp
    src/io/stream.cpp)

set_target_properties(digzip_objects PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

include(GenerateExportHeader)
generate_export_header(digzip_objects
    BASE_NAME digzip
    EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/include/digzip/export.hpp")

target_include_directories(digzip_objects
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_BINARY_DIR}/include"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_compile_features(digzip_objects PUBLIC cxx_std_17)
target_compile_definitions(digzip_objects PRIVATE
    DIGZIP_VERSION_STRING="${PROJECT_VERSION}"
    DIGZIP_API_VERSION=${DIGZIP_API_VERSION})

if(BUILD_SHARED_LIBS)
    target_compile_definitions(digzip_objects PRIVATE digzip_objects_EXPORTS)
else()
    target_compile_definitions(digzip_objects PUBLIC DIGZIP_STATIC_DEFINE)
endif()

target_link_libraries(digzip_objects PUBLIC
    ZLIB::ZLIB
    BZip2::BZip2
    LibLZMA::LibLZMA
    zstd::libzstd)
digzip_set_warnings(digzip_objects)

add_library(digzip_engine)
add_library(DigZip::engine ALIAS digzip_engine)
set_target_properties(digzip_engine PROPERTIES OUTPUT_NAME digzip)
target_link_libraries(digzip_engine PUBLIC digzip_objects)

# Internal headers and objects, for tests only.
add_library(digzip_internal INTERFACE)
target_include_directories(digzip_internal INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(digzip_internal INTERFACE DIGZIP_STATIC_DEFINE)
target_link_libraries(digzip_internal INTERFACE digzip_objects)
target_sources(digzip_internal INTERFACE $<TARGET_OBJECTS:digzip_objects>)
