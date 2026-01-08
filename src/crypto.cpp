#include "crypto.h"

#include <cstring>
#include <stdexcept>
#include <random>

// 使用OpenSSL或类似的加密库
#ifdef USE_OPENSSL
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/aes.h>
#include <openssl/rand.h>

namespace hruft {
    class Crypto::Impl {
    public:
        Impl() {
            ctx_ = EVP_CIPHER_CTX_new();
            if (!ctx_) {
                throw std::runtime_error("Failed to create cipher context");
            }
        }

        ~Impl() {
            if (ctx_) {
                EVP_CIPHER_CTX_free(ctx_);
            }
        }

        bool setKey(const std::string &key) {
            key_ = key;
            return true;
        }

        std::vector<uint8_t> encrypt(const uint8_t *data, size_t length, uint64_t nonce) {
            if (key_.empty()) {
                return std::vector<uint8_t>(data, data + length);
            }

            // AES-CTR 加密
            std::vector<uint8_t> ciphertext(length);
            std::vector<uint8_t> iv(16); // 128-bit IV

            // 使用nonce作为IV的一部分
            memcpy(iv.data(), &nonce, sizeof(nonce));

            int len;
            if (EVP_EncryptInit_ex(ctx_, EVP_aes_256_ctr(), nullptr,
                                   reinterpret_cast<const uint8_t *>(key_.data()), iv.data()) != 1) {
                throw std::runtime_error("Encryption init failed");
            }

            if (EVP_EncryptUpdate(ctx_, ciphertext.data(), &len, data, static_cast<int>(length)) != 1) {
                throw std::runtime_error("Encryption failed");
            }

            return ciphertext;
        }

        std::vector<uint8_t> decrypt(const uint8_t *data, size_t length, uint64_t nonce) {
            if (key_.empty()) {
                return std::vector<uint8_t>(data, data + length);
            }

            // AES-CTR 解密（与加密相同）
            return encrypt(data, length, nonce);
        }

    private:
        EVP_CIPHER_CTX *ctx_ = nullptr;
        std::string key_;
    };

#else // 使用简单实现（仅用于演示）

namespace hruft {
    class Crypto::Impl {
    public:
        bool setKey(const std::string &key) {
            key_ = key;
            return true;
        }

        std::vector<uint8_t> encrypt(const uint8_t *data, size_t length, uint64_t nonce) {
            // 简单XOR加密（仅用于演示）
            std::vector<uint8_t> result(data, data + length);

            if (!key_.empty()) {
                for (size_t i = 0; i < length; ++i) {
                    result[i] ^= key_[i % key_.size()] ^ static_cast<uint8_t>(nonce >> ((i % 8) * 8));
                }
            }

            return result;
        }

        std::vector<uint8_t> decrypt(const uint8_t *data, size_t length, uint64_t nonce) {
            // XOR加密是对称的
            return encrypt(data, length, nonce);
        }

    private:
        std::string key_;
    };

#endif

    // Crypto类实现
    Crypto::Crypto() : impl_(std::make_unique<Impl>()) {
    }

    Crypto::~Crypto() = default;

    bool Crypto::setKey(const std::string &key) {
        return impl_->setKey(key);
    }

    std::string Crypto::generateKey(size_t length) {
        static const char charset[] =
                "0123456789"
                "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                "abcdefghijklmnopqrstuvwxyz";

        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dist(0, sizeof(charset) - 2);

        std::string key;
        key.reserve(length);

        for (size_t i = 0; i < length; ++i) {
            key += charset[dist(gen)];
        }

        return key;
    }

    std::vector<uint8_t> Crypto::encrypt(const uint8_t *data, size_t length, uint64_t nonce) {
        return impl_->encrypt(data, length, nonce);
    }

    std::vector<uint8_t> Crypto::decrypt(const uint8_t *data, size_t length, uint64_t nonce) {
        return impl_->decrypt(data, length, nonce);
    }

#ifdef USE_OPENSSL

    std::vector<uint8_t> Crypto::calculateHMAC(const uint8_t *data, size_t length,
                                               const std::string &key) {
        std::vector<uint8_t> hmac(SHA256_DIGEST_LENGTH);

        HMAC(EVP_sha256(),
             key.data(), static_cast<int>(key.size()),
             data, length,
             hmac.data(), nullptr);

        return hmac;
    }

    std::vector<uint8_t> Crypto::calculateSHA256(const uint8_t *data, size_t length) {
        std::vector<uint8_t> hash(SHA256_DIGEST_LENGTH);

        SHA256_CTX sha256;
        SHA256_Init(&sha256);
        SHA256_Update(&sha256, data, length);
        SHA256_Final(hash.data(), &sha256);

        return hash;
    }

#else // 简单实现

    std::vector<uint8_t> Crypto::calculateHMAC(const uint8_t *data, size_t length,
                                               const std::string &key) {
        // 简单哈希实现（仅用于演示）
        std::vector<uint8_t> hmac(32, 0);

        for (size_t i = 0; i < length; ++i) {
            hmac[i % 32] ^= data[i];
        }

        for (size_t i = 0; i < key.size(); ++i) {
            hmac[i % 32] ^= key[i];
        }

        return hmac;
    }

    std::vector<uint8_t> Crypto::calculateSHA256(const uint8_t *data, size_t length) {
        // 简单哈希实现（仅用于演示）
        std::vector<uint8_t> hash(32, 0);

        for (size_t i = 0; i < length; ++i) {
            hash[i % 32] ^= data[i];
        }

        return hash;
    }

#endif

    bool Crypto::verifyHMAC(const uint8_t *data, size_t length,
                            const uint8_t *hmac, size_t hmacLen,
                            const std::string &key) {
        auto calculated = calculateHMAC(data, length, key);

        if (calculated.size() != hmacLen) {
            return false;
        }

        return memcmp(calculated.data(), hmac, hmacLen) == 0;
    }

    // SecureSession 实现
    SecureSession::SecureSession() = default;

    SecureSession::~SecureSession() = default;

    bool SecureSession::initialize(const std::string &key, uint64_t sessionId) {
        key_ = key;
        sessionId_ = sessionId;
        nonce_ = 0;

        if (!key_.empty()) {
            return crypto_.setKey(key_);
        }

        return true;
    }

    std::vector<uint8_t> SecureSession::encapsulate(const uint8_t *header, size_t headerLen,
                                                    const uint8_t *data, size_t dataLen) {
        // 如果启用加密，先加密数据
        std::vector<uint8_t> encryptedData;
        if (!key_.empty()) {
            encryptedData = crypto_.encrypt(data, dataLen, nonce_++);
            data = encryptedData.data();
            dataLen = encryptedData.size();
        }

        // 计算HMAC（包括header和加密后的数据）
        std::vector<uint8_t> toHash(headerLen + dataLen);
        memcpy(toHash.data(), header, headerLen);
        memcpy(toHash.data() + headerLen, data, dataLen);

        auto hmac = Crypto::calculateHMAC(toHash.data(), toHash.size(), key_);

        // 构建最终包：[header][encrypted data][hmac]
        std::vector<uint8_t> packet(headerLen + dataLen + hmac.size());
        memcpy(packet.data(), header, headerLen);
        memcpy(packet.data() + headerLen, data, dataLen);
        memcpy(packet.data() + headerLen + dataLen, hmac.data(), hmac.size());

        return packet;
    }

    bool SecureSession::decapsulate(const uint8_t *packet, size_t packetLen,
                                    std::vector<uint8_t> &header,
                                    std::vector<uint8_t> &data) {
        if (packetLen < 32) {
            // 至少需要HMAC长度
            return false;
        }

        size_t hmacLen = 32;
        size_t payloadLen = packetLen - hmacLen;

        // 提取HMAC
        const uint8_t *receivedHmac = packet + payloadLen;

        // 验证HMAC
        if (!Crypto::verifyHMAC(packet, payloadLen, receivedHmac, hmacLen, key_)) {
            return false;
        }

        // 分离header和data（需要知道header长度）
        // 这里假设调用者知道header长度
        // 实际实现中，header可能包含长度信息

        // 解密数据
        if (!key_.empty()) {
            // 需要知道哪部分是数据
            // 简化：假设整个payload都是数据
            data = crypto_.decrypt(packet, payloadLen, nonce_++);
        } else {
            data.assign(packet, packet + payloadLen);
        }

        return true;
    }
} // namespace hruft
