find_package(Catch2 3 CONFIG REQUIRED)
include(Catch)

add_executable(digzip_tests
    version_test.cpp)
target_link_libraries(digzip_tests PRIVATE DigZip::engine Catch2::Catch2WithMain)
target_compile_definitions(digzip_tests PRIVATE
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
