add_executable(digzip
    args.cpp
    json.cpp
    main.cpp
    output.cpp
    platform.cpp)
target_link_libraries(digzip PRIVATE DigZip::engine)
digzip_set_warnings(digzip)
