#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <memory>

namespace hruft {
    // 简单加密接口（实际应该使用OpenSSL或类似库）
    class Crypto {
    public:
        Crypto();

        ~Crypto();

        // 设置密钥
        bool setKey(const std::string &key);

        // 生成随机密钥
        static std::string generateKey(size_t length = 32);

        // 加密数据
        std::vector<uint8_t> encrypt(const uint8_t *data, size_t length,
                                     uint64_t nonce);

        // 解密数据
        std::vector<uint8_t> decrypt(const uint8_t *data, size_t length,
                                     uint64_t nonce);

        // 计算HMAC-SHA256
        static std::vector<uint8_t> calculateHMAC(const uint8_t *data, size_t length,
                                                  const std::string &key);

        // 计算SHA-256
        static std::vector<uint8_t> calculateSHA256(const uint8_t *data, size_t length);

        // 验证HMAC
        static bool verifyHMAC(const uint8_t *data, size_t length,
                               const uint8_t *hmac, size_t hmacLen,
                               const std::string &key);

    private:
        class Impl;
        std::unique_ptr<Impl> impl_;
    };

    // 安全会话
    class SecureSession {
    public:
        SecureSession();

        ~SecureSession();

        // 初始化安全参数
        bool initialize(const std::string &key, uint64_t sessionId);

        // 封装数据包（加密 + HMAC）
        std::vector<uint8_t> encapsulate(const uint8_t *header, size_t headerLen,
                                         const uint8_t *data, size_t dataLen);

        // 解封装数据包（验证HMAC + 解密）
        bool decapsulate(const uint8_t *packet, size_t packetLen,
                         std::vector<uint8_t> &header, std::vector<uint8_t> &data);

        // 获取当前nonce
        uint64_t getNonce() const { return nonce_; }

    private:
        std::string key_;
        uint64_t sessionId_ = 0;
        uint64_t nonce_ = 0;
        Crypto crypto_;
    };
} // namespace hruft
