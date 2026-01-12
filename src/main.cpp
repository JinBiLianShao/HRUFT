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

// 引入 UDT 和 MD5
#include "udt.h"
#include "md5.h"

// ==========================================
// 配置与辅助
// ==========================================

struct UDTConfig {
    int mss = 1500;
    int window_size = 1048576;
};

void report_error(const std::string& msg) {
    std::cerr << "{\"type\":\"error\", \"message\":\"" << msg << "\"}" << std::endl;
}

void report_status(const std::string& status) {
    std::cout << "{\"type\":\"status\", \"message\":\"" << status << "\"}" << std::endl;
}

void report_progress(long long current, long long total, double speed_mbps) {
    int percent = (total > 0) ? (int)((current * 100) / total) : 0;
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "{\"type\":\"progress\", \"percent\":" << percent
              << ", \"current\":" << current
              << ", \"total\":" << total
              << ", \"speed_mbps\":" << speed_mbps << "}" << std::endl;
}

// 基于上传的 md5.h/cpp 实现的 MD5 计算函数
std::string calculate_file_md5(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file) return "";

    md5_state_t state;
    md5_byte_t digest[16];
    md5_init(&state);

    const size_t buffer_size = 1024 * 1024; // 1MB block
    std::vector<char> buffer(buffer_size);

    while (file.good()) {
        file.read(buffer.data(), buffer_size);
        std::streamsize bytes_read = file.gcount();
        if (bytes_read > 0) {
            md5_append(&state, (const md5_byte_t*)buffer.data(), (int)bytes_read);
        }
    }
    md5_finish(&state, digest);

    std::stringstream ss;
    for(int i = 0; i < 16; ++i) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)digest[i];
    }
    return ss.str();
}

char* get_cmd_option(char ** begin, char ** end, const std::string & option) {
    char ** itr = std::find(begin, end, option);
    if (itr != end && ++itr != end) return *itr;
    return nullptr;
}

void apply_udt_config(UDTSOCKET sock, const UDTConfig& config) {
    UDT::setsockopt(sock, 0, UDT_MSS, &config.mss, sizeof(int));
    UDT::setsockopt(sock, 0, UDT_SNDBUF, &config.window_size, sizeof(int));
    UDT::setsockopt(sock, 0, UDT_RCVBUF, &config.window_size, sizeof(int));
    int udp_buf = config.window_size;
    UDT::setsockopt(sock, 0, UDP_SNDBUF, &udp_buf, sizeof(int));
    UDT::setsockopt(sock, 0, UDP_RCVBUF, &udp_buf, sizeof(int));
    int fc = (config.window_size / config.mss) * 2;
    if (fc < 25600) fc = 25600;
    UDT::setsockopt(sock, 0, UDT_FC, &fc, sizeof(int));
}

// ==========================================
// 发送端逻辑
// ==========================================

void run_sender(const char* ip, int port, const char* filepath, const UDTConfig& config) {
    report_status("Calculating MD5...");
    std::string local_md5 = calculate_file_md5(filepath);
    if (local_md5.empty()) { report_error("MD5 failed."); return; }
    report_status("Local MD5: " + local_md5);

    UDTSOCKET client = UDT::socket(AF_INET, SOCK_STREAM, 0);
    apply_udt_config(client, config);

    sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &serv_addr.sin_addr);

    report_status("Connecting...");
    if (UDT::ERROR == UDT::connect(client, (sockaddr*)&serv_addr, sizeof(serv_addr))) {
        report_error("Connection failed.");
        return;
    }

    std::ifstream ifs(filepath, std::ios::binary | std::ios::ate);
    long long file_size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);

    // 元数据握手
    UDT::send(client, (char*)&file_size, sizeof(long long), 0);
    UDT::send(client, local_md5.c_str(), 32, 0);

    std::vector<char> buffer(1024 * 64);
    long long total_sent = 0;
    auto start_time = std::chrono::steady_clock::now();
    auto last_report_time = start_time;
    long long last_report_bytes = 0;

    report_status("Transferring...");
    while (total_sent < file_size) {
        ifs.read(buffer.data(), buffer.size());
        int read_count = (int)ifs.gcount();
        if (read_count <= 0) break;

        int offset = 0;
        while (offset < read_count) {
            int sent = UDT::send(client, buffer.data() + offset, read_count - offset, 0);
            if (sent <= 0) goto END;
            offset += sent;
            total_sent += sent;
        }

        auto now = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_report_time).count();
        if (duration >= 500) {
            double instant_speed = ((total_sent - last_report_bytes) / 1024.0 / 1024.0) / (duration / 1000.0);
            report_progress(total_sent, file_size, instant_speed);
            last_report_time = now;
            last_report_bytes = total_sent;
        }
    }
    report_progress(total_sent, file_size, 0);
    report_status("Transfer complete.");

END:
    ifs.close();
    UDT::close(client);
}

// ==========================================
// 接收端逻辑
// ==========================================

void run_receiver(int port, const char* save_path, const UDTConfig& config) {
    UDTSOCKET serv = UDT::socket(AF_INET, SOCK_STREAM, 0);
    apply_udt_config(serv, config);

    sockaddr_in my_addr;
    my_addr.sin_family = AF_INET;
    my_addr.sin_port = htons(port);
    my_addr.sin_addr.s_addr = INADDR_ANY;

    UDT::bind(serv, (sockaddr*)&my_addr, sizeof(my_addr));
    UDT::listen(serv, 5);
    report_status("Waiting for connection...");

    sockaddr_in client_addr;
    int namelen = sizeof(client_addr);
    UDTSOCKET recver = UDT::accept(serv, (sockaddr*)&client_addr, &namelen);

    long long file_size = 0;
    char expected_md5[33] = {0};

    // 接收文件大小和发送端的 MD5
    UDT::recv(recver, (char*)&file_size, sizeof(long long), 0);
    UDT::recv(recver, expected_md5, 32, 0);
    report_status("Incoming file size: " + std::to_string(file_size));

    std::ofstream ofs(save_path, std::ios::binary);
    std::vector<char> buffer(1024 * 64);
    long long total_recv = 0;
    auto start_time = std::chrono::steady_clock::now();
    auto last_report_time = start_time;
    long long last_report_bytes = 0;

    while (total_recv < file_size) {
        int rs = UDT::recv(recver, buffer.data(), (int)buffer.size(), 0);
        if (rs <= 0) break;
        ofs.write(buffer.data(), rs);
        total_recv += rs;

        auto now = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_report_time).count();
        if (duration >= 500) {
            double instant_speed = ((total_recv - last_report_bytes) / 1024.0 / 1024.0) / (duration / 1000.0);
            report_progress(total_recv, file_size, instant_speed);
            last_report_time = now;
            last_report_bytes = total_recv;
        }
    }
    ofs.close();

    report_status("Verifying integrity...");
    std::string actual_md5 = calculate_file_md5(save_path);
    bool success = (actual_md5 == std::string(expected_md5));

    // 输出最终校验 JSON
    std::cout << "{\"type\":\"verify\", \"success\":" << (success ? "true" : "false")
              << ", \"expected\":\"" << expected_md5 << "\", \"actual\":\"" << actual_md5 << "\"}" << std::endl;

    if (success) report_status("Verification passed.");
    else report_error("Verification failed!");

    UDT::close(recver);
    UDT::close(serv);
}

int main(int argc, char* argv[]) {
    UDT::startup();
    if (argc < 5) return 1;

    std::string mode = argv[1];
    std::string ip = argv[2];
    int port = std::atoi(argv[3]);
    std::string path = argv[4];

    UDTConfig config;
    char* m_arg = get_cmd_option(argv, argv + argc, "--mss");
    if (m_arg) config.mss = std::atoi(m_arg);
    char* w_arg = get_cmd_option(argv, argv + argc, "--window");
    if (w_arg) config.window_size = std::atoi(w_arg);

    if (mode == "send") run_sender(ip.c_str(), port, path.c_str(), config);
    else if (mode == "recv") run_receiver(port, path.c_str(), config);

    UDT::cleanup();
    return 0;
}