####################################################################################################
# HRUFT Pro - CMake 构建配置
#
# 构建示例：
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j
#
# 依赖：
#   - UDT4    ：需自行提供（默认自动探测 ../udt4 或系统安装）
#   - BLAKE3  ：lib/ 下已附带各平台预编译库
#   - json.hpp：lib/ 下已附带
####################################################################################################

cmake_minimum_required(VERSION 3.16)

# ---------------------------------------------------------------------------------------
# 0. 交叉编译器（必须在 project() 之前确定）
#
# CMake 跑完 project() 就锁定编译器，之后再改 CMAKE_<LANG>_COMPILER 会被静默忽略——
# 这是旧版 CMakeLists 的坑：-DCOMPILER_TARGET=arm64 实际仍用本机 gcc。这里在
# project() 之前解析 COMPILER_TARGET 并直接设定编译器，首次配置即可用，
# 不再需要「改完编译器请你重跑 cmake」的两段式流程。
# ---------------------------------------------------------------------------------------
if (COMPILER_TARGET STREQUAL "arm64")
    find_program(HRUFT_CROSS_C   aarch64-linux-gnu-gcc   REQUIRED)
    find_program(HRUFT_CROSS_CXX aarch64-linux-gnu-g++   REQUIRED)
    set(CMAKE_C_COMPILER   ${HRUFT_CROSS_C}   CACHE FILEPATH "" FORCE)
    set(CMAKE_CXX_COMPILER ${HRUFT_CROSS_CXX} CACHE FILEPATH "" FORCE)
    set(CMAKE_SYSTEM_NAME Linux)
    set(CMAKE_SYSTEM_PROCESSOR aarch64)
elseif (COMPILER_TARGET STREQUAL "arm32")
    find_program(HRUFT_CROSS_C   arm-linux-gnueabihf-gcc  REQUIRED)
    find_program(HRUFT_CROSS_CXX arm-linux-gnueabihf-g++  REQUIRED)
    set(CMAKE_C_COMPILER   ${HRUFT_CROSS_C}   CACHE FILEPATH "" FORCE)
    set(CMAKE_CXX_COMPILER ${HRUFT_CROSS_CXX} CACHE FILEPATH "" FORCE)
    set(CMAKE_SYSTEM_NAME Linux)
    set(CMAKE_SYSTEM_PROCESSOR arm)
endif ()

project(hruft VERSION 4.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif ()

# ---------------------------------------------------------------------------------------
# 1. 构建选项
# ---------------------------------------------------------------------------------------
set(TARGET_OS "linux" CACHE STRING "Target OS: linux, windows")
set(COMPILER_TARGET "x86" CACHE STRING "Target architecture: x86, arm32, arm64")
set(UDT_ROOT "" CACHE PATH "UDT4 source tree or install prefix")
option(ENABLE_OPENMP "Enable OpenMP parallelism" OFF)

if (NOT TARGET_OS STREQUAL "linux" AND NOT TARGET_OS STREQUAL "windows")
    message(FATAL_ERROR "TARGET_OS must be 'linux' or 'windows', got '${TARGET_OS}'")
endif ()
if (NOT COMPILER_TARGET STREQUAL "x86" AND NOT COMPILER_TARGET STREQUAL "arm32" AND NOT COMPILER_TARGET STREQUAL "arm64")
    message(FATAL_ERROR "COMPILER_TARGET must be x86, arm32 or arm64, got '${COMPILER_TARGET}'")
endif ()

# ---------------------------------------------------------------------------------------
# 3. 依赖：UDT4
# ---------------------------------------------------------------------------------------
set(UDT_INCLUDE_DIR "")
set(UDT_SOURCE_DIR "")

if (UDT_ROOT)
    if (EXISTS "${UDT_ROOT}/src/udt.h")
        set(UDT_SOURCE_DIR "${UDT_ROOT}")
        set(UDT_INCLUDE_DIR "${UDT_ROOT}/src")
    elseif (EXISTS "${UDT_ROOT}/include/udt/udt.h")
        set(UDT_INCLUDE_DIR "${UDT_ROOT}/include/udt")
    elseif (EXISTS "${UDT_ROOT}/udt.h")
        set(UDT_INCLUDE_DIR "${UDT_ROOT}")
    else ()
        message(FATAL_ERROR "UDT_ROOT='${UDT_ROOT}' has no src/udt.h or include/udt/udt.h")
    endif ()
else ()
    foreach (_cand "${CMAKE_CURRENT_SOURCE_DIR}/../udt4" "${CMAKE_CURRENT_SOURCE_DIR}/udt4")
        if (NOT UDT_INCLUDE_DIR AND EXISTS "${_cand}/src/udt.h")
            set(UDT_SOURCE_DIR "${_cand}")
            set(UDT_INCLUDE_DIR "${_cand}/src")
        endif ()
    endforeach ()
    if (NOT UDT_INCLUDE_DIR)
        find_path(UDT_INCLUDE_DIR udt.h PATHS /usr/include/udt /usr/local/include/udt)
    endif ()
endif ()

if (NOT UDT_INCLUDE_DIR)
    message(FATAL_ERROR
            "UDT4 headers not found.\n"
            "  git clone https://github.com/JinBiLianShao/udt4.git ../udt4\n"
            "then re-run cmake (auto-detected), or pass -DUDT_ROOT=/path/to/udt4")
endif ()

if (NOT EXISTS "${UDT_INCLUDE_DIR}/ccc.h")
    message(FATAL_ERROR
            "UDT4 headers at ${UDT_INCLUDE_DIR} are missing ccc.h.\n"
            "HRUFT needs ccc.h for --no-cc (custom congestion control).\n"
            "Point -DUDT_ROOT at the UDT4 SOURCE tree, not a binary-only install.")
endif ()

message(STATUS "UDT4 headers: ${UDT_INCLUDE_DIR}")
if (UDT_SOURCE_DIR)
    message(STATUS "UDT4 source:  ${UDT_SOURCE_DIR} (built as subproject)")
endif ()

# ---------------------------------------------------------------------------------------
# 4. 源文件
# ---------------------------------------------------------------------------------------
file(GLOB SRC_FILES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
if (NOT SRC_FILES)
    message(FATAL_ERROR "No source files found under src/")
endif ()

# ---------------------------------------------------------------------------------------
# 5. 目标
# ---------------------------------------------------------------------------------------
add_executable(${PROJECT_NAME} ${SRC_FILES})

target_include_directories(${PROJECT_NAME} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/lib"
        "${UDT_INCLUDE_DIR}")

# 版本号只在这里定义一次，源码里用 HRUFT_VERSION 宏取，
# 避免 CMakeLists 和 Usage 输出各写一个版本号然后对不上。
target_compile_definitions(${PROJECT_NAME} PRIVATE
        HRUFT_VERSION="${PROJECT_VERSION}")

if (TARGET_OS STREQUAL "windows")
    target_compile_definitions(${PROJECT_NAME} PRIVATE WIN32 _WIN32 _WIN32_WINNT=0x0600 _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32 wsock32 iphlpapi shlwapi)
else ()
    # GCC 7（老 arm32 交叉工具链）的 std::filesystem 是实验性实现，
# 需要额外链接 libstdc++fs.a；GCC 9+ 已合并进 libstdc++，不需要。
if (COMPILER_TARGET STREQUAL "arm32")
    target_link_libraries(${PROJECT_NAME} PRIVATE stdc++fs)
endif ()

find_package(Threads REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE Threads::Threads rt dl m)
endif ()

if (MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W3)
else ()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra)
endif ()

# ---------------------------------------------------------------------------------------
# 6. UDT4 库
# ---------------------------------------------------------------------------------------
if (UDT_SOURCE_DIR)
    set(UDT_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(UDT_BUILD_STATIC ON CACHE BOOL "" FORCE)
    add_subdirectory("${UDT_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/udt4" EXCLUDE_FROM_ALL)
    # Windows 交叉构建：WIN32 宏必须传到 UDT 子项目，否则 udt.h 会走 POSIX
    # 分支去找 sys/socket.h 之类 Linux 头文件，编译直接失败。
    if (TARGET_OS STREQUAL "windows")
        target_compile_definitions(udt_static PRIVATE WIN32 _WIN32 _WIN32_WINNT=0x0600 UDT_STATIC __MINGW__)
        target_compile_options(udt_static PRIVATE -fpermissive)
    endif ()
    target_link_libraries(${PROJECT_NAME} PRIVATE udt_static)
else ()
    find_library(UDT_LIBRARY NAMES udt udt4 PATHS /usr/lib /usr/local/lib /usr/lib/x86_64-linux-gnu)
    if (NOT UDT_LIBRARY)
        message(FATAL_ERROR "UDT4 library not found. Pass -DUDT_ROOT=/path/to/udt4")
    endif ()
    message(STATUS "UDT4 library: ${UDT_LIBRARY}")
    target_link_libraries(${PROJECT_NAME} PRIVATE "${UDT_LIBRARY}")
endif ()

# ---------------------------------------------------------------------------------------
# 7. BLAKE3
# ---------------------------------------------------------------------------------------
if (TARGET_OS STREQUAL "windows")
    set(BLAKE3_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/win")
elseif (COMPILER_TARGET STREQUAL "arm64")
    set(BLAKE3_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/arm64")
elseif (COMPILER_TARGET STREQUAL "arm32")
    set(BLAKE3_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/arm32")
else ()
    set(BLAKE3_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/linux")
endif ()

if (NOT EXISTS "${BLAKE3_LIB_DIR}")
    message(FATAL_ERROR
            "No prebuilt BLAKE3 for TARGET_OS=${TARGET_OS} COMPILER_TARGET=${COMPILER_TARGET}.\n"
            "Expected one of lib/linux, lib/arm32, lib/arm64, lib/win.")
endif ()

target_link_directories(${PROJECT_NAME} PRIVATE "${BLAKE3_LIB_DIR}")
target_link_libraries(${PROJECT_NAME} PRIVATE blake3)
message(STATUS "BLAKE3: ${BLAKE3_LIB_DIR}")

# ---------------------------------------------------------------------------------------
# 8. OpenMP
# ---------------------------------------------------------------------------------------
if (ENABLE_OPENMP)
    find_package(OpenMP REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE OpenMP::OpenMP_CXX)
    message(STATUS "OpenMP: enabled")
endif ()

# ---------------------------------------------------------------------------------------
# 9. 安装
# ---------------------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# ---------------------------------------------------------------------------------------
# 10. 汇总
# ---------------------------------------------------------------------------------------
message(STATUS "------------------------------------------------")
message(STATUS " Project:     ${PROJECT_NAME} ${PROJECT_VERSION}")
message(STATUS " Target OS:   ${TARGET_OS}")
message(STATUS " Arch:        ${COMPILER_TARGET}")
message(STATUS " Build type:  ${CMAKE_BUILD_TYPE}")
message(STATUS " CXX:         ${CMAKE_CXX_COMPILER}")
message(STATUS "------------------------------------------------")
