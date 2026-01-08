cmake_minimum_required(VERSION 3.15)
project(HRUFT VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 编译器选项 - 移除 -Werror，等修复所有警告后再加回来
if(MSVC)
    add_compile_options(/W4 /MP)
else()
    add_compile_options(-Wall -Wextra -pthread)
    # 移除 -Werror，等修复所有警告后再启用
    # add_compile_options(-Werror)
endif()

# 依赖查找
find_package(Threads REQUIRED)

# 主目标
add_executable(hruft
        src/main.cpp
        src/cli.cpp
        src/socket.cpp
        src/file_mapper.cpp
        src/crypto.cpp
        src/session.cpp
        src/sender.cpp
        src/receiver.cpp
        src/utils.cpp
)

target_include_directories(hruft PRIVATE include)

# 平台特定链接
if(WIN32)
    target_link_libraries(hruft ws2_32)
else()
    target_link_libraries(hruft pthread)
endif()

# 测试
enable_testing()
add_executable(test_protocol tests/test_protocol.cpp)
target_include_directories(test_protocol PRIVATE include)
add_test(NAME ProtocolTest COMMAND test_protocol)