/**
* HRUFT Pro (High Reliability UDT File Transfer - Professional)
 * * Dependencies:
 * 1. UDT4 Library (libudt)
 * 2. BLAKE3 Library (libblake3)
 * 3. nlohmann/json (json.hpp)
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
#include <atomic>
#include <condition_variable>
#include <mutex>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>   // ====== 中文支持新增 ======
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#endif

#include <udt.h>
#include "blake3.h"
#include "json.hpp"
#include "ccc.h"

using json = nlohmann::json;
namespace fs = std::filesystem;

#ifdef _WIN32
// ====== 中文支持新增：UTF-8 <-> UTF-16 转换 ======
class Utf8Util {
public:
    static std::wstring toWide(const std::string &s) {
        if (s.empty()) return L"";
        int sz = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w(sz, 0);
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], sz);
        w.pop_back();
        return w;
    }

    static std::string toUtf8(const std::wstring &w) {
        if (w.empty()) return "";
        int sz = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string s(sz, 0);
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], sz, nullptr, nullptr);
        s.pop_back();
        return s;
    }
};
#endif


// --- 64位字节序转换宏 ---
#ifdef _WIN32
#ifdef _MSC_VER
#include <stdlib.h>
#define htonll(x) _byteswap_uint64(x)
#define ntohll(x) _byteswap_uint64(x)
#else
// MinGW
#define htonll(x) (((uint64_t)htonl((x) & 0xFFFFFFFF) << 32) | htonl((x) >> 32))
#define ntohll(x) (((uint64_t)ntohl((x) & 0xFFFFFFFF) << 32) | ntohl((x) >> 32))
#endif
#else
#include <endian.h>
#define htonll htobe64
#define ntohll be64toh
#endif

// --- 高性能配置常量 ---
const uint32_t MAGIC_ID = 0x48525032; // "HRP2" in ASCII
const int APP_BLOCK_SIZE = 4 * 1024 * 1024; // 4MB 应用层分块
const int UDT_MAX_BUF = 256 * 1024 * 1024; // 256MB 最大缓冲
const std::string TRANSFER_COMPLETE = "TRANSFER_COMPLETE";
const std::string ACK_TRANSFER = "ACK_TRANSFER";
const int DEFAULT_MSS = 1500;
const int DEFAULT_WINDOW = 10 * 1024 * 1024; // 10MB default

// --- 协议头 ---
#pragma pack(push, 1)
struct ProtocolHeader {
    uint32_t magic;
    uint32_t mss;
    uint32_t window_size;
    uint64_t file_size;
    uint16_t filename_len;
    // 紧接着是 filename_len 长度的文件名（无终止符）
};
#pragma pack(pop)

// --- 简单的拥塞控制类（用于关闭拥塞控制） ---
class SimpleCC : public CCC {
public:
    SimpleCC() {
        // 设置固定的发送周期和窗口大小，避免拥塞控制调整
        m_dPktSndPeriod = 1.0; // 最小延迟
        m_dCWndSize = 1000000.0; // 极大的窗口
    }

    virtual void init() override {
        // 保持初始值不变，不进行任何调整
    }

    virtual void onACK(int32_t) override {
        // 不调整发送速率
    }

    virtual void onLoss(const int32_t*, int) override {
        // 忽略丢包，不调整速率
    }

    virtual void onTimeout() override {
        // 忽略超时
    }

    virtual void onPktSent(const CPacket*) override {
        // 无操作
    }

    virtual void onPktReceived(const CPacket*) override {
        // 无操作
    }

    virtual void processCustomMsg(const CPacket*) override {
        // 无操作
    }

    virtual void close() override {
        // 无操作
    }
};

// 简单的拥塞控制工厂类
class SimpleCCFactory : public CCCVirtualFactory {
public:
    virtual ~SimpleCCFactory() {}

    virtual CCC* create() override {
        return new SimpleCC;
    }

    virtual CCCVirtualFactory* clone() override {
        return new SimpleCCFactory;
    }
};

// --- 网络分析工具类 ---
class NetworkStats {
public:
    // 将 UDT TRACEINFO 转换为详细 JSON
    static json snapshot(UDT::TRACEINFO &perf, double duration, uint64_t totalBytes) {
        json j;

        // 1. 速率统计
        double avgSpeedMbps = duration > 0 ? (totalBytes * 8.0 / 1000000.0) / duration : 0.0;
        j["throughput"] = json::object({
            {"avg_mbps", avgSpeedMbps},
            {"inst_mbps", perf.mbpsRecvRate},
            {"est_bandwidth_mbps", perf.mbpsBandwidth}
        });

        // 2. 延迟与抖动
        j["latency"] = json::object({
            {"rtt_ms", perf.msRTT}
        });

        // 3. 丢包与可靠性
        j["reliability"] = json::object({
            {"pkt_sent", perf.pktSentTotal},
            {"pkt_recv", perf.pktRecvTotal},
            {"pkt_loss_sent", perf.pktSndLossTotal},
            {"pkt_loss_recv", perf.pktRcvLossTotal},
            {"retrans_total", perf.pktRetransTotal},
            {
                "retrans_ratio",
                (perf.pktSentTotal > 0 ? static_cast<double>(perf.pktRetransTotal) / perf.pktSentTotal : 0.0)
            }
        });

        // 4. 拥塞控制内部状态
        j["congestion"] = json::object({
            {"window_flow", perf.pktFlowWindow},
            {"window_cong", perf.pktCongestionWindow},
            {"flight_size", perf.pktFlightSize}
        });

        // 5. 缓冲区健康度
        j["buffer_health"] = json::object({
            {"snd_buf_avail_bytes", perf.byteAvailSndBuf},
            {"rcv_buf_avail_bytes", perf.byteAvailRcvBuf}
        });

        return j;
    }

    // 智能分析引擎
    static json analyze(const json &stats, int configMss, int configWin) {
        json report = stats;
        std::vector<std::string> advice;
        std::string health = "excellent";

        double lossRate = 0.0;
        double bandwidth = 0.0;
        double rtt = 0.0;
        int availRcvBuf = 0;

        try {
            lossRate = stats["reliability"]["retrans_ratio"];
            bandwidth = stats["throughput"]["est_bandwidth_mbps"];
            rtt = stats["latency"]["rtt_ms"];
            availRcvBuf = stats["buffer_health"]["rcv_buf_avail_bytes"];
        } catch (const json::exception &e) {
            health = "unknown";
            advice.push_back("无法获取完整统计信息");
        }

        // BDP 计算: Bandwidth (bits/sec) * RTT (sec) / 8 = Bytes
        long long bdp = (long long) ((bandwidth * 1e6 * rtt * 1e-3) / 8.0);

        // --- 规则引擎 ---

        // 1. 缓冲区检查
        if (configWin < bdp && bdp > 0) {
            advice.push_back("配置警告: 窗口大小 (" + std::to_string(configWin) +
                             ") 小于带宽时延积 (" + std::to_string(bdp) + ")。吞吐量受物理限制。");
            health = "suboptimal";
        }

        // 2. 瓶颈识别
        if (availRcvBuf < (configWin * 0.1) && configWin > 0) {
            advice.push_back("可能瓶颈: 接收缓冲区快满了。考虑增大窗口大小或检查磁盘IO。");
        }

        // 3. 链路质量
        if (lossRate > 0.01) {
            // > 1% 重传
            advice.push_back("网络质量: 高重传率 (" +
                             std::to_string(lossRate * 100) + "%)。检查网络稳定性。");
            health = "network_lossy";
            if (configMss > 1400) {
                advice.push_back("优化建议: 尝试减少MSS到1400以避免IP分片。");
            }
        } else if (lossRate < 0.001 && configMss < 8900 && rtt < 20) {
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

// --- 配置参数 ---
struct Config {
    std::string mode;
    std::string ip;
    int port;
    std::string path;
    int mss = DEFAULT_MSS;
    int window = DEFAULT_WINDOW;
    bool detailed = false;
    bool no_cc = false;  // 新增：是否关闭拥塞控制

    static Config parse(int argc, char *argv[]) {
        Config c;
        if (argc < 2) {
            printUsage();
            throw std::runtime_error("No command provided");
        }

        c.mode = argv[1];

        int idx = 2;
        if (c.mode == "send") {
            if (argc < 5) {
                printUsage();
                throw std::runtime_error("Invalid send arguments");
            }
            c.ip = argv[idx++];
            c.port = std::stoi(argv[idx++]);
            c.path = argv[idx++];
        } else if (c.mode == "recv") {
            if (argc < 4) {
                printUsage();
                throw std::runtime_error("Invalid recv arguments");
            }
            c.port = std::stoi(argv[idx++]);
            c.path = argv[idx++];
        } else {
            printUsage();
            throw std::runtime_error("Unknown mode: " + c.mode);
        }

        // Parse optional flags
        for (; idx < argc; ++idx) {
            std::string arg = argv[idx];
            if (arg == "--mss" && idx + 1 < argc) {
                c.mss = std::stoi(argv[++idx]);
                if (c.mss < 536 || c.mss > 8900) {
                    throw std::runtime_error("MSS must be between 536 and 8900");
                }
            } else if (arg == "--window" && idx + 1 < argc) {
                c.window = std::stoi(argv[++idx]);
                if (c.window < 65536) {
                    throw std::runtime_error("Window size must be at least 65536 bytes");
                }
                if (c.window > UDT_MAX_BUF) {
                    c.window = UDT_MAX_BUF;
                }
            } else if (arg == "--detailed") {
                c.detailed = true;
            } else if (arg == "--no-cc") {  // 新增：关闭拥塞控制
                c.no_cc = true;
            } else {
                throw std::runtime_error("Unknown option: " + arg);
            }
        }

        return c;
    }

    static void printUsage() {
        std::cerr << "Usage:\n"
                << "  hruft send <ip> <port> <filepath> [options]\n"
                << "  hruft recv <port> <savepath> [options]\n\n"
                << "Options:\n"
                << "  --mss <value>      Maximum Segment Size (default: 1500)\n"
                << "  --window <value>   Window size in bytes (default: "
                << DEFAULT_WINDOW / (1024 * 1024) << "MB)\n"
                << "  --detailed         Show detailed statistics\n"
                << "  --no-cc            Disable congestion control (for direct connections)\n";  // 新增
    }
};

// --- 辅助函数 ---
class Utils {
public:
    static bool waitForAck(UDTSOCKET sock, const std::string &expected, int timeout_ms = 10000) {
        char buf[256];
        int original_timeout = 0;
        int timeout_len = sizeof(int);

        // 保存原始超时设置
        UDT::getsockopt(sock, 0, UDT_RCVTIMEO, &original_timeout, &timeout_len);

        // 设置新超时
        UDT::setsockopt(sock, 0, UDT_RCVTIMEO, &timeout_ms, sizeof(int));

        int r = UDT::recv(sock, buf, sizeof(buf) - 1, 0);

        // 恢复原始超时
        UDT::setsockopt(sock, 0, UDT_RCVTIMEO, &original_timeout, sizeof(int));

        if (r > 0) {
            buf[r] = '\0';
            return std::string(buf) == expected;
        }
        return false;
    }

    static std::string hashToString(const uint8_t *hash, size_t len) {
        std::stringstream ss;
        for (size_t i = 0; i < len; ++i) {
            ss << std::hex << std::setw(2) << std::setfill('0')
                    << static_cast<int>(hash[i]);
        }
        return ss.str();
    }

    static std::string formatSize(uint64_t bytes) {
        const char *units[] = {"B", "KB", "MB", "GB", "TB"};
        int unit = 0;
        double size = static_cast<double>(bytes);

        while (size >= 1024.0 && unit < 4) {
            size /= 1024.0;
            unit++;
        }

        std::stringstream ss;
        ss << std::fixed << std::setprecision(2) << size << " " << units[unit];
        return ss.str();
    }
};

// --- 主程序类 ---
class HruftPro {
    UDTSOCKET sock;
    Config cfg;
    blake3_hasher hasher;

    // 核心性能设置：配置 Socket 缓冲区
    void tuneSocket(UDTSOCKET s, int mss, int winSize) {
        // 1. 设置 MSS (必须在连接前)
        UDT::setsockopt(s, 0, UDT_MSS, &mss, sizeof(int));

        // 2. 如果启用no_cc，设置SimpleCC拥塞控制
        if (cfg.no_cc) {
            SimpleCCFactory factory;
            UDT::setsockopt(s, 0, UDT_CC, &factory, sizeof(SimpleCCFactory));

            // 同时设置一个较大的带宽限制，避免被限制
            int64_t max_bw = 1000000000; // 1 Gbps
            UDT::setsockopt(s, 0, UDT_MAXBW, &max_bw, sizeof(int64_t));
        }

        // 3. UDT 缓冲区 (应用层可见窗口)
        // 限制最大值
        int udtBuf = std::min(winSize, UDT_MAX_BUF);
        UDT::setsockopt(s, 0, UDT_SNDBUF, &udtBuf, sizeof(int));
        UDT::setsockopt(s, 0, UDT_RCVBUF, &udtBuf, sizeof(int));

        // 4. UDP 缓冲区 (操作系统内核级)
        int udpBuf = udtBuf;
        if (udpBuf < 1 * 1024 * 1024) {
            udpBuf = 1 * 1024 * 1024; // 最小给 1MB
        }

        UDT::setsockopt(s, 0, UDP_SNDBUF, &udpBuf, sizeof(int));
        UDT::setsockopt(s, 0, UDP_RCVBUF, &udpBuf, sizeof(int));

        // 5. 阻塞模式
        bool block = true;
        UDT::setsockopt(s, 0, UDT_SNDSYN, &block, sizeof(bool));
        UDT::setsockopt(s, 0, UDT_RCVSYN, &block, sizeof(bool));

        // 6. 启用地址重用
        bool reuse = true;
        UDT::setsockopt(s, 0, UDT_REUSEADDR, &reuse, sizeof(bool));
    }

public:
    HruftPro(Config c) : cfg(c) {
        UDT::startup();
        blake3_hasher_init(&hasher);
        sock = UDT::INVALID_SOCK;
    }

    ~HruftPro() {
        if (sock != UDT::INVALID_SOCK) {
            UDT::close(sock);
            sock = UDT::INVALID_SOCK;
        }
        UDT::cleanup();
    }

    void runSender() {
#ifdef _WIN32
        fs::path filePath = fs::path(Utf8Util::toWide(cfg.path)); // 中文路径
#else
        fs::path filePath = fs::path(cfg.path);
#endif

        if (!fs::exists(filePath)) {
            throw std::runtime_error("File not found: " + cfg.path);
        }

        uint64_t fsize = fs::file_size(filePath);

#ifdef _WIN32
        std::string fname = Utf8Util::toUtf8(filePath.filename().wstring()); // UTF-8 文件名跨平台
#else
        std::string fname = filePath.filename().string();
#endif

        if (fname.size() > 65535) {
            throw std::runtime_error("Filename too long");
        }

        sock = UDT::socket(AF_INET, SOCK_STREAM, 0);
        if (sock == UDT::INVALID_SOCK) {
            throw std::runtime_error("Failed to create socket");
        }

        tuneSocket(sock, cfg.mss, cfg.window);

        sockaddr_in serv_addr;
        memset(&serv_addr, 0, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(cfg.port);

        if (inet_pton(AF_INET, cfg.ip.c_str(), &serv_addr.sin_addr) <= 0) {
            throw std::runtime_error("Invalid IP address: " + cfg.ip);
        }

        std::cout << "[INFO] Connecting to " << cfg.ip << ":" << cfg.port << "..." << std::endl;

        if (UDT::ERROR == UDT::connect(sock, (sockaddr *) &serv_addr, sizeof(serv_addr))) {
            std::string error = UDT::getlasterror().getErrorMessage();
            throw std::runtime_error("Connect failed: " + error);
        }

        std::cout << "[INFO] Connected successfully" << std::endl;

        // Protocol Header
        ProtocolHeader hdr;
        hdr.magic = htonl(MAGIC_ID);
        hdr.mss = htonl(cfg.mss);
        hdr.window_size = htonl(cfg.window);
        hdr.file_size = htonll(fsize);
        hdr.filename_len = htons(static_cast<uint16_t>(fname.size()));

        // 发送协议头
        if (UDT::send(sock, (char *) &hdr, sizeof(hdr), 0) <= 0) {
            throw std::runtime_error("Failed to send protocol header");
        }

        // 发送文件名
        if (UDT::send(sock, fname.c_str(), fname.size(), 0) <= 0) {
            throw std::runtime_error("Failed to send filename");
        }

        // 传输文件数据
        std::ifstream ifs(filePath, std::ios::binary);
        if (!ifs) {
            throw std::runtime_error("Cannot open file for reading: " + cfg.path);
        }

        std::vector<char> buf(APP_BLOCK_SIZE);
        uint64_t sent = 0;

        auto t_start = std::chrono::high_resolution_clock::now();
        auto last_progress_time = t_start;

        std::cout << "[INFO] Sending " << fname << " (" << Utils::formatSize(fsize) << ")..." << std::endl;

        while (ifs.read(buf.data(), buf.size()) || ifs.gcount() > 0) {
            int len = static_cast<int>(ifs.gcount());
            blake3_hasher_update(&hasher, buf.data(), len);

            int offset = 0;
            while (offset < len) {
                int s = UDT::send(sock, buf.data() + offset, len - offset, 0);
                if (s == UDT::ERROR) {
                    throw std::runtime_error("Send failed: " + std::string(UDT::getlasterror().getErrorMessage()));
                }
                offset += s;
            }
            sent += len;

            // 显示进度（每秒最多更新一次）
            auto now = std::chrono::high_resolution_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_progress_time).count();

            if (cfg.detailed || elapsed >= 1000) {
                double progress = (fsize > 0) ? (static_cast<double>(sent) / fsize * 100.0) : 0.0;
                std::cout << "\r[Progress] " << std::fixed << std::setprecision(1) << progress
                        << "% | " << Utils::formatSize(sent) << " / " << Utils::formatSize(fsize);

                if (cfg.detailed) {
                    UDT::TRACEINFO tmp;
                    UDT::perfmon(sock, &tmp);
                    std::cout << " | Rate: " << std::fixed << std::setprecision(1)
                            << tmp.mbpsSendRate << " Mbps";
                }
                std::cout << std::flush;
                last_progress_time = now;
            }
        }

        ifs.close();

        std::cout << "\n[INFO] File data sent, computing hash..." << std::endl;

        // 发送哈希
        uint8_t hash[BLAKE3_OUT_LEN];
        blake3_hasher_finalize(&hasher, hash, BLAKE3_OUT_LEN);

        int hash_sent = 0;
        while (hash_sent < BLAKE3_OUT_LEN) {
            int s = UDT::send(sock, (char *) hash + hash_sent, BLAKE3_OUT_LEN - hash_sent, 0);
            if (s == UDT::ERROR) {
                throw std::runtime_error("Failed to send hash");
            }
            hash_sent += s;
        }

        // 发送完成标记
        int msg_sent = 0;
        while (msg_sent < TRANSFER_COMPLETE.size()) {
            int s = UDT::send(sock, TRANSFER_COMPLETE.c_str() + msg_sent,
                              TRANSFER_COMPLETE.size() - msg_sent, 0);
            if (s == UDT::ERROR) {
                throw std::runtime_error("Failed to send completion marker");
            }
            msg_sent += s;
        }

        auto t_end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> dur = t_end - t_start;

        std::cout << "[INFO] Transfer completed in " << std::fixed << std::setprecision(2)
                << dur.count() << " seconds" << std::endl;
        std::cout << "[INFO] Waiting for receiver confirmation..." << std::endl;

        // 等待接收端确认
        if (Utils::waitForAck(sock, ACK_TRANSFER)) {
            std::cout << "[INFO] Receiver acknowledged transfer" << std::endl;
        } else {
            std::cout << "[WARNING] No ACK received from receiver" << std::endl;
        }

        // 接收报告
        char respBuf[16384];
        int r = UDT::recv(sock, respBuf, sizeof(respBuf) - 1, 0);
        if (r > 0) {
            respBuf[r] = 0;
            try {
                json j = json::parse(respBuf);
                std::cout << "\n=== Receiver Report ===\n" << j.dump(4) << std::endl;
            } catch (const json::exception &e) {
                std::cout << "[INFO] Raw report: " << respBuf << std::endl;
            }
        } else {
            std::cout << "[INFO] No report received from receiver" << std::endl;
        }

        UDT::close(sock);
        sock = UDT::INVALID_SOCK;
    }

    void runReceiver() {
        UDTSOCKET serv = UDT::socket(AF_INET, SOCK_STREAM, 0);
        if (serv == UDT::INVALID_SOCK) {
            throw std::runtime_error("Failed to create server socket");
        }

        // Bind socket with conservative settings
        tuneSocket(serv, cfg.mss, cfg.window);

        sockaddr_in my_addr;
        memset(&my_addr, 0, sizeof(my_addr));
        my_addr.sin_family = AF_INET;
        my_addr.sin_port = htons(cfg.port);
        my_addr.sin_addr.s_addr = INADDR_ANY;

        if (UDT::ERROR == UDT::bind(serv, (sockaddr *) &my_addr, sizeof(my_addr))) {
            std::string error = UDT::getlasterror().getErrorMessage();
            throw std::runtime_error("Bind failed: " + error);
        }

        UDT::listen(serv, 10);
        std::cout << "[INFO] Listening on port " << cfg.port << "..." << std::endl;

        sockaddr_in client_addr;
        int addrlen = sizeof(client_addr);
        sock = UDT::accept(serv, (sockaddr *) &client_addr, &addrlen);
        if (sock == UDT::ERROR || sock == UDT::INVALID_SOCK) {
            std::string error = UDT::getlasterror().getErrorMessage();
            throw std::runtime_error("Accept failed: " + error);
        }

        std::cout << "[INFO] Connection accepted from client" << std::endl;

        // 读取协议头
        ProtocolHeader hdr;
        int bytes_received = UDT::recv(sock, (char *) &hdr, sizeof(hdr), 0);
        if (bytes_received != sizeof(hdr)) {
            throw std::runtime_error("Invalid protocol header received");
        }

        // 验证魔数
        if (ntohl(hdr.magic) != MAGIC_ID) {
            throw std::runtime_error("Invalid protocol magic number");
        }

        int rMSS = ntohl(hdr.mss);
        int rWin = ntohl(hdr.window_size);
        uint64_t rSize = ntohll(hdr.file_size);
        uint16_t nameLen = ntohs(hdr.filename_len);

        // 应用发送方的窗口设置
        tuneSocket(sock, rMSS, rWin);

        std::cout << "[INFO] Remote config - MSS: " << rMSS << ", Window: "
                << Utils::formatSize(rWin) << std::endl;

        // 读取文件名
        if (nameLen > 65535) {
            throw std::runtime_error("Invalid filename length");
        }

        std::vector<char> nameBuf(nameLen + 1);
        bytes_received = UDT::recv(sock, nameBuf.data(), nameLen, 0);
        if (bytes_received != nameLen) {
            throw std::runtime_error("Failed to receive filename");
        }
        nameBuf[nameLen] = 0;
        std::string filename = nameBuf.data();

        std::cout << "[INFO] Receiving file: " << filename << " ("
                << Utils::formatSize(rSize) << ")" << std::endl;

#ifdef _WIN32
        fs::path baseDir = fs::path(Utf8Util::toWide(cfg.path));
        fs::path outPath = baseDir;
        if (fs::is_directory(baseDir)) {
            outPath /= fs::path(Utf8Util::toWide(filename));
        }
#else
        fs::path outPath = cfg.path;
        if (fs::is_directory(outPath)) {
            outPath /= filename;
        }
#endif

        // 确保目录存在
        fs::create_directories(outPath.parent_path());

        // 如果文件已存在，添加时间戳后缀
        if (fs::exists(outPath)) {
            auto now = std::chrono::system_clock::now();
            auto in_time_t = std::chrono::system_clock::to_time_t(now);
            std::stringstream ss;
            ss << std::put_time(std::localtime(&in_time_t), "_%Y%m%d_%H%M%S");
            outPath.replace_filename(outPath.stem().string() + ss.str() + outPath.extension().string());
        }

        std::ofstream ofs(outPath, std::ios::binary);
        if (!ofs) {
            throw std::runtime_error("Cannot open file for writing: " + outPath.string());
        }

        std::vector<char> buf(APP_BLOCK_SIZE);
        uint64_t received = 0;

        auto t_start = std::chrono::high_resolution_clock::now();
        auto last_progress_time = t_start;

        std::cout << "[INFO] Saving to: " <<
#ifdef _WIN32
                Utf8Util::toUtf8(outPath.filename().wstring())
#else
                outPath.filename().string()
#endif
                << std::endl;

        // 接收数据
        while (received < rSize) {
            int to_read = static_cast<int>(std::min(
                static_cast<uint64_t>(APP_BLOCK_SIZE),
                rSize - received
            ));

            int block_offset = 0;
            while (block_offset < to_read) {
                int r = UDT::recv(sock, buf.data() + block_offset, to_read - block_offset, 0);
                if (r <= 0) {
                    if (r == UDT::ERROR) {
                        std::string error = UDT::getlasterror().getErrorMessage();
                        throw std::runtime_error("Receive error: " + error);
                    }
                    break; // 连接关闭
                }
                block_offset += r;
            }

            if (block_offset == 0) break; // 连接关闭

            ofs.write(buf.data(), block_offset);
            if (!ofs) {
                throw std::runtime_error("Failed to write to file");
            }

            blake3_hasher_update(&hasher, buf.data(), block_offset);
            received += block_offset;

            // 显示进度（每秒最多更新一次）
            auto now = std::chrono::high_resolution_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_progress_time).count();

            if (cfg.detailed || elapsed >= 1000) {
                double progress = (rSize > 0) ? (static_cast<double>(received) / rSize * 100.0) : 0.0;
                std::cout << "\r[Progress] " << std::fixed << std::setprecision(1) << progress
                        << "% | " << Utils::formatSize(received) << " / " << Utils::formatSize(rSize);

                if (cfg.detailed) {
                    UDT::TRACEINFO tmp;
                    UDT::perfmon(sock, &tmp);
                    std::cout << " | Rate: " << std::fixed << std::setprecision(1)
                            << tmp.mbpsRecvRate << " Mbps";
                }
                std::cout << std::flush;
                last_progress_time = now;
            }
        }

        ofs.close();

        // 检查是否接收到完整文件
        if (received != rSize) {
            // 删除不完整的文件
            try { fs::remove(outPath); } catch (...) {
            }
            throw std::runtime_error("File transfer incomplete. Expected: " +
                                     Utils::formatSize(rSize) + ", Received: " +
                                     Utils::formatSize(received));
        }

        std::cout << "\n[INFO] File received, computing hash..." << std::endl;

        // 接收远程哈希
        uint8_t rHash[BLAKE3_OUT_LEN];
        int hRead = 0;
        while (hRead < BLAKE3_OUT_LEN) {
            int r = UDT::recv(sock, (char *) rHash + hRead, BLAKE3_OUT_LEN - hRead, 0);
            if (r <= 0) {
                if (r == UDT::ERROR) {
                    std::string error = UDT::getlasterror().getErrorMessage();
                    throw std::runtime_error("Failed to receive hash: " + error);
                }
                break;
            }
            hRead += r;
        }

        if (hRead != BLAKE3_OUT_LEN) {
            throw std::runtime_error("Incomplete hash received");
        }

        // 等待传输完成标记
        char completeMsg[256];
        int msgSize = UDT::recv(sock, completeMsg, sizeof(completeMsg) - 1, 0);
        bool transferComplete = false;
        if (msgSize > 0) {
            completeMsg[msgSize] = 0;
            transferComplete = (std::string(completeMsg) == TRANSFER_COMPLETE);
        }

        if (!transferComplete) {
            std::cout << "[WARNING] Transfer completion marker not received or incorrect" << std::endl;
        }

        // 发送确认
        int ack_sent = 0;
        while (ack_sent < ACK_TRANSFER.size()) {
            int s = UDT::send(sock, ACK_TRANSFER.c_str() + ack_sent,
                              ACK_TRANSFER.size() - ack_sent, 0);
            if (s == UDT::ERROR) {
                std::cout << "[WARNING] Failed to send ACK" << std::endl;
                break;
            }
            ack_sent += s;
        }

        auto t_end = std::chrono::high_resolution_clock::now();
        double duration = std::chrono::duration<double>(t_end - t_start).count();

        // 计算本地哈希
        uint8_t lHash[BLAKE3_OUT_LEN];
        blake3_hasher_finalize(&hasher, lHash, BLAKE3_OUT_LEN);

        std::string localHash = Utils::hashToString(lHash, BLAKE3_OUT_LEN);
        std::string remoteHash = Utils::hashToString(rHash, BLAKE3_OUT_LEN);

        bool match = (localHash == remoteHash);

        std::cout << "[INFO] Hash verification: " << (match ? "PASS" : "FAIL") << std::endl;

        if (!match) {
            std::cout << "[ERROR] Local hash:  " << localHash << std::endl;
            std::cout << "[ERROR] Remote hash: " << remoteHash << std::endl;
        }

        // 生成JSON报告
        UDT::TRACEINFO perf;
        UDT::perfmon(sock, &perf);

        json jStats = NetworkStats::snapshot(perf, duration, rSize);
        json jFinal = NetworkStats::analyze(jStats, rMSS, rWin);

        jFinal["meta"] = json::object({
            {
                "filename",
#ifdef _WIN32
                Utf8Util::toUtf8(outPath.filename().wstring())
#else
                outPath.filename().string()
#endif
            },
            {"filepath", outPath.string()},
            {"filesize", rSize},
            {"filesize_human", Utils::formatSize(rSize)},
            {"status", match ? "success" : "integrity_failure"},
            {"hash_match", match},
            {"local_hash", localHash},
            {"remote_hash", remoteHash},
            {"duration_sec", duration},
            {"avg_speed_mbps", duration > 0 ? (rSize * 8.0 / 1000000.0) / duration : 0.0},
            {"avg_speed_mbs", duration > 0 ? (rSize / (1024.0 * 1024.0)) / duration : 0.0},
            {"congestion_control_disabled", cfg.no_cc}  // 新增：显示拥塞控制状态
        });

        std::string jsonStr = jFinal.dump();

        // 发送报告给发送方
        int report_sent = 0;
        while (report_sent < jsonStr.size()) {
            int s = UDT::send(sock, jsonStr.c_str() + report_sent,
                              jsonStr.size() - report_sent, 0);
            if (s == UDT::ERROR) {
                std::cout << "[WARNING] Failed to send report to sender" << std::endl;
                break;
            }
            report_sent += s;
        }

        // 本地显示
        std::cout << "\n=== Transfer Summary ===\n" << jFinal.dump(4) << std::endl;

        UDT::close(sock);
        sock = UDT::INVALID_SOCK;
        UDT::close(serv);
    }
};

int main(int argc, char* argv[]) {

#ifdef _WIN32
    // ====== 中文支持新增：控制台 UTF-8 ======
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    // ====== 中文支持新增：从系统获取 UTF-16 命令行并转 UTF-8 ======
    int wargc = 0;
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);

    std::vector<std::string> utf8Args;
    for (int i = 0; i < wargc; ++i) {
        utf8Args.push_back(Utf8Util::toUtf8(wargv[i]));
    }
    LocalFree(wargv);

    std::vector<char*> newArgv;
    for (auto& s : utf8Args) newArgv.push_back(s.data());

    argc = (int)newArgv.size();
    argv = newArgv.data();
#endif

    try {
        Config cfg = Config::parse(argc, argv);
        HruftPro app(cfg);

        if (cfg.mode == "send") app.runSender();
        else if (cfg.mode == "recv") app.runReceiver();

    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << std::endl;
        return 1;
    }
    return 0;
}