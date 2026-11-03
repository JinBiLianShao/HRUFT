#!/bin/bash
# ============================================================================
# 性能基准 —— HRUFT Pro
#
# 作者：连思鑫
# 说明：这个脚本只做测量，不做判定——不同机器的绝对数值差别很大，
#       卡死阈值只会制造假失败。真正要盯的是「相对变化」：
#       改完代码跑一遍，和上次的数字比，掉一半就是回归。
#
#       测量方式：每档跑 N 轮取中位数，每轮都校验落地文件大小和哈希，
#       校验不过的那一轮直接丢弃，避免把「传失败了所以很快」当成好成绩。
#
# 用法：
#   ./tests/performance/bench.sh              # 默认 256MB，3 轮
#   BENCH_SIZE=536870912 BENCH_ROUNDS=5 ./tests/performance/bench.sh
# ============================================================================

cd "$(dirname "${BASH_SOURCE[0]}")" || exit 1
source ../common.sh

SIZE="${BENCH_SIZE:-268435456}"
ROUNDS="${BENCH_ROUNDS:-3}"

echo "=== 性能基准 ==="
echo "二进制: $HRUFT_BIN"
echo "文件大小: $((SIZE / 1048576)) MB    轮数: $ROUNDS"
echo

# 基准数据放真实磁盘，不放 /tmp —— 很多系统 /tmp 是 tmpfs，
# 那样测出来的写盘代价是假的，会误判瓶颈在不在 IO。
BENCH_DIR="${BENCH_DIR:-$WORK/bench}"
mkdir -p "$BENCH_DIR/dst"
SRC="$BENCH_DIR/bench.bin"
if [ ! -f "$SRC" ] || [ "$(stat -c%s "$SRC" 2>/dev/null)" != "$SIZE" ]; then
    echo "生成测试文件..."
    head -c "$SIZE" /dev/urandom > "$SRC"
fi

PORT=$((PORT_BASE + 200))

# 跑一轮，成功则输出吞吐（MB/s），失败输出空
one_round() {
    local extra=("$@")
    rm -rf "$BENCH_DIR/dst"; mkdir -p "$BENCH_DIR/dst"
    local t0 t1
    t0=$(date +%s%N)
    "$HRUFT_BIN" recv "$PORT" "$BENCH_DIR/dst/" "${extra[@]}" > "$BENCH_DIR/r.log" 2>&1 &
    local rp=$!
    sleep 0.8
    "$HRUFT_BIN" send 127.0.0.1 "$PORT" "$SRC" "${extra[@]}" > /dev/null 2>&1
    local se=$?
    wait $rp
    local re=$?
    t1=$(date +%s%N)

    # 校验：两端退出码 + 文件大小 + 哈希，任一不过就丢弃这一轮
    [ $se -ne 0 ] && return
    [ $re -ne 0 ] && return
    local out="$BENCH_DIR/dst/$(basename "$SRC")"
    [ -f "$out" ] || return
    [ "$(stat -c%s "$out" 2>/dev/null)" != "$SIZE" ] && return
    same_content "$SRC" "$out" || return

    local secs
    secs=$(awk -v a=$t0 -v b=$t1 'BEGIN{printf "%.3f", (b-a)/1e9-0.8}')
    awk -v s=$secs -v sz=$SIZE 'BEGIN{printf "%.1f", (sz/1048576)/s}'
}

# 跑 N 轮取中位数
median_of() {
    local vals=() v
    local i
    for ((i = 0; i < ROUNDS; i++)); do
        v=$(one_round "$@")
        [ -n "$v" ] && vals+=("$v")
    done
    if [ ${#vals[@]} -eq 0 ]; then
        echo "全部失败"
        return
    fi
    printf '%s\n' "${vals[@]}" | sort -n | \
        awk '{a[NR]=$1} END{printf "%.1f MB/s  (%d/%d 轮有效)", a[int((NR+1)/2)], NR, '"$ROUNDS"'}'
}

echo "--- 默认参数（MSS 1500 / window 10MB）---"
printf '  %s\n' "$(median_of)"

echo "--- MSS 8900 ---"
printf '  %s\n' "$(median_of --mss 8900)"

echo "--- MSS 8900 + window 128MB ---"
printf '  %s\n' "$(median_of --mss 8900 --window 134217728)"

echo "--- MSS 1500 + window 128MB ---"
printf '  %s\n' "$(median_of --window 134217728)"

echo "--- 关闭拥塞控制（--no-cc，仅直连）---"
printf '  %s\n' "$(median_of --no-cc)"

echo
echo "提示：绝对数值随机器差异很大，关注的是改动前后的相对变化。"
echo "      /tmp 若是 tmpfs，写盘开销会被严重低估，请用 BENCH_DIR 指到真实磁盘。"

finish
