# Each target defines LLVMFuzzerTestOneInput. With DIGZIP_LIBFUZZER they are real libFuzzer
# binaries; otherwise replay.cpp provides main(), which runs inputs (and random mutations of them).
foreach(name archive codecs paths)
    add_executable(fuzz_${name} fuzz_${name}.cpp)
    target_link_libraries(fuzz_${name} PRIVATE digzip_internal)
    digzip_set_warnings(fuzz_${name})
    if(DIGZIP_LIBFUZZER)
        target_compile_options(fuzz_${name} PRIVATE -fsanitize=fuzzer)
        target_link_options(fuzz_${name} PRIVATE -fsanitize=fuzzer)
    else()
        target_sources(fuzz_${name} PRIVATE replay.cpp)
        # The test fixtures double as a corpus: every one of them, plus mutations, must run cleanly.
        add_test(NAME fuzz.${name} COMMAND fuzz_${name} --mutations 20 "${PROJECT_SOURCE_DIR}/tests/fixtures")
    endif()
endforeach()
