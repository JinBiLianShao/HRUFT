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
#include <limits>
#include <unordered_map>
#include <ctime>
#include <thread>
#include <filesystem>
#include <unistd.h>
#include <codecvt>
#include <locale>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <direct.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
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
    char md5[32]; // MD5字符串
    int mss; // 发送端指定的MSS
    int window_size; // 发送端指定的Window Size
    char filename[512]; // 文件名（扩大缓冲区以支持UTF-8编码的中文）
    char file_extension[16]; // 文件扩展名（格式）
    char mime_type[64]; // MIME类型（可选）
    uint8_t has_mime_type; // 是否有MIME类型
    uint8_t filename_encoding; // 文件名编码: 0=UTF-8, 1=GBK, 2=GB2312
};
#pragma pack(pop)

// 传输完成确认包
#pragma pack(push, 1)
struct TransferCompletePacket {
    int32_t status; // 0: 等待MD5校验, 1: MD5校验成功, 2: MD5校验失败
    char md5[32]; // 接收端计算的MD5
    int64_t received_bytes; // 接收端实际接收的字节数
    int64_t missing_count; // 缺失的包数量(如果有)
};
#pragma pack(pop)

struct UDTConfig {
    int mss = 1500;
    int window_size = 10485760; // 默认 1MB
};

// ==========================================
// 路径转换辅助函数（跨平台）
// ==========================================

#ifdef _WIN32
// Windows平台：UTF-8到宽字符转换
std::wstring utf8_to_wstring(const std::string &utf8_str) {
    if (utf8_str.empty()) return L"";

    int wsize = MultiByteToWideChar(CP_UTF8, 0, utf8_str.c_str(), (int) utf8_str.size(), NULL, 0);
    if (wsize == 0) return L"";

    std::wstring wstr(wsize, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8_str.c_str(), (int) utf8_str.size(), &wstr[0], wsize);
    return wstr;
}

// Windows平台：宽字符到UTF-8转换
std::string wstring_to_utf8(const std::wstring &wstr) {
    if (wstr.empty()) return "";

    int utf8_size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int) wstr.size(), NULL, 0, NULL, NULL);
    if (utf8_size == 0) return "";

    std::string utf8_str(utf8_size, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int) wstr.size(), &utf8_str[0], utf8_size, NULL, NULL);
    return utf8_str;
}

// Windows平台：打开UTF-8路径的文件流
// Windows平台：打开UTF-8路径的文件流
std::ifstream open_input_file_utf8(const std::string &utf8_path) {
    std::filesystem::path wpath = std::filesystem::path(utf8_to_wstring(utf8_path));
    return std::ifstream(wpath, std::ios::binary | std::ios::ate);
}

std::ofstream open_output_file_utf8(const std::string &utf8_path) {
    std::filesystem::path wpath = std::filesystem::path(utf8_to_wstring(utf8_path));
    return std::ofstream(wpath, std::ios::binary);
}

// 检查文件是否存在（UTF-8路径）
bool file_exists_utf8(const std::string &utf8_path) {
    std::wstring wpath = utf8_to_wstring(utf8_path);
    DWORD attr = GetFileAttributesW(wpath.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
}

// 检查目录是否存在（UTF-8路径）
bool dir_exists_utf8(const std::string &utf8_path) {
    std::wstring wpath = utf8_to_wstring(utf8_path);
    DWORD attr = GetFileAttributesW(wpath.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY));
}

// 创建目录（UTF-8路径）
bool create_directories_utf8(const std::string &utf8_path) {
    std::wstring wpath = utf8_to_wstring(utf8_path);
    return CreateDirectoryW(wpath.c_str(), NULL) != 0;
}

#else
// Linux/macOS平台：直接使用UTF-8路径
std::ifstream open_input_file_utf8(const std::string &utf8_path) {
    return std::ifstream(utf8_path.c_str(), std::ios::binary | std::ios::ate);
}

std::ofstream open_output_file_utf8(const std::string &utf8_path) {
    return std::ofstream(utf8_path.c_str(), std::ios::binary);
}

bool file_exists_utf8(const std::string &utf8_path) {
    struct stat buffer;
    return (stat(utf8_path.c_str(), &buffer) == 0 && S_ISREG(buffer.st_mode));
}

bool dir_exists_utf8(const std::string &utf8_path) {
    struct stat buffer;
    return (stat(utf8_path.c_str(), &buffer) == 0 && S_ISDIR(buffer.st_mode));
}

bool create_directories_utf8(const std::string &utf8_path) {
    std::error_code ec;
    return std::filesystem::create_directories(utf8_path, ec);
}
#endif

// ==========================================
// 传输统计结构
// ==========================================

struct TransferStatistics {
    // 角色标识
    bool is_sender = false;

    // 基础信息
    long long total_bytes = 0;
    long long transferred_bytes = 0;
    long long bytes_since_last_report = 0;
    long long initial_bytes_to_ignore = 0;

    // 时间统计
    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point end_time;
    std::chrono::steady_clock::time_point last_report_time;
    std::chrono::steady_clock::time_point last_speed_calc_time;

    // 速度统计
    double current_speed_mbps = 0.0;
    double average_speed_mbps = 0.0;
    double max_speed_mbps = 0.0;
    double min_speed_mbps = std::numeric_limits<double>::max();

    // 平滑速度计算的队列
    std::queue<double> speed_history;
    const int SPEED_HISTORY_SIZE = 3;

    // UDT性能统计（通过UDT API获取）
    // 全局测量值
    int64_t pktSentTotal = 0; // 总发送数据包数
    int64_t pktRecvTotal = 0; // 总接收数据包数
    int pktSndLossTotal = 0; // 发送端丢失的数据包数
    int pktRcvLossTotal = 0; // 接收端丢失的数据包数
    int pktRetransTotal = 0; // 重传的数据包数
    int pktSentACKTotal = 0; // 发送的ACK包数
    int pktRecvACKTotal = 0; // 接收的ACK包数
    int pktSentNAKTotal = 0; // 发送的NAK包数
    int pktRecvNAKTotal = 0; // 接收的NAK包数
    int64_t usSndDurationTotal = 0; // 总发送时间（微秒）

    // 新增：估计的数据包数（基于文件大小和MSS）
    int64_t estimated_data_packets = 0;
    int64_t actual_data_bytes_received = 0;

    // 即时性能统计
    int64_t pktSent = 0; // 本地发送包数
    int64_t pktRecv = 0; // 本地接收包数
    int pktSndLoss = 0; // 本地发送丢包
    int pktRcvLoss = 0; // 本地接收丢包
    int pktRetrans = 0; // 本地重传
    double mbpsSendRate = 0.0; // UDT报告的发送速率（Mbps）
    double mbpsRecvRate = 0.0; // UDT报告的接收速率（Mbps）
    int64_t usSndDuration = 0; // 本地发送时间

    // 计算出的统计
    double data_packet_loss_rate = 0.0; // 数据包丢包率
    double control_overhead_ratio = 0.0; // 控制包开销比例
    double network_efficiency = 0.0; // 网络传输效率
    double data_integrity_score = 0.0; // 数据完整性评分

    // 传输开始时间戳
    std::chrono::steady_clock::time_point first_speed_calc_time;
    bool first_speed_calculated = false;

    // 初始化统计
    void init(long long total_size, bool sender = false) {
        is_sender = sender;
        total_bytes = total_size;
        transferred_bytes = 0;
        bytes_since_last_report = 0;
        initial_bytes_to_ignore = 0;
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

        estimated_data_packets = 0;
        actual_data_bytes_received = 0;

        pktSent = 0;
        pktRecv = 0;
        pktSndLoss = 0;
        pktRcvLoss = 0;
        pktRetrans = 0;
        mbpsSendRate = 0.0;
        mbpsRecvRate = 0.0;
        usSndDuration = 0;

        data_packet_loss_rate = 0.0;
        control_overhead_ratio = 0.0;
        network_efficiency = 0.0;
        data_integrity_score = 0.0;
    }

    // 更新速度统计
    void update_speed(long long bytes_transferred) {
        auto now = std::chrono::steady_clock::now();
        bytes_since_last_report += bytes_transferred;
        transferred_bytes += bytes_transferred;

        // 忽略前200ms的数据，避免初始速度计算异常
        auto time_since_start = std::chrono::duration_cast<std::chrono::milliseconds>(now - first_speed_calc_time).
                count();
        if (!first_speed_calculated) {
            if (time_since_start < 200) {
                initial_bytes_to_ignore += bytes_transferred;
                return;
            } else {
                first_speed_calculated = true;
                // 重置bytes_since_last_report，减去已经忽略的字节
                bytes_since_last_report = 0;
            }
        }

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
                    if (current_speed_mbps > max_speed_mbps && current_speed_mbps < 10000) {
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
            // 从总传输字节中减去初始忽略的字节
            long long effective_bytes = transferred_bytes - initial_bytes_to_ignore;
            if (effective_bytes < 0) effective_bytes = 0;
            average_speed_mbps = (effective_bytes * 8.0) / (total_time / 1000.0) / (1024.0 * 1024.0);
        }
    }

    // 设置MSS用于估计数据包数
    void set_mss_for_estimation(int mss) {
        if (total_bytes > 0 && mss > 0) {
            // 估计数据包数：文件大小 / MSS
            estimated_data_packets = (total_bytes + mss - 1) / mss; // 向上取整
        }
    }

    // 获取UDT性能统计并计算相关指标
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

            // UDT报告的速率
            mbpsSendRate = perf.mbpsSendRate;
            mbpsRecvRate = perf.mbpsRecvRate;

            usSndDuration = perf.usSndDuration;

            // 计算各项统计指标
            calculate_derived_stats();
        }
    }

    // 计算派生统计指标
    void calculate_derived_stats() {
        if (is_sender) {
            // 发送端统计
            if (pktSentTotal > 0) {
                // 数据包丢包率：发送端报告的丢包数 / 发送的总包数
                data_packet_loss_rate = (pktSndLossTotal * 100.0) / pktSentTotal;

                // 控制包开销比例：(ACK+NAK接收数) / 总发送包数
                int64_t control_packets_received = pktRecvACKTotal + pktRecvNAKTotal;
                control_overhead_ratio = (control_packets_received * 100.0) / pktSentTotal;

                // 网络传输效率：(总发送包数 - 重传包数) / 总发送包数
                network_efficiency = ((pktSentTotal - pktRetransTotal) * 100.0) / pktSentTotal;

                // 数据完整性评分
                data_integrity_score = 100.0 - (data_packet_loss_rate + (pktRetransTotal * 100.0 / pktSentTotal) / 2.0);
                data_integrity_score = std::max(0.0, std::min(100.0, data_integrity_score));
            }
        } else {
            // 接收端统计 - 基于UDT实际统计数据
            // UDT的pktRecvTotal包括所有接收到的包（数据包+控制包）
            // 但接收端主要处理数据包，控制包开销体现在发送的ACK/NAK中

            // 数据包丢包率：接收端报告的丢包数 / (接收包数 + 报告丢包数)
            if ((pktRecvTotal + pktRcvLossTotal) > 0) {
                data_packet_loss_rate = (pktRcvLossTotal * 100.0) / (pktRecvTotal + pktRcvLossTotal);
            }

            // 控制包开销：发送的ACK+NAK包 / 接收包数
            if (pktRecvTotal > 0) {
                control_overhead_ratio = ((pktSentACKTotal + pktSentNAKTotal) * 100.0) / pktRecvTotal;
            }

            // 网络传输效率：基于丢包率
            network_efficiency = 100.0 - data_packet_loss_rate;

            // 数据完整性评分
            data_integrity_score = 100.0 - data_packet_loss_rate;
            data_integrity_score = std::max(0.0, std::min(100.0, data_integrity_score));
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
        ss << "\"role\":\"" << (is_sender ? "sender" : "receiver") << "\",";
        ss << "\"total_bytes\":" << total_bytes << ",";
        ss << "\"transferred_bytes\":" << transferred_bytes << ",";
        ss << "\"completion_percentage\":" << (total_bytes > 0 ? (transferred_bytes * 100.0 / total_bytes) : 0) << ",";
        ss << "\"total_time_seconds\":" << total_seconds << ",";
        ss << "\"average_speed_mbps\":" << average_speed_mbps << ",";
        ss << "\"max_speed_mbps\":" << max_speed_mbps << ",";
        ss << "\"min_speed_mbps\":" << (min_speed_mbps < std::numeric_limits<double>::max() ? min_speed_mbps : 0) <<
                ",";
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

        // UDT原始包统计（包含所有包）
        ss << "\"udt_raw_packet_stats\":{";
        ss << "\"total_sent_packets\":" << pktSentTotal << ",";
        ss << "\"total_received_packets\":" << pktRecvTotal << ",";
        ss << "\"data_packets_lost_at_sender\":" << pktSndLossTotal << ",";
        ss << "\"data_packets_lost_at_receiver\":" << pktRcvLossTotal << ",";
        ss << "\"retransmitted_packets\":" << pktRetransTotal << ",";
        ss << "\"ack_packets_sent\":" << pktSentACKTotal << ",";
        ss << "\"ack_packets_received\":" << pktRecvACKTotal << ",";
        ss << "\"nak_packets_sent\":" << pktSentNAKTotal << ",";
        ss << "\"nak_packets_received\":" << pktRecvNAKTotal << ",";
        ss << "\"estimated_data_packets\":" << estimated_data_packets;
        ss << "},";

        // 计算出的统计指标（基于分析）
        ss << "\"network_analysis\":{";
        ss << "\"data_packet_loss_rate\":" << data_packet_loss_rate << ",";
        ss << "\"control_packet_overhead_ratio\":" << control_overhead_ratio << ",";
        ss << "\"network_transmission_efficiency\":" << network_efficiency << ",";
        ss << "\"data_integrity_score\":" << data_integrity_score;
        ss << "},";

        // 即时性能
        ss << "\"instant_performance\":{";
        ss << "\"packets_sent_last_period\":" << pktSent << ",";
        ss << "\"packets_received_last_period\":" << pktRecv << ",";
        ss << "\"udt_send_rate_mbps\":" << mbpsSendRate << ",";
        ss << "\"udt_receive_rate_mbps\":" << mbpsRecvRate;
        ss << "},";

        // 网络质量评估
        std::string network_quality = "excellent";
        std::string recommendations = "";

        if (data_packet_loss_rate > 10) {
            network_quality = "poor";
            recommendations =
                    "High data loss (>10%). Consider reducing window size, increasing MSS, or checking network stability.";
        } else if (data_packet_loss_rate > 5) {
            network_quality = "fair";
            recommendations =
                    "Moderate data loss (5-10%). Network may be congested. Consider adjusting UDT parameters.";
        } else if (data_packet_loss_rate > 2) {
            network_quality = "good";
            recommendations = "Low data loss (2-5%). Network conditions are acceptable.";
        } else {
            network_quality = "excellent";
            recommendations = "Minimal data loss (<2%). Network conditions are excellent.";
        }

        if (control_overhead_ratio > 50) {
            network_quality = (network_quality == "excellent" ? "good" : network_quality);
            recommendations += " High control overhead. Consider adjusting UDT flow control parameters.";
        }

        ss << "\"network_quality_assessment\":{";
        ss << "\"quality_level\":\"" << network_quality << "\",";
        ss << "\"recommendations\":\"" << recommendations << "\",";
        ss << "\"data_transfer_successful\":true";
        ss << "}";

        ss << "}";

        return ss.str();
    }

    // 生成简洁的总结报告
    std::string summary() const {
        auto total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
        double total_seconds = total_duration / 1000.0;

        std::stringstream ss;
        ss << std::fixed << std::setprecision(2);
        ss << "传输完成！" << (is_sender ? "发送端" : "接收端") << "详细统计：" << std::endl;
        ss << "  文件大小: " << (total_bytes / 1024.0 / 1024.0) << " MB" << std::endl;
        ss << "  传输时间: " << total_seconds << " 秒" << std::endl;
        ss << "  平均速度: " << average_speed_mbps << " Mbps" << std::endl;
        ss << "  最高速度: " << max_speed_mbps << " Mbps" << std::endl;
        ss << "  最低速度: " << (min_speed_mbps < std::numeric_limits<double>::max() ? min_speed_mbps : 0) << " Mbps" <<
                std::endl;
        ss << std::endl;

        ss << "  UDT原始包统计（包含所有包类型/数据源来自于UDT4库内部结构体统计）:" << std::endl;
        ss << "    总发送包数: " << pktSentTotal << " (数据包+控制包)" << std::endl;
        ss << "    总接收包数: " << pktRecvTotal << " (数据包+控制包)" << std::endl;
        ss << "    发送端丢失数据包: " << pktSndLossTotal << std::endl;
        ss << "    接收端丢失数据包: " << pktRcvLossTotal << std::endl;
        ss << "    重传数据包数: " << pktRetransTotal << std::endl;
        ss << "    发送ACK包数: " << pktSentACKTotal << std::endl;
        ss << "    接收ACK包数: " << pktRecvACKTotal << std::endl;
        ss << "    发送NAK包数: " << pktSentNAKTotal << std::endl;
        ss << "    接收NAK包数: " << pktRecvNAKTotal << std::endl;
        if (estimated_data_packets > 0) {
            ss << "    估计的数据包数: " << estimated_data_packets << " (自己算的，基于文件大小和MSS)" << std::endl;
        }
        ss << std::endl;

        ss << "  网络分析指标:" << std::endl;
        ss << "    数据包丢包率: " << data_packet_loss_rate << "%" << std::endl;
        ss << "    控制包开销比例: " << control_overhead_ratio << "%" << std::endl;
        ss << "    网络传输效率: " << network_efficiency << "%" << std::endl;
        ss << "    数据完整性评分: " << data_integrity_score << "/100" << std::endl;
        ss << std::endl;

        ss << "  即时性能（最后统计周期）:" << std::endl;
        ss << "    发送包数: " << pktSent << std::endl;
        ss << "    接收包数: " << pktRecv << std::endl;
        ss << "    UDT报告发送速率: " << mbpsSendRate << " Mbps" << std::endl;
        ss << "    UDT报告接收速率: " << mbpsRecvRate << " Mbps" << std::endl;

        return ss.str();
    }
};

// ==========================================
// 辅助工具
// ==========================================

// 获取文件MIME类型
std::string get_mime_type_from_extension(const std::string &extension) {
    static const std::unordered_map<std::string, std::string> mime_map = {
        {".txt", "text/plain"},
        {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"},
        {".png", "image/png"},
        {".pdf", "application/pdf"},
        {".zip", "application/zip"},
        {".rar", "application/x-rar-compressed"},
        {".7z", "application/x-7z-compressed"},
        {".tar", "application/x-tar"},
        {".gz", "application/gzip"},
        {".bz2", "application/x-bzip2"},
        {".mp4", "video/mp4"},
        {".avi", "video/x-msvideo"},
        {".mkv", "video/x-matroska"},
        {".mp3", "audio/mpeg"},
        {".wav", "audio/wav"},
        {".flac", "audio/flac"},
        {".doc", "application/msword"},
        {".docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
        {".xls", "application/vnd.ms-excel"},
        {".xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
        {".ppt", "application/vnd.ms-powerpoint"},
        {".pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
        {".html", "text/html"},
        {".htm", "text/html"},
        {".css", "text/css"},
        {".js", "application/javascript"},
        {".json", "application/json"},
        {".xml", "application/xml"},
        {".csv", "text/csv"},
        {".exe", "application/x-msdownload"},
        {".dll", "application/x-msdownload"},
        {".so", "application/x-sharedlib"},
        {".deb", "application/x-debian-package"},
        {".rpm", "application/x-rpm"},
        {".iso", "application/x-iso9660-image"}
    };

    std::string ext_lower = extension;
    std::transform(ext_lower.begin(), ext_lower.end(), ext_lower.begin(), ::tolower);

    auto it = mime_map.find(ext_lower);
    return (it != mime_map.end()) ? it->second : "application/octet-stream";
}

void report_json(const std::string &type, const std::string &key, const std::string &val) {
    std::cout << "{\"type\":\"" << type << "\", \"" << key << "\":\"" << val << "\"}" << std::endl;
}

void report_progress(long long current, long long total, double speed_mbps,
                     const TransferStatistics &stats, bool detailed = false) {
    int percent = (total > 0) ? (int) ((current * 100) / total) : 0;
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

// 使用UTF-8路径计算文件MD5
std::string calculate_file_md5_utf8(const std::string &utf8_filepath) {
#ifdef _WIN32
    // 修改这里：使用 std::filesystem::path 来转换宽字符串路径
    std::filesystem::path wpath = std::filesystem::path(utf8_to_wstring(utf8_filepath));
    std::ifstream file(wpath, std::ios::binary);
#else
    std::ifstream file(utf8_filepath, std::ios::binary);
#endif

    if (!file) return "";

    md5_state_t state;
    md5_byte_t digest[16];
    md5_init(&state);
    const size_t buffer_size = 10 * 1024 * 1024;
    std::vector<char> buffer(buffer_size);

    while (file.good()) {
        file.read(buffer.data(), buffer_size);
        if (file.gcount() > 0) md5_append(&state, (const md5_byte_t *) buffer.data(), (int) file.gcount());
    }

    md5_finish(&state, digest);
    std::stringstream ss;
    for (int i = 0; i < 16; ++i) ss << std::hex << std::setw(2) << std::setfill('0') << (int) digest[i];
    return ss.str();
}

// 动态配置 Socket 参数
void apply_socket_opts(UDTSOCKET sock, int mss, int win_size) {
    UDT::setsockopt(sock, 0, UDT_MSS, &mss, sizeof(int));
    int fc = (win_size / mss) * 2;
    if (fc < 512000) fc = 512000;
    UDT::setsockopt(sock, 0, UDT_FC, &fc, sizeof(int));
    UDT::setsockopt(sock, 0, UDT_SNDBUF, &win_size, sizeof(int));
    UDT::setsockopt(sock, 0, UDT_RCVBUF, &win_size, sizeof(int));
    UDT::setsockopt(sock, 0, UDP_SNDBUF, &win_size, sizeof(int));
    UDT::setsockopt(sock, 0, UDP_RCVBUF, &win_size, sizeof(int));
    // =========================
    // 控制阶段：阻塞模式
    // =========================
    // bool block = true;
    // UDT::setsockopt(sock, 0, UDT_SNDSYN, &block, sizeof(block));
    // UDT::setsockopt(sock, 0, UDT_RCVSYN, &block, sizeof(block));
}

// 确保目录存在（使用UTF-8路径）
bool ensure_directory_exists_utf8(const std::string &utf8_dir_path) {
#ifdef _WIN32
    std::wstring wpath = utf8_to_wstring(utf8_dir_path);
    if (wpath.empty()) return false;

    // 逐级创建目录
    size_t pos = 0;
    do {
        pos = wpath.find_first_of(L"\\/", pos + 1);
        std::wstring subpath = wpath.substr(0, pos);

        if (subpath.length() > 2 && subpath[1] == L':') {
            // 跳过盘符
            continue;
        }

        DWORD attr = GetFileAttributesW(subpath.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) {
            if (!CreateDirectoryW(subpath.c_str(), NULL)) {
                DWORD err = GetLastError();
                if (err != ERROR_ALREADY_EXISTS) {
                    report_json("error", "message", "Failed to create directory: error " + std::to_string(err));
                    return false;
                }
            }
        }
    } while (pos != std::wstring::npos);

    return true;
#else
    try {
        if (!std::filesystem::exists(utf8_dir_path)) {
            return std::filesystem::create_directories(utf8_dir_path);
        }
        return true;
    } catch (const std::exception &e) {
        report_json("error", "message", "Failed to create directory: " + std::string(e.what()));
        return false;
    }
#endif
}

// 获取文件大小（使用UTF-8路径）
long long get_file_size_utf8(const std::string &utf8_filepath) {
#ifdef _WIN32
    std::wstring wpath = utf8_to_wstring(utf8_filepath);
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(wpath.c_str(), GetFileExInfoStandard, &fad)) {
        LARGE_INTEGER size;
        size.HighPart = fad.nFileSizeHigh;
        size.LowPart = fad.nFileSizeLow;
        return size.QuadPart;
    }
    return -1;
#else
    struct stat stat_buf;
    int rc = stat(utf8_filepath.c_str(), &stat_buf);
    return rc == 0 ? stat_buf.st_size : -1;
#endif
}

// ==========================================
// 发送端逻辑
// ==========================================

void run_sender(const char *ip, int port, const char *filepath, const UDTConfig &config) {
    // 将文件路径转换为UTF-8（如果命令行参数是GBK编码）
    std::string utf8_filepath;
#ifdef _WIN32
    // Windows命令行参数通常是本地编码（GBK），需要转换为UTF-8
    int wsize = MultiByteToWideChar(CP_ACP, 0, filepath, -1, NULL, 0);
    if (wsize > 0) {
        std::wstring wstr(wsize, 0);
        MultiByteToWideChar(CP_ACP, 0, filepath, -1, &wstr[0], wsize);
        // 移除末尾的null字符
        if (!wstr.empty() && wstr.back() == L'\0') {
            wstr.pop_back();
        }
        utf8_filepath = wstring_to_utf8(wstr);
    } else {
        utf8_filepath = filepath; // 如果转换失败，使用原始路径
    }
#else
    utf8_filepath = filepath; // Linux/macOS直接使用UTF-8
#endif

    report_json("status", "message", "Calculating MD5...");
    std::string local_md5 = calculate_file_md5_utf8(utf8_filepath);
    if (local_md5.empty()) {
        report_json("error", "message", "File error or not found: " + utf8_filepath);
        return;
    }

    UDTSOCKET client = UDT::socket(AF_INET, SOCK_STREAM, 0);
    if (client == UDT::INVALID_SOCK) {
        report_json("error", "message", "Failed to create UDT socket");
        return;
    }

    // 连接前先应用本地配置
    apply_socket_opts(client, config.mss, config.window_size);

    sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &serv_addr.sin_addr) <= 0) {
        report_json("error", "message", "Invalid IP address");
        UDT::close(client);
        return;
    }

    report_json("status", "message", "Connecting...");
    if (UDT::ERROR == UDT::connect(client, (sockaddr *) &serv_addr, sizeof(serv_addr))) {
        report_json("error", "message", UDT::getlasterror().getErrorMessage());
        UDT::close(client);
        return;
    }

    // 使用UTF-8路径打开文件
    auto ifs = open_input_file_utf8(utf8_filepath);
    if (!ifs) {
        report_json("error", "message", "Cannot open file for reading: " + utf8_filepath);
        UDT::close(client);
        return;
    }

    long long f_size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);

    // 提取文件信息（使用UTF-8）
    std::string filename;
    std::string extension;

#ifdef _WIN32
    std::wstring wpath = utf8_to_wstring(utf8_filepath);
    std::filesystem::path file_path(wpath);
#else
    std::filesystem::path file_path(utf8_filepath);
#endif

    filename = file_path.filename().string(); // 保持UTF-8编码
    extension = file_path.extension().string();
    std::string mime_type = get_mime_type_from_extension(extension);

    // 确保不会溢出
    if (filename.length() >= 512) {
        filename = filename.substr(0, 511);
    }
    if (extension.length() >= 16) {
        extension = extension.substr(0, 15);
    }
    if (mime_type.length() >= 64) {
        mime_type = mime_type.substr(0, 63);
    }

    // 报告文件信息
    std::stringstream file_info;
    file_info << "Sending file: " << filename << " (" << (f_size / 1024.0 / 1024.0) << " MB)";
    if (!extension.empty()) file_info << " Type: " << extension;
    if (!mime_type.empty()) file_info << " MIME: " << mime_type;
    report_json("status", "message", file_info.str());

    // 初始化传输统计
    TransferStatistics stats;
    stats.init(f_size, true); // 设置为发送端
    stats.set_mss_for_estimation(config.mss);

    // 1. 发送握手协商包
    HandshakePacket hp;
    hp.file_size = f_size;
    hp.mss = config.mss;
    hp.window_size = config.window_size;
    memcpy(hp.md5, local_md5.c_str(), 32);

    // 清空字符串字段
    memset(hp.filename, 0, sizeof(hp.filename));
    memset(hp.file_extension, 0, sizeof(hp.file_extension));
    memset(hp.mime_type, 0, sizeof(hp.mime_type));

    // 复制文件信息（UTF-8编码）
    strncpy(hp.filename, filename.c_str(), sizeof(hp.filename) - 1);
    strncpy(hp.file_extension, extension.c_str(), sizeof(hp.file_extension) - 1);
    strncpy(hp.mime_type, mime_type.c_str(), sizeof(hp.mime_type) - 1);
    hp.has_mime_type = !mime_type.empty() ? 1 : 0;
    hp.filename_encoding = 0; // UTF-8编码

    if (UDT::ERROR == UDT::send(client, (char *) &hp, sizeof(HandshakePacket), 0)) {
        report_json("error", "message", "Failed to send handshake packet");
        UDT::close(client);
        return;
    }
    stats.transferred_bytes += sizeof(HandshakePacket);

    // 2. 开始传输数据
    std::vector<char> buffer(1024 * 1024 * 8);
    long long total_sent = 0;
    auto last_time = std::chrono::steady_clock::now();
    long long last_bytes = 0;

    // 记录开始传输时间
    stats.start_time = std::chrono::steady_clock::now();

    while (total_sent < f_size) {
        ifs.read(buffer.data(), buffer.size());
        int read_len = (int) ifs.gcount();
        if (read_len <= 0) break;

        int offset = 0;
        while (offset < read_len) {
            int sent = UDT::send(client, buffer.data() + offset, read_len - offset, 0);
            if (sent <= 0) {
                report_json("error", "message", "Failed to send data");
                break;
            }
            offset += sent;
            total_sent += sent;

            // 更新速度统计（累积字节数）
            stats.update_speed(sent);
        }

        if (offset < read_len) {
            // 发送失败，退出循环
            break;
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

    // 检查是否传输完成
    if (total_sent < f_size) {
        report_json("error", "message",
                    "File transmission incomplete: " + std::to_string(total_sent) + "/" + std::to_string(f_size));
        ifs.close();
        UDT::close(client);
        return;
    }

    // 文件数据发送完成
    report_json("status", "message", "File data transmission complete, sending EOF marker...");

    // 发送文件传输结束标记
    const char *eof_marker = "FILE_TRANSMISSION_COMPLETE_EOF";
    if (UDT::ERROR == UDT::send(client, eof_marker, strlen(eof_marker) + 1, 0)) {
        report_json("warning", "message", "Failed to send EOF marker");
    } else {
        report_json("status", "message", "EOF marker sent, waiting for receiver to finish...");
    }

    // 等待接收端完成接收并返回MD5校验结果
    TransferCompletePacket tcp;

    int recv_result = UDT::recv(client, (char *) &tcp, sizeof(TransferCompletePacket), 0);

    // 记录结束时间
    stats.end_time = std::chrono::steady_clock::now();
    stats.transferred_bytes = total_sent;

    if (recv_result == sizeof(TransferCompletePacket)) {
        // 收集最终统计信息
        stats.collect_udt_stats(client);

        std::stringstream result_info;
        result_info << "Receiver MD5 verification ";

        if (tcp.status == 1) {
            result_info << "PASSED! Received: " << tcp.received_bytes << " bytes";
            report_json("success", "message", result_info.str());

            // 比较MD5
            std::string expected_md5(local_md5);
            std::string actual_md5(tcp.md5, 32);

            std::cout << "{\"type\":\"verify\", \"success\":true"
                    << ", \"expected\":\"" << expected_md5
                    << "\", \"actual\":\"" << actual_md5
                    << "\", \"bytes_sent\":" << total_sent
                    << ", \"bytes_received\":" << tcp.received_bytes
                    << "}" << std::endl;
        } else if (tcp.status == 2) {
            result_info << "FAILED! Expected: " << local_md5
                    << ", Got: " << std::string(tcp.md5, 32);
            report_json("error", "message", result_info.str());

            std::cout << "{\"type\":\"verify\", \"success\":false"
                    << ", \"expected\":\"" << local_md5
                    << "\", \"actual\":\"" << std::string(tcp.md5, 32)
                    << "\", \"bytes_sent\":" << total_sent
                    << ", \"bytes_received\":" << tcp.received_bytes
                    << "}" << std::endl;
        } else {
            report_json("warning", "message", "Receiver verification status unknown: " + std::to_string(tcp.status));
        }

        // 发送统计信息给接收端
        std::string stats_json = stats.to_json();
        if (UDT::ERROR == UDT::send(client, stats_json.c_str(), (int) stats_json.length(), 0)) {
            report_json("warning", "message", "Failed to send statistics to receiver");
        }

        // 等待接收端的关闭确认
        char close_ack[32];
        int ack_size = UDT::recv(client, close_ack, sizeof(close_ack), 0);
        if (ack_size > 0) {
            std::string ack_msg(close_ack, ack_size);
            if (ack_msg == "RECEIVER_CLOSE_OK") {
                report_json("status", "message", "Receiver ready to close connection");
            } else {
                report_json("warning", "message", "Unexpected close ack: " + ack_msg);
            }
        }

        // 输出详细统计信息
        std::cout << stats.to_json() << std::endl;
        std::cout << stats.summary() << std::endl;
    } else {
        if (recv_result == UDT::ERROR) {
            report_json("warning", "message", "No verification response from receiver (timeout or error)");
        } else {
            report_json("warning", "message",
                        "Partial verification response from receiver, size: " + std::to_string(recv_result));
        }

        // 收集统计信息
        stats.collect_udt_stats(client);

        // 输出详细统计信息
        std::cout << stats.to_json() << std::endl;
        std::cout << stats.summary() << std::endl;
    }

    // 优雅关闭连接
    report_json("status", "message", "Closing sender connection gracefully...");

    // 设置linger选项确保所有数据发送完成
    bool linger = true;
    int linger_time = 5000;
    UDT::setsockopt(client, 0, UDT_LINGER, &linger, sizeof(bool));

    // 等待一小段时间让接收端处理
#ifdef _WIN32
    Sleep(100);
#else
    usleep(100 * 1000);
#endif

    // 关闭连接
    UDT::close(client);
    ifs.close();

    report_json("status", "message", "Sender connection closed.");
}

// ==========================================
// 接收端逻辑
// ==========================================

void run_receiver(int port, const char *save_path) {
    // 将保存路径转换为UTF-8
    std::string utf8_save_path;
#ifdef _WIN32
    // Windows命令行参数通常是本地编码（GBK），需要转换为UTF-8
    int wsize = MultiByteToWideChar(CP_ACP, 0, save_path, -1, NULL, 0);
    if (wsize > 0) {
        std::wstring wstr(wsize, 0);
        MultiByteToWideChar(CP_ACP, 0, save_path, -1, &wstr[0], wsize);
        // 移除末尾的null字符
        if (!wstr.empty() && wstr.back() == L'\0') {
            wstr.pop_back();
        }
        utf8_save_path = wstring_to_utf8(wstr);
    } else {
        utf8_save_path = save_path; // 如果转换失败，使用原始路径
    }
#else
    utf8_save_path = save_path; // Linux/macOS直接使用UTF-8
#endif

    UDTSOCKET serv = UDT::socket(AF_INET, SOCK_STREAM, 0);
    if (serv == UDT::INVALID_SOCK) {
        report_json("error", "message", "Failed to create UDT server socket");
        return;
    }

    sockaddr_in my_addr;
    my_addr.sin_family = AF_INET;
    my_addr.sin_port = htons(port);
    my_addr.sin_addr.s_addr = INADDR_ANY;

    if (UDT::ERROR == UDT::bind(serv, (sockaddr *) &my_addr, sizeof(my_addr))) {
        report_json("error", "message", "Failed to bind socket: " + std::string(UDT::getlasterror().getErrorMessage()));
        UDT::close(serv);
        return;
    }

    if (UDT::ERROR == UDT::listen(serv, 5)) {
        report_json("error", "message", "Failed to listen on socket");
        UDT::close(serv);
        return;
    }

    report_json("status", "message", "Waiting for sender on port " + std::to_string(port) + "...");

    sockaddr_in client_addr;
    int namelen = sizeof(client_addr);
    UDTSOCKET recver = UDT::accept(serv, (sockaddr *) &client_addr, &namelen);
    if (recver == UDT::INVALID_SOCK) {
        report_json("error", "message", "Failed to accept connection");
        UDT::close(serv);
        return;
    }

    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
    report_json("status", "message", "Connected to sender: " + std::string(client_ip));

    // 1. 接收握手包
    HandshakePacket hp;
    if (UDT::ERROR == UDT::recv(recver, (char *) &hp, sizeof(HandshakePacket), 0)) {
        report_json("error", "message", "Failed to receive handshake packet");
        UDT::close(recver);
        UDT::close(serv);
        return;
    }

    // 2. 根据发送端的建议动态调整接收端 Socket
    report_json("status", "message",
                "Syncing config from sender: MSS=" + std::to_string(hp.mss) + ", WIN=" + std::to_string(
                    hp.window_size));
    apply_socket_opts(recver, hp.mss, hp.window_size);

    // 从握手包获取文件信息
    std::string received_filename = std::string(hp.filename, strnlen(hp.filename, sizeof(hp.filename)));
    std::string received_extension = std::string(hp.file_extension,
                                                 strnlen(hp.file_extension, sizeof(hp.file_extension)));
    std::string mime_type = hp.has_mime_type
                                ? std::string(hp.mime_type, strnlen(hp.mime_type, sizeof(hp.mime_type)))
                                : "";

    // 构建保存路径
    std::string final_save_path;

    // 检查用户指定的是目录还是文件路径
    bool is_directory = false;

    // 检查路径是否存在且是目录
    if (dir_exists_utf8(utf8_save_path)) {
        is_directory = true;
    } else if (file_exists_utf8(utf8_save_path)) {
        is_directory = false; // 是文件
    } else {
        // 路径不存在，检查是否有扩展名来判断
#ifdef _WIN32
        std::wstring wpath = utf8_to_wstring(utf8_save_path);
        std::filesystem::path path_obj(wpath);
#else
        std::filesystem::path path_obj(utf8_save_path);
#endif
        is_directory = path_obj.extension().empty();
    }

    if (is_directory) {
        // 用户指定的是目录
        if (!ensure_directory_exists_utf8(utf8_save_path)) {
            UDT::close(recver);
            UDT::close(serv);
            return;
        }

        // 如果文件名为空，生成一个默认名
        if (received_filename.empty()) {
            std::time_t now = std::time(nullptr);
            received_filename = "received_file_" + std::to_string(now);
        }

        // 构建完整路径
#ifdef _WIN32
        std::wstring w_save_path = utf8_to_wstring(utf8_save_path);
        std::wstring w_filename;

        // 根据编码转换文件名
        if (hp.filename_encoding == 0) {
            // UTF-8编码
            w_filename = utf8_to_wstring(received_filename);
        } else {
            // 其他编码（如GBK），在Windows上可能需要特殊处理
            // 这里假设文件名已经是当前系统编码
            int wsize = MultiByteToWideChar(CP_ACP, 0, received_filename.c_str(), -1, NULL, 0);
            if (wsize > 0) {
                w_filename.resize(wsize);
                MultiByteToWideChar(CP_ACP, 0, received_filename.c_str(), -1, &w_filename[0], wsize);
                if (!w_filename.empty() && w_filename.back() == L'\0') {
                    w_filename.pop_back();
                }
            } else {
                w_filename = L"received_file";
            }
        }

        // 如果有扩展名但文件名中没有，添加扩展名
        if (!received_extension.empty()) {
            std::wstring w_extension = utf8_to_wstring(received_extension);
            std::filesystem::path filename_path(w_filename);
            if (filename_path.extension().empty()) {
                w_filename += w_extension;
            }
        }

        std::filesystem::path final_path = std::filesystem::path(w_save_path) / std::filesystem::path(w_filename);
        final_save_path = wstring_to_utf8(final_path.wstring());
#else
        // Linux/macOS：直接使用UTF-8
        std::filesystem::path final_path = std::filesystem::path(utf8_save_path) / std::filesystem::path(
                                               received_filename);

        // 如果有扩展名但文件名中没有，添加扩展名
        if (!received_extension.empty()) {
            std::filesystem::path filename_path(received_filename);
            if (filename_path.extension().empty()) {
                final_path.replace_extension(received_extension);
            }
        }

        final_save_path = final_path.string();
#endif
    } else {
        // 用户指定的是具体文件路径
        final_save_path = utf8_save_path;

        // 如果没有扩展名而我们有，添加扩展名
        if (!received_extension.empty()) {
#ifdef _WIN32
            std::wstring w_path = utf8_to_wstring(final_save_path);
            std::filesystem::path path_obj(w_path);
            if (path_obj.extension().empty()) {
                std::wstring w_extension = utf8_to_wstring(received_extension);
                path_obj.replace_extension(w_extension);
                final_save_path = wstring_to_utf8(path_obj.wstring());
            }
#else
            std::filesystem::path path_obj(final_save_path);
            if (path_obj.extension().empty()) {
                path_obj.replace_extension(received_extension);
                final_save_path = path_obj.string();
            }
#endif
        }
    }

    // 确保目录存在（如果是嵌套目录）
    try {
#ifdef _WIN32
        std::wstring w_final_path = utf8_to_wstring(final_save_path);
        std::filesystem::path parent_dir = std::filesystem::path(w_final_path).parent_path();
        if (!parent_dir.empty()) {
            ensure_directory_exists_utf8(wstring_to_utf8(parent_dir.wstring()));
        }
#else
        std::filesystem::path parent_dir = std::filesystem::path(final_save_path).parent_path();
        if (!parent_dir.empty()) {
            ensure_directory_exists_utf8(parent_dir.string());
        }
#endif
    } catch (...) {
        // 忽略目录创建错误，让文件创建失败时再报错
    }

    // 报告文件信息
    std::stringstream info;
    info << "Receiving file: " << received_filename;
    if (!received_extension.empty()) info << " (" << received_extension << ")";
    info << " [" << (hp.file_size / 1024.0 / 1024.0) << " MB]";
    if (!mime_type.empty()) info << " MIME: " << mime_type;
    info << " -> " << final_save_path;
    report_json("status", "message", info.str());

    // 3. 初始化传输统计
    TransferStatistics stats;
    stats.init(hp.file_size, false); // 设置为接收端
    stats.set_mss_for_estimation(hp.mss);
    stats.start_time = std::chrono::steady_clock::now();

    // 4. 接收文件数据
    auto ofs = open_output_file_utf8(final_save_path);
    if (!ofs) {
        report_json("error", "message", "Cannot create file: " + final_save_path);
        UDT::close(recver);
        UDT::close(serv);
        return;
    }

    long long total_recv = 0;
    std::vector<char> buffer(1024 * 1024 * 8);
    auto last_time = std::chrono::steady_clock::now();
    long long last_bytes = 0;
    bool received_eof_marker = false;

    // 持续接收直到收到EOF标记
    while (!received_eof_marker && total_recv < hp.file_size) {
        int rs = UDT::recv(recver, buffer.data(), (int) buffer.size(), 0);
        if (rs <= 0) {
            if (rs == UDT::ERROR) {
                report_json("warning", "message",
                            "Receive error: " + std::string(UDT::getlasterror().getErrorMessage()));
            }
            break;
        }

        // 检查是否收到EOF标记
        std::string received_data(buffer.data(), rs);
        if (received_data.find("FILE_TRANSMISSION_COMPLETE_EOF") != std::string::npos) {
            report_json("status", "message", "Received EOF marker from sender.");
            received_eof_marker = true;

            // 写入EOF标记之前的数据
            size_t eof_pos = received_data.find("FILE_TRANSMISSION_COMPLETE_EOF");
            if (eof_pos > 0) {
                ofs.write(buffer.data(), eof_pos);
                total_recv += eof_pos;
                stats.update_speed(eof_pos);
            }
            break;
        }

        // 写入正常数据
        ofs.write(buffer.data(), rs);
        if (!ofs) {
            report_json("error", "message", "Failed to write to file");
            break;
        }

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

    // 如果没有收到EOF标记但已经收到所有数据
    if (!received_eof_marker && total_recv >= hp.file_size) {
        report_json("warning", "message", "Received all data but no EOF marker, waiting for marker...");

        // 继续接收，等待EOF标记
        while (!received_eof_marker) {
            char eof_buffer[256];
            int rs = UDT::recv(recver, eof_buffer, sizeof(eof_buffer), 0);
            if (rs <= 0) break;

            std::string received_data(eof_buffer, rs);
            if (received_data.find("FILE_TRANSMISSION_COMPLETE_EOF") != std::string::npos) {
                report_json("status", "message", "Received EOF marker from sender.");
                received_eof_marker = true;
                break;
            }
        }
    }

    ofs.close();

    // 检查文件完整性并计算MD5
    bool transfer_complete = false;
    std::string actual_md5;

    if (total_recv >= hp.file_size) {
        report_json("status", "message", "File received completely, verifying MD5...");

        // 计算MD5
        actual_md5 = calculate_file_md5_utf8(final_save_path);
        std::string expected_md5(hp.md5, 32);

        bool md5_match = (actual_md5 == expected_md5);
        transfer_complete = md5_match;

        // 发送MD5校验结果给发送端
        TransferCompletePacket tcp;
        tcp.status = md5_match ? 1 : 2;
        tcp.received_bytes = total_recv;
        tcp.missing_count = (hp.file_size - total_recv) > 0 ? (hp.file_size - total_recv) : 0;

        // 复制MD5
        memset(tcp.md5, 0, sizeof(tcp.md5));
        if (!actual_md5.empty() && actual_md5.length() >= 32) {
            memcpy(tcp.md5, actual_md5.c_str(), 32);
        }

        // 设置发送超时
        int send_timeout = 5000;
        UDT::setsockopt(recver, 0, UDT_SNDTIMEO, &send_timeout, sizeof(int));

        if (UDT::ERROR == UDT::send(recver, (char *) &tcp, sizeof(TransferCompletePacket), 0)) {
            report_json("warning", "message", "Failed to send verification result to sender");
        } else {
            if (md5_match) {
                report_json("success", "message", "MD5 verification passed!");
            } else {
                report_json("error", "message", "MD5 verification failed!");
            }

            // 等待发送端的统计信息
            char stats_buffer[65536]; // 足够大的缓冲区
            int stats_size = UDT::recv(recver, stats_buffer, sizeof(stats_buffer), 0);

            if (stats_size > 0) {
                std::string stats_json(stats_buffer, stats_size);
                try {
                    // 解析并保存发送端的统计信息
                    std::cout << stats_json << std::endl;
                    report_json("status", "message", "Received sender statistics");
                } catch (...) {
                    report_json("warning", "message", "Failed to parse sender statistics");
                }
            }

            // 发送关闭确认
            const char *close_ack = "RECEIVER_CLOSE_OK";
            if (UDT::ERROR == UDT::send(recver, close_ack, strlen(close_ack), 0)) {
                report_json("warning", "message", "Failed to send close acknowledgment");
            }
        }
    } else {
        report_json("error", "message",
                    "File incomplete: " + std::to_string(total_recv) + "/" + std::to_string(hp.file_size));

        // 发送失败状态
        TransferCompletePacket tcp;
        tcp.status = 2; // 失败
        tcp.received_bytes = total_recv;
        tcp.missing_count = hp.file_size - total_recv;
        memset(tcp.md5, 0, sizeof(tcp.md5));

        UDT::send(recver, (char *) &tcp, sizeof(TransferCompletePacket), 0);

        // 删除不完整的文件
        try {
#ifdef _WIN32
            std::filesystem::path wpath = std::filesystem::path(utf8_to_wstring(final_save_path));
            std::filesystem::remove(wpath);
#else
            std::remove(final_save_path.c_str());
#endif
            report_json("status", "message", "Deleted incomplete file: " + final_save_path);
        } catch (...) {
            // 忽略删除错误
        }
    }

    // 传输完成，记录结束时间
    stats.end_time = std::chrono::steady_clock::now();
    stats.transferred_bytes = total_recv;

    // 最终收集统计信息
    stats.collect_udt_stats(recver);

    // 报告最终进度
    report_progress(total_recv, hp.file_size, 0, stats, true);

    // 输出本地统计信息
    std::cout << stats.to_json() << std::endl;
    std::cout << stats.summary() << std::endl;

    // 设置linger确保所有数据发送完成
    bool linger = true;
    int linger_time = 3000;
    UDT::setsockopt(recver, 0, UDT_LINGER, &linger, sizeof(bool));

    // 等待一小段时间确保发送端已经关闭
#ifdef _WIN32
    Sleep(50);
#else
    usleep(50 * 1000);
#endif

    // 接收端关闭连接
    report_json("status", "message", "Closing receiver connection gracefully...");
    UDT::close(recver);

    // 关闭服务器socket
    UDT::close(serv);

    report_json("status", "message", "Receiver connection closed.");

    // 输出最终的MD5校验结果（如果传输完成）
    if (transfer_complete) {
        std::cout << "{\"type\":\"final_verify\", \"success\":true"
                << ", \"expected\":\"" << std::string(hp.md5, 32)
                << "\", \"actual\":\"" << actual_md5
                << "\", \"file_path\":\"" << final_save_path
                << "\", \"bytes_expected\":" << hp.file_size
                << ", \"bytes_received\":" << total_recv << "}" << std::endl;
    }
}

// ==========================================
// 主函数
// ==========================================

int main(int argc, char *argv[]) {
#ifdef _WIN32
    // 强制设置控制台输出代码页为 UTF-8 (65001)
    SetConsoleOutputCP(CP_UTF8);
    // 设置控制台输入代码页为 UTF-8
    SetConsoleCP(CP_UTF8);

    // 设置本地化以支持中文
    setlocale(LC_ALL, "zh-CN.UTF-8");

    // 初始化Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed" << std::endl;
        return 1;
    }
#endif

    // 设置全局locale为UTF-8
    try {
        std::locale::global(std::locale("en_US.UTF-8"));
    } catch (...) {
        try {
            std::locale::global(std::locale("zh_CN.UTF-8"));
        } catch (...) {
            // 如果设置失败，使用默认locale
        }
    }

    // 确保cout使用UTF-8
    std::ios_base::sync_with_stdio(false);
    std::locale utf8_locale(std::locale(), new std::codecvt_utf8<wchar_t>);
    std::wcout.imbue(utf8_locale);
    std::wcin.imbue(utf8_locale);

    // 添加命令行参数解析
    bool detailed_stats = false;

    UDT::startup();

    if (argc < 2) {
        std::cerr << "错误: 没有提供参数" << std::endl;
        std::cerr << "使用 --help 查看使用信息" << std::endl;
        UDT::cleanup();
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    std::string mode = argv[1];

    // 检查是否请求帮助
    if (mode == "--help" || mode == "-h") {
        std::cerr << "使用方法:" << std::endl;
        std::cerr << "  发送: " << argv[0] <<
                " send <ip> <port> <文件路径> [--mss 1500] [--window 1048576] [--detailed]" << std::endl;
        std::cerr << "  接收: " << argv[0] << " recv <port> <保存目录或路径> [--detailed]" << std::endl;
        std::cerr << std::endl;
        std::cerr << "增强功能:" << std::endl;
        std::cerr << "  - 支持中文路径和文件名" << std::endl;
        std::cerr << "  - 自动传输文件名和格式" << std::endl;
        std::cerr << "  - 优雅的连接关闭和确认" << std::endl;
        std::cerr << "  - MIME类型检测和传输" << std::endl;
        std::cerr << "  - 基于UDT4 API的准确网络统计" << std::endl;
        std::cerr << "  - 智能文件保存（目录或具体路径）" << std::endl;
        std::cerr << "  - MD5校验确保数据完整性" << std::endl;
        std::cerr << std::endl;
        std::cerr << "示例:" << std::endl;
        std::cerr << "  # 使用自定义参数发送文件" << std::endl;
        std::cerr << "  " << argv[0] << " send 192.168.1.100 9000 大型文件.zip --mss 1460 --window 2097152" << std::endl;
        std::cerr << "  # 接收文件到目录（文件名自动检测）" << std::endl;
        std::cerr << "  " << argv[0] << " recv 9000 ./下载目录/" << std::endl;
        std::cerr << "  # 使用具体文件名接收" << std::endl;
        std::cerr << "  " << argv[0] << " recv 9000 ./下载目录/我的文件.zip" << std::endl;
        std::cerr << "  # 启用详细统计" << std::endl;
        std::cerr << "  " << argv[0] << " send 192.168.1.100 9000 文件.txt --detailed" << std::endl;
    } else if (mode == "send" && argc >= 5) {
        UDTConfig config;
        // 解析发送端特有的配置
        for (int i = 5; i < argc; ++i) {
            if (std::string(argv[i]) == "--mss") {
                if (i + 1 < argc) {
                    config.mss = std::atoi(argv[++i]);
                } else {
                    std::cerr << "错误: --mss 需要参数值" << std::endl;
                    UDT::cleanup();
#ifdef _WIN32
                    WSACleanup();
#endif
                    return 1;
                }
            } else if (std::string(argv[i]) == "--window") {
                if (i + 1 < argc) {
                    config.window_size = std::atoi(argv[++i]);
                } else {
                    std::cerr << "错误: --window 需要参数值" << std::endl;
                    UDT::cleanup();
#ifdef _WIN32
                    WSACleanup();
#endif
                    return 1;
                }
            } else if (std::string(argv[i]) == "--detailed") {
                detailed_stats = true;
            } else {
                std::cerr << "警告: 未知选项 " << argv[i] << std::endl;
            }
        }
        run_sender(argv[2], std::atoi(argv[3]), argv[4], config);
    } else if (mode == "recv" && argc >= 4) {
        // 接收端现在不需要在命令行指定 mss 和 window 了
        for (int i = 4; i < argc; ++i) {
            if (std::string(argv[i]) == "--detailed") {
                detailed_stats = true;
            } else {
                std::cerr << "警告: 未知选项 " << argv[i] << std::endl;
            }
        }
        run_receiver(std::atoi(argv[2]), argv[3]);
    } else {
        std::cerr << "错误: 无效参数" << std::endl;
        std::cerr << "使用 --help 查看使用信息" << std::endl;
    }

    UDT::cleanup();

#ifdef _WIN32
    WSACleanup();
#endif

    return 0;
}
