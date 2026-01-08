#include "cli.h"
#include "utils.h"
#include "session.h"

#include <iostream>
#include <cstring>
#include <getopt.h>
#include <fstream>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#endif

namespace hruft {
    bool parseCommandLine(int argc, char *argv[], CommandLineArgs &args) {
    if (argc < 2) {
        return false;
    }

    // 默认值
    args.mode = "";
    args.filename = "";
    args.remoteIP = "";
    args.remotePort = 10000;
    args.localDataPort = 10001;
    args.workerThreads = 8;
    args.chunkSizeMB = 4;
    args.windowSize = 16;
    args.encryptionKey = "";

    // 修改这里：添加 'i' 选项
    int opt;
    while ((opt = getopt(argc, argv, "m:f:i:p:t:c:w:k:h")) != -1) {
        switch (opt) {
            case 'm':
                args.mode = optarg;
                break;
            case 'f':
                args.filename = optarg;
                break;
            case 'i':  // 添加这个
                args.remoteIP = optarg;
                break;
            case 'p':
                args.remotePort = static_cast<uint16_t>(std::stoi(optarg));
                break;
            case 't':
                args.workerThreads = std::stoi(optarg);
                break;
            case 'c':
                args.chunkSizeMB = std::stoi(optarg);
                break;
            case 'w':
                args.windowSize = std::stoi(optarg);
                break;
            case 'k':
                args.encryptionKey = optarg;
                break;
            case 'h':
                return false;
            default:
                return false;
        }
    }

    // 删除下面的代码，因为我们已经用 -i 选项处理了
    // // 解析剩余的选项（-ip参数）
    // for (int i = optind; i < argc; i++) {
    //     if (strcmp(argv[i], "-ip") == 0 && i + 1 < argc) {
    //         args.remoteIP = argv[i + 1];
    //         i++;
    //     } else if (strncmp(argv[i], "--key=", 6) == 0) {
    //         args.encryptionKey = argv[i] + 6;
    //     }
    // }

    // 验证必需参数
    if (args.mode.empty()) {
        std::cerr << "Error: Mode is required (-m send/recv)" << std::endl;
        return false;
    }

    if (args.filename.empty()) {
        std::cerr << "Error: Filename is required (-f)" << std::endl;
        return false;
    }

    if (args.mode == "send" && args.remoteIP.empty()) {
        std::cerr << "Error: Remote IP is required for send mode (-i)" << std::endl;  // 修改错误信息
        return false;
    }

    return true;
}

    bool parseCommandLineArgs(const CommandLineArgs &cliArgs, SessionConfig &config) {
        // 基础配置
        if (cliArgs.mode == "send") {
            config.remoteIP = cliArgs.remoteIP;
        } else {
            // 接收模式下，remoteIP可以是本机
            config.remoteIP = "0.0.0.0";
        }

        config.remoteControlPort = cliArgs.remotePort;
        config.localDataPort = cliArgs.localDataPort;
        config.workerThreads = cliArgs.workerThreads;
        config.chunkSizeMB = cliArgs.chunkSizeMB;
        config.windowSize = cliArgs.windowSize;

        // 加密配置
        if (!cliArgs.encryptionKey.empty()) {
            config.encryptionKey = cliArgs.encryptionKey;
            config.enableEncryption = true;
        }

        // 验证参数有效性
        if (config.chunkSizeMB < 1 || config.chunkSizeMB > 1024) {
            std::cerr << "Error: Chunk size must be between 1 and 1024 MB" << std::endl;
            return false;
        }

        if (config.windowSize < 1 || config.windowSize > 256) {
            std::cerr << "Error: Window size must be between 1 and 256" << std::endl;
            return false;
        }

        if (config.workerThreads < 1 || config.workerThreads > 64) {
            std::cerr << "Error: Worker threads must be between 1 and 64" << std::endl;
            return false;
        }

        // 检查文件
        if (cliArgs.mode == "send") {
            std::ifstream file(cliArgs.filename, std::ios::binary);
            if (!file) {
                std::cerr << "Error: Cannot open file for reading: " << cliArgs.filename << std::endl;
                return false;
            }
            file.close();
        } else {
            // 接收模式：检查目录是否存在
            std::filesystem::path path(cliArgs.filename);
            auto parentPath = path.parent_path();
            if (!parentPath.empty() && !std::filesystem::exists(parentPath)) {
                std::cerr << "Error: Directory does not exist: " << parentPath.string() << std::endl;
                return false;
            }
        }

        return true;
    }

    void printUsage() {
        std::cout << "Usage: hruft -m <mode> -f <filename> [options]" << std::endl;
        std::cout << std::endl;
        std::cout << "Modes:" << std::endl;
        std::cout << "  send    Send file to remote host" << std::endl;
        std::cout << "  recv    Receive file from remote host" << std::endl;
        std::cout << std::endl;
        std::cout << "Required options:" << std::endl;
        std::cout << "  -m <mode>       Operation mode (send/recv)" << std::endl;
        std::cout << "  -f <filename>   File to send or receive" << std::endl;
        std::cout << "  -i <address>    Remote IP address (send mode only)" << std::endl;  // 修改这里
        std::cout << std::endl;
        std::cout << "Performance options:" << std::endl;
        std::cout << "  -t <threads>    Worker thread count (default: 8)" << std::endl;
        std::cout << "  -c <size>       Chunk size in MB (default: 4)" << std::endl;
        std::cout << "  -w <size>       Window size in chunks (default: 16)" << std::endl;
        std::cout << std::endl;
        std::cout << "Network options:" << std::endl;
        std::cout << "  -p <port>       Remote control port (default: 10000)" << std::endl;
        std::cout << "  -k <key>        Encryption key (optional)" << std::endl;
        std::cout << std::endl;
        std::cout << "Examples:" << std::endl;
        std::cout << "  Send file:      hruft -m send -f bigfile.iso -i 192.168.1.100 -t 16" << std::endl;  // 修改这里
        std::cout << "  Receive file:   hruft -m recv -f received.iso -t 8" << std::endl;
        std::cout << std::endl;
        std::cout << "Performance tips:" << std::endl;
        std::cout << "  - For high-latency networks, increase window size (-w)" << std::endl;
        std::cout << "  - For high-bandwidth networks, increase worker threads (-t)" << std::endl;
        std::cout << "  - Larger chunk sizes reduce protocol overhead but increase memory usage" << std::endl;
    }
} // namespace hruft
