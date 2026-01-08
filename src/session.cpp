#include "session.h"
#include "file_mapper.h"
#include "crypto.h"
#include "utils.h"

#include <iostream>
#include <thread>
#include <chrono>
#include <algorithm>
#include <fstream>
#include <cstring>

#include "cli.h"

namespace hruft {

// SessionState 实现
SessionState::SessionState() {
    lastUpdateTime_ = std::chrono::steady_clock::now();
}

SessionState::SessionState(const SessionState& other) {
    phase_ = other.phase_.load();
    progress_ = other.progress_.load();
    bytesTransferred_ = other.bytesTransferred_.load();
    lastUpdateTime_ = other.lastUpdateTime_;
    retryCount_ = other.retryCount_.load();
    error_ = other.error_;
}

SessionState& SessionState::operator=(const SessionState& other) {
    if (this != &other) {
        phase_ = other.phase_.load();
        progress_ = other.progress_.load();
        bytesTransferred_ = other.bytesTransferred_.load();
        lastUpdateTime_ = other.lastUpdateTime_;
        retryCount_ = other.retryCount_.load();
        error_ = other.error_;
    }
    return *this;
}

uint64_t SessionState::getSpeed() const {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - lastUpdateTime_).count();

    if (elapsed <= 0) {
        return 0;
    }

    return bytesTransferred_ * 1000 / static_cast<uint64_t>(elapsed);
}

// ControlSession 实现
ControlSession::ControlSession() : initialized_(false) {}

ControlSession::~ControlSession() {
    shutdown();
}

bool ControlSession::initialize(uint16_t controlPort, const std::string& bindIP) {
    if (initialized_) {
        return true;
    }

    socket_ = std::make_unique<UdpSocket>();

    if (!socket_->bind(controlPort)) {
        return false;
    }

    // 设置非阻塞
    if (!socket_->setNonBlocking(true)) {
        return false;
    }

    // 设置大缓冲区
    socket_->setRecvBufferSize(1024 * 1024); // 1MB
    socket_->setSendBufferSize(1024 * 1024); // 1MB

    running_ = true;
    receiverThread_ = std::thread(&ControlSession::receiverThread, this);
    initialized_ = true;

    return true;
}

void ControlSession::shutdown() {
    if (!initialized_) return;

    running_ = false;

    if (receiverThread_.joinable()) {
        receiverThread_.join();
    }

    socket_.reset();
    initialized_ = false;
}

bool ControlSession::sendControlPacket(const SocketAddress& addr, ControlType type,
                                      uint32_t chunkId, const uint8_t* payload,
                                      uint16_t payloadLen) {
    if (!initialized_) return false;

    ControlHeader header(type, chunkId, payloadLen);

    std::vector<uint8_t> buffer(sizeof(ControlHeader) + payloadLen);
    memcpy(buffer.data(), &header, sizeof(ControlHeader));

    if (payload && payloadLen > 0) {
        memcpy(buffer.data() + sizeof(ControlHeader), payload, payloadLen);
    }

    ssize_t sent = socket_->sendTo(buffer.data(), buffer.size(), addr);
    return sent == static_cast<ssize_t>(buffer.size());
}

bool ControlSession::receiveControlPacket(ControlHeader& header,
                                         std::vector<uint8_t>& payload,
                                         SocketAddress& sender, int timeoutMs) {
    if (!initialized_) return false;

    std::vector<uint8_t> buffer(1500); // MTU大小

    if (timeoutMs >= 0) {
        socket_->setNonBlocking(false);

        // 设置超时
#ifdef _WIN32
        DWORD timeout = timeoutMs;
        setsockopt(socket_->getHandle(), SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
        struct timeval timeout;
        timeout.tv_sec = timeoutMs / 1000;
        timeout.tv_usec = (timeoutMs % 1000) * 1000;
        setsockopt(socket_->getHandle(), SOL_SOCKET, SO_RCVTIMEO,
                   &timeout, sizeof(timeout));
#endif
    } else {
        socket_->setNonBlocking(true);
    }

    ssize_t received = socket_->recvFrom(buffer.data(), buffer.size(), sender);

    if (received < static_cast<ssize_t>(sizeof(ControlHeader))) {
        return false;
    }

    memcpy(&header, buffer.data(), sizeof(ControlHeader));

    if (!header.validate()) {
        return false;
    }

    payload.resize(header.payloadLen);
    if (header.payloadLen > 0) {
        memcpy(payload.data(), buffer.data() + sizeof(ControlHeader), header.payloadLen);
    }

    return true;
}

void ControlSession::receiverThread() {
    std::vector<uint8_t> buffer(1500);
    SocketAddress sender;

    while (running_) {
        ssize_t received = socket_->recvFrom(buffer.data(), buffer.size(), sender);

        if (received > 0) {
            if (static_cast<size_t>(received) >= sizeof(ControlHeader)) {
                ControlHeader header;
                memcpy(&header, buffer.data(), sizeof(ControlHeader));

                if (header.validate() && callback_) {
                    std::vector<uint8_t> payload(header.payloadLen);
                    if (header.payloadLen > 0) {
                        memcpy(payload.data(), buffer.data() + sizeof(ControlHeader),
                               header.payloadLen);
                    }

                    callback_(header, payload);
                }
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}

void ControlSession::setCallback(Callback callback) {
    callback_ = callback;
}

// EnhancedSlidingWindow 实现
bool TransferSession::EnhancedSlidingWindow::tryAcquireSlot(uint32_t& chunkId) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (inFlight_.size() >= maxSize_) {
        // 检查是否有可以提前释放的slot（比如超时很久）
        auto now = std::chrono::steady_clock::now();
        for (auto& slot : slots_) {
            if (slot.needsRetransmit &&
                slot.retryCount > 3) {
                // 重试次数过多，释放slot
                forceComplete(slot.chunkId);
                break;
            }
        }

        if (inFlight_.size() >= maxSize_) {
            return false;
        }
    }

    chunkId = nextChunkId_++;
    WindowSlot slot;
    slot.chunkId = chunkId;
    slot.sendTime = std::chrono::steady_clock::now();
    slot.acknowledged = false;
    slot.needsRetransmit = false;
    slot.retryCount = 0;
    slots_.push_back(slot);
    inFlight_.insert(chunkId);

    return true;
}

void TransferSession::EnhancedSlidingWindow::markForRetransmit(uint32_t chunkId, const std::vector<uint32_t>& missingPackets) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& slot : slots_) {
        if (slot.chunkId == chunkId) {
            slot.needsRetransmit = true;
            slot.missingPackets = missingPackets;
            slot.retryCount++;

            // 如果是紧急重传，可以提前释放其他slot
            if (slot.retryCount > 5) {
                urgentRetransmit_ = true;
            }
            break;
        }
    }
}

void TransferSession::EnhancedSlidingWindow::forceComplete(uint32_t chunkId) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = inFlight_.find(chunkId);
    if (it != inFlight_.end()) {
        inFlight_.erase(it);
        completed_.insert(chunkId);

        // 从slots中移除
        auto new_end = std::remove_if(slots_.begin(), slots_.end(),
            [chunkId](const WindowSlot& slot) {
                return slot.chunkId == chunkId;
            });
        slots_.erase(new_end, slots_.end());
    }
}

std::vector<uint32_t> TransferSession::EnhancedSlidingWindow::checkTimeouts(std::chrono::milliseconds timeout) {
    std::vector<uint32_t> timedOut;
    auto now = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& slot : slots_) {
        if (!slot.acknowledged && !slot.needsRetransmit) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - slot.sendTime);

            if (elapsed > timeout) {
                timedOut.push_back(slot.chunkId);
            }
        }
    }

    return timedOut;
}

std::vector<std::pair<uint32_t, std::vector<uint32_t>>> TransferSession::EnhancedSlidingWindow::getRetransmitCandidates() {
    std::vector<std::pair<uint32_t, std::vector<uint32_t>>> candidates;

    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& slot : slots_) {
        if (slot.needsRetransmit && !slot.missingPackets.empty()) {
            candidates.emplace_back(slot.chunkId, slot.missingPackets);
        }
    }

    return candidates;
}

size_t TransferSession::EnhancedSlidingWindow::availableSlots() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return maxSize_ - inFlight_.size();
}

// TransferSession 实现
TransferSession::TransferSession() : lastProgressTime_(std::chrono::steady_clock::now()) {
    // 不在这里初始化滑动窗口
}

TransferSession::~TransferSession() {
    stop();
}

bool TransferSession::startAsSender(const SessionConfig& config, const std::string& filename) {
    config_ = config;
    filename_ = filename;

    // 初始化文件映射器
    fileMapper_ = std::make_unique<FileMapper>();
    if (!fileMapper_->openForRead(filename)) {
        state_.setError("Failed to open file for reading");
        return false;
    }

    // 映射整个文件到内存
    if (!fileMapper_->mapFile()) {
        state_.setError("Failed to map file to memory");
        return false;
    }

    // 初始化chunk管理器
    chunkManager_ = std::make_unique<ChunkManager>(filename, config.getChunkSizeBytes());
    if (!chunkManager_->initializeForSend()) {
        state_.setError("Failed to initialize chunk manager");
        return false;
    }

    // 初始化控制会话
    controlSession_ = std::make_unique<ControlSession>();
    if (!controlSession_->initialize(config.remoteControlPort)) {
        state_.setError("Failed to initialize control session");
        return false;
    }

    // 初始化数据服务器
    dataServer_ = std::make_unique<UdpServer>();
    dataServer_->setPacketHandler([this](const uint8_t* data, size_t len,
                                         const SocketAddress& sender) {
        try {
            auto packet = DataPacket::deserialize(data, len);
            handleDataPacket(packet, sender);
        } catch (const std::exception& e) {
            std::cerr << "Failed to parse data packet: " << e.what() << std::endl;
        }
    });

    if (!dataServer_->start(config.localDataPort, config.workerThreads)) {
        state_.setError("Failed to start data server");
        return false;
    }

    // 设置远程地址
    remoteAddr_ = SocketAddress(config.remoteIP, config.remoteControlPort);

    // 初始化滑动窗口 - 使用动态分配
    slidingWindow_ = std::make_unique<EnhancedSlidingWindow>(config.windowSize);

    running_ = true;
    state_.setPhase(SessionState::Phase::HANDSHAKE);

    // 启动主线程
    mainThread_ = std::thread(&TransferSession::senderMain, this);

    return true;
}

bool TransferSession::startAsReceiver(const SessionConfig& config, const std::string& filename) {
    config_ = config;
    filename_ = filename;
    finalFilename_ = filename;

    // 初始化控制会话
    controlSession_ = std::make_unique<ControlSession>();
    if (!controlSession_->initialize(config.remoteControlPort)) {
        state_.setError("Failed to initialize control session");
        return false;
    }

    // 设置控制包回调
    controlSession_->setCallback([this](const ControlHeader& header,
                                        const std::vector<uint8_t>& payload) {
        SocketAddress sender;
        handleControlPacket(header, payload, sender);
    });

    // 初始化数据服务器
    dataServer_ = std::make_unique<UdpServer>();
    dataServer_->setPacketHandler([this](const uint8_t* data, size_t len,
                                         const SocketAddress& sender) {
        try {
            auto packet = DataPacket::deserialize(data, len);
            handleDataPacket(packet, sender);
        } catch (const std::exception& e) {
            std::cerr << "Failed to parse data packet: " << e.what() << std::endl;
        }
    });

    if (!dataServer_->start(config.localDataPort, config.workerThreads)) {
        state_.setError("Failed to start data server");
        return false;
    }

    // 启动主动NACK监控线程
    running_ = true;
    nackMonitorThread_ = std::thread(&TransferSession::nackMonitorThread, this);

    state_.setPhase(SessionState::Phase::HANDSHAKE);

    // 启动主线程
    mainThread_ = std::thread(&TransferSession::receiverMain, this);

    return true;
}

void TransferSession::stop() {
    if (!running_) return;

    running_ = false;

    // 通知所有线程
    cv_.notify_all();

    // 等待线程结束
    if (mainThread_.joinable()) {
        mainThread_.join();
    }

    for (auto& thread : workerThreads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    if (retransmitThread_.joinable()) {
        retransmitThread_.join();
    }

    if (nackMonitorThread_.joinable()) {
        nackMonitorThread_.join();
    }

    if (deadlockMonitorThread_.joinable()) {
        deadlockMonitorThread_.join();
    }

    // 关闭组件
    if (controlSession_) {
        controlSession_->shutdown();
    }

    if (dataServer_) {
        dataServer_->stop();
    }
}

bool TransferSession::waitForCompletion(int timeoutMs) {
    std::unique_lock<std::mutex> lock(mutex_);

    if (timeoutMs < 0) {
        cv_.wait(lock, [this]() {
            return !running_ || state_.getPhase() == SessionState::Phase::COMPLETED ||
                   state_.getPhase() == SessionState::Phase::ERROR;
        });
    } else {
        cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this]() {
            return !running_ || state_.getPhase() == SessionState::Phase::COMPLETED ||
                   state_.getPhase() == SessionState::Phase::ERROR;
        });
    }

    return state_.getPhase() == SessionState::Phase::COMPLETED;
}

// 以下为内部方法实现
void TransferSession::senderMain() {
    try {
        if (!performHandshakeAsSender()) {
            state_.setError("Handshake failed");
            state_.setPhase(SessionState::Phase::ERROR);
            cv_.notify_all();
            return;
        }

        state_.setPhase(SessionState::Phase::TRANSFER);
        lastProgressTime_ = std::chrono::steady_clock::now();

        // 死锁检测定时器
        deadlockMonitorThread_ = std::thread([this]() {
            while (running_) {
                std::this_thread::sleep_for(std::chrono::seconds(5));

                auto now = std::chrono::steady_clock::now();
                auto elapsed = now - lastProgressTime_;

                if (elapsed > DEADLOCK_TIMEOUT) {
                    std::cerr << "WARNING: Possible deadlock detected, attempting recovery..." << std::endl;
                    deadlockRecovery();
                    lastProgressTime_ = now;
                }
            }
        });

        // 启动工作线程
        for (uint32_t i = 0; i < config_.workerThreads && running_; ++i) {
            workerThreads_.emplace_back([this, i]() {
                workerThread(i);
            });
        }

        // 启动重传线程
        retransmitThread_ = std::thread(&TransferSession::retransmitWorker, this);

        // 等待完成
        while (running_ && chunkManager_->getCompletedChunks() < chunkManager_->getTotalChunks()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            // 更新进度
            state_.setProgress(chunkManager_->getProgress());
        }

        if (deadlockMonitorThread_.joinable()) {
            deadlockMonitorThread_.join();
        }

        // 发送完成包
        auto fileHash = chunkManager_->calculateFileHash();
        FileDonePayload done;
        memcpy(done.fileHash, fileHash.data(), std::min(fileHash.size(), size_t(32)));

        controlSession_->sendControlPacket(remoteAddr_, ControlType::FILE_DONE, 0,
                                          reinterpret_cast<uint8_t*>(&done), sizeof(done));

        if (state_.getPhase() == SessionState::Phase::TRANSFER) {
            state_.setPhase(SessionState::Phase::COMPLETED);
        }

        cv_.notify_all();
    } catch (const std::exception& e) {
        state_.setError(std::string("Sender error: ") + e.what());
        state_.setPhase(SessionState::Phase::ERROR);
        cv_.notify_all();
    }
}

void TransferSession::receiverMain() {
    try {
        if (!performHandshakeAsReceiver()) {
            state_.setError("Handshake failed");
            state_.setPhase(SessionState::Phase::ERROR);
            cv_.notify_all();
            return;
        }

        state_.setPhase(SessionState::Phase::TRANSFER);

        // 等待数据传输完成
        while (running_ && chunkManager_->getCompletedChunks() < chunkManager_->getTotalChunks()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            // 更新进度
            state_.setProgress(chunkManager_->getProgress());
        }

        // 验证文件完整性
        state_.setPhase(SessionState::Phase::VERIFICATION);

        // 保存文件
        if (chunkManager_->saveFile()) {
            // 重命名临时文件到最终文件名
            std::string tempFile = filename_ + ".hruft_tmp";
            std::string finalFile = finalFilename_;

            // 删除已存在的最终文件
            std::remove(finalFile.c_str());

            // 重命名临时文件
            if (rename(tempFile.c_str(), finalFile.c_str()) != 0) {
                // 如果重命名失败，尝试复制
                std::ifstream src(tempFile, std::ios::binary);
                std::ofstream dst(finalFile, std::ios::binary);

                if (src && dst) {
                    dst << src.rdbuf();
                    src.close();
                    dst.close();
                    std::remove(tempFile.c_str());
                } else {
                    state_.setError("Failed to rename/copy file to final destination");
                    state_.setPhase(SessionState::Phase::ERROR);
                    return;
                }
            }
        } else {
            state_.setError("Failed to save file");
            state_.setPhase(SessionState::Phase::ERROR);
            return;
        }

        state_.setPhase(SessionState::Phase::COMPLETED);
        cv_.notify_all();
    } catch (const std::exception& e) {
        state_.setError(std::string("Receiver error: ") + e.what());
        state_.setPhase(SessionState::Phase::ERROR);
        cv_.notify_all();
    }
}

bool TransferSession::performHandshakeAsSender() {
    // 构建SYN包
    SynPayload syn;
    syn.fileSize = chunkManager_->getFileSize();
    syn.chunkSize = config_.getChunkSizeBytes();
    syn.totalChunks = chunkManager_->getTotalChunks();
    syn.fileNameLen = static_cast<uint16_t>(filename_.length());

    std::vector<uint8_t> synBuffer(sizeof(SynPayload) - 1 + filename_.length());
    memcpy(synBuffer.data(), &syn, sizeof(SynPayload) - 1);
    memcpy(synBuffer.data() + sizeof(SynPayload) - 1, filename_.c_str(), filename_.length());

    // 发送SYN
    if (!controlSession_->sendControlPacket(remoteAddr_, ControlType::SYN, 0,
                                           synBuffer.data(),
                                           static_cast<uint16_t>(synBuffer.size()))) {
        return false;
    }

    // 等待SYN_ACK
    ControlHeader response;
    std::vector<uint8_t> responsePayload;
    SocketAddress sender;

    if (!controlSession_->receiveControlPacket(response, responsePayload,
                                              sender, config_.handshakeTimeout)) {
        return false;
    }

    if (response.type != ControlType::SYN_ACK) {
        return false;
    }

    if (responsePayload.size() < sizeof(SynAckPayload)) {
        return false;
    }

    SynAckPayload* ack = reinterpret_cast<SynAckPayload*>(responsePayload.data());
    if (!ack->acceptTransfer) {
        state_.setError("Receiver rejected transfer: " + std::string(ack->reason));
        return false;
    }

    return true;
}

bool TransferSession::performHandshakeAsReceiver() {
    // 等待SYN
    ControlHeader syn;
    std::vector<uint8_t> synPayload;
    SocketAddress sender;

    // 设置超时
    Timer timer;

    while (running_ && timer.elapsed() < config_.handshakeTimeout) {
        if (controlSession_->receiveControlPacket(syn, synPayload, sender, 100)) {
            if (syn.type == ControlType::SYN) {
                // 解析SYN
                if (synPayload.size() < sizeof(SynPayload) - 1) {
                    return false;
                }

                SynPayload* synData = reinterpret_cast<SynPayload*>(synPayload.data());
                uint64_t fileSize = synData->fileSize;
                uint32_t chunkSize = synData->chunkSize;
                uint32_t totalChunks = synData->totalChunks;

                // 提取文件名
                std::string filename;
                if (synData->fileNameLen > 0) {
                    filename.assign(reinterpret_cast<char*>(synPayload.data() +
                                     sizeof(SynPayload) - 1),
                                     synData->fileNameLen);
                }

                // 检查磁盘空间
                uint64_t freeSpace = Platform::getFreeDiskSpace(".");

                // 计算实际需要的空间（考虑文件系统开销）
                uint64_t requiredSpace = fileSize;

                // NTFS额外开销（4KB对齐 + MFT条目等）
#ifdef _WIN32
                uint64_t clusterSize = 4096; // 典型NTFS簇大小
                requiredSpace = ((fileSize + clusterSize - 1) / clusterSize) * clusterSize;
                requiredSpace += 1024 * 1024; // 额外1MB用于元数据
#endif

                // 检查是否有足够的连续空间
                bool accept = false;
                std::string reason;

                if (freeSpace < requiredSpace * 1.2) { // 留20%余量
                    reason = "Insufficient disk space. Required: " +
                             formatBytes(requiredSpace) +
                             " (with overhead), Available: " +
                             formatBytes(freeSpace);
                } else if (freeSpace < 100 * 1024 * 1024) { // 少于100MB
                    reason = "Low disk space. Available: " +
                             formatBytes(freeSpace) +
                             " (less than 100MB)";
                } else {
                    // 尝试预分配文件测试实际可用空间
                    std::string testFile = Platform::getTempDirectory() + "/hruft_space_test.tmp";
                    FileMapper testMapper;
                    bool canAllocate = testMapper.openForWrite(testFile, 10 * 1024 * 1024); // 10MB测试

                    if (!canAllocate) {
                        reason = "Cannot allocate test file. Disk may be read-only or full.";
                    } else {
                        accept = true;
                        testMapper.close();
                        std::remove(testFile.c_str());
                    }
                }

                // 发送SYN_ACK
                SynAckPayload ack(freeSpace, chunkSize, accept, reason);

                remoteAddr_ = sender;

                if (!controlSession_->sendControlPacket(remoteAddr_, ControlType::SYN_ACK, 0,
                                                       reinterpret_cast<uint8_t*>(&ack),
                                                       sizeof(ack))) {
                    return false;
                }

                if (!accept) {
                    state_.setError("Handshake rejected: " + reason);
                    return false;
                }

                // 创建临时文件验证实际空间
                std::string tempFile = filename_ + ".hruft_tmp";
                chunkManager_ = std::make_unique<ChunkManager>(tempFile, chunkSize);

                if (!chunkManager_->initializeForReceive(fileSize, totalChunks)) {
                    state_.setError("Failed to initialize temporary file");
                    return false;
                }

                // 保存最终文件名，传输完成后重命名
                finalFilename_ = filename_;

                // 初始化滑动窗口
                slidingWindow_ = std::make_unique<EnhancedSlidingWindow>(config_.windowSize);

                return true;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return false;
}

void TransferSession::workerThread(uint32_t threadId) {
    while (running_) {
        // 使用滑动窗口获取发送权限
        uint32_t chunkId;
        if (!slidingWindow_->tryAcquireSlot(chunkId)) {
            // 窗口已满，等待
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

            // 检查是否需要处理重传
            auto retransmits = slidingWindow_->getRetransmitCandidates();
            for (const auto& [cid, packets] : retransmits) {
                scheduleRetransmit(cid, packets);
                slidingWindow_->markForRetransmit(cid, packets);
            }

            continue;
        }

        // 获取chunk数据
        auto* chunk = chunkManager_->getChunk(chunkId);
        if (!chunk) {
            // 所有chunk已发送完成
            break;
        }

        // 发送块的元数据
        ChunkMetaPayload meta(chunk->hash, static_cast<uint32_t>(chunk->packetReceived.size()));
        controlSession_->sendControlPacket(remoteAddr_, ControlType::CHUNK_META, chunk->id,
                                          reinterpret_cast<uint8_t*>(&meta), sizeof(meta));

        // 发送块的所有包
        uint32_t packetCount = static_cast<uint32_t>(chunk->packetReceived.size());
        uint64_t chunkOffset = chunk->offset;

        for (uint32_t seq = 0; seq < packetCount && running_; ++seq) {
            uint64_t packetOffset = seq * config_.packetSize;
            uint32_t packetSize = static_cast<uint32_t>(
                std::min(static_cast<uint64_t>(config_.packetSize),
                         chunk->size - packetOffset));

            // 从映射的内存中读取数据
            const uint8_t* packetData = fileMapper_->data() + chunkOffset + packetOffset;

            // 创建数据包
            DataPacket packet(chunk->id, seq, chunkOffset + packetOffset,
                             packetData, packetSize);

            // 发送数据包
            std::vector<uint8_t> serialized = packet.serialize();
            dataServer_->sendTo(serialized.data(), serialized.size(), remoteAddr_);

            // 更新统计
            totalBytesSent_ += packetSize;
            state_.addBytesTransferred(packetSize);

            // 限速（如果需要）
            if (config_.packetSize > 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }

        // 记录发送时间
        {
            std::lock_guard<std::mutex> lock(mutex_);
            chunkSendTime_[chunk->id] = std::chrono::steady_clock::now();
        }

        // 更新最后进度时间
        lastProgressTime_ = std::chrono::steady_clock::now();

        // 等待确认或超时
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void TransferSession::handleDataPacket(const DataPacket& packet, const SocketAddress& sender) {
    if (!chunkManager_) {
        return;
    }

    // 处理接收到的数据包
    if (chunkManager_->processReceivedPacket(packet.header.chunkId,
                                            packet.header.seq,
                                            packet.header.offset,
                                            packet.data.data(),
                                            packet.header.dataLen)) {
        // 更新统计
        totalBytesReceived_ += packet.header.dataLen;
        state_.addBytesTransferred(packet.header.dataLen);
    }
}

void TransferSession::handleControlPacket(const ControlHeader& header, const std::vector<uint8_t>& payload,
                                         const SocketAddress& sender) {
    switch (header.type) {
        case ControlType::CHUNK_CONFIRM: {
            // 块确认
            std::lock_guard<std::mutex> lock(mutex_);
            chunkSendTime_.erase(header.chunkId);
            slidingWindow_->forceComplete(header.chunkId);
            break;
        }

        case ControlType::CHUNK_NACK: {
            // 处理主动NACK
            auto missingPackets = ChunkNackPayload::parse(payload.data(), payload.size());

            if (!missingPackets.empty()) {
                // 立即调度重传
                scheduleRetransmit(header.chunkId, missingPackets);

                // 如果是紧急NACK，可以标记该chunk为高优先级
                std::lock_guard<std::mutex> lock(mutex_);
                urgentRetransmitSet_.insert(header.chunkId);
            }
            break;
        }

        case ControlType::CHUNK_RETRY: {
            // 重传请求
            if (!payload.empty() && payload.size() >= 4) {
                uint32_t missingCount = *reinterpret_cast<const uint32_t*>(payload.data());
                const uint32_t* missingPackets = reinterpret_cast<const uint32_t*>(payload.data() + 4);

                std::vector<uint32_t> packets(missingPackets,
                                              missingPackets + missingCount);

                scheduleRetransmit(header.chunkId, packets);
            }
            break;
        }

        case ControlType::FILE_DONE: {
            // 文件传输完成
            if (!payload.empty() && payload.size() >= sizeof(FileDonePayload)) {
                const FileDonePayload* done = reinterpret_cast<const FileDonePayload*>(payload.data());

                // 验证文件哈希
                auto fileHash = chunkManager_->calculateFileHash();
                if (memcmp(done->fileHash, fileHash.data(), 32) == 0) {
                    state_.setPhase(SessionState::Phase::COMPLETED);
                } else {
                    state_.setError("File hash verification failed");
                    state_.setPhase(SessionState::Phase::ERROR);
                }
            }
            break;
        }

        case ControlType::HEARTBEAT: {
            // 心跳包，更新最后活动时间
            lastProgressTime_ = std::chrono::steady_clock::now();
            break;
        }

        default:
            break;
    }
}

void TransferSession::scheduleRetransmit(uint32_t chunkId,
                                        const std::vector<uint32_t>& missingPackets) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 分组重传，避免一次发送太多
    const size_t BATCH_SIZE = 10;
    for (size_t i = 0; i < missingPackets.size(); i += BATCH_SIZE) {
        size_t end = std::min(i + BATCH_SIZE, missingPackets.size());
        std::vector<uint32_t> batch(missingPackets.begin() + i,
                                    missingPackets.begin() + end);

        retransmitQueue_[chunkId].insert(retransmitQueue_[chunkId].end(),
                                        batch.begin(), batch.end());
    }

    // 限制每个chunk的最大待重传包数
    const size_t MAX_PENDING_RETRANSMITS = 100;
    if (retransmitQueue_[chunkId].size() > MAX_PENDING_RETRANSMITS) {
        retransmitQueue_[chunkId].resize(MAX_PENDING_RETRANSMITS);
    }

    // 去重和排序
    std::sort(retransmitQueue_[chunkId].begin(), retransmitQueue_[chunkId].end());
    retransmitQueue_[chunkId].erase(std::unique(retransmitQueue_[chunkId].begin(),
                                               retransmitQueue_[chunkId].end()),
                                   retransmitQueue_[chunkId].end());

    cv_.notify_all();
}

void TransferSession::retransmitWorker() {
    while (running_) {
        std::unordered_map<uint32_t, std::vector<uint32_t>> queue;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(100), [this]() {
                return !retransmitQueue_.empty() || !running_;
            });

            if (!running_) break;

            if (!retransmitQueue_.empty()) {
                queue.swap(retransmitQueue_);
            } else {
                continue;
            }
        }

        // 处理重传
        for (const auto& entry : queue) {
            uint32_t chunkId = entry.first;
            const std::vector<uint32_t>& missingSeqs = entry.second;

            const auto* chunk = chunkManager_->getChunk(chunkId);
            if (!chunk) {
                std::cerr << "Retransmit: Chunk " << chunkId << " not found" << std::endl;
                continue;
            }

            // 检查是否是紧急重传
            bool urgent = urgentRetransmitSet_.find(chunkId) != urgentRetransmitSet_.end();

            // 重传丢失的包
            for (uint32_t seq : missingSeqs) {
                if (!running_) break;

                uint64_t packetOffset = seq * config_.packetSize;
                uint32_t packetSize = std::min(
                    config_.packetSize,
                    static_cast<uint32_t>(chunk->size - packetOffset)
                );

                // 从文件读取数据
                const uint8_t* packetData = fileMapper_->data() + chunk->offset + packetOffset;

                // 创建重传包，设置重传标志
                DataPacket packet(chunkId, seq, chunk->offset + packetOffset,
                                 packetData, packetSize, PacketFlags::RETRANSMIT);

                auto serialized = packet.serialize();

                // 发送重传包
                if (!dataServer_->sendTo(serialized.data(), serialized.size(), remoteAddr_)) {
                    std::cerr << "Retransmit: Failed to send packet " << seq
                              << " for chunk " << chunkId << std::endl;
                } else {
                    // 更新统计
                    totalBytesSent_ += packetSize;
                    state_.addBytesTransferred(packetSize);
                }

                // 如果是紧急重传，立即发送，否则短暂延迟
                if (!urgent) {
                    std::this_thread::sleep_for(std::chrono::microseconds(10));
                }
            }

            // 更新重传统计
            state_.incrementRetryCount();

            // 重置块的发送时间
            {
                std::lock_guard<std::mutex> lock(mutex_);
                chunkSendTime_[chunkId] = std::chrono::steady_clock::now();
                urgentRetransmitSet_.erase(chunkId);
            }
        }
    }
}

void TransferSession::nackMonitorThread() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50)); // 20Hz监控

        if (!chunkManager_) continue;

        // 获取需要主动NACK的包
        auto nacks = chunkManager_->getProactiveNacks();

        for (const auto& nackInfo : nacks) {
            // 构建NACK包
            auto nackPayload = ChunkNackPayload::create(nackInfo.missingPackets);

            // 发送NACK
            controlSession_->sendControlPacket(remoteAddr_,
                                               ControlType::CHUNK_NACK,
                                               nackInfo.chunkId,
                                               nackPayload.data(),
                                               static_cast<uint16_t>(nackPayload.size()));

            // 紧急NACK需要立即重传
            if (nackInfo.urgent) {
                scheduleRetransmit(nackInfo.chunkId, nackInfo.missingPackets);
            }

            // 更新统计
            state_.incrementRetryCount();
        }
    }
}

void TransferSession::deadlockRecovery() {
    std::lock_guard<std::mutex> lock(mutex_);

    // 1. 检查滑动窗口状态
    if (slidingWindow_->availableSlots() == 0) {
        // 窗口已满，可能死锁

        // 2. 获取超时的chunk
        auto timedOut = slidingWindow_->checkTimeouts(std::chrono::milliseconds(config_.chunkTimeout));

        for (uint32_t chunkId : timedOut) {
            // 强制完成超时的chunk（放弃重传）
            slidingWindow_->forceComplete(chunkId);

            // 记录错误
            std::cerr << "Deadlock recovery: forcing completion of chunk "
                      << chunkId << std::endl;

            // 更新状态
            state_.incrementRetryCount();
        }

        // 3. 如果是紧急重传状态，扩大窗口
        if (slidingWindow_->isUrgent()) {
            // 临时扩大窗口大小
            std::cerr << "Deadlock recovery: temporarily increasing window size" << std::endl;
            // 可以通过动态调整config_.windowSize来实现
        }
    }

    // 4. 重置某些chunk的重传计数
    retransmitQueue_.clear();
    urgentRetransmitSet_.clear();

    // 5. 发送心跳包确认连接
    controlSession_->sendControlPacket(remoteAddr_, ControlType::HEARTBEAT, 0, nullptr, 0);
}

} // namespace hruft