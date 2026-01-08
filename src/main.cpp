#include "cli.h"
#include "session.h"
#include "utils.h"

#include <iostream>
#include <csignal>
#include <atomic>
#include <chrono>
#include <iomanip>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <signal.h>
#endif

namespace hruft {
    std::atomic<bool> g_shouldStop{false};

    void signalHandler(int signal) {
        if (signal == SIGINT || signal == SIGTERM) {
            std::cout << "\nReceived termination signal, shutting down..." << std::endl;
            g_shouldStop = true;
        }
    }

    void setupSignalHandlers() {
#ifdef _WIN32
        signal(SIGINT, signalHandler);
        signal(SIGTERM, signalHandler);
#else
        struct sigaction sa;
        sa.sa_handler = signalHandler;
        sa.sa_flags = 0;
        sigemptyset(&sa.sa_mask);

        sigaction(SIGINT, &sa, nullptr);
        sigaction(SIGTERM, &sa, nullptr);
        // 忽略SIGPIPE信号
        signal(SIGPIPE, SIG_IGN);
#endif
    }

    std::string formatBytes(uint64_t bytes) {
        const char *units[] = {"B", "KB", "MB", "GB", "TB"};
        int unitIndex = 0;
        double value = static_cast<double>(bytes);

        while (value >= 1024 && unitIndex < 4) {
            value /= 1024;
            ++unitIndex;
        }

        std::stringstream ss;
        ss << std::fixed << std::setprecision(2) << value << " " << units[unitIndex];
        return ss.str();
    }

    int run(const CommandLineArgs &args) {
        setupSignalHandlers();

        try {
            // 创建配置
            SessionConfig config;

            // 解析命令行参数
            if (!parseCommandLineArgs(args, config)) {
                return 1;
            }

            // 创建传输会话
            TransferSession session;

            // 启动传输
            if (args.mode == "send") {
                if (!session.startAsSender(config, args.filename)) {
                    std::cerr << "Failed to start sender session" << std::endl;
                    return 1;
                }
            } else if (args.mode == "recv") {
                if (!session.startAsReceiver(config, args.filename)) {
                    std::cerr << "Failed to start receiver session" << std::endl;
                    return 1;
                }
            } else {
                std::cerr << "Invalid mode: " << args.mode << std::endl;
                return 1;
            }

            // 显示进度信息
            ProgressBar progressBar(100);
            progressBar.enableBytesFormat(false);
            Timer timer;

            while (!g_shouldStop) {
                auto state = session.getState();
                auto phase = state.getPhase();

                if (phase == SessionState::Phase::COMPLETED ||
                    phase == SessionState::Phase::ERROR) {
                    break;
                }

                // 更新进度条
                progressBar.update(static_cast<uint64_t>(state.getProgress() * 100));

                // 显示统计信息
                if (timer.elapsed() >= 1000) {
                    // 每秒更新一次
                    double speed = state.getSpeed() / 1024.0 / 1024.0; // MB/s

                    std::cout << "\n";
                    std::cout << "Progress: " << std::fixed << std::setprecision(1)
                            << state.getProgress() * 100 << "%\n";
                    std::cout << "Speed: " << speed << " MB/s\n";
                    std::cout << "Transferred: "
                            << formatBytes(state.getBytesTransferred()) << "\n";
                    std::cout << "Retries: " << state.getRetryCount() << "\n";

                    timer.reset();
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            // 完成传输
            if (session.waitForCompletion(30000)) {
                auto state = session.getState();

                if (state.getPhase() == SessionState::Phase::COMPLETED) {
                    std::cout << "\nTransfer completed successfully!" << std::endl;
                    std::cout << "Total time: " << timer.elapsed<std::chrono::seconds>()
                            << " seconds" << std::endl;
                    std::cout << "Average speed: "
                            << state.getBytesTransferred() / (timer.elapsed<std::chrono::seconds>() + 1) / 1024.0 /
                            1024.0
                            << " MB/s" << std::endl;
                    return 0;
                } else {
                    std::cerr << "\nTransfer failed: " << state.getError() << std::endl;
                    return 1;
                }
            } else {
                std::cerr << "\nTransfer timeout" << std::endl;
                return 1;
            }
        } catch (const std::exception &e) {
            std::cerr << "Error: " << e.what() << std::endl;
            return 1;
        }

        return 0;
    }
} // namespace hruft

int main(int argc, char *argv[]) {
    // 初始化日志
    std::cout << "HRUFT - High-performance Reliable UDP File Transfer" << std::endl;
    std::cout << "Version 1.0.0" << std::endl;
    std::cout << std::endl;

    // 解析命令行参数
    hruft::CommandLineArgs args;
    if (!hruft::parseCommandLine(argc, argv, args)) {
        hruft::printUsage();
        return 1;
    }

    // 运行主程序
    return hruft::run(args);
}
