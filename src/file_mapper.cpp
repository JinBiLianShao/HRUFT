#include "file_mapper.h"
#include "utils.h"
#include "crypto.h"

#include <cstring>
#include <iostream>
#include <algorithm>
#include <fstream>
#include <chrono>

namespace hruft {
#ifdef _WIN32

    // 系统信息缓存
    static SYSTEM_INFO GetSystemInfoCached() {
        static SYSTEM_INFO sysInfo = {0};
        static std::once_flag onceFlag;
        std::call_once(onceFlag, []() {
            GetSystemInfo(&sysInfo);
        });
        return sysInfo;
    }

    FileMapper::FileMapper() : fileHandle_(INVALID_HANDLE_VALUE), mapHandle_(NULL),
                                mappedData_(nullptr), mappedSize_(0), isReadOnly_(false) {
    }

    FileMapper::~FileMapper() {
        close();
    }

    bool FileMapper::openForRead(const std::string &filename) {
        if (isOpen()) {
            close();
        }

        filename_ = filename;

        fileHandle_ = CreateFileA(filename.c_str(),
                                  GENERIC_READ,
                                  FILE_SHARE_READ,
                                  NULL,
                                  OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL,
                                  NULL);

        if (fileHandle_ == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            std::cerr << "CreateFile failed with error: " << err << std::endl;
            return false;
        }

        LARGE_INTEGER size;
        if (!GetFileSizeEx(fileHandle_, &size)) {
            CloseHandle(fileHandle_);
            fileHandle_ = INVALID_HANDLE_VALUE;
            return false;
        }

        fileSize_ = static_cast<uint64_t>(size.QuadPart);
        isReadOnly_ = true;

        std::cout << "FileMapper: Opened file " << filename
                  << " for reading, size: " << fileSize_ << " bytes" << std::endl;
        return true;
    }

    bool FileMapper::openForWrite(const std::string &filename, uint64_t fileSize) {
        if (isOpen()) {
            close();
        }

        filename_ = filename;
        fileSize_ = fileSize;

        fileHandle_ = CreateFileA(filename.c_str(),
                                  GENERIC_READ | GENERIC_WRITE,
                                  0,
                                  NULL,
                                  CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL,
                                  NULL);

        if (fileHandle_ == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            std::cerr << "CreateFile failed with error: " << err << std::endl;
            return false;
        }

        // 设置文件大小
        LARGE_INTEGER size;
        size.QuadPart = static_cast<LONGLONG>(fileSize);
        if (!SetFilePointerEx(fileHandle_, size, NULL, FILE_BEGIN) ||
            !SetEndOfFile(fileHandle_)) {
            CloseHandle(fileHandle_);
            fileHandle_ = INVALID_HANDLE_VALUE;
            return false;
        }

        isReadOnly_ = false;

        std::cout << "FileMapper: Opened file " << filename
                  << " for writing, size: " << fileSize_ << " bytes" << std::endl;
        return true;
    }

    bool FileMapper::mapFile(uint64_t offset, uint64_t length) {
        if (!isOpen() || isMapped()) {
            std::cerr << "FileMapper: Cannot map - file not open or already mapped" << std::endl;
            return false;
        }

        if (length == 0) {
            length = fileSize_ - offset;
        }

        // 获取系统分配粒度并计算对齐偏移
        SYSTEM_INFO sysInfo = GetSystemInfoCached();
        DWORD allocationGranularity = sysInfo.dwAllocationGranularity;

        // 计算对齐偏移量
        uint64_t alignedOffset = (offset / allocationGranularity) * allocationGranularity;
        uint64_t offsetAdjustment = offset - alignedOffset;
        uint64_t adjustedLength = length + offsetAdjustment;

        DWORD offsetHigh = static_cast<DWORD>(alignedOffset >> 32);
        DWORD offsetLow = static_cast<DWORD>(alignedOffset & 0xFFFFFFFF);

        // 确保映射长度不为0
        if (adjustedLength == 0) {
            adjustedLength = allocationGranularity;
        }

        DWORD protect = isReadOnly_ ? PAGE_READONLY : PAGE_READWRITE;
        DWORD access = isReadOnly_ ? FILE_MAP_READ : FILE_MAP_ALL_ACCESS;

        mapHandle_ = CreateFileMapping(fileHandle_,
                                       NULL,
                                       protect,
                                       0,
                                       0,
                                       NULL);

        if (mapHandle_ == NULL) {
            DWORD err = GetLastError();
            std::cerr << "CreateFileMapping failed: " << err << std::endl;
            return false;
        }

        // 使用对齐后的偏移和调整后的长度
        mappedData_ = static_cast<uint8_t *>(MapViewOfFile(mapHandle_,
                                                           access,
                                                           offsetHigh,
                                                           offsetLow,
                                                           static_cast<SIZE_T>(adjustedLength)));

        if (mappedData_ == NULL) {
            DWORD err = GetLastError();
            std::cerr << "MapViewOfFile failed: " << err << std::endl;
            CloseHandle(mapHandle_);
            mapHandle_ = NULL;
            return false;
        }

        // 调整指针到实际偏移位置
        mappedData_ += offsetAdjustment;
        mappedSize_ = length;

        std::cout << "FileMapper: Successfully mapped file at address "
                  << static_cast<void*>(mappedData_)
                  << ", size: " << mappedSize_ << std::endl;
        return true;
    }

    void FileMapper::unmapFile() {
        if (mappedData_) {
            // 还原指针到对齐的起始地址再取消映射
            SYSTEM_INFO sysInfo = GetSystemInfoCached();
            DWORD allocationGranularity = sysInfo.dwAllocationGranularity;

            // 计算对齐的起始地址
            uintptr_t addr = reinterpret_cast<uintptr_t>(mappedData_);
            uintptr_t alignedAddr = (addr / allocationGranularity) * allocationGranularity;

            // 恢复原始指针
            mappedData_ -= (addr - alignedAddr);

            UnmapViewOfFile(mappedData_);
            mappedData_ = nullptr;
            mappedSize_ = 0;
        }

        if (mapHandle_) {
            CloseHandle(mapHandle_);
            mapHandle_ = NULL;
        }
    }

    bool FileMapper::sync() {
        if (mappedData_) {
            return FlushViewOfFile(mappedData_, mappedSize_) != 0;
        }
        return true;
    }

    bool FileMapper::isOpen() const {
        return fileHandle_ != INVALID_HANDLE_VALUE;
    }

    void FileMapper::close() {
        unmapFile();

        if (fileHandle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(fileHandle_);
            fileHandle_ = INVALID_HANDLE_VALUE;
        }

        fileSize_ = 0;
        filename_.clear();
        isReadOnly_ = false;
    }

#else // Linux/Unix implementation

    FileMapper::FileMapper() : fileDescriptor_(-1), mappedData_(nullptr),
                               mappedSize_(0), isReadOnly_(false) {
    }

    FileMapper::~FileMapper() {
        close();
    }

    bool FileMapper::openForRead(const std::string &filename) {
        if (isOpen()) {
            close();
        }

        filename_ = filename;

        fileDescriptor_ = open(filename.c_str(), O_RDONLY);
        if (fileDescriptor_ < 0) {
            perror("open failed");
            return false;
        }

        struct stat st;
        if (fstat(fileDescriptor_, &st) < 0) {
            hruft::close(fileDescriptor_);
            fileDescriptor_ = -1;
            return false;
        }

        fileSize_ = static_cast<uint64_t>(st.st_size);
        isReadOnly_ = true;

        std::cout << "FileMapper: Opened file " << filename
                  << " for reading, size: " << fileSize_ << " bytes" << std::endl;
        return true;
    }

    bool FileMapper::openForWrite(const std::string &filename, uint64_t fileSize) {
        if (isOpen()) {
            close();
        }

        filename_ = filename;
        fileSize_ = fileSize;

        fileDescriptor_ = open(filename.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fileDescriptor_ < 0) {
            perror("open failed");
            return false;
        }

        // 设置文件大小
        if (ftruncate(fileDescriptor_, static_cast<off_t>(fileSize)) < 0) {
            hruft::close(fileDescriptor_);
            fileDescriptor_ = -1;
            return false;
        }

        isReadOnly_ = false;

        std::cout << "FileMapper: Opened file " << filename
                  << " for writing, size: " << fileSize_ << " bytes" << std::endl;
        return true;
    }

    bool FileMapper::mapFile(uint64_t offset, uint64_t length) {
        if (!isOpen() || isMapped()) {
            std::cerr << "FileMapper: Cannot map - file not open or already mapped" << std::endl;
            return false;
        }

        if (length == 0) {
            length = fileSize_ - offset;
        }

        int prot = isReadOnly_ ? PROT_READ : (PROT_READ | PROT_WRITE);

        void *addr = mmap(nullptr,
                          length,
                          prot,
                          MAP_SHARED,
                          fileDescriptor_,
                          static_cast<off_t>(offset));

        if (addr == MAP_FAILED) {
            perror("mmap failed");
            std::cerr << "FileMapper: mmap failed for file " << filename_ << std::endl;
            return false;
        }

        mappedData_ = static_cast<uint8_t *>(addr);
        mappedSize_ = length;

        std::cout << "FileMapper: Successfully mapped file at address " << addr
                  << ", size: " << mappedSize_ << std::endl;
        return true;
    }

    void FileMapper::unmapFile() {
        if (mappedData_) {
            munmap(mappedData_, mappedSize_);
            mappedData_ = nullptr;
            mappedSize_ = 0;
        }
    }

    bool FileMapper::sync() {
        if (mappedData_) {
            return msync(mappedData_, mappedSize_, MS_SYNC) == 0;
        }
        return true;
    }

    bool FileMapper::isOpen() const {
        return fileDescriptor_ >= 0;
    }

    void FileMapper::close() {
        unmapFile();

        if (fileDescriptor_ >= 0) {
            hruft::close(fileDescriptor_);
            fileDescriptor_ = -1;
        }

        fileSize_ = 0;
        filename_.clear();
        isReadOnly_ = false;
    }

#endif

    // ChunkManager 实现 - 修改构造函数
    ChunkManager::ChunkManager(const std::string &filename, uint64_t chunkSize, FileMapper* externalMapper)
        : filename_(filename)
          , chunkSize_(chunkSize)
          , fileMapper_(nullptr)
          , ownsFileMapper_(false) {

        if (externalMapper) {
            fileMapper_ = externalMapper;
            ownsFileMapper_ = false;
        } else {
            fileMapper_ = new FileMapper();
            ownsFileMapper_ = true;
        }
    }

    ChunkManager::~ChunkManager() {
        if (ownsFileMapper_ && fileMapper_) {
            delete fileMapper_;
        }
    }

    bool ChunkManager::initializeForSend(FileMapper* mapper) {
        // 如果有外部mapper，使用它
        if (mapper && !ownsFileMapper_) {
            fileMapper_ = mapper;
            ownsFileMapper_ = false;
        }

        // 如果fileMapper_没有打开，则打开它
        if (!fileMapper_->isOpen()) {
            if (!fileMapper_->openForRead(filename_)) {
                std::cerr << "ChunkManager: Failed to open file " << filename_ << std::endl;
                return false;
            }
        }

        // 如果文件没有映射，映射它（发送方需要映射整个文件用于读取）
        if (!fileMapper_->isMapped()) {
            if (!fileMapper_->mapFile()) {
                std::cerr << "ChunkManager: Failed to map file " << filename_ << std::endl;
                return false;
            }
        }

        totalFileSize_ = fileMapper_->size();
        std::cout << "ChunkManager: Using file mapper, size: " << totalFileSize_ << " bytes" << std::endl;

        // 计算总块数
        uint32_t totalChunks = static_cast<uint32_t>((totalFileSize_ + chunkSize_ - 1) / chunkSize_);
        chunks_.resize(totalChunks);

        std::cout << "ChunkManager: Total chunks: " << totalChunks
                  << ", chunk size: " << chunkSize_ << " bytes" << std::endl;

        // 初始化块信息
        for (uint32_t i = 0; i < totalChunks; ++i) {
            chunks_[i].id = i;
            chunks_[i].offset = i * chunkSize_;
            chunks_[i].size = std::min(chunkSize_, totalFileSize_ - chunks_[i].offset);

            // 计算包数量
            uint32_t packetCount = static_cast<uint32_t>((chunks_[i].size + 1400 - 1) / 1400);
            chunks_[i].packetReceived.resize(packetCount, true); // 发送方标记为全部已接收

            // 计算哈希
            calculateChunkHash(chunks_[i]);
        }

        return true;
    }

    bool ChunkManager::initializeForReceive(uint64_t totalSize, uint32_t totalChunks) {
        totalFileSize_ = totalSize;
        chunks_.resize(totalChunks);

        // 如果是外部mapper，确保它已打开
        if (!fileMapper_->isOpen()) {
            // 创建文件用于写入
            if (!fileMapper_->openForWrite(filename_, totalSize)) {
                std::cerr << "ChunkManager: Failed to create file " << filename_ << std::endl;
                return false;
            }
        }

        // 映射整个文件用于写入
        if (!fileMapper_->mapFile(0, totalSize)) {
            std::cerr << "ChunkManager: Failed to map file " << filename_ << std::endl;
            return false;
        }

        // 初始化块信息
        for (uint32_t i = 0; i < totalChunks; ++i) {
            chunks_[i].id = i;
            chunks_[i].offset = i * chunkSize_;
            chunks_[i].size = std::min(chunkSize_, totalFileSize_ - chunks_[i].offset);

            uint32_t packetCount = static_cast<uint32_t>((chunks_[i].size + 1400 - 1) / 1400);
            chunks_[i].packetReceived.resize(packetCount, false); // 接收方初始化为未接收
        }

        return true;
    }

    void ChunkManager::calculateChunkHash(Chunk &chunk) {
        if (!fileMapper_ || !fileMapper_->isMapped()) {
            std::cerr << "Error: File is not mapped for hash calculation" << std::endl;
            return;
        }

        // 直接从映射的内存中读取数据
        const uint8_t* chunkData = fileMapper_->data() + chunk.offset;

        // 计算SHA-256
        auto hash = Crypto::calculateSHA256(chunkData, chunk.size);
        memcpy(chunk.hash, hash.data(), std::min(hash.size(), size_t(32)));

        std::cout << "ChunkManager: Calculated hash for chunk " << chunk.id
                  << " (offset: " << chunk.offset << ", size: " << chunk.size << ")" << std::endl;
    }

    ChunkManager::Chunk *ChunkManager::getNextChunkToSend() {
        std::lock_guard<std::mutex> lock(chunksMutex_);

        if (nextChunkToSend_ >= chunks_.size()) {
            return nullptr;
        }

        return &chunks_[nextChunkToSend_++];
    }

    void ChunkManager::markChunkSent(uint32_t chunkId) {
        if (chunkId >= chunks_.size()) return;

        // 发送方不需要做特殊处理
    }

    bool ChunkManager::processReceivedPacket(uint32_t chunkId, uint32_t seq,
                                             uint64_t offset, const uint8_t *data,
                                             uint16_t length) {
        std::lock_guard<std::mutex> lock(chunksMutex_);

        if (chunkId >= chunks_.size()) {
            return false;
        }

        Chunk &chunk = chunks_[chunkId];

        // 验证偏移量
        uint64_t packetOffset = seq * 1400;
        if (packetOffset + length > chunk.size) {
            return false;
        }

        // 写入数据
        uint8_t *dest = fileMapper_->data() + chunk.offset + packetOffset;
        memcpy(dest, data, length);

        // 标记包为已接收
        if (seq < chunk.packetReceived.size()) {
            if (!chunk.packetReceived[seq]) {
                chunk.packetReceived[seq] = true;

                // 更新接收状态并检测是否需要NACK
                updateChunkReceiveState(chunk, seq);
            }
        }

        // 检查块是否完成
        if (!chunk.completed) {
            bool complete = true;
            for (bool received: chunk.packetReceived) {
                if (!received) {
                    complete = false;
                    break;
                }
            }

            if (complete) {
                chunk.completed = true;
                completedChunks_++;
                chunk.pendingNacks.clear(); // 完成时清空待NACK列表
            }
        }

        return true;
    }

    bool ChunkManager::updateChunkReceiveState(Chunk &chunk, uint32_t seq) {
        bool needNack = false;

        // 如果收到的包序号远远超过期望序号，说明有丢包
        if (seq > chunk.nextExpectedSeq + NACK_THRESHOLD_URGENT) {
            // 紧急情况：跳跃超过10个包
            for (uint32_t i = chunk.nextExpectedSeq; i < seq; ++i) {
                if (i < chunk.packetReceived.size() && !chunk.packetReceived[i]) {
                    chunk.pendingNacks.push_back(i);
                }
            }
            chunk.pendingNacks.push_back(seq); // 标记当前包也需要重传确认
            needNack = true;
            chunk.urgentNack = true;
        } else if (seq > chunk.nextExpectedSeq + NACK_THRESHOLD_NORMAL) {
            // 一般情况：跳跃超过3个包
            for (uint32_t i = chunk.nextExpectedSeq; i < seq; ++i) {
                if (i < chunk.packetReceived.size() && !chunk.packetReceived[i]) {
                    chunk.pendingNacks.push_back(i);
                }
            }
            needNack = true;
        }

        // 更新期望的下一包序号
        if (seq == chunk.nextExpectedSeq) {
            // 如果是期望的包，移动期望序号直到遇到缺失的包
            while (chunk.nextExpectedSeq < chunk.packetReceived.size() &&
                   chunk.packetReceived[chunk.nextExpectedSeq]) {
                chunk.nextExpectedSeq++;
            }
        }

        // 去重并排序
        std::sort(chunk.pendingNacks.begin(), chunk.pendingNacks.end());
        chunk.pendingNacks.erase(std::unique(chunk.pendingNacks.begin(),
                                             chunk.pendingNacks.end()),
                                 chunk.pendingNacks.end());

        return needNack;
    }

    std::vector<ChunkManager::NackInfo> ChunkManager::getProactiveNacks() {
        std::vector<NackInfo> nacks;
        std::lock_guard<std::mutex> lock(chunksMutex_);

        auto now = std::chrono::steady_clock::now();

        for (auto &chunk: chunks_) {
            if (chunk.completed || chunk.pendingNacks.empty()) {
                continue;
            }

            // 检查冷却时间
            if (now - chunk.lastNackTime < NACK_COOLDOWN) {
                continue;
            }

            // 限制每次NACK的最大包数
            const size_t MAX_PACKETS_PER_NACK = 50;
            size_t count = std::min(chunk.pendingNacks.size(), MAX_PACKETS_PER_NACK);

            NackInfo info;
            info.chunkId = chunk.id;
            info.missingPackets.assign(chunk.pendingNacks.begin(),
                                       chunk.pendingNacks.begin() + count);
            info.urgent = chunk.urgentNack;

            nacks.push_back(info);

            // 更新最后发送时间
            chunk.lastNackTime = now;
            chunk.nackCount++;

            // 如果发送了NACK，清空已处理的pending（但保留未处理的）
            if (count == chunk.pendingNacks.size()) {
                chunk.pendingNacks.clear();
            } else {
                chunk.pendingNacks.erase(chunk.pendingNacks.begin(),
                                         chunk.pendingNacks.begin() + count);
            }

            // 紧急NACK只发一次
            if (chunk.urgentNack) {
                chunk.urgentNack = false;
            }
        }

        return nacks;
    }

    bool ChunkManager::isChunkComplete(uint32_t chunkId) const {
        std::lock_guard<std::mutex> lock(chunksMutex_);

        if (chunkId >= chunks_.size()) {
            return false;
        }

        return chunks_[chunkId].completed;
    }

    bool ChunkManager::verifyChunk(uint32_t chunkId, const uint8_t *expectedHash) {
        std::lock_guard<std::mutex> lock(chunksMutex_);

        if (chunkId >= chunks_.size()) {
            return false;
        }

        Chunk &chunk = chunks_[chunkId];

        if (!chunk.completed) {
            return false;
        }

        // 计算接收数据的哈希
        const uint8_t *chunkData = fileMapper_->data() + chunk.offset;
        auto actualHash = Crypto::calculateSHA256(chunkData, chunk.size);

        bool verified = memcmp(expectedHash, actualHash.data(), 32) == 0;
        chunk.verified = verified;

        return verified;
    }

    std::vector<uint32_t> ChunkManager::getMissingPackets(uint32_t chunkId) const {
        std::vector<uint32_t> missing;
        std::lock_guard<std::mutex> lock(chunksMutex_);

        if (chunkId >= chunks_.size()) {
            return missing;
        }

        const Chunk &chunk = chunks_[chunkId];

        for (uint32_t i = 0; i < chunk.packetReceived.size(); ++i) {
            if (!chunk.packetReceived[i]) {
                missing.push_back(i);
            }
        }

        return missing;
    }

    double ChunkManager::getProgress() const {
        std::lock_guard<std::mutex> lock(chunksMutex_);
        if (chunks_.empty()) return 0.0;
        return static_cast<double>(completedChunks_) / chunks_.size();
    }

    uint32_t ChunkManager::getCompletedChunks() const {
        return completedChunks_;
    }

    uint32_t ChunkManager::getTotalChunks() const {
        std::lock_guard<std::mutex> lock(chunksMutex_);
        return static_cast<uint32_t>(chunks_.size());
    }

    bool ChunkManager::saveFile() {
        return fileMapper_->sync();
    }

    std::vector<uint8_t> ChunkManager::calculateFileHash() {
        return Crypto::calculateSHA256(fileMapper_->data(), totalFileSize_);
    }

    const ChunkManager::Chunk *ChunkManager::getChunk(uint32_t chunkId) const {
        std::lock_guard<std::mutex> lock(chunksMutex_);

        if (chunkId >= chunks_.size()) {
            return nullptr;
        }
        return &chunks_[chunkId];
    }
} // namespace hruft