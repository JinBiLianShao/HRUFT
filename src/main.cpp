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

// 平台差异处理
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
#else
    #include <unistd.h>
    #include <arpa/inet.h>
    #include <netdb.h>
#endif

// 引入 UDT 头文件
#include "udt.h"

// ==========================================
// 数据结构与辅助工具
// ==========================================

struct UDTConfig {
    int mss = 1500;            // 最大分段大小 (bytes)
    int window_size = 1048576; // 缓冲区窗口大小 (bytes), 默认1MB
};

// JSON 输出辅助
void report_error(const std::string& msg) {
    std::cerr << "{\"type\":\"error\", \"message\":\"" << msg << "\"}" << std::endl;
}

void report_status(const std::string& status) {
    std::cout << "{\"type\":\"status\", \"message\":\"" << status << "\"}" << std::endl;
}

void report_progress(long long current, long long total, double speed_mbps) {
    int percent = (total > 0) ? (int)((current * 100) / total) : 0;
    // 使用 fixed 和 setprecision 保证 JSON 数值格式正常
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "{\"type\":\"progress\", \"percent\":" << percent 
              << ", \"current\":" << current 
              << ", \"total\":" << total 
              << ", \"speed_mbps\":" << speed_mbps << "}" << std::endl;
}

// 命令行参数获取
char* get_cmd_option(char ** begin, char ** end, const std::string & option) {
    char ** itr = std::find(begin, end, option);
    if (itr != end && ++itr != end) {
        return *itr;
    }
    return nullptr;
}

// ==========================================
// UDT 核心逻辑
// ==========================================

// 应用配置 (MSS, Window Size, Buffer)
void apply_udt_config(UDTSOCKET sock, const UDTConfig& config) {
    // 1. MSS
    UDT::setsockopt(sock, 0, UDT_MSS, &config.mss, sizeof(int));

    // 2. UDT 协议缓冲区 (控制流控窗口上限)
    UDT::setsockopt(sock, 0, UDT_SNDBUF, &config.window_size, sizeof(int));
    UDT::setsockopt(sock, 0, UDT_RCVBUF, &config.window_size, sizeof(int));

    // 3. UDP 系统套接字缓冲区 (防止底层丢包)
    int udp_buf = config.window_size; 
    UDT::setsockopt(sock, 0, UDP_SNDBUF, &udp_buf, sizeof(int));
    UDT::setsockopt(sock, 0, UDP_RCVBUF, &udp_buf, sizeof(int));

    // 4. 流控窗口 (Flight Flag Size)
    int fc = (config.window_size / config.mss) * 2; 
    if (fc < 25600) fc = 25600; // 保持一个合理的最小值
    UDT::setsockopt(sock, 0, UDT_FC, &fc, sizeof(int));
}

// 发送端逻辑
void run_sender(const char* ip, int port, const char* filepath, const UDTConfig& config) {
    UDTSOCKET client = UDT::socket(AF_INET, SOCK_STREAM, 0);
    
    // 应用参数配置
    apply_udt_config(client, config);

    sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    #ifdef _WIN32
        // Windows 上 inet_pton 需要较新版本 SDK，这里简单兼容处理
        inet_pton(AF_INET, ip, &serv_addr.sin_addr);
    #else
        inet_pton(AF_INET, ip, &serv_addr.sin_addr);
    #endif
    memset(&(serv_addr.sin_zero), '\0', 8);

    report_status("Connecting...");

    // 连接超时机制 (可选，UDT 默认有超时，但为了快速反馈可以设置 blocking)
    if (UDT::ERROR == UDT::connect(client, (sockaddr*)&serv_addr, sizeof(serv_addr))) {
        report_error("Connection failed: " + std::string(UDT::getlasterror().getErrorMessage()));
        UDT::close(client);
        return;
    }

    report_status("Connected. Opening file...");

    std::ifstream ifs(filepath, std::ios::binary | std::ios::ate);
    if (!ifs) {
        report_error("File open failed: " + std::string(filepath));
        UDT::close(client);
        return;
    }

    long long file_size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);

    // 协议第一步：发送文件大小 (64位整数)
    if (UDT::ERROR == UDT::send(client, (char*)&file_size, sizeof(long long), 0)) {
        report_error("Failed to send metadata.");
        UDT::close(client);
        return;
    }

    // 发送数据循环
    const int CHUNK_SIZE = 1024 * 64; // 64KB Chunk for read
    std::vector<char> buffer(CHUNK_SIZE);
    long long total_sent = 0;

    auto start_time = std::chrono::steady_clock::now();
    auto last_report_time = start_time;

    report_status("Starting transmission...");

    while (!ifs.eof() && total_sent < file_size) {
        ifs.read(buffer.data(), CHUNK_SIZE);
        int read_count = (int)ifs.gcount();
        if (read_count <= 0) break;

        int offset = 0;
        while (offset < read_count) {
            int sent = UDT::send(client, buffer.data() + offset, read_count - offset, 0);
            if (sent == UDT::ERROR) {
                report_error("Send lost: " + std::string(UDT::getlasterror().getErrorMessage()));
                goto CLEANUP;
            }
            offset += sent;
            total_sent += sent;
        }

        // 进度汇报 (200ms 间隔)
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_report_time).count() > 200) {
            double elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(now - start_time).count();
            double speed = (elapsed > 0) ? (total_sent / 1024.0 / 1024.0) / elapsed : 0.0;
            report_progress(total_sent, file_size, speed);
            last_report_time = now;
        }
    }

    // 最后一次强制汇报
    {
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(now - start_time).count();
        double speed = (elapsed > 0) ? (total_sent / 1024.0 / 1024.0) / elapsed : 0.0;
        report_progress(total_sent, file_size, speed);
    }

    report_status("Transfer complete.");

CLEANUP:
    ifs.close();
    UDT::close(client);
}

// 接收端逻辑
void run_receiver(int port, const char* save_path, const UDTConfig& config) {
    UDTSOCKET serv = UDT::socket(AF_INET, SOCK_STREAM, 0);

    // 接收端也必须配置，尤其是 RCVBUF
    apply_udt_config(serv, config);

    sockaddr_in my_addr;
    my_addr.sin_family = AF_INET;
    my_addr.sin_port = htons(port);
    my_addr.sin_addr.s_addr = INADDR_ANY;
    memset(&(my_addr.sin_zero), '\0', 8);

    if (UDT::ERROR == UDT::bind(serv, (sockaddr*)&my_addr, sizeof(my_addr))) {
        report_error("Bind failed: " + std::string(UDT::getlasterror().getErrorMessage()));
        return;
    }

    UDT::listen(serv, 10);
    report_status("Waiting for sender...");

    sockaddr_in client_addr;
    int namelen = sizeof(client_addr);
    UDTSOCKET recver = UDT::accept(serv, (sockaddr*)&client_addr, &namelen);

    if (recver == UDT::INVALID_SOCK) {
         report_error("Accept failed.");
         return;
    }

    // 协议第一步：接收文件大小
    long long file_size = 0;
    int rsize = 0;
    int target_size = sizeof(long long);
    
    // 确保读满 8 字节
    char* size_buf = (char*)&file_size;
    int size_read = 0;
    while(size_read < target_size) {
        int rs = UDT::recv(recver, size_buf + size_read, target_size - size_read, 0);
        if (rs == UDT::ERROR) {
            report_error("Recv metadata failed.");
            UDT::close(recver);
            UDT::close(serv);
            return;
        }
        size_read += rs;
    }

    report_status("Receiving file size: " + std::to_string(file_size));

    std::ofstream ofs(save_path, std::ios::binary);
    if (!ofs) {
        report_error("Cannot create file: " + std::string(save_path));
        UDT::close(recver);
        UDT::close(serv);
        return;
    }

    const int CHUNK_SIZE = 1024 * 64;
    std::vector<char> buffer(CHUNK_SIZE);
    long long total_recv = 0;

    auto start_time = std::chrono::steady_clock::now();
    auto last_report_time = start_time;

    while (total_recv < file_size) {
        int rs = UDT::recv(recver, buffer.data(), CHUNK_SIZE, 0);
        if (rs == UDT::ERROR) {
            report_error("Recv data error: " + std::string(UDT::getlasterror().getErrorMessage()));
            break;
        }
        if (rs == 0) break; // 连接关闭

        ofs.write(buffer.data(), rs);
        total_recv += rs;

        // 进度汇报
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_report_time).count() > 200) {
            double elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(now - start_time).count();
            double speed = (elapsed > 0) ? (total_recv / 1024.0 / 1024.0) / elapsed : 0.0;
            report_progress(total_recv, file_size, speed);
            last_report_time = now;
        }
    }

    // 最后一次汇报
    {
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(now - start_time).count();
        double speed = (elapsed > 0) ? (total_recv / 1024.0 / 1024.0) / elapsed : 0.0;
        report_progress(total_recv, file_size, speed);
    }

    report_status("Receive complete.");
    
    ofs.close();
    UDT::close(recver);
    UDT::close(serv);
}

// ==========================================
// 主入口
// ==========================================
int main(int argc, char* argv[]) {
    // UDT 初始化
    if (UDT::startup() == UDT::ERROR) {
        report_error("UDT Startup failed.");
        return 1;
    }

    // 基础参数校验
    if (argc < 5) {
        // 格式: hruft <mode> <ip> <port> <file> [--mss X] [--window Y]
        report_error("Usage: hruft <mode> <ip> <port> <file> [options]");
        return 1;
    }

    std::string mode = argv[1];
    std::string ip = argv[2];
    int port = std::atoi(argv[3]);
    std::string path = argv[4];

    // 解析配置参数
    UDTConfig config;
    
    char* mss_arg = get_cmd_option(argv, argv + argc, "--mss");
    if (mss_arg) {
        config.mss = std::atoi(mss_arg);
    }

    char* win_arg = get_cmd_option(argv, argv + argc, "--window");
    if (win_arg) {
        config.window_size = std::atoi(win_arg);
    }

    try {
        if (mode == "send") {
            run_sender(ip.c_str(), port, path.c_str(), config);
        } else if (mode == "recv") {
            run_receiver(port, path.c_str(), config);
        } else {
            report_error("Unknown mode. Use 'send' or 'recv'.");
        }
    } catch (const std::exception& e) {
        report_error("Exception: " + std::string(e.what()));
    } catch (...) {
        report_error("Unknown exception occurred.");
    }

    // UDT 清理
    UDT::cleanup();
    return 0;
}