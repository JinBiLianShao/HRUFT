#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <array>
#include <stdexcept>

namespace hruft {
    // 魔术字
    constexpr uint32_t HRUF_MAGIC = 0x48525546; // 'HRUF'

    // 协议版本
    constexpr uint16_t PROTOCOL_VERSION = 0x0001;

    // 控制包类型
    enum class ControlType : uint8_t {
        SYN = 0x01,
        SYN_ACK = 0x02,
        CHUNK_META = 0x03,
        CHUNK_CONFIRM = 0x04,
        CHUNK_RETRY = 0x05,
        FILE_DONE = 0x06,
        CHUNK_NACK = 0x07, // 主动NACK
        HEARTBEAT = 0x08, // 心跳包
        ERROR = 0xFF
    };

    // 数据包标志位
    enum PacketFlags : uint16_t {
        LAST_PACKET = 0x01,
        RETRANSMIT = 0x02,
        ENCRYPTED = 0x04
    };

#pragma pack(push, 1)

    // 控制包头部
    struct ControlHeader {
        uint32_t magic;
        uint16_t version;
        ControlType type;
        uint32_t chunkId;
        uint16_t payloadLen;
        uint8_t reserved[2];

        ControlHeader() = default;

        ControlHeader(ControlType t, uint32_t id, uint16_t len = 0)
            : magic(HRUF_MAGIC)
              , version(PROTOCOL_VERSION)
              , type(t)
              , chunkId(id)
              , payloadLen(len)
              , reserved{0, 0} {
        }

        bool validate() const {
            return magic == HRUF_MAGIC &&
                   version == PROTOCOL_VERSION &&
                   static_cast<uint8_t>(type) <= static_cast<uint8_t>(ControlType::ERROR);
        }
    };

    // 数据包头部
    struct DataHeader {
        uint32_t magic;
        uint16_t version;
        uint32_t chunkId;
        uint32_t seq;
        uint64_t offset;
        uint16_t dataLen;
        uint16_t flags;
        uint32_t crc32;

        DataHeader() = default;

        DataHeader(uint32_t cid, uint32_t s, uint64_t off, uint16_t len, uint16_t f = 0)
            : magic(HRUF_MAGIC)
              , version(PROTOCOL_VERSION)
              , chunkId(cid)
              , seq(s)
              , offset(off)
              , dataLen(len)
              , flags(f)
              , crc32(0) {
        }

        bool validate() const {
            return magic == HRUF_MAGIC && version == PROTOCOL_VERSION;
        }

        bool isLastPacket() const { return flags & LAST_PACKET; }
        bool isRetransmit() const { return flags & RETRANSMIT; }
        bool isEncrypted() const { return flags & ENCRYPTED; }
    };

#pragma pack(pop)

    // 控制包载荷结构
    struct SynPayload {
        uint64_t fileSize;
        uint32_t chunkSize;
        uint32_t totalChunks;
        uint16_t fileNameLen;
        char fileName[1]; // 可变长度

        static size_t totalSize(const std::string &filename) {
            return sizeof(SynPayload) - 1 + filename.length();
        }
    };

    struct SynAckPayload {
        uint64_t availableSpace;
        uint32_t maxChunkSize;
        uint8_t acceptTransfer; // 1 = 接受，0 = 拒绝
        char reason[256]; // 拒绝原因

        SynAckPayload() = default;

        SynAckPayload(uint64_t space, uint32_t maxChunk, bool accept, const std::string &reasonStr = "") {
            availableSpace = space;
            maxChunkSize = maxChunk;
            acceptTransfer = accept ? 1 : 0;
            strncpy(reason, reasonStr.c_str(), sizeof(reason) - 1);
            reason[sizeof(reason) - 1] = '\0';
        }
    };

    struct ChunkMetaPayload {
        uint8_t hash[32]; // SHA-256
        uint32_t packetCount;

        ChunkMetaPayload() = default;

        ChunkMetaPayload(const uint8_t *hashData, uint32_t count) {
            memcpy(hash, hashData, 32);
            packetCount = count;
        }
    };

    struct FileDonePayload {
        uint8_t fileHash[32]; // 整个文件的SHA-256
    };

    // NACK载荷结构
    struct ChunkNackPayload {
        uint32_t missingCount;
        uint32_t missingPackets[1]; // 可变长度

        static std::vector<uint8_t> create(const std::vector<uint32_t> &missingPackets) {
            size_t size = sizeof(ChunkNackPayload) - sizeof(uint32_t) +
                          missingPackets.size() * sizeof(uint32_t);
            std::vector<uint8_t> buffer(size);
            ChunkNackPayload *nack = reinterpret_cast<ChunkNackPayload *>(buffer.data());
            nack->missingCount = static_cast<uint32_t>(missingPackets.size());
            for (size_t i = 0; i < missingPackets.size(); ++i) {
                nack->missingPackets[i] = missingPackets[i];
            }
            return buffer;
        }

        static std::vector<uint32_t> parse(const uint8_t *data, size_t length) {
            if (length < sizeof(ChunkNackPayload) - sizeof(uint32_t)) {
                return {};
            }

            const ChunkNackPayload *nack = reinterpret_cast<const ChunkNackPayload *>(data);
            std::vector<uint32_t> missingPackets;

            size_t maxCount = (length - (sizeof(ChunkNackPayload) - sizeof(uint32_t))) / sizeof(uint32_t);
            size_t count = std::min(static_cast<size_t>(nack->missingCount), maxCount);

            missingPackets.reserve(count);
            for (size_t i = 0; i < count; ++i) {
                missingPackets.push_back(nack->missingPackets[i]);
            }

            return missingPackets;
        }
    };

    // 工具函数
    inline uint32_t calculate_crc32(const uint8_t *data, size_t length) {
        // 简单的CRC32实现（实际可以使用zlib或table）
        uint32_t crc = 0xFFFFFFFF;
        for (size_t i = 0; i < length; ++i) {
            crc ^= data[i];
            for (int j = 0; j < 8; ++j) {
                crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
            }
        }
        return ~crc;
    }

    // 数据包类
    class DataPacket {
    public:
        DataHeader header;
        std::vector<uint8_t> data;

        DataPacket() = default;

        DataPacket(uint32_t chunkId, uint32_t seq, uint64_t offset,
                   const uint8_t *dataPtr, uint16_t dataLen, uint16_t flags = 0)
            : header(chunkId, seq, offset, dataLen, flags)
              , data(dataPtr, dataPtr + dataLen) {
            header.crc32 = calculate_crc32(data.data(), data.size());
        }

        std::vector<uint8_t> serialize() const {
            std::vector<uint8_t> buffer(sizeof(DataHeader) + data.size());
            memcpy(buffer.data(), &header, sizeof(DataHeader));
            memcpy(buffer.data() + sizeof(DataHeader), data.data(), data.size());
            return buffer;
        }

        static DataPacket deserialize(const uint8_t *buffer, size_t length) {
            if (length < sizeof(DataHeader)) {
                throw std::runtime_error("Invalid packet length");
            }

            DataPacket packet;
            memcpy(&packet.header, buffer, sizeof(DataHeader));

            if (!packet.header.validate()) {
                throw std::runtime_error("Invalid packet header");
            }

            size_t dataLen = length - sizeof(DataHeader);
            if (dataLen != packet.header.dataLen) {
                throw std::runtime_error("Packet data length mismatch");
            }

            packet.data.resize(dataLen);
            memcpy(packet.data.data(), buffer + sizeof(DataHeader), dataLen);

            // 验证CRC
            uint32_t expectedCrc = calculate_crc32(packet.data.data(), dataLen);
            if (packet.header.crc32 != expectedCrc) {
                throw std::runtime_error("CRC32 validation failed");
            }

            return packet;
        }
    };
} // namespace hruft
