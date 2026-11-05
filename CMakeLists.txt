cmake_minimum_required(VERSION 3.20)
project(ntfscompact LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(NTFS_CXX_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
set(NTFS_STD_CPPM "${NTFS_CXX_BIN}/../share/libc++/v1/std.cppm")
set(NTFS_STD_PCM "${CMAKE_BINARY_DIR}/std.pcm")
add_custom_command(
  OUTPUT "${NTFS_STD_PCM}"
  COMMAND "${CMAKE_CXX_COMPILER}" -std=c++26 -O2 -DNDEBUG
          -ffunction-sections -fdata-sections -Wno-reserved-module-identifier
          --precompile "${NTFS_STD_CPPM}" -o "${NTFS_STD_PCM}"
  DEPENDS "${NTFS_STD_CPPM}"
  COMMENT "precompiling std module PCM")
add_custom_target(ntfs_std_pcm DEPENDS "${NTFS_STD_PCM}")

add_compile_options(-Wall -Wextra -Wpedantic -O2 -DNDEBUG -ffunction-sections
                    -fdata-sections -fmodule-file=std=${NTFS_STD_PCM})
add_link_options(-Wl,--gc-sections)

add_library(ntfscompact_core STATIC src/cli.cpp src/scan.cpp src/detector.cpp
                                    src/pipeline.cpp src/uncompress.cpp)
target_include_directories(ntfscompact_core PUBLIC src)
add_dependencies(ntfscompact_core ntfs_std_pcm)

add_executable(ntfscompact src/main.cpp)
target_link_libraries(ntfscompact PRIVATE ntfscompact_core)
add_dependencies(ntfscompact ntfs_std_pcm)
target_link_options(ntfscompact PRIVATE -municode)

enable_testing()

add_executable(ntfscompact_tests tests/test_cli.cpp)
target_link_libraries(ntfscompact_tests PRIVATE ntfscompact_core)
add_dependencies(ntfscompact_tests ntfs_std_pcm)
add_test(NAME cli_parse COMMAND ntfscompact_tests)

add_executable(ntfscompact_decide_tests tests/test_decide.cpp)
target_link_libraries(ntfscompact_decide_tests PRIVATE ntfscompact_core)
add_dependencies(ntfscompact_decide_tests ntfs_std_pcm)
add_test(NAME decide_table COMMAND ntfscompact_decide_tests)

add_executable(ntfscompact_pipeline_tests tests/test_pipeline.cpp)
target_link_libraries(ntfscompact_pipeline_tests PRIVATE ntfscompact_core)
add_dependencies(ntfscompact_pipeline_tests ntfs_std_pcm)
add_test(NAME pipeline_format COMMAND ntfscompact_pipeline_tests)

add_executable(ntfscompact_scan_tests tests/test_scan.cpp)
target_link_libraries(ntfscompact_scan_tests PRIVATE ntfscompact_core)
add_dependencies(ntfscompact_scan_tests ntfs_std_pcm)
add_test(NAME scan_traverse COMMAND ntfscompact_scan_tests)

add_executable(ntfscompact_detector_tests tests/test_detector.cpp)
target_link_libraries(ntfscompact_detector_tests PRIVATE ntfscompact_core)
add_dependencies(ntfscompact_detector_tests ntfs_std_pcm)
add_test(NAME detector_real COMMAND ntfscompact_detector_tests)

add_executable(ntfscompact_uncompress_tests tests/test_uncompress.cpp)
target_link_libraries(ntfscompact_uncompress_tests PRIVATE ntfscompact_core)
add_dependencies(ntfscompact_uncompress_tests ntfs_std_pcm)
add_test(NAME uncompress_real COMMAND ntfscompact_uncompress_tests)

add_executable(ntfscompact_run_tests tests/test_run.cpp)
target_link_libraries(ntfscompact_run_tests PRIVATE ntfscompact_core)
add_dependencies(ntfscompact_run_tests ntfs_std_pcm)
add_test(NAME pipeline_run COMMAND ntfscompact_run_tests)
