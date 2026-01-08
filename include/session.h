#pragma once

#include <cstdint>
#include <string>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <thread>
#include <chrono>
#include <iostream>

#include "protocol.h"
#include "socket.h"

namespace hruft {

class SessionConfig {
public:
    // 基础配置
    std::string remoteIP;
    uint16_t remoteControlPort = 10000;
    uint16_t localDataPort = 10001;
    uint32_t chunkSizeMB = 4;          // MB
    uint32_t windowSize = 16;          // chunks
    uint32_t workerThreads = 8;
    uint32_t packetSize = 1400;        // bytes

    // 超时和重试
    uint32_t handshakeTimeout = 5000;  // ms
    uint32_t chunkTimeout = 30000;     // ms
    uint32_t maxRetries = 5;

    // 加密
    std::string encryptionKey;
    bool enableEncryption = false;

    // 校验和
    bool enableCRC32 = true;
    bool enableSHA256 = true;

    uint32_t getChunkSizeBytes() const { return chunkSizeMB * 1024 * 1024; }
    uint32_t getPacketsPerChunk() const {
        return (getChunkSizeBytes() + packetSize - 1) / packetSize;
    }
};

class SessionState {
public:
    enum class Phase {
        INIT,
        HANDSHAKE,
        TRANSFER,
        VERIFICATION,
        COMPLETED,
        ERROR
    };

    SessionState();

    // 添加拷贝构造函数
    SessionState(const SessionState& other);
    SessionState& operator=(const SessionState& other);

    Phase getPhase() const { return phase_; }
    void setPhase(Phase phase) { phase_ = phase; }

    double getProgress() const { return progress_; }
    void setProgress(double progress) { progress_ = progress; }

    uint64_t getBytesTransferred() const { return bytesTransferred_; }
    void addBytesTransferred(uint64_t bytes) {
        bytesTransferred_ += bytes;
        lastUpdateTime_ = std::chrono::steady_clock::now();
    }

    uint64_t getSpeed() const;

    uint32_t getRetryCount() const { return retryCount_; }
    void incrementRetryCount() { ++retryCount_; }

    std::string getError() const { return error_; }
    void setError(const std::string& error) { error_ = error; }

private:
    std::atomic<Phase> phase_{Phase::INIT};
    std::atomic<double> progress_{0.0};
    std::atomic<uint64_t> bytesTransferred_{0};
    std::chrono::steady_clock::time_point lastUpdateTime_;
    std::atomic<uint32_t> retryCount_{0};
    std::string error_;
};

class ControlSession {
public:
    using Callback = std::function<void(const ControlHeader&, const std::vector<uint8_t>&)>;

    ControlSession();
    ~ControlSession();

    bool initialize(uint16_t controlPort, const std::string& bindIP = "0.0.0.0");
    void shutdown();

    // 发送控制包
    bool sendControlPacket(const SocketAddress& addr, ControlType type,
                          uint32_t chunkId, const uint8_t* payload = nullptr,
                          uint16_t payloadLen = 0);

    // 接收控制包（阻塞）
    bool receiveControlPacket(ControlHeader& header, std::vector<uint8_t>& payload,
                             SocketAddress& sender, int timeoutMs = -1);

    // 设置回调（异步模式）
    void setCallback(Callback callback);

    bool isInitialized() const { return initialized_; }

private:
    void receiverThread();

    std::unique_ptr<UdpSocket> socket_;
    Callback callback_;
    std::thread receiverThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> initialized_{false};
};

class TransferSession {
public:
    TransferSession();
    ~TransferSession();

    // 作为发送方启动会话
    bool startAsSender(const SessionConfig& config, const std::string& filename);

    // 作为接收方启动会话
    bool startAsReceiver(const SessionConfig& config, const std::string& filename);

    // 停止会话
    void stop();

    // 获取状态
    SessionState getState() const { return state_; }

    // 等待完成
    bool waitForCompletion(int timeoutMs = -1);

private:
    // Chunk窗口管理
    class EnhancedSlidingWindow {
    public:
        struct WindowSlot {
            uint32_t chunkId;
            std::chrono::steady_clock::time_point sendTime;
            bool acknowledged{false};
            bool needsRetransmit{false};
            std::vector<uint32_t> missingPackets;
            uint32_t retryCount{0};

            // 手动实现拷贝和移动操作
            WindowSlot() = default;
            WindowSlot(const WindowSlot& other)
                : chunkId(other.chunkId)
                , sendTime(other.sendTime)
                , acknowledged(other.acknowledged)
                , needsRetransmit(other.needsRetransmit)
                , missingPackets(other.missingPackets)
                , retryCount(other.retryCount) {}

            WindowSlot& operator=(const WindowSlot& other) {
                if (this != &other) {
                    chunkId = other.chunkId;
                    sendTime = other.sendTime;
                    acknowledged = other.acknowledged;
                    needsRetransmit = other.needsRetransmit;
                    missingPackets = other.missingPackets;
                    retryCount = other.retryCount;
                }
                return *this;
            }

            WindowSlot(WindowSlot&& other) noexcept
                : chunkId(other.chunkId)
                , sendTime(other.sendTime)
                , acknowledged(other.acknowledged)
                , needsRetransmit(other.needsRetransmit)
                , missingPackets(std::move(other.missingPackets))
                , retryCount(other.retryCount) {}

            WindowSlot& operator=(WindowSlot&& other) noexcept {
                if (this != &other) {
                    chunkId = other.chunkId;
                    sendTime = other.sendTime;
                    acknowledged = other.acknowledged;
                    needsRetransmit = other.needsRetransmit;
                    missingPackets = std::move(other.missingPackets);
                    retryCount = other.retryCount;
                }
                return *this;
            }
        };

        EnhancedSlidingWindow(size_t maxSize)
            : maxSize_(maxSize), nextChunkId_(0), urgentRetransmit_(false) {}

        // 删除拷贝构造函数和赋值运算符
        EnhancedSlidingWindow(const EnhancedSlidingWindow&) = delete;
        EnhancedSlidingWindow& operator=(const EnhancedSlidingWindow&) = delete;

        // 允许移动
        EnhancedSlidingWindow(EnhancedSlidingWindow&&) = delete;
        EnhancedSlidingWindow& operator=(EnhancedSlidingWindow&&) = delete;

        // 尝试获取一个窗口槽位
        bool tryAcquireSlot(uint32_t& chunkId);

        // 标记为需要重传
        void markForRetransmit(uint32_t chunkId, const std::vector<uint32_t>& missingPackets);

        // 强制完成（用于死锁恢复）
        void forceComplete(uint32_t chunkId);

        // 检查超时
        std::vector<uint32_t> checkTimeouts(std::chrono::milliseconds timeout);

        // 获取需要重传的chunk
        std::vector<std::pair<uint32_t, std::vector<uint32_t>>> getRetransmitCandidates();

        size_t availableSlots() const;
        bool isUrgent() const { return urgentRetransmit_; }

    private:
        size_t maxSize_;
        std::atomic<uint32_t> nextChunkId_;
        std::vector<WindowSlot> slots_;
        std::unordered_set<uint32_t> inFlight_;
        std::unordered_set<uint32_t> completed_;
        mutable std::mutex mutex_;
        bool urgentRetransmit_{false};
    };

    // 发送方逻辑
    void senderMain();
    bool performHandshakeAsSender();
    void transferDataAsSender();

    // 接收方逻辑
    void receiverMain();
    bool performHandshakeAsReceiver();
    void transferDataAsReceiver();

    // 数据包处理
    void handleDataPacket(const DataPacket& packet, const SocketAddress& sender);
    void handleControlPacket(const ControlHeader& header, const std::vector<uint8_t>& payload,
                            const SocketAddress& sender);

    // 重传管理
    void scheduleRetransmit(uint32_t chunkId, const std::vector<uint32_t>& missingPackets);
    void retransmitWorker();

    // 主动NACK监控
    void nackMonitorThread();

    // 死锁恢复
    void deadlockRecovery();

    // 工作线程
    void workerThread(uint32_t threadId);

    SessionConfig config_;
    SessionState state_;

    // 网络组件
    std::unique_ptr<ControlSession> controlSession_;
    std::unique_ptr<UdpServer> dataServer_;
    SocketAddress remoteAddr_;

    // 文件管理
    std::unique_ptr<class ChunkManager> chunkManager_;
    std::unique_ptr<class FileMapper> fileMapper_;
    std::string filename_;
    std::string finalFilename_;

    // 滑动窗口
    std::unique_ptr<EnhancedSlidingWindow> slidingWindow_;

    // 线程管理
    std::thread mainThread_;
    std::vector<std::thread> workerThreads_;
    std::thread retransmitThread_;
    std::thread nackMonitorThread_;
    std::thread deadlockMonitorThread_;

    // 同步
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> running_{false};

    // 传输状态
    std::queue<uint32_t> sendQueue_;
    std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> chunkSendTime_;
    std::unordered_map<uint32_t, std::vector<uint32_t>> retransmitQueue_;
    std::unordered_set<uint32_t> urgentRetransmitSet_;

    // 性能统计
    uint64_t startTime_ = 0;
    uint64_t totalBytesSent_ = 0;
    uint64_t totalBytesReceived_ = 0;

    // 死锁检测
    std::chrono::steady_clock::time_point lastProgressTime_;
    static constexpr auto DEADLOCK_TIMEOUT = std::chrono::seconds(30);
};

} // namespace hruft