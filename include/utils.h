#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <iostream>  // 添加这个
#include <thread>    // 添加这个
#include <atomic>    // 添加这个
#include <mutex>     // 添加这个
#include <condition_variable> // 添加这个

namespace hruft {
    class Timer {
    public:
        Timer() : start_(std::chrono::steady_clock::now()) {
        }

        void reset() { start_ = std::chrono::steady_clock::now(); }

        template<typename Duration = std::chrono::milliseconds>
        int64_t elapsed() const {
            auto now = std::chrono::steady_clock::now();
            return std::chrono::duration_cast<Duration>(now - start_).count();
        }

    private:
        std::chrono::steady_clock::time_point start_;
    };

    class RateLimiter {
    public:
        RateLimiter(uint64_t bytesPerSecond) : rate_(bytesPerSecond) {
        }

        void add(uint64_t bytes) {
            uint64_t now = getCurrentTime();
            uint64_t expectedTime = bytes * 1000000 / rate_;

            if (lastTime_ + expectedTime > now) {
                uint64_t sleepTime = lastTime_ + expectedTime - now;
                std::this_thread::sleep_for(std::chrono::microseconds(sleepTime));
            }

            lastTime_ = getCurrentTime();
        }

    private:
        static uint64_t getCurrentTime() {
            auto now = std::chrono::steady_clock::now();
            return std::chrono::duration_cast<std::chrono::microseconds>(
                now.time_since_epoch()).count();
        }

        uint64_t rate_;
        uint64_t lastTime_ = 0;
    };

    class ProgressBar {
    public:
        ProgressBar(uint64_t total) : total_(total), current_(0) {
        }

        void update(uint64_t current) {
            current_ = current;
            print();
        }

        void increment(uint64_t delta) {
            current_ += delta;
            print();
        }

        void print() {
            if (total_ == 0) return;

            double percentage = static_cast<double>(current_) / total_ * 100;
            int barWidth = 50;
            int pos = static_cast<int>(barWidth * percentage / 100);

            std::cout << "\r[";
            for (int i = 0; i < barWidth; ++i) {
                if (i < pos) std::cout << "=";
                else if (i == pos) std::cout << ">";
                else std::cout << " ";
            }
            std::cout << "] " << std::fixed << std::setprecision(1)
                    << percentage << "% ";

            if (formatBytes_) {
                std::cout << formatBytes(current_)
                        << " / " << formatBytes(total_);
            }
            std::cout.flush();
        }

        void finish() {
            current_ = total_;
            print();
            std::cout << std::endl;
        }

        void enableBytesFormat(bool enable = true) { formatBytes_ = enable; }

    private:
        static std::string formatBytes(uint64_t bytes) {
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

        uint64_t total_;
        uint64_t current_;
        bool formatBytes_ = true;
    };

    class Statistics {
    public:
        Statistics() : startTime_(std::chrono::steady_clock::now()) {
        }

        void addSentBytes(uint64_t bytes) {
            totalSent_ += bytes;
            lastSentBytes_ += bytes;
        }

        void addReceivedBytes(uint64_t bytes) {
            totalReceived_ += bytes;
            lastReceivedBytes_ += bytes;
        }

        void addRetransmit() { ++retransmitCount_; }
        void addError() { ++errorCount_; }

        double getSendSpeed() {
            update();
            return sendSpeed_;
        }

        double getReceiveSpeed() {
            update();
            return receiveSpeed_;
        }

        std::string getSummary() const {
            std::stringstream ss;
            ss << "Total sent: " << totalSent_ << " bytes\n"
                    << "Total received: " << totalReceived_ << " bytes\n"
                    << "Retransmits: " << retransmitCount_ << "\n"
                    << "Errors: " << errorCount_;
            return ss.str();
        }

    private:
        void update() {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - lastUpdateTime_).count();

            if (elapsed >= 1000) {
                // 每秒更新一次
                sendSpeed_ = lastSentBytes_ * 1000.0 / elapsed;
                receiveSpeed_ = lastReceivedBytes_ * 1000.0 / elapsed;

                lastSentBytes_ = 0;
                lastReceivedBytes_ = 0;
                lastUpdateTime_ = now;
            }
        }

        std::chrono::steady_clock::time_point startTime_;
        std::chrono::steady_clock::time_point lastUpdateTime_;

        std::atomic<uint64_t> totalSent_{0};
        std::atomic<uint64_t> totalReceived_{0};
        std::atomic<uint64_t> lastSentBytes_{0};
        std::atomic<uint64_t> lastReceivedBytes_{0};
        std::atomic<uint32_t> retransmitCount_{0};
        std::atomic<uint32_t> errorCount_{0};

        double sendSpeed_ = 0;
        double receiveSpeed_ = 0;
    };

    // 平台工具函数
    namespace Platform {
        uint64_t getFreeDiskSpace(const std::string &path);

        std::string getCurrentTimeString();

        bool createDirectory(const std::string &path);

        std::string getHomeDirectory();

        std::string getTempDirectory();
    }

    // 网络工具函数
    namespace Network {
        std::string getLocalIP();

        uint16_t getAvailablePort(uint16_t startPort = 10000);

        bool isPortAvailable(uint16_t port);

        std::vector<std::string> getNetworkInterfaces();
    }
} // namespace hruft
