cmake_minimum_required(VERSION 3.16)
project(my_kvraft CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)                           #在 build/ 目录生成 `compile_commands.json`，保证能正确找到头文件

# 可执行文件输出到 bin/
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

# ==================== muduo 配置 ====================
# 方式 A（推荐）：muduo 已安装到系统（make install 后默认在 /usr/local）
#   直接使用，无需额外配置
#
# 方式 B：muduo 源码在外部目录，未安装
#   执行 cmake 时传 -DMUDUO_ROOT=/path/to/muduo
#   例如：cmake -DMUDUO_ROOT=$HOME/third_party/muduo ..

set(MUDUO_ROOT "muduo" CACHE PATH "muduo")

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

# ==================== 头文件搜索路径 ====================
include_directories(${CMAKE_SOURCE_DIR}/src)

# ==================== 源文件分组 ====================
set(CORE_SOURCES
    src/common/command.cc
    src/kv/kv_store.cc
    src/persist/persister.cc
    src/raft/raft_log.cc
    src/raft/raft_message.cc
    src/raft/raft_node.cc
    src/raft/raft_storage.cc
    src/rpc/rpc_message.cc
    src/rpc/rpc_client.cc
    src/rpc/rpc_server.cc
)

# ==================== 主程序 ====================
add_executable(my_kvraft
    src/main.cc
    ${CORE_SOURCES}
)
target_link_libraries(my_kvraft ${MUDUO_LIBS})

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

# ==================== 全局编译选项 ====================
add_compile_options(-Wall -Wextra -g)
