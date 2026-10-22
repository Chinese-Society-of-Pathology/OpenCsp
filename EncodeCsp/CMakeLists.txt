cmake_minimum_required(VERSION 3.17)
project(csp_writer C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(csp_writer
    main.cpp
    csp_writer.cpp
)

# 只为本项目实际存在的目标设置编译选项（原先引用了不存在的 csp_diagnose / test_csp_open，导致 CMake 配置失败）。
if(MSVC)
    target_compile_options(csp_writer PRIVATE /W4 /utf-8)
else()
    target_compile_options(csp_writer PRIVATE -Wall -Wextra)
endif()

