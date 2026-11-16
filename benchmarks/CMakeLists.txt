cmake_minimum_required(VERSION 3.16)
project(lsm_baseline LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(LSM_ROOT "${CMAKE_CURRENT_LIST_DIR}/../src/lsm")
add_library(baseline_engine STATIC
    ${LSM_ROOT}/lib/internal_key.cc ${LSM_ROOT}/lib/skip_list.cc
    ${LSM_ROOT}/lib/mem_table.cc ${LSM_ROOT}/lib/coding.cc
    ${LSM_ROOT}/lib/wal.cc ${LSM_ROOT}/lib/bloom_filter.cc
    ${LSM_ROOT}/lib/data_block.cc ${LSM_ROOT}/lib/sstable.cc
    ${LSM_ROOT}/lib/db.cc ${LSM_ROOT}/lib/manifest.cc)
target_include_directories(baseline_engine PUBLIC ${LSM_ROOT}/include PRIVATE ${LSM_ROOT}/include/lsm)
add_executable(lsm-bench lsm_bench.cc)
target_link_libraries(lsm-bench PRIVATE baseline_engine)
target_compile_options(lsm-bench PRIVATE -Wall -Wextra -Wpedantic)
