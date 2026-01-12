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
#
# ✅ 构建示例：
#   mkdir build && cd build
#   cmake .. -DCOMPILER_TARGET=arm32 -DPUBLIC_PACKAGE_DIR=/home/lsx/PublicPackage -DTARGET_TYPE=static
#   make -j
#
####################################################################################################

cmake_minimum_required(VERSION 3.10)

# 项目名称与语言
if (NOT DEFINED PROJECT_NAME)
    set(PROJECT_NAME "hruft")
endif ()
project(${PROJECT_NAME} LANGUAGES C CXX)

# 构建类型默认值
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type (Release or Debug)" FORCE)
endif ()

# 编译标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ===================== 用户配置变量 =====================
set(COMPILER_TARGET "x86" CACHE STRING "Target architecture: x86, arm32, arm64")
set(PUBLIC_PACKAGE_DIR "NO" CACHE PATH "External packages root path or NO")
set(TARGET_TYPE "executable" CACHE STRING "Build target type: executable | static | shared")
option(ENABLE_OPENMP "Enable OpenMP parallelism" OFF)
# ========================================================

# ===================== 是否启用外部包 =====================
set(USE_PUBLIC_PACKAGE ON)
if (PUBLIC_PACKAGE_DIR STREQUAL "NO")
    set(USE_PUBLIC_PACKAGE OFF)
endif ()
# ==========================================================

# ===================== 架构与交叉编译 =====================
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

else ()
    message(FATAL_ERROR "Unsupported COMPILER_TARGET: ${COMPILER_TARGET}")
endif ()

link_directories(${SQLITE_LIB_DIR})

# ===================== 自动收集源文件与头文件 =====================
file(GLOB_RECURSE SRC_FILES
        "${CMAKE_SOURCE_DIR}/*.cpp"
        "${CMAKE_SOURCE_DIR}/*.cc"
)

file(GLOB_RECURSE HEADER_FILES
        "${CMAKE_SOURCE_DIR}/*.h"
        "${CMAKE_SOURCE_DIR}/*.hpp"
)

if (USE_PUBLIC_PACKAGE)
    file(GLOB_RECURSE EXT_SRC_FILES
            "${PUBLIC_PACKAGE_DIR}/*.cpp"
            "${PUBLIC_PACKAGE_DIR}/*.cc"
    )
    file(GLOB_RECURSE EXT_HDR_FILES
            "${PUBLIC_PACKAGE_DIR}/*.h"
            "${PUBLIC_PACKAGE_DIR}/*.hpp"
    )
    list(APPEND SRC_FILES ${EXT_SRC_FILES})
    list(APPEND HEADER_FILES ${EXT_HDR_FILES})
endif ()

# 排除构建目录文件
list(FILTER SRC_FILES EXCLUDE REGEX "${CMAKE_BINARY_DIR}")
list(FILTER SRC_FILES EXCLUDE REGEX ".*/CMakeFiles/.*")
list(FILTER SRC_FILES EXCLUDE REGEX "${CMAKE_BINARY_DIR}/.*")


# 提取唯一头文件目录
set(INCLUDE_DIRS "")
foreach (HDR ${HEADER_FILES})
    get_filename_component(DIR ${HDR} DIRECTORY)
    list(APPEND INCLUDE_DIRS ${DIR})
endforeach ()
list(REMOVE_DUPLICATES INCLUDE_DIRS)

# ===================== 构建目标定义 =====================
if (TARGET_TYPE STREQUAL "executable")
    add_executable(${PROJECT_NAME} ${SRC_FILES})
elseif (TARGET_TYPE STREQUAL "static")
    add_library(${PROJECT_NAME} STATIC ${SRC_FILES})
elseif (TARGET_TYPE STREQUAL "shared")
    add_library(${PROJECT_NAME} SHARED ${SRC_FILES})
else ()
    message(FATAL_ERROR "Invalid TARGET_TYPE: ${TARGET_TYPE} (must be executable|static|shared)")
endif ()

# 包含头文件路径
target_include_directories(${PROJECT_NAME} PUBLIC ${INCLUDE_DIRS})

# ===================== 链接库设置 =====================
# 只有构建可执行文件时才链接系统库
if (TARGET_TYPE STREQUAL "executable")
    target_link_libraries(${PROJECT_NAME}
            PRIVATE
            pthread
            rt
            dl
            m
            "-Wl,--start-group"
            "-Wl,--end-group"
    )

    # 链接安全设置（仅限可执行）
    set_target_properties(${PROJECT_NAME} PROPERTIES
            LINK_WHAT_YOU_USE TRUE
    )
endif ()

# ===================== OpenMP 支持（可选） =====================
option(ENABLE_OPENMP "Enable OpenMP parallelism" OFF)

if (ENABLE_OPENMP)
    find_package(OpenMP REQUIRED)
    if (OpenMP_FOUND)
        message(STATUS "OpenMP enabled")
        target_compile_options(${PROJECT_NAME} PRIVATE ${OpenMP_C_FLAGS} ${OpenMP_CXX_FLAGS})
        target_link_libraries(${PROJECT_NAME} PRIVATE ${OpenMP_EXE_LINKER_FLAGS})
    endif ()
endif ()
