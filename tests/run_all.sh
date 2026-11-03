#!/bin/bash
# ============================================================================
# 测试总入口 —— HRUFT Pro
#
# 作者：连思鑫
# 用法：
#   ./tests/run_all.sh              # 跑全部（不含 512MB 大文件，约 2 分钟）
#   ./tests/run_all.sh --full       # 含 512MB 大文件
#   HRUFT_BIN=/path/to/hruft ./tests/run_all.sh
# ============================================================================

cd "$(dirname "${BASH_SOURCE[0]}")" || exit 1

FULL=0
[ "$1" = "--full" ] && FULL=1

TOTAL_PASS=0
TOTAL_FAIL=0
TOTAL_SKIP=0
SUITE_FAILED=()

run_suite() {
    local name="$1"; local script="$2"
    echo
    echo "════════════════════════════════════════════════════════"
    echo "  $name"
    echo "════════════════════════════════════════════════════════"
    bash "$script"
    local rc=$?
    if [ $rc -ne 0 ]; then SUITE_FAILED+=("$name"); fi
    return $rc
}

# 大文件用例只在 --full 下跑
if [ "$FULL" = "1" ]; then
    export HRUFT_TEST_BIG=1
fi

run_suite "功能测试" functional/test_transfer.sh
run_suite "安全测试" security/test_malicious.sh
run_suite "健壮性测试" robustness/test_protocol.sh
run_suite "性能基准" performance/bench.sh

echo
echo "════════════════════════════════════════════════════════"
if [ ${#SUITE_FAILED[@]} -eq 0 ]; then
    echo "  全部测试套件通过"
    exit 0
else
    echo "  以下套件有失败："
    for s in "${SUITE_FAILED[@]}"; do echo "    - $s"; done
    exit 1
fi
