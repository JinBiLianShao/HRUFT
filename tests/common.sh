#!/bin/bash
# ============================================================================
# 测试公共库 —— HRUFT Pro
#
# 作者：连思鑫
# 说明：所有测试脚本都 source 这个文件，统一处理端口分配、临时目录、
#       断言计数和退出码，避免每个脚本各写一套。
# ============================================================================

set +e

# 仓库根目录。注意 common.sh 在 tests/ 下，所以往上只跳一层。
# （最早写成 ../../，结果指向了仓库的父目录，导致构建好了也找不到二进制。）
_REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

HRUFT_BIN="${HRUFT_BIN:-}"
if [ -z "$HRUFT_BIN" ]; then
    for c in "$_REPO_ROOT/build/hruft" \
             "$_REPO_ROOT/cmake-build-debug/hruft" \
             "$_REPO_ROOT/cmake-build-release/hruft" \
             "$(command -v hruft 2>/dev/null)"; do
        [ -n "$c" ] && [ -x "$c" ] && HRUFT_BIN="$c" && break
    done
fi
if [ -z "$HRUFT_BIN" ] || [ ! -x "$HRUFT_BIN" ]; then
    cat >&2 <<'ERR'
找不到 hruft 可执行文件。
请先在仓库根目录构建：
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j
或显式指定：
    HRUFT_BIN=/path/to/hruft ./tests/run_all.sh
ERR
    exit 2
fi

# UDT 路径探测：安全测试和健壮性测试要自己编译「伪造对端」，
# 得知道 UDT 的头文件和静态库在哪。
#
# 探测顺序：
#   1. 构建目录里 CMake 作为子项目编出来的 libudt.a（最可靠，版本一定对）
#   2. 仓库同级的 udt4 源码树（自己编一份）
HRUFT_UDT_INCLUDE="${HRUFT_UDT_INCLUDE:-}"
HRUFT_UDT_LIB="${HRUFT_UDT_LIB:-}"

if [ -z "$HRUFT_UDT_INCLUDE" ]; then
    for c in "$_REPO_ROOT/../udt4/src" "$_REPO_ROOT/udt4/src" /usr/include/udt; do
        [ -f "$c/udt.h" ] && HRUFT_UDT_INCLUDE="$c" && break
    done
fi

if [ -z "$HRUFT_UDT_LIB" ]; then
    for c in "$_REPO_ROOT/build/udt4/libudt.a" \
             "$_REPO_ROOT/cmake-build-debug/udt4/libudt.a" \
             "$_REPO_ROOT/cmake-build-release/udt4/libudt.a"; do
        [ -f "$c" ] && HRUFT_UDT_LIB="$c" && break
    done
fi

# 构建目录里没有，就现编一份 UDT（只在需要伪造对端的套件里用到）
if [ -z "$HRUFT_UDT_LIB" ] && [ -n "$HRUFT_UDT_INCLUDE" ]; then
    _udt_src="$(cd "$HRUFT_UDT_INCLUDE/.." 2>/dev/null && pwd)"
    if [ -n "$_udt_src" ] && [ -d "$_udt_src/src" ]; then
        _udt_build="$WORK/udt4-build"
        mkdir -p "$_udt_build"
        if cmake -S "$_udt_src" -B "$_udt_build" \
                 -DUDT_BUILD_SHARED=OFF -DUDT_BUILD_STATIC=ON \
                 -DCMAKE_BUILD_TYPE=Release > /dev/null 2>&1 && \
           cmake --build "$_udt_build" -j"$(nproc)" > /dev/null 2>&1; then
            [ -f "$_udt_build/libudt.a" ] && HRUFT_UDT_LIB="$_udt_build/libudt.a"
        fi
    fi
fi

# 端口从环境变量取，多个测试并行跑时不会撞车
PORT_BASE="${PORT_BASE:-23000}"
WORK="${WORK:-$(mktemp -d /tmp/hruft-test-XXXXXX)}"
mkdir -p "$WORK/src" "$WORK/dst"

PASS=0
FAIL=0
SKIP=0
FAILED_NAMES=()

# 断言：成功
ok() {
    printf '  \033[32mPASS\033[0m  %s\n' "$1"
    PASS=$((PASS + 1))
}

# 断言：失败（会记录名字，最后汇总）
bad() {
    printf '  \033[31mFAIL\033[0m  %s\n' "$1"
    [ -n "$2" ] && printf '        %s\n' "$2"
    FAIL=$((FAIL + 1))
    FAILED_NAMES+=("$1")
}

# 断言：跳过（依赖不满足）
skip() {
    printf '  \033[33mSKIP\033[0m  %s\n' "$1"
    SKIP=$((SKIP + 1))
}

# 断言：字符串相等
assert_eq() {
    if [ "$1" = "$2" ]; then ok "$3"; else bad "$3" "期望 [$2]，实际 [$1]"; fi
}

# 断言：文件存在
assert_file() {
    if [ -f "$1" ]; then ok "$2"; else bad "$2" "文件不存在: $1"; fi
}

# 断言：文件不存在
assert_no_file() {
    if [ ! -f "$1" ]; then ok "$2"; else bad "$2" "文件不该存在: $1"; fi
}

# 断言：输出包含某字符串
assert_contains() {
    if grep -q -- "$2" "$1" 2>/dev/null; then ok "$3"; else bad "$3" "输出里没有 [$2]"; fi
}

# 一次完整传输：起接收端 -> 发送 -> 等两端退出
# 用法：transfer <源文件> <端口> [额外参数...]
# 结果：TRANSFER_SEND_EXIT / TRANSFER_RECV_EXIT / TRANSFER_RECV_LOG / TRANSFER_SEND_LOG
transfer() {
    local file="$1"; local port="$2"; shift 2
    local extra=("$@")
    local rlog="$WORK/recv-$port.log"
    local slog="$WORK/send-$port.log"

    "$HRUFT_BIN" recv "$port" "$WORK/dst/" "${extra[@]}" > "$rlog" 2>&1 &
    local rp=$!
    sleep 0.8
    "$HRUFT_BIN" send 127.0.0.1 "$port" "$file" "${extra[@]}" > "$slog" 2>&1
    TRANSFER_SEND_EXIT=$?
    wait $rp
    TRANSFER_RECV_EXIT=$?
    TRANSFER_RECV_LOG="$rlog"
    TRANSFER_SEND_LOG="$slog"
}

# 比对两个文件的 md5
same_content() {
    local a b
    a=$(md5sum "$1" 2>/dev/null | cut -d' ' -f1)
    b=$(md5sum "$2" 2>/dev/null | cut -d' ' -f1)
    [ -n "$a" ] && [ "$a" = "$b" ]
}

# 生成指定大小的随机文件（带缓存，同一大小只生成一次）
make_file() {
    local size="$1"; local path="$2"
    if [ ! -f "$path" ] || [ "$(stat -c%s "$path" 2>/dev/null)" != "$size" ]; then
        head -c "$size" /dev/urandom > "$path"
    fi
}

# 测试结束汇总；返回非 0 表示有失败
finish() {
    echo
    echo "────────────────────────────────────────────────"
    printf '  通过 %d   失败 %d   跳过 %d\n' "$PASS" "$FAIL" "$SKIP"
    if [ "$FAIL" -gt 0 ]; then
        printf '  失败项：\n'
        for n in "${FAILED_NAMES[@]}"; do printf '    - %s\n' "$n"; done
        echo "  临时目录保留在: $WORK"
    else
        [ "${KEEP_WORK:-0}" = "1" ] || rm -rf "$WORK"
    fi
    echo "────────────────────────────────────────────────"
    [ "$FAIL" -eq 0 ]
}
