####################################################################################################
# 📘 通用 CMake 构建系统模板说明文档（支持编译可执行文件或库）
# 作者 ： 连思鑫
# 本构建系统支持以下功能：
#   ✅ 自动收集源文件和头文件
#   ✅ 支持三种构建目标：可执行文件 / 静态库 / 动态库
#   ✅ 适配多平台交叉编译（x86、arm32、arm64）
#   ✅ 链接外部静态库（如 SQLite）
#   ✅ 可选启用 OpenMP 支持
#   ✅ 编译选项集中管理，易扩展
#
# 📁 推荐项目目录结构：
#   project_root/
#   ├── CMakeLists.txt
#   ├── src/                # 项目源文件
#   ├── include/            # 项目头文件
#   ├── lib/                # 可选本地库
#   ├── build/              # 构建目录
#   └── external/           # 第三方库（如 Sqlite）
#
# 🧩 可配置选项（通过命令行传入）：
#   -DCOMPILER_TARGET=x86|arm32|arm64             # 目标平台
#   -DPUBLIC_PACKAGE_DIR=/path/to/packages|NO     # 第三方库路径 / NO表示不引入第三方库路径
#   -DCMAKE_BUILD_TYPE=Release|Debug              # 构建类型
#   -DENABLE_OPENMP=ON|OFF                        # 是否启用 OpenMP
#   -DTARGET_TYPE=executable|static|shared        # 目标类型：可执行 / 静态库 / 动态库
#   -DTARGET_OS=linux|windows                     # 目标操作系统

# ✅ 构建示例：
#   mkdir build && cd build
#   cmake .. -DCOMPILER_TARGET=arm32 -DPUBLIC_PACKAGE_DIR=/home/lsx/PublicPackage -DTARGET_TYPE=static
#   make -j

# ✅ 跨平台构建示例：
#   (Linux)   cmake .. -DTARGET_OS=linux -DCOMPILER_TARGET=x86
#   (Windows) cmake .. -DTARGET_OS=windows
####################################################################################################

cmake_minimum_required(VERSION 3.10)

# ---------------------------------------------------------------------------------------
# 1. 项目基础配置
# ---------------------------------------------------------------------------------------
if (NOT DEFINED PROJECT_NAME)
    set(PROJECT_NAME "hruft")
endif ()
project(${PROJECT_NAME} LANGUAGES C CXX)

# 设置构建类型 (Debug/Release)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif ()

# 强制 C++17 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------------------
# 2. 用户选项目录
# ---------------------------------------------------------------------------------------
set(TARGET_OS "linux" CACHE STRING "Target OS: linux, windows")
set(COMPILER_TARGET "x86" CACHE STRING "Target architecture: x86, arm32, arm64")
set(PUBLIC_PACKAGE_DIR "NO" CACHE PATH "External packages root path or NO")
set(TARGET_TYPE "executable" CACHE STRING "Build target type: executable | static | shared")
option(ENABLE_OPENMP "Enable OpenMP parallelism" OFF)

# ---------------------------------------------------------------------------------------
# 3. 编译器与交叉编译逻辑
# ---------------------------------------------------------------------------------------
if (TARGET_OS STREQUAL "windows")
    # --- Windows 平台配置 ---
    add_definitions(-D_CRT_SECURE_NO_WARNINGS) # 忽略 MSVC 安全警告
    message(STATUS "Build Mode: Windows")

    # 如果在 Linux 下使用 MinGW 交叉编译 Windows
    if (CMAKE_HOST_SYSTEM_NAME MATCHES "Linux")
        set(CMAKE_SYSTEM_NAME Windows)
        set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
        set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
    endif()

else()
    # --- Linux 平台配置 ---
    message(STATUS "Build Mode: Linux, Target Arch: ${COMPILER_TARGET}")

    if (COMPILER_TARGET STREQUAL "arm64")
        set(CMAKE_SYSTEM_NAME Linux)
        set(CMAKE_C_COMPILER "/bin/aarch64-linux-gnu-gcc")
        set(CMAKE_CXX_COMPILER "/bin/aarch64-linux-gnu-g++")

    elseif (COMPILER_TARGET STREQUAL "arm32")
        set(CMAKE_SYSTEM_NAME Linux)
        set(CMAKE_C_COMPILER "/opt/gcc-linaro-7.5.0-2019.12-x86_64_arm-linux-gnueabihf/bin/arm-linux-gnueabihf-gcc")
        set(CMAKE_CXX_COMPILER "/opt/gcc-linaro-7.5.0-2019.12-x86_64_arm-linux-gnueabihf/bin/arm-linux-gnueabihf-g++")

    elseif (COMPILER_TARGET STREQUAL "x86")
        set(CMAKE_C_COMPILER "gcc")
        set(CMAKE_CXX_COMPILER "g++")
    endif()
endif()

# ---------------------------------------------------------------------------------------
# 4. 自动收集源文件与头文件
# ---------------------------------------------------------------------------------------
# 递归搜索 src 和 include 目录（如果存在）
file(GLOB_RECURSE SRC_FILES
        "${CMAKE_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_SOURCE_DIR}/src/*.c"
        "${CMAKE_SOURCE_DIR}/*.cc"
        "${CMAKE_SOURCE_DIR}/*.cpp" # 兼容根目录
)

file(GLOB_RECURSE HEADER_FILES
        "${CMAKE_SOURCE_DIR}/include/*.h"
        "${CMAKE_SOURCE_DIR}/include/*.hpp"
        "${CMAKE_SOURCE_DIR}/*.h"
)

# 处理外部包路径
if (NOT PUBLIC_PACKAGE_DIR STREQUAL "NO")
    file(GLOB_RECURSE EXT_SRC
            "${PUBLIC_PACKAGE_DIR}/*.cpp"
            "${PUBLIC_PACKAGE_DIR}/*.cc"
            "${PUBLIC_PACKAGE_DIR}/*.c"
    )
    file(GLOB_RECURSE EXT_HDR
            "${PUBLIC_PACKAGE_DIR}/*.h"
            "${PUBLIC_PACKAGE_DIR}/*.hpp"
    )
    list(APPEND SRC_FILES ${EXT_SRC})
    list(APPEND HEADER_FILES ${EXT_HDR})
endif ()

# 排除编译输出目录
list(FILTER SRC_FILES EXCLUDE REGEX "${CMAKE_BINARY_DIR}")
list(FILTER SRC_FILES EXCLUDE REGEX ".*/CMakeFiles/.*")
list(FILTER SRC_FILES EXCLUDE REGEX "${CMAKE_BINARY_DIR}/.*")

# 自动提取所有头文件所在的目录作为包含路径
set(INCLUDE_DIRS "")
foreach (HDR ${HEADER_FILES})
    get_filename_component(DIR ${HDR} DIRECTORY)
    list(APPEND INCLUDE_DIRS ${DIR})
endforeach ()
list(REMOVE_DUPLICATES INCLUDE_DIRS)

# ---------------------------------------------------------------------------------------
# 5. 构建目标
# ---------------------------------------------------------------------------------------
if (TARGET_TYPE STREQUAL "executable")
    add_executable(${PROJECT_NAME} ${SRC_FILES})
elseif (TARGET_TYPE STREQUAL "static")
    add_library(${PROJECT_NAME} STATIC ${SRC_FILES})
elseif (TARGET_TYPE STREQUAL "shared")
    add_library(${PROJECT_NAME} SHARED ${SRC_FILES})
else ()
    message(FATAL_ERROR "Invalid TARGET_TYPE: ${TARGET_TYPE} (must be executable|static|shared)")
endif ()

# 绑定头文件目录
target_include_directories(${PROJECT_NAME} PUBLIC ${INCLUDE_DIRS})

# ---------------------------------------------------------------------------------------
# 6. 链接库设置 (区分系统)
# ---------------------------------------------------------------------------------------
if (TARGET_TYPE STREQUAL "executable")
    if (TARGET_OS STREQUAL "windows")
        # Windows 必须链接的库 (网络、基础 API)
        target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32 wsock32 iphlpapi shlwapi)
    else()
        # Linux 基础系统库
        target_link_libraries(${PROJECT_NAME} PRIVATE pthread rt dl m)
    endif()
endif ()

# ---------------------------------------------------------------------------------------
# 7. OpenMP 支持
# ---------------------------------------------------------------------------------------
if (ENABLE_OPENMP)
    find_package(OpenMP REQUIRED)
    if (OpenMP_FOUND)
        message(STATUS "OpenMP support enabled.")
        target_link_libraries(${PROJECT_NAME} PUBLIC OpenMP::OpenMP_CXX)
    endif ()
endif ()

# ---------------------------------------------------------------------------------------
# 8. 编译信息汇总
# ---------------------------------------------------------------------------------------
message(STATUS "------------------------------------------------")
message(STATUS " Project Name:   ${PROJECT_NAME}")
message(STATUS " Target OS:      ${TARGET_OS}")
message(STATUS " Target Type:    ${TARGET_TYPE}")
message(STATUS " Build Type:     ${CMAKE_BUILD_TYPE}")
message(STATUS " Install Dir:    ${CMAKE_INSTALL_PREFIX}")
message(STATUS "------------------------------------------------")