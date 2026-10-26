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
add_library(lsm_engine STATIC
    src/lsm/lib/internal_key.cc
    src/lsm/lib/skip_list.cc
    src/lsm/lib/mem_table.cc
)

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

add_executable(test-lsm-internal-key tests/lsm/test_internal_key.cc)
target_include_directories(test-lsm-internal-key PRIVATE ${LSM_TEST_INCLUDE_DIRS})
target_link_libraries(test-lsm-internal-key PRIVATE lsm_engine)

add_executable(test-lsm-skip-list tests/lsm/test_skip_list.cc)
target_include_directories(test-lsm-skip-list PRIVATE ${LSM_TEST_INCLUDE_DIRS})
target_link_libraries(test-lsm-skip-list PRIVATE lsm_engine)

add_executable(test-lsm-mem-table tests/lsm/test_mem_table.cc)
target_include_directories(test-lsm-mem-table PRIVATE ${LSM_TEST_INCLUDE_DIRS})
target_link_libraries(test-lsm-mem-table PRIVATE lsm_engine)

# ==================== LSM tests ====================
enable_testing()
add_test(NAME lsm-internal-key COMMAND test-lsm-internal-key)
add_test(NAME lsm-skip-list COMMAND test-lsm-skip-list)
add_test(NAME lsm-mem-table COMMAND test-lsm-mem-table)
