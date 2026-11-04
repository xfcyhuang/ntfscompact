cmake_minimum_required(VERSION 3.20)
project(ntfscompact LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra)

# 解析层（及后续管线）作为静态库，供可执行文件与单元测试共用
add_library(ntfscompact_core STATIC src/cli.cpp)
target_include_directories(ntfscompact_core PUBLIC src)

add_executable(ntfscompact src/main.cpp)
target_link_libraries(ntfscompact PRIVATE ntfscompact_core)
# 宽字符入口 wmain，原生保留任意（含中文）路径
target_link_options(ntfscompact PRIVATE -municode)

enable_testing()
add_executable(ntfscompact_tests tests/test_cli.cpp)
target_link_libraries(ntfscompact_tests PRIVATE ntfscompact_core)
add_test(NAME cli_parse COMMAND ntfscompact_tests)
