#!/bin/bash
# ============================================================================
# 丢包环境测试 —— HRUFT Pro
#
# 作者：连思鑫
# 说明：loopback 上不丢包，很多问题测不出来。这个脚本用 unshare + tc netem
#       造一个丢包的网络命名空间，在里面验证传输正确性和性能。
#
#       为什么一定要测丢包：UDT 默认的 CUDTCC 慢启动在丢包环境下会卡住，
#       实测 2% 丢包时 64MB 只有 6.8 MB/s，而 --fast-start 能到 128 MB/s。
#       这种差距只有真丢包才看得出来。
#
# 依赖：unshare（util-linux）、tc（iproute2）、内核 netem 模块
# 用法：./tests/robustness/test_packet_loss.sh
# ============================================================================

cd "$(dirname "${BASH_SOURCE[0]}")" || exit 1
source ../common.sh

echo "=== 丢包环境测试 ==="
echo "二进制: $HRUFT_BIN"
echo

if ! command -v unshare >/dev/null 2>&1; then skip "unshare 不可用"; finish; exit $?; fi
if ! command -v tc >/dev/null 2>&1; then skip "tc 不可用"; finish; exit $?; fi
if ! unshare --user --net --map-root-user -- true >/dev/null 2>&1; then
    skip "无法创建网络命名空间"; finish; exit $?; fi
if ! unshare --user --net --map-root-user -- bash -c \
        "ip link set lo up && tc qdisc add dev lo root netem loss 1%" >/dev/null 2>&1; then
    skip "netem 不可用"; finish; exit $?; fi

make_file $((16 * 1024 * 1024)) "$WORK/src/loss16.bin"

loss_case() {
    local rate="$1"; local label="$2"; shift 2
    local extra="$*"
    local port=$((PORT_BASE + 300 + rate))
    local outdir="$WORK/dst-loss"
    rm -rf "$outdir"; mkdir -p "$outdir"
    unshare --user --net --map-root-user -- bash -c "
        ip link set lo up
        tc qdisc add dev lo root netem loss ${rate}% delay 1ms 2>/dev/null
        $HRUFT_BIN recv $port $outdir/ --mss 8900 $extra > $WORK/loss-r.log 2>&1 &
        RP=\$!
        sleep 1
        timeout 180 $HRUFT_BIN send 127.0.0.1 $port $WORK/src/loss16.bin --mss 8900 $extra > /dev/null 2>&1
        echo \$? > $WORK/loss-se
        wait \$RP
        echo \$? > $WORK/loss-re" > /dev/null 2>&1
    local se re
    se=$(cat "$WORK/loss-se" 2>/dev/null || echo 99)
    re=$(cat "$WORK/loss-re" 2>/dev/null || echo 99)
    if [ "$se" != "0" ] || [ "$re" != "0" ]; then
        bad "$label (丢包 ${rate}%)" "send=$se recv=$re"; return
    fi
    if same_content "$WORK/src/loss16.bin" "$outdir/loss16.bin"; then
        ok "$label (丢包 ${rate}%) 内容完整"
    else
        bad "$label (丢包 ${rate}%)" "内容不一致"
    fi
}

echo "[1] 各丢包率下的完整性"
for rate in 1 2 5; do loss_case $rate "默认"; done
for rate in 1 2 5; do loss_case $rate "--fast-start" --fast-start; done

echo
echo "[2] 丢包环境下 --fast-start 应显著快于默认"
run_timed() {
    local rate="$1"; shift
    local extra="$*"
    local port=$((PORT_BASE + 400 + rate))
    rm -rf "$WORK/dst-t"; mkdir -p "$WORK/dst-t"
    unshare --user --net --map-root-user -- bash -c "
        ip link set lo up
        tc qdisc add dev lo root netem loss ${rate}% delay 1ms 2>/dev/null
        $HRUFT_BIN recv $port $WORK/dst-t/ --mss 8900 $extra > /dev/null 2>&1 &
        RP=\$!
        sleep 1
        S=\$(date +%s%N)
        timeout 180 $HRUFT_BIN send 127.0.0.1 $port $WORK/src/loss16.bin --mss 8900 $extra > /dev/null 2>&1
        wait \$RP
        E=\$(date +%s%N)
        echo \$(( (E - S) / 1000000 ))" 2>/dev/null | tail -1
}

# 默认模式在丢包下的耗时本身波动极大（实测 1.0s ~ 10.2s，近 10 倍），
# 单次计时会随机失败。所以取多次的**最小值**做对比：
#   - 最小值代表「这次调度运气最好时」的表现，不受偶发卡顿干扰
#   - fast-start 即便按最好的情况比，也应该明显更快
# 这样测的是「能力上限」而不是「平均运气」，对 CI 更稳定。
take_min() {
    local rate="$1"; shift
    local n="$1"; shift
    local best=""
    local i v
    for ((i = 0; i < n; i++)); do
        v=$(run_timed "$rate" "$@")
        if [ -n "$v" ] && [ "$v" -gt 0 ]; then
            if [ -z "$best" ] || [ "$v" -lt "$best" ]; then best="$v"; fi
        fi
    done
    echo "$best"
}

t_def=$(take_min 2 3)
t_fast=$(take_min 2 3 --fast-start)
if [ -n "$t_def" ] && [ -n "$t_fast" ] && [ "$t_def" -gt 0 ] && [ "$t_fast" -gt 0 ]; then
    # 阈值 2 倍：实测通常 2~13 倍，取最小值后仍应稳定超过 2 倍
    if [ $((t_def / t_fast)) -ge 2 ]; then
        ok "2% 丢包: fast=$t_fast""ms 默认=$t_def""ms（各取 3 次最好）"
    else
        bad "2% 丢包加速" "fast=$t_fast""ms def=$t_def""ms 未达 2 倍"
    fi
else
    skip "2% 丢包计时（无有效耗时）"
fi

finish

