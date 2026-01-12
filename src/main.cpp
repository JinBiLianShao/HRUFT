#include <iostream>
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>
#include <thread>
#include <iomanip>
#include <sstream>
#include <cmath>
#include <queue>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h> // inet_pton 定义在这里
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include "udt.h"
#include "md5.h"

// ==========================================
// 协议定义
// ==========================================

// 握手包结构：确保两端按同样的字节序解析
#pragma pack(push, 1)
struct HandshakePacket {
    long long file_size; // 文件总大小
    char md5[32]; // MD5 字符串
    int mss; // 发送端指定的 MSS
    int window_size; // 发送端指定的 Window Size
};
#pragma pack(pop)

struct UDTConfig {
    int mss = 1500;
    int window_size = 1048576; // 默认 1MB
};

// ==========================================
// 传输统计结构
// ==========================================

struct TransferStatistics {
    // 基础信息
    long long total_bytes = 0;
    long long transferred_bytes = 0;
    long long bytes_since_last_report = 0; // 用于累计上次报告以来的字节数

    // 时间统计
    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point end_time;
    std::chrono::steady_clock::time_point last_report_time;
    std::chrono::steady_clock::time_point last_speed_calc_time;

    // 速度统计
    double current_speed_mbps = 0.0;
    double average_speed_mbps = 0.0;
    double max_speed_mbps = 0.0;
    double min_speed_mbps = 0.0;

    // 平滑速度计算的队列（用于避免初始速度异常高）
    std::queue<double> speed_history;
    const int SPEED_HISTORY_SIZE = 3; // 保存最近3个速度值用于平滑

    // UDT性能统计（通过UDT API获取）
    int64_t pktSentTotal = 0; // 总发送包数
    int64_t pktRecvTotal = 0; // 总接收包数
    int pktSndLossTotal = 0; // 发送端丢包数
    int pktRcvLossTotal = 0; // 接收端丢包数
    int pktRetransTotal = 0; // 重传包数
    int pktSentACKTotal = 0; // 发送ACK数
    int pktRecvACKTotal = 0; // 接收ACK数
    int pktSentNAKTotal = 0; // 发送NAK数
    int pktRecvNAKTotal = 0; // 接收NAK数
    int64_t usSndDurationTotal = 0; // 总发送时间（微秒）

    // 即时性能统计
    int64_t pktSent = 0; // 本地发送包数
    int64_t pktRecv = 0; // 本地接收包数
    int pktSndLoss = 0; // 本地发送丢包
    int pktRcvLoss = 0; // 本地接收丢包
    int pktRetrans = 0; // 本地重传
    double mbpsSendRate = 0.0; // UDT报告的发送速率（Mbps）
    double mbpsRecvRate = 0.0; // UDT报告的接收速率（Mbps）
    int64_t usSndDuration = 0; // 本地发送时间

    // 连接质量
    double loss_rate = 0.0;
    double retransmission_rate = 0.0;
    double efficiency_ratio = 0.0; // 传输效率

    // 传输开始时间戳（用于过滤初始速度异常）
    std::chrono::steady_clock::time_point first_speed_calc_time;
    bool first_speed_calculated = false;

    // 初始化统计
    void init(long long total_size) {
        total_bytes = total_size;
        transferred_bytes = 0;
        bytes_since_last_report = 0;
        start_time = std::chrono::steady_clock::now();
        last_report_time = start_time;
        last_speed_calc_time = start_time;
        first_speed_calc_time = start_time;
        first_speed_calculated = false;
        max_speed_mbps = 0.0;
        min_speed_mbps = std::numeric_limits<double>::max();
        current_speed_mbps = 0.0;
        average_speed_mbps = 0.0;

        // 清空速度历史
        while (!speed_history.empty()) {
            speed_history.pop();
        }

        // 重置所有统计
        pktSentTotal = 0;
        pktRecvTotal = 0;
        pktSndLossTotal = 0;
        pktRcvLossTotal = 0;
        pktRetransTotal = 0;
        pktSentACKTotal = 0;
        pktRecvACKTotal = 0;
        pktSentNAKTotal = 0;
        pktRecvNAKTotal = 0;
        usSndDurationTotal = 0;

        pktSent = 0;
        pktRecv = 0;
        pktSndLoss = 0;
        pktRcvLoss = 0;
        pktRetrans = 0;
        mbpsSendRate = 0.0;
        mbpsRecvRate = 0.0;
        usSndDuration = 0;

        loss_rate = 0.0;
        retransmission_rate = 0.0;
        efficiency_ratio = 0.0;
    }

    // 更新速度统计 - 修复版本（避免初始速度异常）
    void update_speed(long long bytes_transferred) {
        auto now = std::chrono::steady_clock::now();
        bytes_since_last_report += bytes_transferred;
        transferred_bytes += bytes_transferred;

        // 忽略前100ms的速度计算，避免初始速度异常高
        auto time_since_start = std::chrono::duration_cast<std::chrono::milliseconds>(now - first_speed_calc_time).count();
        if (!first_speed_calculated && time_since_start < 100) {
            return; // 跳过前100ms的速度计算
        }
        first_speed_calculated = true;

        // 至少500ms计算一次瞬时速度
        auto time_diff = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_speed_calc_time).count();

        if (time_diff >= 500) {
            // 计算瞬时速度：字节数 × 8 / 时间(秒) / 1024 / 1024
            if (time_diff > 0) {
                double raw_speed = (bytes_since_last_report * 8.0) / (time_diff / 1000.0) / (1024.0 * 1024.0);

                // 使用滑动平均平滑速度
                speed_history.push(raw_speed);
                if (speed_history.size() > SPEED_HISTORY_SIZE) {
                    speed_history.pop();
                }

                // 计算平均速度
                double sum = 0.0;
                int count = 0;
                std::queue<double> temp = speed_history;
                while (!temp.empty()) {
                    sum += temp.front();
                    temp.pop();
                    count++;
                }

                if (count > 0) {
                    current_speed_mbps = sum / count;

                    // 更新最大/最小速度（忽略初始异常值）
                    if (current_speed_mbps > max_speed_mbps && current_speed_mbps < 10000) { // 限制最大速度阈值
                        max_speed_mbps = current_speed_mbps;
                    }
                    if (current_speed_mbps > 0 && current_speed_mbps < min_speed_mbps) {
                        min_speed_mbps = current_speed_mbps;
                    }
                }
            }

            bytes_since_last_report = 0;
            last_speed_calc_time = now;
        }

        // 计算平均速度
        auto total_time = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count();
        if (total_time > 0) {
            average_speed_mbps = (transferred_bytes * 8.0) / (total_time / 1000.0) / (1024.0 * 1024.0);
        }
    }

    // 获取UDT性能统计 - 修复版本
    void collect_udt_stats(UDTSOCKET sock) {
        UDT::TRACEINFO perf;
        if (UDT::ERROR != UDT::perfmon(sock, &perf)) {
            // 全局测量值
            pktSentTotal = perf.pktSentTotal;
            pktRecvTotal = perf.pktRecvTotal;
            pktSndLossTotal = perf.pktSndLossTotal;
            pktRcvLossTotal = perf.pktRcvLossTotal;
            pktRetransTotal = perf.pktRetransTotal;
            pktSentACKTotal = perf.pktSentACKTotal;
            pktRecvACKTotal = perf.pktRecvACKTotal;
            pktSentNAKTotal = perf.pktSentNAKTotal;
            pktRecvNAKTotal = perf.pktRecvNAKTotal;
            usSndDurationTotal = perf.usSndDurationTotal;

            // 本地测量值
            pktSent = perf.pktSent;
            pktRecv = perf.pktRecv;
            pktSndLoss = perf.pktSndLoss;
            pktRcvLoss = perf.pktRcvLoss;
            pktRetrans = perf.pktRetrans;

            // 使用UDT报告的速率
            mbpsSendRate = perf.mbpsSendRate;
            mbpsRecvRate = perf.mbpsRecvRate;

            usSndDuration = perf.usSndDuration;

            // 修复丢包率计算
            if (pktSentTotal > 0) {
                loss_rate = (pktSndLossTotal * 100.0) / pktSentTotal;
            } else if (pktRecvTotal > 0) {
                loss_rate = (pktRcvLossTotal * 100.0) / pktRecvTotal;
            } else {
                loss_rate = 0.0;
            }

            // 计算重传率
            if (pktSentTotal > 0) {
                retransmission_rate = (pktRetransTotal * 100.0) / pktSentTotal;
            }

            // 计算传输效率：有效包数 / 总发送包数
            if (pktSentTotal > 0) {
                efficiency_ratio = ((pktSentTotal - pktRetransTotal) * 100.0) / pktSentTotal;
            }
        }
    }

    // 生成JSON格式的详细统计报告
    std::string to_json() const {
        auto total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
        double total_seconds = total_duration / 1000.0;

        std::stringstream ss;
        ss << std::fixed << std::setprecision(2);
        ss << "{";
        ss << "\"type\":\"statistics\",";
        ss << "\"total_bytes\":" << total_bytes << ",";
        ss << "\"transferred_bytes\":" << transferred_bytes << ",";
        ss << "\"completion_percentage\":" << (total_bytes > 0 ? (transferred_bytes * 100.0 / total_bytes) : 0) << ",";
        ss << "\"total_time_seconds\":" << total_seconds << ",";
        ss << "\"average_speed_mbps\":" << average_speed_mbps << ",";
        ss << "\"max_speed_mbps\":" << max_speed_mbps << ",";
        ss << "\"min_speed_mbps\":" << (min_speed_mbps < std::numeric_limits<double>::max() ? min_speed_mbps : 0) << ",";
        ss << "\"current_speed_mbps\":" << current_speed_mbps << ",";

        // 时间分析
        if (total_bytes > 0 && average_speed_mbps > 0) {
            double remaining_bytes = total_bytes - transferred_bytes;
            double estimated_remaining_seconds = (remaining_bytes * 8.0 / 1024.0 / 1024.0) / average_speed_mbps;
            ss << "\"estimated_remaining_seconds\":" << estimated_remaining_seconds << ",";
            ss << "\"estimated_total_seconds\":" << (total_seconds + estimated_remaining_seconds) << ",";
        } else {
            ss << "\"estimated_remaining_seconds\":0,";
            ss << "\"estimated_total_seconds\":" << total_seconds << ",";
        }

        // 包统计
        ss << "\"packet_stats\":{";
        ss << "\"pktSentTotal\":" << pktSentTotal << ",";
        ss << "\"pktRecvTotal\":" << pktRecvTotal << ",";
        ss << "\"pktSndLossTotal\":" << pktSndLossTotal << ",";
        ss << "\"pktRcvLossTotal\":" << pktRcvLossTotal << ",";
        ss << "\"pktRetransTotal\":" << pktRetransTotal << ",";
        ss << "\"loss_rate\":" << loss_rate << ",";
        ss << "\"retransmission_rate\":" << retransmission_rate << ",";
        ss << "\"efficiency_ratio\":" << efficiency_ratio;
        ss << "},";

        // 即时性能
        ss << "\"instant_stats\":{";
        ss << "\"pktSent\":" << pktSent << ",";
        ss << "\"pktRecv\":" << pktRecv << ",";
        ss << "\"mbpsSendRate\":" << mbpsSendRate << ",";
        ss << "\"mbpsRecvRate\":" << mbpsRecvRate;
        ss << "},";

        // 传输效率评估
        std::string efficiency_level = "good";
        if (loss_rate > 10) efficiency_level = "poor";
        else if (loss_rate > 5) efficiency_level = "fair";
        else if (loss_rate > 2) efficiency_level = "good";
        else efficiency_level = "excellent";

        ss << "\"efficiency_level\":\"" << efficiency_level << "\",";

        // 建议（基于统计）
        std::string suggestions = "";
        if (loss_rate > 10) suggestions = "High packet loss. Consider reducing window size or increasing MSS.";
        else if (loss_rate > 5) suggestions = "Moderate packet loss. Network may be congested.";
        else if (retransmission_rate > 10) suggestions = "High retransmission rate. Check network stability.";
        else suggestions = "Network conditions are good.";

        ss << "\"suggestions\":\"" << suggestions << "\"";
        ss << "}";

        return ss.str();
    }

    // 生成简洁的总结报告
    std::string summary() const {
        auto total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
        double total_seconds = total_duration / 1000.0;

        std::stringstream ss;
        ss << std::fixed << std::setprecision(2);
        ss << "传输完成！详细统计信息：" << std::endl;
        ss << "  文件大小: " << (total_bytes / 1024.0 / 1024.0) << " MB" << std::endl;
        ss << "  传输时间: " << total_seconds << " 秒" << std::endl;
        ss << "  平均速度: " << average_speed_mbps << " Mbps" << std::endl;
        ss << "  最高速度: " << max_speed_mbps << " Mbps" << std::endl;
        ss << "  最低速度: " << (min_speed_mbps < std::numeric_limits<double>::max() ? min_speed_mbps : 0) << " Mbps" << std::endl;
        ss << std::endl;
        ss << "  包统计:" << std::endl;
        ss << "    总发送包数: " << pktSentTotal << std::endl;
        ss << "    总接收包数: " << pktRecvTotal << std::endl;
        ss << "    发送端丢包: " << pktSndLossTotal << std::endl;
        ss << "    接收端丢包: " << pktRcvLossTotal << std::endl;
        ss << "    重传包数: " << pktRetransTotal << std::endl;
        ss << "    丢包率: " << loss_rate << "%" << std::endl;
        ss << "    重传率: " << retransmission_rate << "%" << std::endl;
        ss << "    传输效率: " << efficiency_ratio << "%" << std::endl;
        ss << std::endl;
        ss << "  即时性能:" << std::endl;
        ss << "    UDT发送速率: " << mbpsSendRate << " Mbps" << std::endl;
        ss << "    UDT接收速率: " << mbpsRecvRate << " Mbps" << std::endl;

        return ss.str();
    }
};

// ==========================================
// 辅助工具
// ==========================================

void report_json(const std::string &type, const std::string &key, const std::string &val) {
    std::cout << "{\"type\":\"" << type << "\", \"" << key << "\":\"" << val << "\"}" << std::endl;
}

void report_progress(long long current, long long total, double speed_mbps,
                     const TransferStatistics &stats, bool detailed = false) {
    int percent = (total > 0) ? (int)((current * 100) / total) : 0;
    std::cout << std::fixed << std::setprecision(2);

    if (detailed) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - stats.start_time).count();

        std::cout << "{\"type\":\"progress\", \"percent\":" << percent
                  << ", \"current\":" << current
                  << ", \"total\":" << total
                  << ", \"speed_mbps\":" << speed_mbps
                  << ", \"elapsed_seconds\":" << elapsed
                  << ", \"average_speed_mbps\":" << stats.average_speed_mbps
                  << ", \"remaining_bytes\":" << (total - current)
                  << "}" << std::endl;
    } else {
        std::cout << "{\"type\":\"progress\", \"percent\":" << percent
                  << ", \"current\":" << current
                  << ", \"total\":" << total
                  << ", \"speed_mbps\":" << speed_mbps << "}" << std::endl;
    }
}

std::string calculate_file_md5(const std::string &filepath) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file) return "";
    md5_state_t state;
    md5_byte_t digest[16];
    md5_init(&state);
    const size_t buffer_size = 1024 * 1024;
    std::vector<char> buffer(buffer_size);
    while (file.good()) {
        file.read(buffer.data(), buffer_size);
        if (file.gcount() > 0) md5_append(&state, (const md5_byte_t*)buffer.data(), (int)file.gcount());
    }
    md5_finish(&state, digest);
    std::stringstream ss;
    for (int i = 0; i < 16; ++i) ss << std::hex << std::setw(2) << std::setfill('0') << (int)digest[i];
    return ss.str();
}

// 动态配置 Socket 参数
void apply_socket_opts(UDTSOCKET sock, int mss, int win_size) {
    UDT::setsockopt(sock, 0, UDT_MSS, &mss, sizeof(int));
    UDT::setsockopt(sock, 0, UDT_SNDBUF, &win_size, sizeof(int));
    UDT::setsockopt(sock, 0, UDT_RCVBUF, &win_size, sizeof(int));
    UDT::setsockopt(sock, 0, UDP_SNDBUF, &win_size, sizeof(int));
    UDT::setsockopt(sock, 0, UDP_RCVBUF, &win_size, sizeof(int));
    int fc = (win_size / mss) * 2;
    if (fc < 25600) fc = 25600;
    UDT::setsockopt(sock, 0, UDT_FC, &fc, sizeof(int));
}

// ==========================================
// 发送端逻辑
// ==========================================

void run_sender(const char* ip, int port, const char* filepath, const UDTConfig& config) {
    report_json("status", "message", "Calculating MD5...");
    std::string local_md5 = calculate_file_md5(filepath);
    if (local_md5.empty()) {
        report_json("error", "message", "File error");
        return;
    }

    UDTSOCKET client = UDT::socket(AF_INET, SOCK_STREAM, 0);
    // 连接前先应用本地配置（用于建立连接的策略）
    apply_socket_opts(client, config.mss, config.window_size);

    sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &serv_addr.sin_addr);

    report_json("status", "message", "Connecting...");
    if (UDT::ERROR == UDT::connect(client, (sockaddr*)&serv_addr, sizeof(serv_addr))) {
        report_json("error", "message", UDT::getlasterror().getErrorMessage());
        return;
    }

    std::ifstream ifs(filepath, std::ios::binary | std::ios::ate);
    long long f_size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);

    // 初始化传输统计
    TransferStatistics stats;
    stats.init(f_size);

    // 1. 发送握手协商包
    HandshakePacket hp;
    hp.file_size = f_size;
    hp.mss = config.mss;
    hp.window_size = config.window_size;
    memcpy(hp.md5, local_md5.c_str(), 32);

    UDT::send(client, (char*)&hp, sizeof(HandshakePacket), 0);
    stats.transferred_bytes += sizeof(HandshakePacket);

    // 2. 开始传输数据
    std::vector<char> buffer(1024 * 64);
    long long total_sent = 0;
    auto last_time = std::chrono::steady_clock::now();
    long long last_bytes = 0;

    // 记录开始传输时间
    stats.start_time = std::chrono::steady_clock::now();

    while (total_sent < f_size) {
        ifs.read(buffer.data(), buffer.size());
        int read_len = (int)ifs.gcount();
        if (read_len <= 0) break;

        int offset = 0;
        while (offset < read_len) {
            int sent = UDT::send(client, buffer.data() + offset, read_len - offset, 0);
            if (sent <= 0) break;
            offset += sent;
            total_sent += sent;

            // 更新速度统计（累积字节数）
            stats.update_speed(sent);
        }

        auto now = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_time).count();
        if (duration >= 500) {
            // 使用stats中的当前速度，而不是重新计算
            report_progress(total_sent, f_size, stats.current_speed_mbps, stats, true);
            last_time = now;
            last_bytes = total_sent;

            // 收集UDT性能统计
            stats.collect_udt_stats(client);
        }
    }

    // 传输完成，记录结束时间
    stats.end_time = std::chrono::steady_clock::now();
    stats.transferred_bytes = total_sent;

    // 最终收集统计信息
    stats.collect_udt_stats(client);

    // 报告最终进度
    report_progress(total_sent, f_size, 0, stats, true);

    // 输出详细统计信息
    std::cout << stats.to_json() << std::endl;

    // 输出人类可读的总结
    std::cout << stats.summary() << std::endl;

    report_json("status", "message", "Transfer complete.");

    ifs.close();
    UDT::close(client);
}

// ==========================================
// 接收端逻辑
// ==========================================

void run_receiver(int port, const char* save_path) {
    UDTSOCKET serv = UDT::socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in my_addr;
    my_addr.sin_family = AF_INET;
    my_addr.sin_port = htons(port);
    my_addr.sin_addr.s_addr = INADDR_ANY;

    UDT::bind(serv, (sockaddr*)&my_addr, sizeof(my_addr));
    UDT::listen(serv, 5);
    report_json("status", "message", "Waiting for sender...");

    sockaddr_in client_addr;
    int namelen = sizeof(client_addr);
    UDTSOCKET recver = UDT::accept(serv, (sockaddr*)&client_addr, &namelen);

    // 1. 接收协商包
    HandshakePacket hp;
    UDT::recv(recver, (char*)&hp, sizeof(HandshakePacket), 0);

    // 2. 关键：根据发送端的建议动态调整接收端 Socket
    report_json("status", "message",
        "Syncing config: MSS=" + std::to_string(hp.mss) + ", WIN=" + std::to_string(hp.window_size));
    apply_socket_opts(recver, hp.mss, hp.window_size);

    // 3. 初始化传输统计
    TransferStatistics stats;
    stats.init(hp.file_size);
    stats.start_time = std::chrono::steady_clock::now();

    // 4. 接收文件数据
    std::ofstream ofs(save_path, std::ios::binary);
    long long total_recv = 0;
    std::vector<char> buffer(1024 * 64);
    auto last_time = std::chrono::steady_clock::now();
    long long last_bytes = 0;

    while (total_recv < hp.file_size) {
        int rs = UDT::recv(recver, buffer.data(), (int)buffer.size(), 0);
        if (rs <= 0) break;
        ofs.write(buffer.data(), rs);
        total_recv += rs;

        // 更新速度统计（累积字节数）
        stats.update_speed(rs);

        auto now = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_time).count();
        if (duration >= 500) {
            // 使用stats中的当前速度，而不是重新计算
            report_progress(total_recv, hp.file_size, stats.current_speed_mbps, stats, true);
            last_time = now;
            last_bytes = total_recv;

            // 收集UDT性能统计
            stats.collect_udt_stats(recver);
        }
    }
    ofs.close();

    // 传输完成，记录结束时间
    stats.end_time = std::chrono::steady_clock::now();
    stats.transferred_bytes = total_recv;

    // 最终收集统计信息
    stats.collect_udt_stats(recver);

    // 报告最终进度
    report_progress(total_recv, hp.file_size, 0, stats, true);

    // 输出详细统计信息
    std::cout << stats.to_json() << std::endl;

    // 输出人类可读的总结
    std::cout << stats.summary() << std::endl;

    // 5. 校验 MD5
    report_json("status", "message", "Verifying MD5...");
    std::string actual_md5 = calculate_file_md5(save_path);
    std::string expected_md5(hp.md5, 32);
    bool success = (actual_md5 == expected_md5);

    std::cout << "{\"type\":\"verify\", \"success\":" << (success ? "true" : "false")
              << ", \"expected\":\"" << expected_md5 << "\", \"actual\":\"" << actual_md5 << "\"}" << std::endl;

    UDT::close(recver);
    UDT::close(serv);
}

// ==========================================
// 主函数
// ==========================================

int main(int argc, char* argv[]) {
#ifdef _WIN32
    // 强制设置控制台输出代码页为 UTF-8 (65001)
    SetConsoleOutputCP(65001);
#endif
    // 添加命令行参数解析
    bool detailed_stats = false;

    UDT::startup();
    if (argc < 2) return 1;

    std::string mode = argv[1];
    if (mode == "send" && argc >= 5) {
        UDTConfig config;
        // 解析发送端特有的配置
        for (int i = 5; i < argc; ++i) {
            if (std::string(argv[i]) == "--mss") config.mss = std::atoi(argv[++i]);
            else if (std::string(argv[i]) == "--window") config.window_size = std::atoi(argv[++i]);
            else if (std::string(argv[i]) == "--detailed") detailed_stats = true;
        }
        run_sender(argv[2], std::atoi(argv[3]), argv[4], config);
    }
    else if (mode == "recv" && argc >= 4) {
        // 接收端现在不需要在命令行指定 mss 和 window 了
        for (int i = 4; i < argc; ++i) {
            if (std::string(argv[i]) == "--detailed") detailed_stats = true;
        }
        run_receiver(std::atoi(argv[2]), argv[3]);
    }
    else {
        std::cerr << "Usage:" << std::endl;
        std::cerr << "  Send: " << argv[0] <<
            " send <ip> <port> <filepath> [--mss 1500] [--window 1048576] [--detailed]" << std::endl;
        std::cerr << "  Receive: " << argv[0] << " recv <port> <savepath> [--detailed]" << std::endl;
        std::cerr << std::endl;
        std::cerr << "Enhanced Statistics Features:" << std::endl;
        std::cerr << "  - Real-time speed monitoring (min/max/average)" << std::endl;
        std::cerr << "  - Complete packet statistics (sent/received/lost/retransmitted)" << std::endl;
        std::cerr << std::endl;
        std::cerr << "  - Loss rate and retransmission rate calculation" << std::endl;
        std::cerr << std::endl;
        std::cerr << "  - Transfer efficiency analysis (effective packets/total packets)" << std::endl;
        std::cerr << std::endl;
        std::cerr << "  - Time estimation for remaining transfer" << std::endl;
        std::cerr << std::endl;
        std::cerr << "  - Network quality assessment and suggestions" << std::endl;
    }

    UDT::cleanup();
    return 0;
}