cmake_minimum_required(VERSION 3.10)                             # CMAKE版本要求

project(kvraft)                                                 # 项目名称

# 设置C++标准 + 强制执行
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(${PROJECT_SOURCE_DIR}/src/*.h)              # 包含头文件

file(GLOB_RECURSE SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc)     # 递归寻找源文件


add_executable(main ${SRC})                                     # 生成可执行程序

set(EXECUTABLE_OUTPUT_PATH ${CMAKE_CURRENT_SOURCE_DIR}/bin)     # 设置可执行程序输出路径

target_compile_options(main PRIVATE -Wall -Wextra -g -O0)       # 开启大部分警告+额外警告 生成调试信息 关闭优化

target_link_libraries(main PRIVATE pthread)                     # 链接线程库 (PRIVATE：仅本层可见 链接本层的不可见)


# 生成测试程序
#add_executable(test_store 
#                            tests/test_kv_store.cc 
#                            src/kv/kv_store.cc
#                )
#target_compile_options(test_store PRIVATE -Wall -Wextra -g -O0)       
#target_link_libraries(test_store PRIVATE pthread)


#add_executable(test_persister 
#                                tests/test_persist.cc
#                                src/kv/kv_store.cc
#                                src/persist/persister.cc
#                                src/common/command.cc
#                )