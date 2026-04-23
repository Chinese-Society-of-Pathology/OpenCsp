cmake_minimum_required(VERSION 3.17)
project(csp_writer C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(csp_writer
    main.cpp
    csp_writer.cpp
)

if(MSVC)
    target_compile_options(csp_writer PRIVATE /W4 /utf-8)
    target_compile_options(csp_diagnose PRIVATE /W3 /utf-8)
    target_compile_options(test_csp_open PRIVATE /W1 /utf-8 /std:c++20)
else()
    target_compile_options(csp_writer PRIVATE -Wall -Wextra)
    target_compile_options(csp_diagnose PRIVATE -Wall -Wextra)
    target_compile_options(test_csp_open PRIVATE -w)
endif()

