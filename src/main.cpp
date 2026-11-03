/**
 * HRUFT Pro v4.0.0 (High Reliability UDT File Transfer - Professional)
 *
 * Dependencies:
 *   1. UDT4  (libudt)  - https://github.com/JinBiLianShao/udt4
 *   2. BLAKE3 (libblake3)
 *   3. nlohmann/json
 *
 * v4.0.0 破坏性变更：控制消息全部改为带长度前缀的定长帧，与 v3.x 不互通。
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
#include "json.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

// ============================ 常量 ============================

static const uint32_t MAGIC_ID = 0x48525034;   // "HRP4"
static const uint16_t PROTO_VERSION = 4;

static const int APP_BLOCK_SIZE = 4 * 1024 * 1024;    // 4MB 应用层分块
static const int UDT_MAX_BUF   = 256 * 1024 * 1024;   // 256MB 上限
static const int DEFAULT_MSS   = 1500;
static const int DEFAULT_WINDOW = 10 * 1024 * 1024;   // 10MB
static const int MIN_MSS = 536;
static const int MAX_MSS = 8900;
static const int MIN_WINDOW = 64 * 1024;
static const int PIPELINE_DEPTH = 4;                  // 流水线缓冲块数

// 帧类型
enum FrameType : uint8_t {
    FRAME_HEADER = 1,   // 协议头 + 文件名
    FRAME_HASH   = 2,   // 32 字节 BLAKE3
    FRAME_DONE   = 3,   // 数据发送完毕
    FRAME_ACK    = 4,   // 1 字节状态
    FRAME_REPORT = 5,   // JSON 报告
};

enum AckStatus : uint8_t { ACK_OK = 0, ACK_HASH_FAIL = 1, ACK_ABORT = 2 };

// ============================ 协议头 ============================

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

// 路径与字符串互转：Windows 下统一走 UTF-8 <-> UTF-16
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

class SimpleCCFactory : public CCCVirtualFactory {
public:
    CCC *create() override { return new SimpleCC; }
    CCCVirtualFactory *clone() override { return new SimpleCCFactory; }
};

// ============================ 网络分析 ============================

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
        j["reliability"] = json::object({
            {"pkt_sent", perf.pktSentTotal},
            {"pkt_recv", perf.pktRecvTotal},
            {"pkt_loss_sent", perf.pktSndLossTotal},
            {"pkt_loss_recv", perf.pktRcvLossTotal},
            {"retrans_total", perf.pktRetransTotal},
            {"retrans_ratio", perf.pktSentTotal > 0
                 ? static_cast<double>(perf.pktRetransTotal) / static_cast<double>(perf.pktSentTotal)
                 : 0.0}
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
            lossRate  = stats.at("reliability").at("retrans_ratio").get<double>();
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
            advice.push_back("网络质量: 高重传率 (" + std::to_string(lossRate * 100) + "%)。检查网络稳定性。");
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
    int mss = DEFAULT_MSS;
    int window = DEFAULT_WINDOW;
    bool detailed = false;
    bool no_cc = false;
    uint64_t max_size = 0;   // 0 = 不限

    static void printUsage() {
        std::cerr << "HRUFT Pro " << "4.0.0" << "\n"
                  << "Usage:\n"
                  << "  hruft send <ip> <port> <filepath> [options]\n"
                  << "  hruft recv <port> <savepath> [options]\n\n"
                  << "Options:\n"
                  << "  --mss <value>      Maximum Segment Size, 536-8900 (default: 1500)\n"
                  << "  --window <value>   Window size in bytes (default: 10MB, max: 256MB)\n"
                  << "  --max-size <value> Receiver: reject files larger than this many bytes (default: unlimited)\n"
                  << "  --detailed         Show detailed statistics\n"
                  << "  --no-cc            Disable congestion control (direct links only)\n";
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
            } else {
                throw std::runtime_error("Unknown option: " + arg);
            }
        }
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

// ---- 完整读写（UDT 是字节流，绝不假设一次 recv/send 恰好一条消息）----

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

// 返回 false 表示对端在读取任何字节前干净关闭
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

// 帧格式: [u32 长度(含类型字节, 大端)][u8 类型][payload]
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

// ============================ 流水线缓冲池 ============================

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

// 线程内异常收集：任何一线程失败都要让主流程失败，而不是静默继续
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

    void tuneSocket(UDTSOCKET s, int mss, int winSize) {
        auto setInt = [&](UDT::SOCKOPT opt, int v, const char *name) {
            if (UDT::setsockopt(s, 0, opt, &v, sizeof(int)) == UDT::ERROR)
                throw std::runtime_error(std::string("setsockopt ") + name + " failed: " + Utils::errText());
        };

        // 1. MSS 必须在 bind/connect 之前
        setInt(UDT_MSS, mss, "UDT_MSS");

        // 2. 关闭拥塞控制（可选）
        if (cfg_.no_cc) {
            SimpleCCFactory factory;
            if (UDT::setsockopt(s, 0, UDT_CC, &factory, sizeof(SimpleCCFactory)) == UDT::ERROR)
                throw std::runtime_error("setsockopt UDT_CC failed: " + Utils::errText());
            int64_t max_bw = 1000000000;   // 单位：字节/秒 = 8 Gbps
            if (UDT::setsockopt(s, 0, UDT_MAXBW, &max_bw, sizeof(int64_t)) == UDT::ERROR)
                throw std::runtime_error("setsockopt UDT_MAXBW failed: " + Utils::errText());
        }

        // 3. 关键：UDT_FC 单位是「包」。不放开 FC，RCVBUF 会被 25600 包截断，
        //    MSS 1500 时接收窗口实际上限约 37MB，--window 再大也无效。
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
        hdr.flags = htons(cfg_.no_cc ? 1 : 0);
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

        // --- 等 ACK（带超时，绝不无限阻塞）---
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

        // 窗口/MSS 必须在 bind 之前设置：bind 之后 UDT 会置 m_bOpened，
        // 此后 UDT_MSS/UDT_SNDBUF/UDT_RCVBUF/UDP_* 一律被拒。
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
        if (cfg_.max_size > 0 && rSize > cfg_.max_size)
            throw std::runtime_error("Incoming file (" + Utils::formatSize(rSize) +
                                     ") exceeds --max-size limit (" + Utils::formatSize(cfg_.max_size) + ")");

        // 路径穿越防护：只保留纯文件名
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

        // 创建父目录（空 parent 必须跳过，否则抛 filesystem error）
        fs::path parent = outPath.parent_path();
        if (!parent.empty()) {
            fs::create_directories(parent, ec);
            if (ec) throw std::runtime_error("Cannot create directory " + pathToUtf8(parent) + ": " + ec.message());
        }

        // 同名冲突：循环加序号，绝不复用可能被覆盖的名字
        if (fs::exists(outPath, ec)) {
            fs::path dir = outPath.parent_path();
            std::string stem = pathToUtf8(outPath.stem());
            std::string ext = pathToUtf8(outPath.extension());
            for (int i = 1; i < 10000; ++i) {
                fs::path cand = dir / toPath(stem + " (" + std::to_string(i) + ")" + ext);
                if (!fs::exists(cand, ec)) { outPath = cand; break; }
            }
        }

        // 先写 .part，校验通过后再改名，避免留下损坏文件
        fs::path partPath = outPath;
        partPath += ".part";

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
            // 校验通过才落地
            fs::rename(partPath, outPath, ec);
            if (ec) {
                fs::remove(partPath, ec);
                throw std::runtime_error("Cannot finalize output file: " + pathToUtf8(outPath));
            }
        } else {
            fs::remove(partPath, ec);
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
