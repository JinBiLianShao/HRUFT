#pragma once

#include <string>
#include <cstdint>
#include <filesystem>

namespace hruft {
    struct CommandLineArgs {
        std::string mode; // "send" or "recv"
        std::string filename; // file to send/receive
        std::string remoteIP; // remote IP address (for send mode)
        uint16_t remotePort = 10000; // remote control port
        uint16_t localDataPort = 10001; // local data port
        uint32_t workerThreads = 8; // number of worker threads
        uint32_t chunkSizeMB = 4; // chunk size in MB
        uint32_t windowSize = 16; // window size in chunks
        std::string encryptionKey; // encryption key
    };

    // 解析命令行参数
    bool parseCommandLine(int argc, char *argv[], CommandLineArgs &args);

    // 解析命令行参数到会话配置
    bool parseCommandLineArgs(const CommandLineArgs &cliArgs, class SessionConfig &config);

    // 打印使用说明
    void printUsage();

    // 格式化字节数为可读字符串
    std::string formatBytes(uint64_t bytes);
} // namespace hruft
