add_executable(digzip
    args.cpp
    i18n.cpp
    json.cpp
    main.cpp
    output.cpp
    platform.cpp)
target_link_libraries(digzip PRIVATE DigZip::engine)
digzip_set_warnings(digzip)
if(MINGW)
    target_link_options(digzip PRIVATE -municode) # entry point is wmain
endif()
