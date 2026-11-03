#!/bin/bash
# ============================================================================
# 安全测试 —— HRUFT Pro
#
# 作者：连思鑫
# 说明：这里测的不是「功能对不对」，而是「被恶意对端攻击时会怎样」。
#       文件名完全来自网络，是最容易出问题的地方——历史上这里能写到
#       任意路径去。所以每个用例都要确认：要么拒绝，要么降级到安全位置，
#       绝不能落到预期目录之外。
# ============================================================================

cd "$(dirname "${BASH_SOURCE[0]}")" || exit 1
source ../common.sh

echo "=== 安全测试：恶意对端 ==="
echo "二进制: $HRUFT_BIN"
echo

P=$PORT_BASE
FAKE="$WORK/fake_peer"
OUTSIDE="$WORK/outside"
mkdir -p "$OUTSIDE"

# 伪造对端：能发任意文件名的协议头，用来模拟攻击者
# 不用 hruft 自己发，因为发送端会规规矩矩只发 basename
cat > "$WORK/fake.cpp" <<'CPPEOF'
#include <udt.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <unistd.h>
#include <arpa/inet.h>
#include <endian.h>

static bool wf(UDTSOCKET s, const char *d, size_t n) {
    size_t o = 0;
    while (o < n) {
        int r = UDT::send(s, d + o, (int)(n - o), 0);
        if (r <= 0) return false;
        o += r;
    }
    return true;
}

struct H { uint32_t magic; uint16_t ver; uint16_t flags;
           uint64_t size; uint32_t nlen; uint32_t res; } __attribute__((packed));

static void frame(UDTSOCKET s, uint8_t type, const std::string &pl) {
    uint32_t be = htonl((uint32_t)pl.size() + 1);
    wf(s, (char*)&be, 4);
    char t = (char)type;
    wf(s, &t, 1);
    if (!pl.empty()) wf(s, pl.data(), pl.size());
}

static std::string hdr(const std::string &name, uint64_t size, uint16_t ver = 4) {
    H h; memset(&h, 0, sizeof(h));
    h.magic = htonl(0x48525034); h.ver = htons(ver);
    h.size = htobe64(size); h.nlen = htonl((uint32_t)name.size());
    std::string p((char*)&h, sizeof(h));
    return p + name;
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    int port = atoi(argv[1]);
    std::string mode = argv[2];
    std::string name = argc > 3 ? argv[3] : "x.bin";

    UDT::startup();
    UDTSOCKET s = UDT::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (UDT::connect(s, (sockaddr*)&a, sizeof(a)) == UDT::ERROR) {
        printf("CONNECT_FAIL\n"); return 1;
    }

    if (mode == "name") {
        // 发一个指定文件名的完整合法传输
        const char payload[] = "MALICIOUS-CONTENT";   // 17 字节含结尾 NUL
        frame(s, 1, hdr(name, sizeof(payload) - 1));
        wf(s, payload, sizeof(payload) - 1);
        uint8_t h[32]; memset(h, 0, 32);
        frame(s, 2, std::string((char*)h, 32));
        frame(s, 3, "");
    } else if (mode == "nul") {
        std::string nm = name;
        nm.insert(nm.begin() + 1, '\0');
        frame(s, 1, hdr(nm, 4));
        wf(s, "abcd", 4);
    } else if (mode == "badutf8") {
        std::string nm = name;
        nm += "\xff\xfe";
        frame(s, 1, hdr(nm, 4));
        wf(s, "abcd", 4);
    } else if (mode == "hugesize") {
        frame(s, 1, hdr("big.bin", 0xFFFFFFFFFFFFFFFFULL));
    }
    usleep(200000);
    UDT::close(s);
    UDT::cleanup();
    return 0;
}
CPPEOF

if ! g++ -std=c++17 -I"$HRUFT_UDT_INCLUDE" "$WORK/fake.cpp" -o "$FAKE" \
        "$HRUFT_UDT_LIB" -lpthread 2>"$WORK/fake-build.log"; then
    echo "  伪造对端编译失败，跳过需要它的用例（见 $WORK/fake-build.log）"
    FAKE=""
fi

# 通用：起接收端，让伪造对端连上去，检查有没有逃逸
probe() {
    local label="$1"; local mode="$2"; local name="$3"; local port="$4"
    rm -rf "$WORK/dst"; mkdir -p "$WORK/dst"
    rm -rf "$OUTSIDE"; mkdir -p "$OUTSIDE"

    "$HRUFT_BIN" recv "$port" "$WORK/dst/" > "$WORK/p.log" 2>&1 &
    local rp=$!
    sleep 0.8
    timeout 10 "$FAKE" "$port" "$mode" "$name" > /dev/null 2>&1
    wait $rp
    local re=$?

    # 逃逸判定：预期目录之外出现文件，或上层目录被写
    local escaped=0
    [ -f "$WORK/evil.sh" ] && escaped=1
    [ -f "$WORK/passwd" ] && escaped=1
    [ -f "/tmp/evil.sh" ] && escaped=1
    [ -n "$(ls -A "$OUTSIDE" 2>/dev/null)" ] && escaped=1

    if [ $escaped -eq 1 ]; then
        bad "$label" "文件落到了预期目录之外！"
        return
    fi
    if [ $re -ge 124 ]; then
        bad "$label" "接收端挂死（exit=$re）"
        return
    fi
    ok "$label"
}

echo "[1] 路径穿越"
if [ -n "$FAKE" ]; then
    probe "相对路径穿越 ../../evil.sh" name "../../evil.sh" $((P+1))
    probe "多级穿越 ../../../../tmp/evil.sh" name "../../../../tmp/evil.sh" $((P+2))
    probe "绝对路径 /etc/passwd" name "/etc/passwd" $((P+3))
    probe "绝对路径 /tmp/evil.sh" name "/tmp/evil.sh" $((P+4))
    probe "反斜杠 Windows 风格 ..\\..\\evil.sh" name '..\\..\\evil.sh' $((P+5))
    probe "只有点点 .." name ".." $((P+6))
    probe "只有点 ." name "." $((P+7))
else
    skip "路径穿越（伪造对端不可用）"
fi

echo "[2] 文件名编码攻击"
if [ -n "$FAKE" ]; then
    probe "文件名含 NUL" nul "a" $((P+8))
    probe "文件名非法 UTF-8" badutf8 "x" $((P+9))
else
    skip "文件名编码攻击（伪造对端不可用）"
fi

echo "[3] 资源限制"
if [ -n "$FAKE" ]; then
    # 声称 16EB 的文件，接收端不能因此预分配或挂死
    probe "谎报 16EB 文件大小" hugesize "x" $((P+10))
else
    skip "资源限制（伪造对端不可用）"
fi

# --max-size 必须挡住超大文件
echo "[4] --max-size 限制"
make_file 8192 "$WORK/src/big8k.bin"
"$HRUFT_BIN" recv $((P+11)) "$WORK/dst/" --max-size 1024 > "$WORK/ms.log" 2>&1 &
RP=$!
sleep 0.8
"$HRUFT_BIN" send 127.0.0.1 $((P+11)) "$WORK/src/big8k.bin" > /dev/null 2>&1
wait $RP
if grep -q 'exceeds --max-size' "$WORK/ms.log"; then
    ok "--max-size 拒绝超大文件"
else
    bad "--max-size 拒绝超大文件" "日志里没有拒绝记录"
fi
assert_no_file "$WORK/dst/big8k.bin" "--max-size 拒绝后不落地"

echo "[5] 参数注入 / 非法输入"
"$HRUFT_BIN" recv 0 "$WORK/dst/" > "$WORK/p0.log" 2>&1
assert_contains "$WORK/p0.log" "Port must be" "端口 0 被拒绝"

"$HRUFT_BIN" recv 70000 "$WORK/dst/" > "$WORK/p70.log" 2>&1
assert_contains "$WORK/p70.log" "Port must be" "端口 70000 被拒绝"

"$HRUFT_BIN" recv abc "$WORK/dst/" > "$WORK/pab.log" 2>&1
assert_contains "$WORK/pab.log" "Invalid port" "非数字端口被拒绝"

"$HRUFT_BIN" recv -1 "$WORK/dst/" > "$WORK/pn.log" 2>&1
assert_contains "$WORK/pn.log" "Port must be" "负端口被拒绝"

make_file 64 "$WORK/src/t.bin"
"$HRUFT_BIN" send 127.0.0.1 1 "$WORK/src/t.bin" --mss 100 > "$WORK/m1.log" 2>&1
assert_contains "$WORK/m1.log" "MSS must be" "MSS 过小被拒绝"

"$HRUFT_BIN" send 127.0.0.1 1 "$WORK/src/t.bin" --mss 99999 > "$WORK/m2.log" 2>&1
assert_contains "$WORK/m2.log" "MSS must be" "MSS 过大被拒绝"

"$HRUFT_BIN" send 127.0.0.1 1 "$WORK/src/t.bin" --window 1 > "$WORK/w1.log" 2>&1
assert_contains "$WORK/w1.log" "at least" "窗口过小被拒绝"

"$HRUFT_BIN" send 127.0.0.1 1 "$WORK/src/t.bin" --bogus > "$WORK/u.log" 2>&1
assert_contains "$WORK/u.log" "Unknown option" "未知选项被拒绝"

"$HRUFT_BIN" send 127.0.0.1 1 "$WORK/src/t.bin" --mss > "$WORK/m3.log" 2>&1
if [ $? -ne 0 ]; then ok "--mss 缺参数不崩溃"; else bad "--mss 缺参数"; fi

echo "[6] 非法源路径"
"$HRUFT_BIN" send 127.0.0.1 1 /nonexistent/nope > "$WORK/nf.log" 2>&1
assert_contains "$WORK/nf.log" "File not found" "不存在的文件被拒绝"

"$HRUFT_BIN" send 127.0.0.1 1 "$WORK/src" > "$WORK/dir.log" 2>&1
assert_contains "$WORK/dir.log" "Not a regular file" "目录被拒绝（不是裸 filesystem error）"

finish
