#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstring>  // 添加这个头文件用于memset
#include <stdexcept> // 添加这个头文件用于异常

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET SocketHandle;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>  // 确保有memset
typedef int SocketHandle;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#endif

namespace hruft {
    class SocketAddress {
    public:
        SocketAddress() = default;

        SocketAddress(const std::string &ip, uint16_t port) {
            set(ip, port);
        }

        void set(const std::string &ip, uint16_t port) {
            memset(&addr_, 0, sizeof(addr_));
            addr_.sin_family = AF_INET;
            addr_.sin_port = htons(port);

            if (inet_pton(AF_INET, ip.c_str(), &addr_.sin_addr) != 1) {
                throw std::runtime_error("Invalid IP address: " + ip);
            }
        }

        sockaddr_in get() const { return addr_; }
        sockaddr_in &get() { return addr_; } // 添加非const版本
        socklen_t size() const { return sizeof(addr_); }

        std::string ip() const {
            char buffer[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &addr_.sin_addr, buffer, sizeof(buffer));
            return std::string(buffer);
        }

        uint16_t port() const {
            return ntohs(addr_.sin_port);
        }

        // 添加获取sockaddr指针的方法
        sockaddr *asSockaddr() {
            return reinterpret_cast<sockaddr *>(&addr_);
        }

        const sockaddr *asSockaddr() const {
            return reinterpret_cast<const sockaddr *>(&addr_);
        }

    private:
        sockaddr_in addr_;
    };

    class UdpSocket {
    public:
        UdpSocket();

        ~UdpSocket();

        // 禁用拷贝
        UdpSocket(const UdpSocket &) = delete;

        UdpSocket &operator=(const UdpSocket &) = delete;

        // 移动语义
        UdpSocket(UdpSocket &&other) noexcept;

        UdpSocket &operator=(UdpSocket &&other) noexcept;

        bool bind(uint16_t port);

        bool connect(const SocketAddress &address);

        ssize_t sendTo(const void *data, size_t length, const SocketAddress &address);

        ssize_t recvFrom(void *buffer, size_t length, SocketAddress &sender);

        bool setNonBlocking(bool nonBlocking);

        bool setSendBufferSize(int size);

        bool setRecvBufferSize(int size);

        bool setReuseAddress(bool reuse);

        uint16_t getLocalPort() const;

        std::string getLocalIP() const;

        SocketHandle getHandle() const { return socket_; }

    private:
        SocketHandle socket_ = INVALID_SOCKET;
        SocketAddress localAddr_;

#ifdef _WIN32
        static bool wsaInitialized_;
        static void initializeWSA();
#endif
    };

    // UDP 服务器，可以绑定到多个端口
    class UdpServer {
    public:
        using PacketHandler = std::function<void(const uint8_t *data, size_t len,
                                                 const SocketAddress &sender)>;

        UdpServer();

        ~UdpServer();

        bool start(uint16_t basePort, size_t threadCount);

        void stop();

        bool isRunning() const { return running_; }

        void setPacketHandler(PacketHandler handler);

        bool sendTo(const uint8_t *data, size_t len, const SocketAddress &addr);

    private:
        void workerThread(uint16_t port);

        std::vector<std::thread> threads_;
        std::vector<std::unique_ptr<UdpSocket> > sockets_;
        PacketHandler packetHandler_;
        std::atomic<bool> running_{false};
        mutable std::mutex handlerMutex_;
    };
} // namespace hruft
