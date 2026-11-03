#!/bin/bash
# ============================================================================
# 健壮性测试 —— HRUFT Pro
#
# 作者：连思鑫
# 说明：协议头、帧长度、文件名长度全部来自网络，任何一处处理不当都会
#       变成崩溃或挂死。这里用手工构造的畸形帧去打接收端，判定标准是：
#       要么明确报错退出，要么正常结束，**绝不允许崩溃、挂死或内存错误**。
#
#       帧格式：[u32 长度(大端,含类型字节)][u8 类型][payload]
# ============================================================================

cd "$(dirname "${BASH_SOURCE[0]}")" || exit 1
source ../common.sh

echo "=== 健壮性测试：畸形输入 ==="
echo "二进制: $HRUFT_BIN"
echo

P=$PORT_BASE
FUZZ="$WORK/fuzz_peer"

cat > "$WORK/fuzz.cpp" <<'CPPEOF'
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

static std::string hdr(uint32_t magic, uint16_t ver, uint64_t size,
                       uint32_t nlen, const std::string &name) {
    H h; memset(&h, 0, sizeof(h));
    h.magic = htonl(magic); h.ver = htons(ver);
    h.size = htobe64(size); h.nlen = htonl(nlen);
    std::string p((char*)&h, sizeof(h));
    return p + name;
}

static void frame(UDTSOCKET s, uint32_t lenField, uint8_t type,
                  const std::string &pl, bool trunc = false) {
    uint32_t be = htonl(lenField);
    wf(s, (char*)&be, 4);
    char t = (char)type;
    wf(s, &t, 1);
    if (!pl.empty()) {
        size_t n = trunc ? pl.size() / 2 : pl.size();
        wf(s, pl.data(), n);
    }
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    int port = atoi(argv[1]);
    std::string c = argv[2];

    UDT::startup();
    UDTSOCKET s = UDT::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (UDT::connect(s, (sockaddr*)&a, sizeof(a)) == UDT::ERROR) return 1;

    auto sendHdr = [&](uint32_t magic, uint16_t ver, uint64_t size,
                       uint32_t nlen, const std::string &name) {
        std::string pl = hdr(magic, ver, size, nlen, name);
        frame(s, (uint32_t)pl.size() + 1, 1, pl);
    };

    if      (c == "magic_bad")     sendHdr(0xDEADBEEF, 4, 10, 5, "a.bin");
    else if (c == "version_bad")   sendHdr(0x48525034, 99, 10, 5, "a.bin");
    else if (c == "version_zero")  sendHdr(0x48525034, 0, 10, 5, "a.bin");
    else if (c == "len_zero")      frame(s, 0, 1, "");
    else if (c == "len_one")       frame(s, 1, 1, "");
    else if (c == "len_max")       frame(s, 0xFFFFFFFF, 1, "x");
    else if (c == "len_over64m")   frame(s, 64u*1024*1024 + 1, 1, "x");
    else if (c == "len_64m_exact") frame(s, 64u*1024*1024, 1, "x");
    else if (c == "truncated") {
        std::string pl = hdr(0x48525034, 4, 10, 5, "a.bin");
        frame(s, (uint32_t)pl.size() + 1, 1, pl, true);
    }
    else if (c == "type_unknown")  frame(s, 1, 200, "");
    else if (c == "type_zero")     frame(s, 1, 0, "");
    else if (c == "nlen_zero")     sendHdr(0x48525034, 4, 10, 0, "");
    else if (c == "nlen_mismatch") sendHdr(0x48525034, 4, 10, 9999, "a.bin");
    else if (c == "nlen_huge")     sendHdr(0x48525034, 4, 10, 0xFFFFFFFF, "a.bin");
    else if (c == "nlen_65536")    sendHdr(0x48525034, 4, 10, 65536, "a.bin");
    else if (c == "size_huge")     sendHdr(0x48525034, 4, 0xFFFFFFFFFFFFFFFFULL, 5, "a.bin");
    else if (c == "size_zero_extra") {
        // 声明 0 字节，但之后还发数据 —— 接收端应按 0 字节处理，把多出来的当下一帧
        sendHdr(0x48525034, 4, 0, 5, "a.bin");
        std::string junk(100, 'J');
        wf(s, junk.data(), junk.size());
    }
    else if (c == "data_short") {
        sendHdr(0x48525034, 4, 100000, 5, "a.bin");
        std::string d(1000, 'x');
        wf(s, d.data(), d.size());
    }
    else if (c == "hash_short") {
        sendHdr(0x48525034, 4, 0, 5, "a.bin");
        frame(s, 10, 2, "shorthash");
    }
    else if (c == "hash_long") {
        sendHdr(0x48525034, 4, 0, 5, "a.bin");
        frame(s, 100, 2, std::string(99, 'H'));
    }
    else if (c == "no_hash") {
        sendHdr(0x48525034, 4, 0, 5, "a.bin");
        frame(s, 1, 3, "");          // 直接发 DONE，跳过 HASH
    }
    else if (c == "done_wrong_type") {
        sendHdr(0x48525034, 4, 0, 5, "a.bin");
        frame(s, 1, 4, "");          // 发 ACK 而不是 HASH
    }
    else if (c == "abrupt_close") {
        sendHdr(0x48525034, 4, 1000000, 5, "a.bin");
        UDT::close(s); UDT::cleanup();
        printf("CLOSED\n");
        return 0;
    }
    else if (c == "immediate_close") {
        UDT::close(s); UDT::cleanup();
        printf("CLOSED\n");
        return 0;
    }
    else if (c == "flood_headers") {
        for (int i = 0; i < 100; ++i) sendHdr(0x48525034, 4, 4, 5, "a.bin");
    }
    else if (c == "slow_drip") {
        // 一次只发一个字节，测接收端 readFully 的循环是否健壮
        std::string pl = hdr(0x48525034, 4, 4, 5, "a.bin");
        uint32_t be = htonl((uint32_t)pl.size() + 1);
        const char *h = (const char*)&be;
        for (int i = 0; i < 4; ++i) { wf(s, h + i, 1); usleep(5000); }
        char t = 1;
        wf(s, &t, 1);
        for (size_t i = 0; i < pl.size(); ++i) { wf(s, pl.data() + i, 1); usleep(2000); }
    }
    else { printf("UNKNOWN\n"); return 2; }

    usleep(300000);
    UDT::close(s);
    UDT::cleanup();
    return 0;
}
CPPEOF

if ! g++ -std=c++17 -I"$HRUFT_UDT_INCLUDE" "$WORK/fuzz.cpp" -o "$FUZZ" \
        "$HRUFT_UDT_LIB" -lpthread 2>"$WORK/fuzz-build.log"; then
    echo "畸形对端编译失败："
    head -10 "$WORK/fuzz-build.log"
    exit 2
fi

CASES="magic_bad version_bad version_zero len_zero len_one len_max len_over64m
len_64m_exact truncated type_unknown type_zero nlen_zero nlen_mismatch nlen_huge
nlen_65536 size_huge size_zero_extra data_short hash_short hash_long no_hash
done_wrong_type abrupt_close immediate_close flood_headers slow_drip"

i=0
for c in $CASES; do
    i=$((i+1))
    port=$((P + i))
    rm -rf "$WORK/dst"; mkdir -p "$WORK/dst"

    timeout 25 "$HRUFT_BIN" recv "$port" "$WORK/dst/" > "$WORK/f.log" 2>&1 &
    RP=$!
    sleep 0.7
    timeout 25 "$FUZZ" "$port" "$c" > /dev/null 2>&1
    wait $RP
    RE=$?

    # 判定：崩溃（信号，>=128）或挂死（超时 124）都算失败
    if [ $RE -ge 124 ]; then
        bad "$c" "接收端崩溃或挂死（exit=$RE）"
        tail -3 "$WORK/f.log" | sed 's/^/        /'
        continue
    fi

    # 不能留下 .part 残留
    if ls "$WORK/dst"/*.part >/dev/null 2>&1; then
        bad "$c" "残留 .part 文件"
        continue
    fi

    msg=$(grep -a -oE '\[ERROR\].*' "$WORK/f.log" | head -1)
    ok "$c  ->  ${msg:-正常结束}"
done

echo
echo "[额外] 同名 .part 冲突后仍能正常工作"
make_file 4096 "$WORK/src/ok.bin"
transfer "$WORK/src/ok.bin" $((P+90))
same_content "$WORK/src/ok.bin" "$WORK/dst/ok.bin" && \
    ok "畸形输入之后正常传输仍可用" || bad "畸形输入之后正常传输仍可用"

finish
