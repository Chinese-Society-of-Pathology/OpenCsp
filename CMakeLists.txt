# Top-level build for OpenCsp: decoder, encoder and tests.
#   cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
cmake_minimum_required(VERSION 3.17)
project(OpenCsp CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Decoder (the source file is C++17 despite the .c extension).
set_source_files_properties(DecodeCsp/DecodeCsp.c PROPERTIES LANGUAGE CXX)
add_executable(DecodeCsp DecodeCsp/DecodeCsp.c)
set_target_properties(DecodeCsp PROPERTIES LINKER_LANGUAGE CXX)

# Encoder (target: csp_writer).
add_subdirectory(EncodeCsp)

# Tests.
enable_testing()
set_source_files_properties(tests/csp_tests.cpp PROPERTIES OBJECT_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/DecodeCsp/DecodeCsp.c)
add_executable(csp_tests tests/csp_tests.cpp)

# 1) The shipped sample decodes and every tile is a complete JPEG.
add_test(NAME decode_sample
         COMMAND csp_tests sample ${CMAKE_CURRENT_SOURCE_DIR}/DecodeCsp/sample.csp)

# 2) The command-line decoder runs and exports one tile.
add_test(NAME decode_cli_tile
         COMMAND DecodeCsp ${CMAKE_CURRENT_SOURCE_DIR}/DecodeCsp/sample.csp 0 0 0
                 ${CMAKE_CURRENT_BINARY_DIR}/tile_L0_C0_R0.jpg)

# 3) Round trip: unzip slide.zip -> csp_writer -> decode and compare bytes.
set(RT_DIR ${CMAKE_CURRENT_BINARY_DIR}/roundtrip)
file(MAKE_DIRECTORY ${RT_DIR})
add_test(NAME roundtrip_extract
         COMMAND ${CMAKE_COMMAND} -E tar xf ${CMAKE_CURRENT_SOURCE_DIR}/EncodeCsp/slide.zip
         WORKING_DIRECTORY ${RT_DIR})
add_test(NAME roundtrip_encode COMMAND csp_writer WORKING_DIRECTORY ${RT_DIR})
add_test(NAME roundtrip_verify
         COMMAND csp_tests roundtrip ${RT_DIR}/output.csp ${RT_DIR}/slide)
set_tests_properties(roundtrip_extract PROPERTIES FIXTURES_SETUP slide_dir)
set_tests_properties(roundtrip_encode PROPERTIES FIXTURES_SETUP encoded
                                                 FIXTURES_REQUIRED slide_dir)
set_tests_properties(roundtrip_verify PROPERTIES FIXTURES_REQUIRED "slide_dir;encoded")
