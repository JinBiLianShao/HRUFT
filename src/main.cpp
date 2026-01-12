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

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <unistd.h>
    #include <arpa/inet.h>
#endif

#include "udt.h"
#include "md5.h"

// ==========================================
// 协议定义
// ==========================================

// 握手包结构：确保两端按同样的字节序解析
#pragma pack(push, 1)
struct HandshakePacket {
    long long file_size;     // 文件总大小
    char md5[32];            // MD5 字符串
    int mss;                 // 发送端指定的 MSS
    int window_size;         // 发送端指定的 Window Size
};
#pragma pack(pop)

struct UDTConfig {
    int mss = 1500;
    int window_size = 1048576; // 默认 1MB
};

// ==========================================
// 辅助工具
// ==========================================

void report_json(const std::string& type, const std::string& key, const std::string& val) {
    std::cout << "{\"type\":\"" << type << "\", \"" << key << "\":\"" << val << "\"}" << std::endl;
}

void report_progress(long long current, long long total, double speed_mbps) {
    int percent = (total > 0) ? (int)((current * 100) / total) : 0;
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "{\"type\":\"progress\", \"percent\":" << percent
              << ", \"current\":" << current
              << ", \"total\":" << total
              << ", \"speed_mbps\":" << speed_mbps << "}" << std::endl;
}

std::string calculate_file_md5(const std::string& filepath) {
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
    for(int i = 0; i < 16; ++i) ss << std::hex << std::setw(2) << std::setfill('0') << (int)digest[i];
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
    if (local_md5.empty()) { report_json("error", "message", "File error"); return; }

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

    // 1. 发送握手协商包
    HandshakePacket hp;
    hp.file_size = f_size;
    hp.mss = config.mss;
    hp.window_size = config.window_size;
    memcpy(hp.md5, local_md5.c_str(), 32);

    UDT::send(client, (char*)&hp, sizeof(HandshakePacket), 0);

    // 2. 开始传输数据
    std::vector<char> buffer(1024 * 64);
    long long total_sent = 0;
    auto last_time = std::chrono::steady_clock::now();
    long long last_bytes = 0;

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
        }

        auto now = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_time).count();
        if (duration >= 500) {
            double speed = ((total_sent - last_bytes) / 1024.0 / 1024.0) / (duration / 1000.0);
            report_progress(total_sent, f_size, speed);
            last_time = now;
            last_bytes = total_sent;
        }
    }
    report_progress(total_sent, f_size, 0);
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
    report_json("status", "message", "Syncing config: MSS=" + std::to_string(hp.mss) + ", WIN=" + std::to_string(hp.window_size));
    apply_socket_opts(recver, hp.mss, hp.window_size);

    // 3. 接收文件数据
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

        auto now = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_time).count();
        if (duration >= 500) {
            double speed = ((total_recv - last_bytes) / 1024.0 / 1024.0) / (duration / 1000.0);
            report_progress(total_recv, hp.file_size, speed);
            last_time = now;
            last_bytes = total_recv;
        }
    }
    ofs.close();

    // 4. 校验 MD5
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
    UDT::startup();
    if (argc < 2) return 1;

    std::string mode = argv[1];
    if (mode == "send" && argc >= 5) {
        UDTConfig config;
        // 解析发送端特有的配置
        for (int i = 5; i < argc; ++i) {
            if (std::string(argv[i]) == "--mss") config.mss = std::atoi(argv[++i]);
            if (std::string(argv[i]) == "--window") config.window_size = std::atoi(argv[++i]);
        }
        run_sender(argv[2], std::atoi(argv[3]), argv[4], config);
    } else if (mode == "recv" && argc >= 4) {
        // 接收端现在不需要在命令行指定 mss 和 window 了
        run_receiver(std::atoi(argv[2]), argv[3]);
    }

    UDT::cleanup();
    return 0;
}