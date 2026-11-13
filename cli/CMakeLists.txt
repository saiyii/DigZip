add_executable(digzip main.cpp)
target_link_libraries(digzip PRIVATE DigZip::engine)
digzip_set_warnings(digzip)
