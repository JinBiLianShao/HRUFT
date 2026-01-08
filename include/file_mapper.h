#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <memory>
#include <stdexcept>
#include <vector>
#include <mutex>

namespace hruft {
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

    class FileMapper {
    public:
        FileMapper();

        ~FileMapper();

        // 打开文件用于读取
        bool openForRead(const std::string &filename);

        // 创建或打开文件用于写入
        bool openForWrite(const std::string &filename, uint64_t fileSize = 0);

        // 映射整个文件或部分文件到内存
        bool mapFile(uint64_t offset = 0, uint64_t length = 0);

        // 取消映射
        void unmapFile();

        // 同步数据到磁盘
        bool sync();

        // 获取映射的内存指针
        uint8_t *data() const { return mappedData_; }

        // 获取文件大小
        uint64_t size() const { return fileSize_; }

        // 检查文件是否打开
        bool isOpen() const;

        // 检查文件是否映射
        bool isMapped() const { return mappedData_ != nullptr; }

        // 关闭文件
        void close();

    private:
        std::string filename_;
        uint64_t fileSize_ = 0;
        uint8_t *mappedData_ = nullptr;
        uint64_t mappedSize_ = 0;

#ifdef _WIN32
        HANDLE fileHandle_ = INVALID_HANDLE_VALUE;
        HANDLE mapHandle_ = NULL;
#else
        int fileDescriptor_ = -1;
#endif
    };

    // 文件块管理器
    class ChunkManager {
    public:
        struct Chunk {
            uint32_t id;
            uint64_t offset;
            uint64_t size;
            std::vector<bool> packetReceived; // 每个包是否收到
            uint8_t hash[32]; // SHA-256哈希
            bool completed = false;
            bool verified = false;

            // 主动NACK相关字段
            uint32_t nextExpectedSeq = 0;
            std::vector<uint32_t> pendingNacks;
            std::chrono::steady_clock::time_point lastNackTime;
            uint32_t nackCount = 0;
            bool urgentNack = false;
        };

        // 主动NACK信息
        struct NackInfo {
            uint32_t chunkId;
            std::vector<uint32_t> missingPackets;
            bool urgent; // 是否紧急（序号跳跃大）
        };

        ChunkManager(const std::string &filename, uint64_t chunkSize);

        ~ChunkManager();

        // 初始化用于发送
        bool initializeForSend();

        // 初始化用于接收
        bool initializeForReceive(uint64_t totalSize, uint32_t totalChunks);

        // 获取下一个待发送的块
        Chunk *getNextChunkToSend();

        // 标记块为发送完成
        void markChunkSent(uint32_t chunkId);

        // 处理接收到的数据包
        bool processReceivedPacket(uint32_t chunkId, uint32_t seq,
                                   uint64_t offset, const uint8_t *data, uint16_t length);

        // 检查块是否完整
        bool isChunkComplete(uint32_t chunkId) const;

        // 验证块的哈希
        bool verifyChunk(uint32_t chunkId, const uint8_t *expectedHash);

        // 获取缺失的数据包信息（用于重传）
        std::vector<uint32_t> getMissingPackets(uint32_t chunkId) const;

        // 获取主动NACK信息
        std::vector<NackInfo> getProactiveNacks();

        // 获取进度信息
        double getProgress() const;

        uint32_t getCompletedChunks() const;

        uint32_t getTotalChunks() const;

        // 保存文件
        bool saveFile();

        // 计算整个文件的哈希
        std::vector<uint8_t> calculateFileHash();

        const Chunk *getChunk(uint32_t chunkId) const;

        // 获取文件大小
        uint64_t getFileSize() const { return totalFileSize_; }

    private:
        void calculateChunkHash(Chunk &chunk);

        bool loadChunkToMemory(Chunk &chunk);

        bool updateChunkReceiveState(Chunk &chunk, uint32_t seq);

        static constexpr uint32_t NACK_THRESHOLD_NORMAL = 3;
        static constexpr uint32_t NACK_THRESHOLD_URGENT = 10;
        static constexpr auto NACK_COOLDOWN = std::chrono::milliseconds(100);

        std::string filename_;
        uint64_t chunkSize_;
        uint64_t totalFileSize_ = 0;
        std::vector<Chunk> chunks_;
        std::unique_ptr<FileMapper> fileMapper_;
        std::vector<uint8_t> chunkBuffer_; // 用于计算哈希的缓冲区

        uint32_t nextChunkToSend_ = 0;
        std::atomic<uint32_t> completedChunks_{0};
        mutable std::mutex chunksMutex_;
    };
} // namespace hruft
