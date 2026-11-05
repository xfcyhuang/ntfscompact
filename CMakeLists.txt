cmake_minimum_required(VERSION 3.30)
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
set(CMAKE_CXX_MODULE_STD 1)
project(ntfscompact LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wpedantic -Wno-reserved-module-identifier -ffunction-sections -fdata-sections -O2 -DNDEBUG)
add_link_options(-Wl,--gc-sections)

# 解析层（及后续管线）作为静态库，供可执行文件与单元测试共用
add_library(ntfscompact_core STATIC src/cli.cpp src/scan.cpp src/detector.cpp src/pipeline.cpp src/executor.cpp src/uncompress.cpp src/release.cpp)
target_include_directories(ntfscompact_core PUBLIC src)

add_executable(ntfscompact src/main.cpp)
target_link_libraries(ntfscompact PRIVATE ntfscompact_core)
target_link_options(ntfscompact PRIVATE -municode)

enable_testing()

add_executable(ntfscompact_tests tests/test_cli.cpp)
target_link_libraries(ntfscompact_tests PRIVATE ntfscompact_core)
add_test(NAME cli_parse COMMAND ntfscompact_tests)

add_executable(ntfscompact_decide_tests tests/test_decide.cpp)
target_link_libraries(ntfscompact_decide_tests PRIVATE ntfscompact_core)
add_test(NAME decide_table COMMAND ntfscompact_decide_tests)

add_executable(ntfscompact_pipeline_tests tests/test_pipeline.cpp)
target_link_libraries(ntfscompact_pipeline_tests PRIVATE ntfscompact_core)
add_test(NAME pipeline_format COMMAND ntfscompact_pipeline_tests)

add_executable(ntfscompact_scan_tests tests/test_scan.cpp)
target_link_libraries(ntfscompact_scan_tests PRIVATE ntfscompact_core)
add_test(NAME scan_traverse COMMAND ntfscompact_scan_tests)

add_executable(ntfscompact_detector_tests tests/test_detector.cpp)
target_link_libraries(ntfscompact_detector_tests PRIVATE ntfscompact_core)
add_test(NAME detector_real COMMAND ntfscompact_detector_tests)

add_executable(ntfscompact_uncompress_tests tests/test_uncompress.cpp)
target_link_libraries(ntfscompact_uncompress_tests PRIVATE ntfscompact_core)
add_test(NAME uncompress_real COMMAND ntfscompact_uncompress_tests)

add_executable(ntfscompact_run_tests tests/test_run.cpp)
target_link_libraries(ntfscompact_run_tests PRIVATE ntfscompact_core)
add_test(NAME pipeline_run COMMAND ntfscompact_run_tests)

add_executable(ntfscompact_executor_tests tests/test_executor.cpp)
target_link_libraries(ntfscompact_executor_tests PRIVATE ntfscompact_core)
add_test(NAME executor_real COMMAND ntfscompact_executor_tests)

add_executable(ntfscompact_release_tests tests/test_release.cpp)
target_link_libraries(ntfscompact_release_tests PRIVATE ntfscompact_core)
add_test(NAME release_real COMMAND ntfscompact_release_tests)

get_property(NTFSCOMPACT_ALL_TESTS DIRECTORY PROPERTY TESTS)
set_tests_properties(${NTFSCOMPACT_ALL_TESTS} PROPERTIES TIMEOUT 180)
