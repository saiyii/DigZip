find_package(Catch2 3 CONFIG REQUIRED)
include(Catch)

add_executable(digzip_tests
    codec_test.cpp
    encryption_test.cpp
    formats_test.cpp
    options_test.cpp
    path_rules_test.cpp
    robustness_test.cpp
    security_test.cpp
    sevenzip_test.cpp
    text_test.cpp
    version_test.cpp
    zip_test.cpp)
target_include_directories(digzip_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(digzip_tests PRIVATE digzip_internal Catch2::Catch2WithMain)
target_compile_definitions(digzip_tests PRIVATE
    DIGZIP_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    DIGZIP_EXPECTED_VERSION="${PROJECT_VERSION}"
    DIGZIP_EXPECTED_API_VERSION=${DIGZIP_API_VERSION})
digzip_set_warnings(digzip_tests)

catch_discover_tests(digzip_tests DISCOVERY_MODE PRE_TEST)

if(TARGET digzip)
    add_test(NAME cli.version COMMAND digzip --version)
    set_tests_properties(cli.version PROPERTIES
        PASS_REGULAR_EXPRESSION "^digzip ${PROJECT_VERSION}\napi ${DIGZIP_API_VERSION}\nzlib ")
    add_test(NAME cli.usage_on_bad_arguments COMMAND digzip --bogus)
    set_tests_properties(cli.usage_on_bad_arguments PROPERTIES WILL_FAIL ON)
endif()
