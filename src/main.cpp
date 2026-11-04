/**
 * HRUFT Pro v4.1.0 —— 基于 UDT4 的可靠大文件传输
 *
 * 作者：连思鑫
 *
 * 依赖：
 *   1. UDT4        —— https://github.com/JinBiLianShao/udt4
 *   2. BLAKE3      —— lib/ 下附带四个平台的预编译库
 *   3. nlohmann/json
 *
 * 写这个程序的起因很简单：普通 TCP 传大文件，跨运营商或者出国链路一丢包
 * 速度就塌到几百 KB/s，等一个 100GB 的包要一整天。换成基于 UDP 的 UDT 之后
 * 同样的链路能跑满带宽，所以有了这个东西。
 *
 * v4.0.0 是破坏性更新。v3 的控制消息没有长度前缀，靠「一次 recv 恰好读到
 * 一条消息」来区分，而 UDT 是字节流语义，这个假设根本不成立——ACK 会被
 * 报告数据污染、文件名会被截断、报告会丢前半截。v4 全部改成带长度前缀的
 * 定长帧，代价是和 v3 不互通，收发两端必须一起升。
 *
 * 性能上走过的弯路记在 tests/README.md 里了，最要紧的一条：MSS 必须两端
 * 显式给同一个值，别指望 UDT 自己协商——协商取的是双方较小值，一端给
 * 8900 一端给 1500 的话，FC 会按 8900 算包数却按 1500 发包，实测掉 30 倍。
 */

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <cmath>
#include <algorithm>
#include <sstream>
#include <cstring>
#include <cstdint>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <deque>
#include <utility>
#include <stdexcept>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#endif

#include <udt.h>
#include <ccc.h>
#include "blake3.h"

// 版本号由 CMake 通过 target_compile_definitions 注入，
// 避免出现「CMakeLists 写 4.1.0、Usage 里还印着 4.0.0」这种不一致。
#ifndef HRUFT_VERSION
#define HRUFT_VERSION "unknown"
#endif
#include "json.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

// ============================ 常量 ============================

// 应用层一次读写 4MB。这个值是我试出来的：太小系统调用次数多，
// 太大缓冲池吃内存且流水线延迟变高。4MB 在 NVMe 和千兆网下都合适。

static const uint32_t MAGIC_ID = 0x48525034;   // "HRP4"
static const uint16_t PROTO_VERSION = 4;   // 协议本身没变，只是软件版本号升

#ifndef HRUFT_BLOCK_KB
#define HRUFT_BLOCK_KB 4096
#endif
// 应用层一次读写的大小。默认 4MB。
// 太小则系统调用和 UDT send 次数变多，太大则缓冲池吃内存、流水线延迟变高。
// 实测 1/2/4/8/16MB 在 loopback 上吞吐差别在噪声内，所以取 4MB 折中。
static const int APP_BLOCK_SIZE = HRUFT_BLOCK_KB * 1024;
static const int UDT_MAX_BUF   = 256 * 1024 * 1024;   // 256MB 上限
static const int DEFAULT_MSS   = 1500;
static const int DEFAULT_WINDOW = 10 * 1024 * 1024;   // 10MB
static const int MIN_MSS = 536;
static const int MAX_MSS = 8900;
static const int MIN_WINDOW = 64 * 1024;
#ifndef HRUFT_PIPELINE_DEPTH
#define HRUFT_PIPELINE_DEPTH 4
#endif
static const int PIPELINE_DEPTH = HRUFT_PIPELINE_DEPTH;   // 流水线缓冲块数

// 帧类型。只有控制消息走帧，文件数据是裸字节流——
// 给每 4MB 数据套一层帧头纯属浪费，长度由协议头里的 file_size 精确界定。
enum FrameType : uint8_t {
    FRAME_HEADER = 1,   // 协议头 + 文件名
    FRAME_HASH   = 2,   // 32 字节 BLAKE3
    FRAME_DONE   = 3,   // 数据发送完毕
    FRAME_ACK    = 4,   // 1 字节状态
    FRAME_REPORT = 5,   // JSON 报告
};

enum AckStatus : uint8_t { ACK_OK = 0, ACK_HASH_FAIL = 1, ACK_ABORT = 2 };

// ============================ 协议头 ============================

// 大端、定长 24 字节。文件名紧跟在后面，不带结尾的 NUL。
// 加了 version 和 flags 是为了以后能协商，不用再改一次协议。

#pragma pack(push, 1)
struct ProtocolHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint64_t file_size;
    uint32_t filename_len;
    uint32_t reserved;
};
#pragma pack(pop)

static_assert(sizeof(ProtocolHeader) == 24, "ProtocolHeader must be 24 bytes");

// ============================ 平台工具 ============================

#ifdef _WIN32
class Utf8Util {
public:
    static std::wstring toWide(const std::string &s) {
        if (s.empty()) return L"";
        int sz = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
        if (sz <= 0) throw std::runtime_error("Invalid UTF-8 path");
        std::wstring w(sz, 0);
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], sz);
        return w;
    }
    static std::string toUtf8(const std::wstring &w) {
        if (w.empty()) return "";
        int sz = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        if (sz <= 0) throw std::runtime_error("Invalid wide path");
        std::string s(sz, 0);
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], sz, nullptr, nullptr);
        return s;
    }
};
#endif

// 校验 UTF-8 合法性。
//
// 为什么非查不可：文件名是网络对面送来的，我用 std::string 收，转 fs::path
// 的时候 Windows 会走 MultiByteToWideChar，非法 UTF-8 要么直接失败，要么
// 被替换成 U+FFFD，用户看到的文件名就变了。所以宁可拒收，也不能落一个
// 名字对不上的文件。
//
// 这里按标准严格查：过长编码、代理区、超 U+10FFFF 全部拒绝。
static bool isValidUtf8(const std::string &s) {
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t extra = 0;
        uint32_t cp = 0;

        if (c < 0x80) { ++i; continue; }
        else if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
        else return false;   // 0x80-0xBF 作首字节，或 5/6 字节过长编码

        if (i + extra >= n) return false;   // 后续字节不足

        for (size_t k = 1; k <= extra; ++k) {
            unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        // 拒绝过长编码与代理区
        if (extra == 1 && cp < 0x80) return false;
        if (extra == 2 && cp < 0x800) return false;
        if (extra == 3 && cp < 0x10000) return false;
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;
        if (cp > 0x10FFFF) return false;

        i += extra + 1;
    }
    return true;
}

// 路径与字符串互转。
// Windows 上全程 UTF-8 <-> UTF-16，因为 fs::path 内部是 wchar_t，
// 直接用 std::string 构造会在非中文 locale 下乱码。Linux 下本来就是 UTF-8，
// 直接透传即可。
static fs::path toPath(const std::string &utf8) {
#ifdef _WIN32
    return fs::path(Utf8Util::toWide(utf8));
#else
    return fs::path(utf8);
#endif
}

static std::string pathToUtf8(const fs::path &p) {
#ifdef _WIN32
    return Utf8Util::toUtf8(p.wstring());
#else
    return p.string();
#endif
}

// ============================ 字节序 ============================

#ifdef _WIN32
#ifdef _MSC_VER
#define htonll(x) _byteswap_uint64(x)
#define ntohll(x) _byteswap_uint64(x)
#else
#define htonll(x) (((uint64_t)htonl((uint32_t)((x) & 0xFFFFFFFF)) << 32) | htonl((uint32_t)((x) >> 32)))
#define ntohll(x) (((uint64_t)ntohl((uint32_t)((x) & 0xFFFFFFFF)) << 32) | ntohl((uint32_t)((x) >> 32)))
#endif
#else
#include <endian.h>
#define htonll htobe64
#define ntohll be64toh
#endif

// ============================ 拥塞控制 ============================

// 直连场景用的「不控制」拥塞算法。
//
// 为什么需要：两台机器网线直连时根本不存在中间链路拥塞，UDT 默认的
// CUDTCC 还在那儿慢启动、丢一个包就退让，纯属自找麻烦。这个类什么都不做，
// 发送周期和窗口都定死。
//
// 注意单位：m_dPktSndPeriod 是微秒/包，m_dCWndSize 是**包数**不是字节。
// 我一开始按字节写，结果窗口大了几千倍，一发包就把接收端打爆。

class SimpleCC : public CCC {
public:
    SimpleCC() {
        m_dPktSndPeriod = 1.0;       // 微秒/包
        m_dCWndSize = 1000000.0;     // 单位是「包」，不是字节
    }
    void init() override {}
    void onACK(int32_t) override {}
    void onLoss(const int32_t *, int) override {}
    void onTimeout() override {}
    void onPktSent(const CPacket *) override {}
    void onPktReceived(const CPacket *) override {}
    void processCustomMsg(const CPacket *) override {}
    void close() override {}
};

// 跳过慢启动的拥塞控制。
//
// 为什么需要：UDT 默认 CUDTCC 的 init() 里 m_dCWndSize = 16，也就是从
// 16 个包开始翻倍爬升，而每次翻倍要等一个 10ms 的 RC 周期。传大文件时
// 这点爬坡时间可以忽略，但传几十 MB 的文件就很明显了 —— 实测 16MB 要
// 0.42s，其中绝大部分花在从 16 包爬到窗口上限。
//
// 这里直接把初始窗口给到最大，让发送端一开始就满速。代价是不做拥塞退让，
// 所以只在 --fast-start 且链路可控时启用。
class FastStartCC : public CCC {
public:
    void init() override {
        // 直接给到大窗口，跳过爬坡；m_dMaxCWndSize 由 UDT 按接收端通告的
        // 飞行窗口设置，这里给一个足够大的值，实际会被它夹住。
        m_dCWndSize = m_dMaxCWndSize > 0 ? m_dMaxCWndSize : 25600.0;
        m_dPktSndPeriod = 1.0;
        // ACK 间隔调小，让窗口更新更及时
        setACKInterval(2);
    }
    void onACK(int32_t) override {
        // 维持大窗口，不因 ACK 变化而收缩
        if (m_dCWndSize < m_dMaxCWndSize) m_dCWndSize = m_dMaxCWndSize;
    }
    void onLoss(const int32_t *, int) override {
        // 不因丢包退让（直连场景）
    }
    void onTimeout() override {}
    void onPktSent(const CPacket *) override {}
    void onPktReceived(const CPacket *) override {}
    void processCustomMsg(const CPacket *) override {}
    void close() override {}
};

class SimpleCCFactory : public CCCVirtualFactory {
public:
    CCC *create() override { return new SimpleCC; }
    CCCVirtualFactory *clone() override { return new SimpleCCFactory; }
};

class FastStartCCFactory : public CCCVirtualFactory {
public:
    CCC *create() override { return new FastStartCC; }
    CCCVirtualFactory *clone() override { return new FastStartCCFactory; }
};

// ============================ 网络分析 ============================

// 把 UDT 的 perfmon 数据转成 JSON，再按几条经验规则给点建议。
// 规则都很朴素，就是拿 BDP 和配置窗口比大小、看重传率。
// 别指望它能诊断出所有问题，它只能告诉你「窗口配小了」这类明显的。

class NetworkStats {
public:
    static json snapshot(UDT::TRACEINFO &perf, double duration, uint64_t totalBytes) {
        double avgSpeedMbps = duration > 0 ? (totalBytes * 8.0 / 1000000.0) / duration : 0.0;
        json j;
        j["throughput"] = json::object({
            {"avg_mbps", avgSpeedMbps},
            {"inst_mbps", perf.mbpsRecvRate},
            {"est_bandwidth_mbps", perf.mbpsBandwidth}
        });
        j["latency"] = json::object({{"rtt_ms", perf.msRTT}});
        // 注意统计口径：pktRetransTotal 只统计「本端重传了多少包」。
        // 接收端不发送数据，所以这个值恒为 0 —— 拿它算重传率会得出
        // 「网络零丢包」的错误结论。真正反映链路质量的是 pkt_loss_recv
        // （本端检测到丢了多少包），重传率应该在**发送端**看。
        // 这里两个都给，并在字段名上标清楚，避免看报告的人误读。
        j["reliability"] = json::object({
            {"pkt_sent", perf.pktSentTotal},
            {"pkt_recv", perf.pktRecvTotal},
            {"pkt_loss_sent", perf.pktSndLossTotal},
            {"pkt_loss_recv", perf.pktRcvLossTotal},
            {"retrans_total", perf.pktRetransTotal},
            {"retrans_ratio", perf.pktSentTotal > 0
                 ? static_cast<double>(perf.pktRetransTotal) / static_cast<double>(perf.pktSentTotal)
                 : 0.0},
            // 收包丢包率：接收端唯一有意义的链路质量指标
            {"recv_loss_ratio", perf.pktRecvTotal > 0
                 ? static_cast<double>(perf.pktRcvLossTotal) / static_cast<double>(perf.pktRecvTotal)
                 : 0.0},
            {"retrans_note", "retrans_* 仅统计本端重传；接收端恒为 0，请看 recv_loss_ratio"}
        });
        j["congestion"] = json::object({
            {"window_flow", perf.pktFlowWindow},
            {"window_cong", perf.pktCongestionWindow},
            {"flight_size", perf.pktFlightSize}
        });
        j["buffer_health"] = json::object({
            {"snd_buf_avail_bytes", perf.byteAvailSndBuf},
            {"rcv_buf_avail_bytes", perf.byteAvailRcvBuf}
        });
        return j;
    }

    static json analyze(const json &stats, int configMss, int configWin) {
        json report = stats;
        std::vector<std::string> advice;
        std::string health = "excellent";

        double lossRate = 0.0, bandwidth = 0.0, rtt = 0.0;
        int64_t availRcvBuf = 0;
        try {
            // 用 recv_loss_ratio 而不是 retrans_ratio：接收端不发送数据，
            // retrans 恒为 0，用它判断链路质量会永远是「优秀」。
            lossRate  = stats.at("reliability").at("recv_loss_ratio").get<double>();
            bandwidth = stats.at("throughput").at("est_bandwidth_mbps").get<double>();
            rtt       = stats.at("latency").at("rtt_ms").get<double>();
            availRcvBuf = stats.at("buffer_health").at("rcv_buf_avail_bytes").get<int64_t>();
        } catch (const json::exception &) {
            health = "unknown";
            advice.push_back("无法获取完整统计信息");
        }

        long long bdp = static_cast<long long>((bandwidth * 1e6 * rtt * 1e-3) / 8.0);

        if (configWin < bdp && bdp > 0) {
            advice.push_back("配置警告: 窗口大小 (" + std::to_string(configWin) +
                             ") 小于带宽时延积 (" + std::to_string(bdp) + ")。吞吐量受物理限制。");
            health = "suboptimal";
        }
        if (availRcvBuf < (static_cast<int64_t>(configWin) / 10) && configWin > 0) {
            advice.push_back("可能瓶颈: 接收缓冲区快满了。考虑增大窗口大小或检查磁盘IO。");
        }
        if (lossRate > 0.01) {
            advice.push_back("网络质量: 高丢包率 (" + std::to_string(lossRate * 100) + "%)。检查网络稳定性。");
            health = "network_lossy";
            if (configMss > 1400) advice.push_back("优化建议: 尝试减少MSS到1400以避免IP分片。");
        } else if (lossRate > 0.001) {
            health = "good";
        } else if (configMss < 8900 && rtt < 20) {
            advice.push_back("网络质量: 优秀。如果支持巨型帧，可尝试 --mss 8900。");
        }

        report["analysis"] = json::object({
            {"network_health", health},
            {"bdp_bytes_est", bdp},
            {"advice", advice}
        });
        return report;
    }
};

// ============================ 配置 ============================

struct Config {
    std::string mode;
    std::string ip;
    int port = 0;
    std::string path;
    // MSS 默认 1500，两端一致。
    // 试过「默认不设、让 UDT 自己协商」，结果更糟：UDT 握手取双方较小值，
    // 发送端不设就是 1500，而接收端若给了 8900，FC 会按 8900 算包数、
    // 实际却按 1500 发包，窗口严重失配，实测吞吐从 370 掉到 9 MB/s。
    // 结论：两端必须显式给同一个值，所以这里保留具体默认值。
    int mss = DEFAULT_MSS;
    bool mss_explicit = false;
    int window = DEFAULT_WINDOW;
    bool detailed = false;
    bool no_cc = false;
    bool fast_start = false;   // 跳过慢启动爬坡
    uint64_t max_size = 0;     // 0 = 不限

    static void printUsage() {
        std::cerr << "HRUFT Pro " << HRUFT_VERSION << "\n"
                  << "Usage:\n"
                  << "  hruft send <ip> <port> <filepath> [options]\n"
                  << "  hruft recv <port> <savepath> [options]\n\n"
                  << "Options:\n"
                  << "  --mss <value>      Maximum Segment Size, 536-8900 (default: 1500; give BOTH ends the same value)\n"
                  << "  --window <value>   Window size in bytes (default: 10MB, max: 256MB)\n"
                  << "  --max-size <value> Receiver: reject files larger than this many bytes (default: unlimited)\n"
                  << "  --detailed         Show detailed statistics\n"
                  << "  --no-cc            Disable congestion control (direct links only)\n"
                  << "  --fast-start       Skip slow-start ramp-up (much faster on small files,\n"
                  << "                     but no congestion back-off: direct links only)\n";
    }

    static int parsePort(const std::string &s) {
        long v = 0;
        try { v = std::stol(s); } catch (const std::exception &) {
            throw std::runtime_error("Invalid port: " + s);
        }
        if (v < 1 || v > 65535) throw std::runtime_error("Port must be between 1 and 65535: " + s);
        return static_cast<int>(v);
    }

    static uint64_t parseU64(const std::string &s, const char *what) {
        try {
            size_t used = 0;
            unsigned long long v = std::stoull(s, &used);
            if (used != s.size()) throw std::runtime_error("");
            return static_cast<uint64_t>(v);
        } catch (const std::exception &) {
            throw std::runtime_error(std::string("Invalid ") + what + ": " + s);
        }
    }

    static Config parse(int argc, char *argv[]) {
        Config c;
        if (argc < 2) {
            printUsage();
            throw std::runtime_error("No command provided");
        }
        c.mode = argv[1];
        int idx = 2;

        if (c.mode == "send") {
            if (argc < 5) { printUsage(); throw std::runtime_error("Invalid send arguments"); }
            c.ip = argv[idx++];
            c.port = parsePort(argv[idx++]);
            c.path = argv[idx++];
        } else if (c.mode == "recv") {
            if (argc < 4) { printUsage(); throw std::runtime_error("Invalid recv arguments"); }
            c.port = parsePort(argv[idx++]);
            c.path = argv[idx++];
        } else {
            printUsage();
            throw std::runtime_error("Unknown mode: " + c.mode);
        }

        for (; idx < argc; ++idx) {
            std::string arg = argv[idx];
            if (arg == "--mss" && idx + 1 < argc) {
                c.mss = static_cast<int>(parseU64(argv[++idx], "MSS"));
                if (c.mss < MIN_MSS || c.mss > MAX_MSS)
                    throw std::runtime_error("MSS must be between " + std::to_string(MIN_MSS) +
                                             " and " + std::to_string(MAX_MSS));
                c.mss_explicit = true;
            } else if (arg == "--window" && idx + 1 < argc) {
                uint64_t w = parseU64(argv[++idx], "window size");
                if (w < static_cast<uint64_t>(MIN_WINDOW))
                    throw std::runtime_error("Window size must be at least " + std::to_string(MIN_WINDOW) + " bytes");
                if (w > static_cast<uint64_t>(UDT_MAX_BUF)) w = UDT_MAX_BUF;
                c.window = static_cast<int>(w);
            } else if (arg == "--max-size" && idx + 1 < argc) {
                c.max_size = parseU64(argv[++idx], "max size");
            } else if (arg == "--detailed") {
                c.detailed = true;
            } else if (arg == "--no-cc") {
                c.no_cc = true;
            } else if (arg == "--fast-start") {
                c.fast_start = true;
            } else {
                throw std::runtime_error("Unknown option: " + arg);
            }
        }

        // --no-cc 与 --fast-start 同时给时，tuneSocket 里 no_cc 分支会先命中，
        // fast_start 会被静默吞掉——宁可在这里直接报错。
        if (c.no_cc && c.fast_start)
            throw std::runtime_error("--no-cc and --fast-start are mutually exclusive");

        return c;
    }
};

// ============================ 工具 ============================

class Utils {
public:
    static std::string hashToString(const uint8_t *hash, size_t len) {
        static const char *hex = "0123456789abcdef";
        std::string out;
        out.reserve(len * 2);
        for (size_t i = 0; i < len; ++i) {
            out.push_back(hex[hash[i] >> 4]);
            out.push_back(hex[hash[i] & 0x0F]);
        }
        return out;
    }

    static std::string formatSize(uint64_t bytes) {
        static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
        int unit = 0;
        double size = static_cast<double>(bytes);
        while (size >= 1024.0 && unit < 4) { size /= 1024.0; unit++; }
        std::stringstream ss;
        ss << std::fixed << std::setprecision(2) << size << " " << units[unit];
        return ss.str();
    }

    static std::string errText() {
        return std::string(UDT::getlasterror().getErrorMessage());
    }
};

// ---- 完整读写 ----
//
// UDT 是字节流，recv 返回的是「当前可读多少」，不是「一条消息」。
// v3 就是栽在这儿：一次 recv 想读整条 ACK，结果把后面的 JSON 报告也吞了，
// 于是 ACK 比对失败、报告解析失败，两边日志都是错的。
// 所以现在一律循环读写，读到够数为止。

static void writeFully(UDTSOCKET s, const char *data, size_t len) {
    size_t off = 0;
    while (off < len) {
        int n = UDT::send(s, data + off, static_cast<int>(len - off), 0);
        if (n == UDT::ERROR)
            throw std::runtime_error("Send failed: " + Utils::errText());
        if (n == 0) throw std::runtime_error("Send returned 0 bytes (connection closed)");
        off += static_cast<size_t>(n);
    }
}

// 返回 false 表示对端在一个字节都没发的情况下干净关闭了。
// 这种情况是正常的（比如客户端连上就断），不算错误，由调用方决定怎么处理。
static bool readFully(UDTSOCKET s, char *data, size_t len) {
    size_t off = 0;
    while (off < len) {
        int n = UDT::recv(s, data + off, static_cast<int>(len - off), 0);
        if (n == UDT::ERROR) {
            if (off == 0 && UDT::getlasterror().getErrorCode() == 5004) return false; // 对端关闭
            throw std::runtime_error("Receive failed: " + Utils::errText());
        }
        if (n == 0) {
            if (off == 0) return false;
            throw std::runtime_error("Connection closed mid-message");
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

// 帧格式：[u32 长度(大端, 含类型字节)][u8 类型][payload]
//
// 长度字段把类型字节也算进去，这样解析时 payload 长度 = 长度 - 1，
// 少一次加减心算，也不容易搞错。上限 64MB，正常控制消息远达不到，
// 设这个上限纯粹是为了不让恶意对端用一个巨大的长度值把内存吃光。
static void sendFrame(UDTSOCKET s, uint8_t type, const std::string &payload) {
    uint32_t total = static_cast<uint32_t>(payload.size() + 1);
    if (total > 64u * 1024u * 1024u) throw std::runtime_error("Frame too large");
    char head[5];
    uint32_t be = htonl(total);
    std::memcpy(head, &be, 4);
    head[4] = static_cast<char>(type);
    writeFully(s, head, 5);
    if (!payload.empty()) writeFully(s, payload.data(), payload.size());
}

static bool recvFrame(UDTSOCKET s, uint8_t &type, std::string &payload) {
    char head[4];
    if (!readFully(s, head, 4)) return false;
    uint32_t total = 0;
    std::memcpy(&total, head, 4);
    total = ntohl(total);
    if (total < 1 || total > 64u * 1024u * 1024u)
        throw std::runtime_error("Invalid frame length: " + std::to_string(total));
    char t = 0;
    if (!readFully(s, &t, 1)) throw std::runtime_error("Truncated frame header");
    type = static_cast<uint8_t>(t);
    payload.assign(total - 1, '\0');
    if (total > 1 && !readFully(s, &payload[0], total - 1))
        throw std::runtime_error("Truncated frame payload");
    return true;
}

static void setRecvTimeout(UDTSOCKET s, int ms) {
    if (UDT::setsockopt(s, 0, UDT_RCVTIMEO, &ms, sizeof(int)) == UDT::ERROR)
        throw std::runtime_error("Failed to set receive timeout: " + Utils::errText());
}

// ============================ 临时文件守卫 ============================

// 临时文件守卫。
//
// 接收端先写 <目标>.part，哈希对上了才 rename 成正式名字。这样即使传输
// 中途崩了，用户目录里也不会出现一个「看着像完整文件其实缺一半」的东西。
//
// 但光这样还不够——.part 本身也得清掉。我最早是在每个失败分支里手动
// remove，结果健壮性测试一跑，26 种畸形输入里有 10 种会留下 .part：
// 对端断开、帧错误、超时、磁盘写满……异常路径太多，漏一个就残留。
// 改成 RAII 之后，只要离开作用域就自动删，不用再一个个补。
//
// 用的时候记住：rename 成功之后一定要调 release()，否则析构时会把
// 刚落地的正式文件删掉。
class TempFileGuard {
public:
    explicit TempFileGuard(fs::path p) : path_(std::move(p)) {}
    ~TempFileGuard() {
        if (!armed_) return;
        std::error_code ec;
        fs::remove(path_, ec);   // 已 rename 走或已手动删过时，这里就是 no-op
    }
    // 正式落地后调用，撤销自动清理
    void release() { armed_ = false; }
    const fs::path &path() const { return path_; }

private:
    fs::path path_;
    bool armed_ = true;
};

// ============================ 流水线缓冲池 ============================

// 收发都用三段流水线：读盘/写盘、BLAKE3 计算、网络收发各占一个线程，
// 中间靠这个池子传递缓冲块。
//
// 设计上刻意不做队列长度限制以外的调度：acquire 拿空块，填好后 pushReady，
// 消费端 popReady 处理完再 release。池子满了自然就把生产者挡住，等于背压。
//
// 深度实测过 4 / 8 / 16 三档，吞吐基本没差别（564 / 569 / 561 MB/s），
// 所以取 4 就行，再深只是多吃内存。

class BufferPool {
public:
    BufferPool(size_t count, size_t size)
        : bufs_(count, std::vector<char>(size)), free_(count) {
        for (size_t i = 0; i < count; ++i) free_[i] = i;
    }

    // 取一块空闲缓冲；池已关闭时返回 false
    bool acquire(size_t &idx) {
        std::unique_lock<std::mutex> lk(m_);
        cvFree_.wait(lk, [&] { return !free_.empty() || closed_; });
        if (free_.empty()) return false;
        idx = free_.front();
        free_.pop_front();
        return true;
    }

    void release(size_t idx) {
        {
            std::lock_guard<std::mutex> lk(m_);
            free_.push_back(idx);
        }
        cvFree_.notify_one();
    }

    char *data(size_t idx) { return bufs_[idx].data(); }

    // 提交一块已填充的缓冲（idx, 有效长度）
    void pushReady(size_t idx, size_t len) {
        {
            std::lock_guard<std::mutex> lk(m_);
            ready_.emplace_back(idx, len);
        }
        cvReady_.notify_one();
    }

    // 取一块待处理缓冲；返回 false 表示已关闭且无剩余
    bool popReady(size_t &idx, size_t &len) {
        std::unique_lock<std::mutex> lk(m_);
        cvReady_.wait(lk, [&] { return !ready_.empty() || closed_; });
        if (ready_.empty()) return false;
        idx = ready_.front().first;
        len = ready_.front().second;
        ready_.pop_front();
        return true;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lk(m_);
            closed_ = true;
        }
        cvFree_.notify_all();
        cvReady_.notify_all();
    }

private:
    std::vector<std::vector<char>> bufs_;
    std::deque<size_t> free_;
    std::deque<std::pair<size_t, size_t>> ready_;
    std::mutex m_;
    std::condition_variable cvFree_, cvReady_;
    bool closed_ = false;
};

// 线程内异常收集。
//
// 流水线里的读写线程不能往外抛异常（抛了没人接，直接 terminate），
// 所以统一存到这儿，主线程 join 完再检查。关键是「不能静默」——
// 写盘线程失败了主线程还在傻等，最后报一个莫名其妙的超时，排查起来很痛苦。
struct ErrorSlot {
    std::mutex m;
    std::string msg;
    void set(const std::string &s) {
        std::lock_guard<std::mutex> lk(m);
        if (msg.empty()) msg = s;
    }
    bool has() { std::lock_guard<std::mutex> lk(m); return !msg.empty(); }
    std::string get() { std::lock_guard<std::mutex> lk(m); return msg; }
};

// ============================ 主程序 ============================

class HruftPro {
    UDTSOCKET sock_ = UDT::INVALID_SOCK;
    Config cfg_;

    // Socket 参数调优。所有 setsockopt 都检查返回值——
    // 这些调用在错误时机（比如 bind 之后）会静默失败，不检查就等着
    // 参数没生效还以为生效了。接收端原来就是在 accept 之后调这个函数，
    // UDT 那时已经 m_bOpened，11 个 setsockopt 全部无效，日志却还在打印
    // 「Remote config - MSS: 8900」，看着像成功了。
    void tuneSocket(UDTSOCKET s, int mss, int winSize) {
        auto setInt = [&](UDT::SOCKOPT opt, int v, const char *name) {
            if (UDT::setsockopt(s, 0, opt, &v, sizeof(int)) == UDT::ERROR)
                throw std::runtime_error(std::string("setsockopt ") + name + " failed: " + Utils::errText());
        };

        // 1. MSS 必须在 bind/connect 之前
        setInt(UDT_MSS, mss, "UDT_MSS");

        // 2. 拥塞控制（可选）
        //
        // --no-cc 和 --fast-start 二选一，前者完全不控制，后者只跳过爬坡。
        // 两者都必须在 connect/bind 之前设，UDT 会 clone 这个工厂。
        if (cfg_.no_cc) {
            SimpleCCFactory factory;
            if (UDT::setsockopt(s, 0, UDT_CC, &factory, sizeof(SimpleCCFactory)) == UDT::ERROR)
                throw std::runtime_error("setsockopt UDT_CC failed: " + Utils::errText());
            int64_t max_bw = 1000000000;   // 单位：字节/秒 = 8 Gbps
            if (UDT::setsockopt(s, 0, UDT_MAXBW, &max_bw, sizeof(int64_t)) == UDT::ERROR)
                throw std::runtime_error("setsockopt UDT_MAXBW failed: " + Utils::errText());
        } else if (cfg_.fast_start) {
            FastStartCCFactory factory;
            if (UDT::setsockopt(s, 0, UDT_CC, &factory, sizeof(FastStartCCFactory)) == UDT::ERROR)
                throw std::runtime_error("setsockopt UDT_CC failed: " + Utils::errText());
        }

        // 3. UDT_FC 必须一起设，这是最容易漏的一条。
        //
        //    UDT 里 FC 是「飞行窗口」，单位是包；RCVBUF 是字节。设 RCVBUF 的
        //    时候内部会拿它跟 FC 比，超过就截断到 FC。而 FC 默认只有 25600 包，
        //    MSS 1500 时相当于 37MB —— 用户给 --window 128MB 也白给。
        //
        //    实测：只设 RCVBUF 时 window_flow 卡在 25600；两个都设之后
        //    变成 91180，接收缓冲从 38MB 涨到 136MB。
        int pktPayload = std::max(1, mss - 28);
        int fcPackets = static_cast<int>((static_cast<int64_t>(winSize) + pktPayload - 1) / pktPayload);
        fcPackets = std::max(fcPackets, 32);
        if (fcPackets > 4 * 1024 * 1024) fcPackets = 4 * 1024 * 1024;
        setInt(UDT_FC, fcPackets, "UDT_FC");

        // 4. UDT 缓冲区（应用层可见窗口）
        int udtBuf = std::min(winSize, UDT_MAX_BUF);
        setInt(UDT_SNDBUF, udtBuf, "UDT_SNDBUF");
        setInt(UDT_RCVBUF, udtBuf, "UDT_RCVBUF");

        // 5. 内核 UDP 缓冲区
        int udpBuf = std::max(udtBuf, 1 * 1024 * 1024);
        setInt(UDP_SNDBUF, udpBuf, "UDP_SNDBUF");
        setInt(UDP_RCVBUF, udpBuf, "UDP_RCVBUF");

        // 6. 阻塞模式
        bool block = true;
        if (UDT::setsockopt(s, 0, UDT_SNDSYN, &block, sizeof(bool)) == UDT::ERROR)
            throw std::runtime_error("setsockopt UDT_SNDSYN failed: " + Utils::errText());
        if (UDT::setsockopt(s, 0, UDT_RCVSYN, &block, sizeof(bool)) == UDT::ERROR)
            throw std::runtime_error("setsockopt UDT_RCVSYN failed: " + Utils::errText());

        // 7. 地址重用
        bool reuse = true;
        if (UDT::setsockopt(s, 0, UDT_REUSEADDR, &reuse, sizeof(bool)) == UDT::ERROR)
            throw std::runtime_error("setsockopt UDT_REUSEADDR failed: " + Utils::errText());
    }

public:
    explicit HruftPro(Config c) : cfg_(c) {
        if (UDT::startup() == UDT::ERROR)
            throw std::runtime_error("UDT::startup failed");
        blake3_hasher_init(&hasher_);
    }

    ~HruftPro() {
        if (sock_ != UDT::INVALID_SOCK) {
            UDT::close(sock_);
            sock_ = UDT::INVALID_SOCK;
        }
        UDT::cleanup();
    }

    // ------------------------- 发送端 -------------------------

    void runSender() {
        fs::path filePath = toPath(cfg_.path);

        std::error_code ec;
        if (!fs::exists(filePath, ec) || ec)
            throw std::runtime_error("File not found: " + cfg_.path);
        if (!fs::is_regular_file(filePath, ec) || ec)
            throw std::runtime_error("Not a regular file: " + cfg_.path);

        uint64_t fsize = fs::file_size(filePath, ec);
        if (ec) throw std::runtime_error("Cannot get file size: " + cfg_.path);

        std::string fname = pathToUtf8(filePath.filename());
        if (fname.empty()) throw std::runtime_error("Empty filename");
        if (fname.size() > 65535) throw std::runtime_error("Filename too long");

        // 先打开文件，确认可读，再拿一次大小（防止 TOCTOU）
        std::ifstream ifs(filePath, std::ios::binary);
        if (!ifs) throw std::runtime_error("Cannot open file for reading: " + cfg_.path);
        uint64_t fsize2 = fs::file_size(filePath, ec);
        if (ec || fsize2 != fsize)
            throw std::runtime_error("File changed while opening: " + cfg_.path);

        sock_ = UDT::socket(AF_INET, SOCK_STREAM, 0);
        if (sock_ == UDT::INVALID_SOCK)
            throw std::runtime_error("Failed to create socket: " + Utils::errText());

        tuneSocket(sock_, cfg_.mss, cfg_.window);

        sockaddr_in serv_addr;
        std::memset(&serv_addr, 0, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(static_cast<uint16_t>(cfg_.port));
        if (inet_pton(AF_INET, cfg_.ip.c_str(), &serv_addr.sin_addr) <= 0)
            throw std::runtime_error("Invalid IP address: " + cfg_.ip);

        std::cout << "[INFO] Connecting to " << cfg_.ip << ":" << cfg_.port << "..." << std::endl;
        if (UDT::connect(sock_, reinterpret_cast<sockaddr *>(&serv_addr), sizeof(serv_addr)) == UDT::ERROR)
            throw std::runtime_error("Connect failed: " + Utils::errText());

        std::cout << "[INFO] Connected successfully" << std::endl;

        // --- 协议头帧 ---
        ProtocolHeader hdr;
        hdr.magic = htonl(MAGIC_ID);
        hdr.version = htons(PROTO_VERSION);
        hdr.flags = htons(static_cast<uint16_t>((cfg_.no_cc ? 1 : 0) | (cfg_.fast_start ? 2 : 0)));
        hdr.file_size = htonll(fsize);
        hdr.filename_len = htonl(static_cast<uint32_t>(fname.size()));
        hdr.reserved = 0;

        std::string hdrPayload(reinterpret_cast<const char *>(&hdr), sizeof(hdr));
        hdrPayload += fname;
        sendFrame(sock_, FRAME_HEADER, hdrPayload);

        std::cout << "[INFO] Sending " << fname << " (" << Utils::formatSize(fsize) << ")..." << std::endl;

        auto t_start = std::chrono::high_resolution_clock::now();
        auto last_progress = t_start;

        // --- 流水线：读盘线程 → 主线程发送 ---
        BufferPool pool(PIPELINE_DEPTH, APP_BLOCK_SIZE);
        ErrorSlot err;

        std::thread reader([&] {
            try {
                uint64_t remaining = fsize;
                while (remaining > 0) {
                    size_t idx = 0;
                    if (!pool.acquire(idx)) break;
                    size_t want = static_cast<size_t>(std::min<uint64_t>(APP_BLOCK_SIZE, remaining));
                    ifs.read(pool.data(idx), static_cast<std::streamsize>(want));
                    size_t got = static_cast<size_t>(ifs.gcount());
                    if (got != want)
                        throw std::runtime_error("Read error: expected " +
                                                 std::to_string(want) + ", got " + std::to_string(got));
                    blake3_hasher_update(&hasher_, pool.data(idx), got);
                    remaining -= got;
                    pool.pushReady(idx, got);
                }
            } catch (const std::exception &e) {
                err.set(e.what());
            }
            pool.close();
        });

        uint64_t sent = 0;
        size_t idx = 0, len = 0;
        try {
            while (pool.popReady(idx, len)) {
                writeFully(sock_, pool.data(idx), len);
                pool.release(idx);
                sent += len;

                auto now = std::chrono::high_resolution_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_progress).count();
                if (cfg_.detailed || elapsed >= 1000) {
                    double progress = fsize > 0 ? (static_cast<double>(sent) / static_cast<double>(fsize) * 100.0) : 100.0;
                    std::cout << "\r[Progress] " << std::fixed << std::setprecision(1) << progress
                              << "% | " << Utils::formatSize(sent) << " / " << Utils::formatSize(fsize);
                    if (cfg_.detailed) {
                        UDT::TRACEINFO tmp;
                        // clear=true（默认）让 m_LastSampleTime 前移，mbpsSendRate 才是「本秒速率」；
                        // 累计量 pktSentTotal 等不受 clear 影响，所以最终报告仍然正确。
                        if (UDT::perfmon(sock_, &tmp, true) == 0)
                            std::cout << " | Rate: " << std::fixed << std::setprecision(1) << tmp.mbpsSendRate << " Mbps";
                    }
                    std::cout << std::flush;
                    last_progress = now;
                }
            }
        } catch (...) {
            pool.close();
            if (reader.joinable()) reader.join();
            throw;
        }
        if (reader.joinable()) reader.join();
        if (err.has()) throw std::runtime_error(err.get());
        if (sent != fsize)
            throw std::runtime_error("Sent " + std::to_string(sent) + " of " + std::to_string(fsize) + " bytes");

        ifs.close();

        // --- 哈希帧 + 完成帧 ---
        uint8_t hash[BLAKE3_OUT_LEN];
        blake3_hasher_finalize(&hasher_, hash, BLAKE3_OUT_LEN);
        sendFrame(sock_, FRAME_HASH, std::string(reinterpret_cast<char *>(hash), BLAKE3_OUT_LEN));
        sendFrame(sock_, FRAME_DONE, std::string());

        auto t_end = std::chrono::high_resolution_clock::now();
        double dur = std::chrono::duration<double>(t_end - t_start).count();

        std::cout << "\n[INFO] Transfer completed in " << std::fixed << std::setprecision(2)
                  << dur << " seconds" << std::endl;
        std::cout << "[INFO] Waiting for receiver confirmation..." << std::endl;

        // --- 等 ACK ---
        // 这里必须带超时。原来是不带超时的阻塞 recv，对端一崩发送端就
        // 永远挂在那儿，Ctrl-C 才能退。
        setRecvTimeout(sock_, 30000);

        uint8_t type = 0;
        std::string payload;
        bool gotAck = false;
        while (recvFrame(sock_, type, payload)) {
            if (type == FRAME_ACK && !payload.empty()) {
                gotAck = true;
                uint8_t st = static_cast<uint8_t>(payload[0]);
                if (st == ACK_OK)
                    std::cout << "[INFO] Receiver acknowledged transfer (integrity OK)" << std::endl;
                else if (st == ACK_HASH_FAIL)
                    std::cout << "[ERROR] Receiver reported hash mismatch" << std::endl;
                else
                    std::cout << "[WARNING] Receiver reported abort" << std::endl;
            } else if (type == FRAME_REPORT) {
                try {
                    std::cout << "\n=== Receiver Report ===\n" << json::parse(payload).dump(4) << std::endl;
                } catch (const json::exception &) {
                    std::cout << "[INFO] Raw report: " << payload << std::endl;
                }
                break;
            }
        }
        if (!gotAck) std::cout << "[WARNING] No ACK received from receiver" << std::endl;

        UDT::close(sock_);
        sock_ = UDT::INVALID_SOCK;
    }

    // ------------------------- 接收端 -------------------------

    void runReceiver() {
        UDTSOCKET serv = UDT::socket(AF_INET, SOCK_STREAM, 0);
        if (serv == UDT::INVALID_SOCK)
            throw std::runtime_error("Failed to create server socket: " + Utils::errText());

        // 注意顺序：MSS 和窗口必须在 bind 之前设好。
        //
        // UDT 的 bind 会调 open()，之后 m_bOpened 就是 true，再调
        // UDT_MSS / UDT_SNDBUF / UDT_RCVBUF / UDP_SNDBUF / UDP_RCVBUF
        // 全部抛异常。我原来的写法是 accept 之后才 tuneSocket，
        // 那时早就 open 过了，参数一个都没生效，还照样打印成功日志。
        // 发送端参数靠 UDT 握手协商过去，接收端窗口只能靠本地设，
        // 所以这里必须在 bind 前把本地配置设对。
        tuneSocket(serv, cfg_.mss, cfg_.window);

        sockaddr_in my_addr;
        std::memset(&my_addr, 0, sizeof(my_addr));
        my_addr.sin_family = AF_INET;
        my_addr.sin_port = htons(static_cast<uint16_t>(cfg_.port));
        my_addr.sin_addr.s_addr = INADDR_ANY;

        if (UDT::bind(serv, reinterpret_cast<sockaddr *>(&my_addr), sizeof(my_addr)) == UDT::ERROR) {
            std::string e = Utils::errText();
            UDT::close(serv);
            throw std::runtime_error("Bind failed: " + e);
        }
        if (UDT::listen(serv, 10) == UDT::ERROR) {
            std::string e = Utils::errText();
            UDT::close(serv);
            throw std::runtime_error("Listen failed: " + e);
        }
        std::cout << "[INFO] Listening on port " << cfg_.port << "..." << std::endl;

        sockaddr_in client_addr;
        int addrlen = sizeof(client_addr);
        sock_ = UDT::accept(serv, reinterpret_cast<sockaddr *>(&client_addr), &addrlen);
        if (sock_ == UDT::ERROR || sock_ == UDT::INVALID_SOCK) {
            std::string e = Utils::errText();
            UDT::close(serv);
            throw std::runtime_error("Accept failed: " + e);
        }
        std::cout << "[INFO] Connection accepted" << std::endl;

        // --- 协议头帧 ---
        uint8_t type = 0;
        std::string payload;
        if (!recvFrame(sock_, type, payload))
            throw std::runtime_error("Connection closed before protocol header");
        if (type != FRAME_HEADER)
            throw std::runtime_error("Expected protocol header frame, got type " + std::to_string(type));
        if (payload.size() < sizeof(ProtocolHeader))
            throw std::runtime_error("Protocol header too short");

        ProtocolHeader hdr;
        std::memcpy(&hdr, payload.data(), sizeof(hdr));
        if (ntohl(hdr.magic) != MAGIC_ID)
            throw std::runtime_error("Invalid protocol magic (peer is not HRUFT v4)");
        if (ntohs(hdr.version) != PROTO_VERSION)
            throw std::runtime_error("Unsupported protocol version: " + std::to_string(ntohs(hdr.version)));

        uint64_t rSize = ntohll(hdr.file_size);
        uint32_t nameLen = ntohl(hdr.filename_len);
        if (nameLen == 0 || nameLen > 65535)
            throw std::runtime_error("Invalid filename length: " + std::to_string(nameLen));
        if (payload.size() != sizeof(ProtocolHeader) + nameLen)
            throw std::runtime_error("Protocol header size mismatch");

        std::string rawName = payload.substr(sizeof(ProtocolHeader));

        // 文件名是网络对面给的，必须当成不可信数据处理。
        //
        // 先查 NUL：std::string 能存 NUL，但转 fs::path 时会被当成结尾，
        // 对端发 "a\0b.bin"，本地落成 "a"。不报错、不告警，就是名字变了。
        // 再查 UTF-8：非法编码在 Windows 上会变成替换字符，用户看到的
        // 文件名和实际对不上。
        // 这两条都是健壮性测试抓出来的，以前完全没意识到。
        if (rawName.find('\0') != std::string::npos)
            throw std::runtime_error("Filename contains NUL byte");
        if (!isValidUtf8(rawName))
            throw std::runtime_error("Filename is not valid UTF-8");

        if (cfg_.max_size > 0 && rSize > cfg_.max_size)
            throw std::runtime_error("Incoming file (" + Utils::formatSize(rSize) +
                                     ") exceeds --max-size limit (" + Utils::formatSize(cfg_.max_size) + ")");

        // 路径穿越防护：只留纯文件名，路径部分一律丢掉。
        //
        // 不做这一步的话，对端发一个 "../../.bashrc" 或者 "/etc/passwd"
        // 就能写到任意位置去 —— fs::path 的 operator/= 遇到绝对路径会
        // 直接替换掉整个前缀，实测确实是这样。
        // 现在 "../../evil.sh" 会降级成 "evil.sh"，落在预期目录里；
        // ".." 和 "." 这种没法降级的直接拒收。
        fs::path namePath = toPath(rawName);
        std::string safeName = pathToUtf8(namePath.filename());
        if (safeName.empty() || safeName == "." || safeName == ".." ||
            safeName.find('/') != std::string::npos || safeName.find('\\') != std::string::npos)
            throw std::runtime_error("Unsafe filename from peer: " + rawName);
        if (rawName != safeName)
            std::cout << "[WARNING] Peer sent path-like filename, reduced to: " << safeName << std::endl;

        std::cout << "[INFO] Receiving file: " << safeName << " ("
                  << Utils::formatSize(rSize) << ")" << std::endl;

        fs::path baseDir = toPath(cfg_.path);
        fs::path outPath = baseDir;
        std::error_code ec;
        if (fs::is_directory(baseDir, ec)) {
            outPath = baseDir / toPath(safeName);
        } else {
            // savepath 是不存在的目录（以分隔符结尾）时也当目录处理
            if (!fs::exists(baseDir, ec) && !cfg_.path.empty() &&
                (cfg_.path.back() == '/' || cfg_.path.back() == '\\'))
                outPath = baseDir / toPath(safeName);
        }

        // 创建父目录。空 parent 一定要跳过 ——
        // 用户写 "hruft recv 9000 out.bin" 时 parent_path() 是空串，
        // create_directories("") 在 libstdc++ 下直接抛
        // "filesystem error: cannot create directories: Invalid argument"。
        // 这个 bug 从 v3 一直带到现在，README 里还教人这么用。
        fs::path parent = outPath.parent_path();
        if (!parent.empty()) {
            fs::create_directories(parent, ec);
            if (ec) throw std::runtime_error("Cannot create directory " + pathToUtf8(parent) + ": " + ec.message());
        }

        // 同名冲突加序号。
        // 原来用的是秒级时间戳后缀，同秒传两次就重名，第二次直接把第一次覆盖掉。
        if (fs::exists(outPath, ec)) {
            fs::path dir = outPath.parent_path();
            std::string stem = pathToUtf8(outPath.stem());
            std::string ext = pathToUtf8(outPath.extension());
            for (int i = 1; i < 10000; ++i) {
                fs::path cand = dir / toPath(stem + " (" + std::to_string(i) + ")" + ext);
                if (!fs::exists(cand, ec)) { outPath = cand; break; }
            }
        }

        // 先写 .part，校验通过再改名。
        // guard 的作用域要覆盖到整个接收流程结束，中途任何异常退出都会清掉。
        fs::path partPath = outPath;
        partPath += ".part";
        TempFileGuard partGuard(partPath);

        std::ofstream ofs(partPath, std::ios::binary | std::ios::trunc);
        if (!ofs) throw std::runtime_error("Cannot open file for writing: " + pathToUtf8(partPath));

        std::cout << "[INFO] Saving to: " << pathToUtf8(outPath.filename()) << std::endl;

        auto t_start = std::chrono::high_resolution_clock::now();
        auto last_progress = t_start;

        // --- 流水线：主线程接收 → 写盘线程写文件并算哈希 ---
        BufferPool pool(PIPELINE_DEPTH, APP_BLOCK_SIZE);
        ErrorSlot err;

        std::thread writer([&] {
            try {
                size_t idx = 0, len = 0;
                while (pool.popReady(idx, len)) {
                    ofs.write(pool.data(idx), static_cast<std::streamsize>(len));
                    if (!ofs) throw std::runtime_error("Failed to write to file");
                    blake3_hasher_update(&hasher_, pool.data(idx), len);
                    pool.release(idx);
                }
            } catch (const std::exception &e) {
                err.set(e.what());
            }
            pool.close();
        });

        uint64_t received = 0;
        try {
            while (received < rSize) {
                size_t idx = 0;
                if (!pool.acquire(idx)) throw std::runtime_error("Buffer pool closed unexpectedly");
                size_t want = static_cast<size_t>(std::min<uint64_t>(APP_BLOCK_SIZE, rSize - received));
                if (!readFully(sock_, pool.data(idx), want)) {
                    pool.release(idx);
                    throw std::runtime_error("Connection closed at " + std::to_string(received) +
                                             " of " + std::to_string(rSize) + " bytes");
                }
                pool.pushReady(idx, want);
                received += want;

                auto now = std::chrono::high_resolution_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_progress).count();
                if (cfg_.detailed || elapsed >= 1000) {
                    double progress = rSize > 0 ? (static_cast<double>(received) / static_cast<double>(rSize) * 100.0) : 100.0;
                    std::cout << "\r[Progress] " << std::fixed << std::setprecision(1) << progress
                              << "% | " << Utils::formatSize(received) << " / " << Utils::formatSize(rSize);
                    if (cfg_.detailed) {
                        UDT::TRACEINFO tmp;
                        if (UDT::perfmon(sock_, &tmp, true) == 0)
                            std::cout << " | Rate: " << std::fixed << std::setprecision(1) << tmp.mbpsRecvRate << " Mbps";
                    }
                    std::cout << std::flush;
                    last_progress = now;
                }
            }
        } catch (...) {
            pool.close();
            if (writer.joinable()) writer.join();
            throw;
        }
        pool.close();
        if (writer.joinable()) writer.join();
        if (err.has()) throw std::runtime_error(err.get());
        ofs.close();

        if (received != rSize)
            throw std::runtime_error("File transfer incomplete. Expected: " +
                                     Utils::formatSize(rSize) + ", Received: " + Utils::formatSize(received));

        std::cout << "\n[INFO] File received, verifying integrity..." << std::endl;

        // --- 哈希帧 ---
        if (!recvFrame(sock_, type, payload))
            throw std::runtime_error("Connection closed before hash");
        if (type != FRAME_HASH || payload.size() != BLAKE3_OUT_LEN)
            throw std::runtime_error("Invalid hash frame");
        uint8_t rHash[BLAKE3_OUT_LEN];
        std::memcpy(rHash, payload.data(), BLAKE3_OUT_LEN);

        // --- 完成帧 ---
        if (!recvFrame(sock_, type, payload))
            throw std::runtime_error("Connection closed before completion marker");
        if (type != FRAME_DONE)
            throw std::runtime_error("Invalid completion marker");

        auto t_end = std::chrono::high_resolution_clock::now();
        double duration = std::chrono::duration<double>(t_end - t_start).count();

        uint8_t lHash[BLAKE3_OUT_LEN];
        blake3_hasher_finalize(&hasher_, lHash, BLAKE3_OUT_LEN);

        std::string localHash = Utils::hashToString(lHash, BLAKE3_OUT_LEN);
        std::string remoteHash = Utils::hashToString(rHash, BLAKE3_OUT_LEN);
        bool match = (localHash == remoteHash);

        std::cout << "[INFO] Hash verification: " << (match ? "PASS" : "FAIL") << std::endl;

        if (match) {
            // rename 成功后必须 release，不然 guard 析构时会把刚写好的正式文件删了
            fs::rename(partPath, outPath, ec);
            if (ec)
                throw std::runtime_error("Cannot finalize output file: " + pathToUtf8(outPath) +
                                         ": " + ec.message());
            partGuard.release();
        } else {
            // 这里不用手动删，guard 析构时会清掉 .part
            std::cout << "[ERROR] Local hash:  " << localHash << std::endl;
            std::cout << "[ERROR] Remote hash: " << remoteHash << std::endl;
            std::cout << "[ERROR] Corrupted file removed" << std::endl;
        }

        // --- 发 ACK ---
        sendFrame(sock_, FRAME_ACK, std::string(1, static_cast<char>(match ? ACK_OK : ACK_HASH_FAIL)));

        // --- 生成并发送报告 ---
        UDT::TRACEINFO perf;
        std::memset(&perf, 0, sizeof(perf));
        UDT::perfmon(sock_, &perf, true);

        json jStats = NetworkStats::snapshot(perf, duration, rSize);
        json jFinal = NetworkStats::analyze(jStats, cfg_.mss, cfg_.window);

        jFinal["meta"] = json::object({
            {"filename", pathToUtf8(outPath.filename())},
            {"filepath", pathToUtf8(outPath)},
            {"filesize", rSize},
            {"filesize_human", Utils::formatSize(rSize)},
            {"status", match ? "success" : "integrity_failure"},
            {"hash_match", match},
            {"local_hash", localHash},
            {"remote_hash", remoteHash},
            {"duration_sec", duration},
            {"avg_speed_mbps", duration > 0 ? (rSize * 8.0 / 1000000.0) / duration : 0.0},
            {"avg_speed_mbs", duration > 0 ? (rSize / (1024.0 * 1024.0)) / duration : 0.0},
            {"congestion_control_disabled", cfg_.no_cc},
            {"fast_start", cfg_.fast_start},
            {"protocol_version", PROTO_VERSION}
        });

        std::string jsonStr = jFinal.dump();
        try {
            sendFrame(sock_, FRAME_REPORT, jsonStr);
        } catch (const std::exception &e) {
            std::cout << "[WARNING] Failed to send report: " << e.what() << std::endl;
        }

        std::cout << "\n=== Transfer Summary ===\n" << jFinal.dump(4) << std::endl;

        UDT::close(sock_);
        sock_ = UDT::INVALID_SOCK;
        UDT::close(serv);
    }

private:
    blake3_hasher hasher_{};
};

// ============================ 入口 ============================

int main(int argc, char *argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    int wargc = 0;
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (!wargv) {
        std::cerr << "[ERROR] CommandLineToArgvW failed" << std::endl;
        return 1;
    }
    std::vector<std::string> utf8Args;
    utf8Args.reserve(wargc);
    for (int i = 0; i < wargc; ++i) utf8Args.push_back(Utf8Util::toUtf8(wargv[i]));
    LocalFree(wargv);

    std::vector<char *> newArgv;
    newArgv.reserve(utf8Args.size());
    for (auto &s : utf8Args) newArgv.push_back(s.data());
    argc = static_cast<int>(newArgv.size());
    argv = newArgv.data();
#endif

    try {
        Config cfg = Config::parse(argc, argv);
        HruftPro app(cfg);
        if (cfg.mode == "send") app.runSender();
        else if (cfg.mode == "recv") app.runReceiver();
    } catch (const std::exception &e) {
        std::cerr << "[ERROR] " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "[ERROR] Unknown fatal error" << std::endl;
        return 1;
    }
    return 0;
}
