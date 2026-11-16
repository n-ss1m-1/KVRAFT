cmake_minimum_required(VERSION 3.16)
project(my_kvraft CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)                           #在 build/ 目录生成 `compile_commands.json`，保证能正确找到头文件

# 可执行文件输出到 bin/  (如果没有该文件夹则创建)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

# ==================== muduo 配置 ====================
set(MUDUO_ROOT "${CMAKE_SOURCE_DIR}/third_party/muduo" CACHE PATH "muduo")
if(MUDUO_ROOT)
    message(STATUS "Using muduo from: ${MUDUO_ROOT}")
    include_directories(${MUDUO_ROOT})
    link_directories(${MUDUO_ROOT}/build/lib)
else()
    message(STATUS "Using system-installed muduo")
endif()
# muduo 的库
set(MUDUO_LIBS
    muduo_net
    muduo_base
    pthread
)

# ==================== spdlog header-only ====================
add_subdirectory(third_party/spdlog)


# ==================== 头文件搜索路径 ====================
include_directories(${CMAKE_SOURCE_DIR}/src)

# ==================== 源文件分组 ====================
set(CORE_SOURCES
    src/common/command.cc
    src/common/thread_pool.cc
    src/kv/kv_store.cc
    src/persist/persister.cc
    src/raft/raft_log.cc
    src/raft/raft_message.cc
    src/raft/raft_node.cc
    src/raft/raft_storage.cc
    src/rpc/rpc_message.cc
    src/rpc/rpc_client.cc
    src/rpc/rpc_server.cc
    src/client_server/client_server.cc
)

# ==================== 主程序 ====================
add_executable(raft-kv
    src/main.cc
    ${CORE_SOURCES}
)
target_link_libraries(raft-kv PRIVATE ${MUDUO_LIBS} spdlog::spdlog_header_only)



# ==================== 测试程序 ====================
# 每个测试独立编译，只包含被测模块

# # KVStore 测试（不依赖 muduo）
# add_executable(test-kv
#     tests/test_kv_store.cc
#     src/kv/kv_store.cc
#     src/common/command.cc
# )

# # RaftLog 测试（不依赖 muduo）
# add_executable(test-raft-log
#     tests/test_raft_log.cc
#     src/raft/raft_log.cc
#     src/raft/raft_message.cc
#     src/common/command.cc
# )

# # RaftStorage 测试（不依赖 muduo）
# add_executable(test-raft-storage
#     tests/test_raft_storage.cc
#     src/raft/raft_storage.cc
#     src/raft/raft_message.cc
#     src/persist/persister.cc
#     src/common/command.cc
# )

# # RaftNode 测试（不依赖 muduo，用 FakeTransport）
# add_executable(test-raft-node
#     tests/test_raft_node.cc
#     src/raft/raft_node.cc
#     src/raft/raft_log.cc
#     src/raft/raft_storage.cc
#     src/raft/raft_message.cc
#     src/persist/persister.cc
#     src/kv/kv_store.cc
#     src/common/command.cc
# )

# # RPC 消息序列化测试（不依赖 muduo）
# add_executable(test-rpc-message
#     tests/test_rpc_message.cc
#     src/rpc/rpc_message.cc
#     src/raft/raft_message.cc
#     src/common/command.cc
# )

# # KVStore 测试
# add_executable(test-kv-store
#     tests/test_kv_store.cc
#     src/kv/kv_store.cc
#     src/common/command.cc
# )
# target_include_directories(test-kv-store PUBLIC src)

# # RaftStorage 测试
# add_executable(test-raft-storage
#     tests/test_raft_storage.cc
#     src/raft/raft_storage.cc
#     src/raft/raft_message.cc
#     src/persist/persister.cc
#     src/common/command.cc
# )
# target_include_directories(test-raft-storage PUBLIC src)
# target_link_libraries(test-raft-storage PRIVATE spdlog::spdlog_header_only)

# # Persister 测试
# add_executable(test-persister
#     tests/test_persister.cc
#     src/persist/persister.cc
# )
# target_include_directories(test-persister PUBLIC src)
# target_link_libraries(test-persister PRIVATE spdlog::spdlog_header_only)

# # RaftLog 测试
# add_executable(test-raft-log
#     tests/test_raft_log.cc
#     src/raft/raft_log.cc
#     src/raft/raft_message.cc
#     src/common/command.cc
# )
# target_include_directories(test-raft-log PUBLIC src)
# target_link_libraries(test-raft-log PRIVATE spdlog::spdlog_header_only)

# # ==================== 全局编译选项 ====================
# add_compile_options(-Wall -Wextra -g)

# ==================== LSM engine ====================

# 引擎静态库：所有 LSM 实现集中在这里。
find_package(Threads REQUIRED)

add_library(lsm_engine STATIC
    src/lsm/lib/internal_key.cc
    src/lsm/lib/skip_list.cc
    src/lsm/lib/mem_table.cc
    src/lsm/lib/coding.cc
    src/lsm/lib/wal.cc
    src/lsm/lib/bloom_filter.cc
    src/lsm/lib/data_block.cc
    src/lsm/lib/sstable.cc
    src/lsm/lib/db.cc
    src/lsm/lib/manifest.cc
)

target_link_libraries(lsm_engine PUBLIC Threads::Threads)

# 公开头文件使用 #include "lsm/xxx.h"；实现文件暂时还使用裸文件名，
# 因此额外保留 PRIVATE 的 src/lsm/include/lsm 搜索路径。
target_include_directories(lsm_engine
    PUBLIC
        ${CMAKE_SOURCE_DIR}/src/lsm/include
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src/lsm/include/lsm
)

# 测试程序只编译测试文件，具体实现统一从 lsm_engine 静态库链接。
set(LSM_TEST_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/tests/kvraft
)

add_executable(test-lsm-manifest tests/lsm/test_manifest.cc)
target_include_directories(test-lsm-manifest PRIVATE ${LSM_TEST_INCLUDE_DIRS})
target_link_libraries(test-lsm-manifest PRIVATE lsm_engine)
# 模拟短读写、EINTR、同步失败和rename失败，验证发布边界。
target_link_options(test-lsm-manifest PRIVATE "-Wl,--wrap=read" "-Wl,--wrap=write"
    "-Wl,--wrap=fsync" "-Wl,--wrap=rename" "-Wl,--wrap=fstat")

add_executable(test-lsm-db tests/lsm/test_db.cc)
target_include_directories(test-lsm-db PRIVATE ${LSM_TEST_INCLUDE_DIRS})
target_link_libraries(test-lsm-db PRIVATE lsm_engine)
# DB后台刷盘测试：模拟短读写、暂停SSTable写入及WAL删除失败。
target_link_options(test-lsm-db PRIVATE "-Wl,--wrap=write" "-Wl,--wrap=read" "-Wl,--wrap=unlink")

add_executable(test-lsm-internal-key tests/lsm/test_internal_key.cc)
target_include_directories(test-lsm-internal-key PRIVATE ${LSM_TEST_INCLUDE_DIRS})
target_link_libraries(test-lsm-internal-key PRIVATE lsm_engine)

add_executable(test-lsm-skip-list tests/lsm/test_skip_list.cc)
target_include_directories(test-lsm-skip-list PRIVATE ${LSM_TEST_INCLUDE_DIRS})
target_link_libraries(test-lsm-skip-list PRIVATE lsm_engine)

add_executable(test-lsm-mem-table tests/lsm/test_mem_table.cc)
target_include_directories(test-lsm-mem-table PRIVATE ${LSM_TEST_INCLUDE_DIRS})
target_link_libraries(test-lsm-mem-table PRIVATE lsm_engine)

add_executable(test-lsm-bloom-filter tests/lsm/test_bloom_filter.cc)
target_include_directories(test-lsm-bloom-filter PRIVATE ${LSM_TEST_INCLUDE_DIRS})
target_link_libraries(test-lsm-bloom-filter PRIVATE lsm_engine)

add_executable(test-lsm-data-block tests/lsm/test_data_block.cc)
target_include_directories(test-lsm-data-block PRIVATE ${LSM_TEST_INCLUDE_DIRS})
target_link_libraries(test-lsm-data-block PRIVATE lsm_engine)

add_executable(test-lsm-coding tests/lsm/test_coding.cc)
target_include_directories(test-lsm-coding PRIVATE ${LSM_TEST_INCLUDE_DIRS})
target_link_libraries(test-lsm-coding PRIVATE lsm_engine)

add_executable(test-lsm-wal-reader tests/lsm/test_wal_reader.cc)
target_include_directories(test-lsm-wal-reader PRIVATE ${LSM_TEST_INCLUDE_DIRS})
target_link_libraries(test-lsm-wal-reader PRIVATE lsm_engine)

add_executable(test-lsm-wal-writer tests/lsm/test_wal_writer.cc)
target_include_directories(test-lsm-wal-writer PRIVATE ${LSM_TEST_INCLUDE_DIRS})
target_link_libraries(test-lsm-wal-writer PRIVATE lsm_engine)
# Linux 测试使用链接器包装，模拟短写、EINTR 和 I/O 失败；不修改引擎接口。
target_link_options(test-lsm-wal-writer PRIVATE "-Wl,--wrap=write" "-Wl,--wrap=fsync")

add_executable(test-lsm-sstable-builder tests/lsm/test_sstable_builder.cc)
target_include_directories(test-lsm-sstable-builder PRIVATE ${LSM_TEST_INCLUDE_DIRS})
target_link_libraries(test-lsm-sstable-builder PRIVATE lsm_engine)

add_executable(test-lsm-sstable-reader tests/lsm/test_sstable_reader.cc)
target_include_directories(test-lsm-sstable-reader PRIVATE ${LSM_TEST_INCLUDE_DIRS})
target_link_libraries(test-lsm-sstable-reader PRIVATE lsm_engine)
# Reader 测试模拟短读、EINTR、提前 EOF 和 I/O 失败。
target_link_options(test-lsm-sstable-reader PRIVATE "-Wl,--wrap=pread")

add_executable(test-lsm-sstable-writer tests/lsm/test_sstable_writer.cc)
target_include_directories(test-lsm-sstable-writer PRIVATE ${LSM_TEST_INCLUDE_DIRS})
target_link_libraries(test-lsm-sstable-writer PRIVATE lsm_engine)
target_link_options(test-lsm-sstable-writer PRIVATE "-Wl,--wrap=write" "-Wl,--wrap=fsync")

# ==================== LSM tests ====================
enable_testing()
add_test(NAME lsm-manifest COMMAND test-lsm-manifest)
set_tests_properties(lsm-manifest PROPERTIES TIMEOUT 20)
add_test(NAME lsm-db COMMAND test-lsm-db)
set_tests_properties(lsm-db PROPERTIES TIMEOUT 30)
add_test(NAME lsm-internal-key COMMAND test-lsm-internal-key)
add_test(NAME lsm-skip-list COMMAND test-lsm-skip-list)
add_test(NAME lsm-mem-table COMMAND test-lsm-mem-table)

add_test(NAME lsm-bloom-filter COMMAND test-lsm-bloom-filter)
add_test(NAME lsm-data-block COMMAND test-lsm-data-block)
add_test(NAME lsm-coding COMMAND test-lsm-coding)
add_test(NAME lsm-wal-reader COMMAND test-lsm-wal-reader)
add_test(NAME lsm-wal-writer COMMAND test-lsm-wal-writer)
set_tests_properties(lsm-wal-writer PROPERTIES TIMEOUT 20)

add_test(NAME lsm-sstable-builder COMMAND test-lsm-sstable-builder)

add_test(NAME lsm-sstable-reader COMMAND test-lsm-sstable-reader)
set_tests_properties(lsm-sstable-reader PROPERTIES TIMEOUT 20)

add_test(NAME lsm-sstable-writer COMMAND test-lsm-sstable-writer)
set_tests_properties(lsm-sstable-writer PROPERTIES TIMEOUT 20)

# Optional cost diagnostics; disabled in ordinary builds.
option(LSM_DIAGNOSTICS "Enable LSM cost diagnostics" OFF)
if(LSM_DIAGNOSTICS)
 target_compile_definitions(lsm_engine PUBLIC LSM_DIAGNOSTICS=1)
endif()
