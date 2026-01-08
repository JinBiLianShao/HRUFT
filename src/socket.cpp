#include "socket.h"
#include <iostream>
#include <algorithm>

#ifdef _WIN32
bool hruft::UdpSocket::wsaInitialized_ = false;
#endif

namespace hruft {
#ifdef _WIN32
    void UdpSocket::initializeWSA() {
        if (!wsaInitialized_) {
            WSADATA wsaData;
            if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
                throw std::runtime_error("WSAStartup failed");
            }
            wsaInitialized_ = true;
        }
    }
#endif

    UdpSocket::UdpSocket() {
#ifdef _WIN32
        initializeWSA();
        socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ == INVALID_SOCKET) {
            throw std::runtime_error("Socket creation failed");
        }
#else
        socket_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (socket_ < 0) {
            throw std::runtime_error("Socket creation failed");
        }
#endif
    }

    UdpSocket::~UdpSocket() {
        if (socket_ != INVALID_SOCKET) {
#ifdef _WIN32
            closesocket(socket_);
#else
            close(socket_);
#endif
        }
    }

    UdpSocket::UdpSocket(UdpSocket &&other) noexcept : socket_(other.socket_) {
        other.socket_ = INVALID_SOCKET;
    }

    UdpSocket &UdpSocket::operator=(UdpSocket &&other) noexcept {
        if (this != &other) {
            if (socket_ != INVALID_SOCKET) {
#ifdef _WIN32
                closesocket(socket_);
#else
                close(socket_);
#endif
            }
            socket_ = other.socket_;
            other.socket_ = INVALID_SOCKET;
        }
        return *this;
    }

    bool UdpSocket::bind(uint16_t port) {
        sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);

        if (::bind(socket_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
#ifdef _WIN32
            std::cerr << "Bind failed: " << WSAGetLastError() << std::endl;
#else
            perror("Bind failed");
#endif
            return false;
        }

        // 获取绑定的地址
        socklen_t len = sizeof(addr);
        if (getsockname(socket_, reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
            return false;
        }

        localAddr_.set(inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));

        return true;
    }

    bool UdpSocket::connect(const SocketAddress &address) {
        const sockaddr_in &addr = address.get();
        if (::connect(socket_, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
            return false;
        }
        return true;
    }

    ssize_t UdpSocket::sendTo(const void *data, size_t length, const SocketAddress &address) {
        const sockaddr_in &addr = address.get();
        ssize_t sent = ::sendto(socket_,
                                reinterpret_cast<const char *>(data),
                                static_cast<int>(length),
                                0,
                                reinterpret_cast<const sockaddr *>(&addr),
                                static_cast<int>(sizeof(addr)));

        return sent;
    }

    ssize_t UdpSocket::recvFrom(void *buffer, size_t length, SocketAddress &sender) {
        sockaddr_in addr;
        socklen_t addrLen = sizeof(addr);
        memset(&addr, 0, sizeof(addr));

        ssize_t received = ::recvfrom(socket_,
                                      reinterpret_cast<char *>(buffer),
                                      static_cast<int>(length),
                                      0,
                                      reinterpret_cast<sockaddr *>(&addr),
                                      &addrLen);

        if (received > 0) {
            char ipStr[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &addr.sin_addr, ipStr, INET_ADDRSTRLEN);
            sender.set(ipStr, ntohs(addr.sin_port));
        }

        return received;
    }

    bool UdpSocket::setNonBlocking(bool nonBlocking) {
#ifdef _WIN32
        u_long mode = nonBlocking ? 1 : 0;
        if (ioctlsocket(socket_, FIONBIO, &mode) != 0) {
            return false;
        }
#else
        int flags = fcntl(socket_, F_GETFL, 0);
        if (flags < 0) return false;

        if (nonBlocking) {
            flags |= O_NONBLOCK;
        } else {
            flags &= ~O_NONBLOCK;
        }

        if (fcntl(socket_, F_SETFL, flags) != 0) {
            return false;
        }
#endif
        return true;
    }

    bool UdpSocket::setSendBufferSize(int size) {
        if (setsockopt(socket_, SOL_SOCKET, SO_SNDBUF,
                       reinterpret_cast<const char *>(&size), sizeof(size)) != 0) {
            return false;
        }
        return true;
    }

    bool UdpSocket::setRecvBufferSize(int size) {
        if (setsockopt(socket_, SOL_SOCKET, SO_RCVBUF,
                       reinterpret_cast<const char *>(&size), sizeof(size)) != 0) {
            return false;
        }
        return true;
    }

    bool UdpSocket::setReuseAddress(bool reuse) {
        int opt = reuse ? 1 : 0;
        if (setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR,
                       reinterpret_cast<const char *>(&opt), sizeof(opt)) != 0) {
            return false;
        }
        return true;
    }

    uint16_t UdpSocket::getLocalPort() const {
        return localAddr_.port();
    }

    std::string UdpSocket::getLocalIP() const {
        return localAddr_.ip();
    }

    // UdpServer 实现
    UdpServer::UdpServer() = default;

    UdpServer::~UdpServer() {
        stop();
    }

    bool UdpServer::start(uint16_t basePort, size_t threadCount) {
        if (running_) {
            return false;
        }

        sockets_.resize(threadCount);
        threads_.reserve(threadCount);

        for (size_t i = 0; i < threadCount; ++i) {
            auto socket = std::make_unique<UdpSocket>();
            uint16_t port = basePort + static_cast<uint16_t>(i);

            if (!socket->bind(port)) {
                std::cerr << "Failed to bind port " << port << std::endl;
                return false;
            }

            if (!socket->setRecvBufferSize(1024 * 1024 * 16)) {
                // 16MB接收缓冲区
                std::cerr << "Warning: Failed to set receive buffer size" << std::endl;
            }

            sockets_[i] = std::move(socket);
        }

        running_ = true;

        // 启动工作线程
        for (size_t i = 0; i < threadCount; ++i) {
            threads_.emplace_back(&UdpServer::workerThread, this,
                                  basePort + static_cast<uint16_t>(i));
        }

        return true;
    }

    void UdpServer::stop() {
        if (!running_) return;

        running_ = false;

        // 发送空包唤醒阻塞的recv
        for (auto &socket: sockets_) {
            if (socket) {
                SocketAddress addr("127.0.0.1", socket->getLocalPort());
                socket->sendTo("", 0, addr);
            }
        }

        // 等待线程结束
        for (auto &thread: threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }

        threads_.clear();
        sockets_.clear();
    }

    void UdpServer::workerThread(uint16_t port) {
        // 找到对应的socket
        UdpSocket *socket = nullptr;
        for (auto &s: sockets_) {
            if (s->getLocalPort() == port) {
                socket = s.get();
                break;
            }
        }

        if (!socket) return;

        // 动态调整缓冲区大小，处理超大UDP包
        const size_t MAX_UDP_PACKET_SIZE = 65507; // IPv4 UDP最大理论值
        std::vector<uint8_t> buffer(MAX_UDP_PACKET_SIZE);
        SocketAddress sender;

        while (running_) {
            ssize_t received = socket->recvFrom(buffer.data(), buffer.size(), sender);

            if (received > 0) {
                // 确保packetHandler_在调用期间有效
                PacketHandler handler; {
                    std::lock_guard<std::mutex> lock(handlerMutex_);
                    handler = packetHandler_;
                }

                if (handler) {
                    // 在单独的线程中处理，避免阻塞接收循环
                    std::thread([handler, buffer = std::vector<uint8_t>(buffer.begin(),
                                                                        buffer.begin() + received), sender]() mutable {
                        try {
                            handler(buffer.data(), buffer.size(), sender);
                        } catch (const std::exception &e) {
                            std::cerr << "Packet handler exception: " << e.what() << std::endl;
                        }
                    }).detach();
                }
            } else if (received < 0) {
                // 更详细的错误处理
#ifdef _WIN32
                int error = WSAGetLastError();
                switch (error) {
                    case WSAEWOULDBLOCK:
                    case WSAEMSGSIZE: // 包太大，可能需要调整MTU
                        break; // 不记录正常情况
                    case WSAECONNRESET:
                        std::cerr << "ERROR: Connection reset" << std::endl;
                        break;
                    case WSAETIMEDOUT:
                        // 正常超时，不记录错误
                        break;
                    default:
                        if (error != 0) {
                            std::cerr << "Socket error: " << error << std::endl;
                            // 严重错误，可能需要重启socket
                            if (error == WSAENETDOWN || error == WSAENETRESET) {
                                running_ = false;
                            }
                        }
                        break;
                }
#else
                if (errno != EWOULDBLOCK && errno != EAGAIN && errno != ECONNREFUSED) {
                    perror("Socket error");
                    if (errno == ENETDOWN || errno == ENETRESET) {
                        running_ = false;
                    }
                }
#endif
            }

            // 动态调整休眠时间
            static int idleCount = 0;
            if (received <= 0) {
                idleCount++;
                std::this_thread::sleep_for(std::chrono::microseconds(100 * std::min(idleCount, 10)));
            } else {
                idleCount = 0;
            }
        }
    }

    void UdpServer::setPacketHandler(PacketHandler handler) {
        std::lock_guard<std::mutex> lock(handlerMutex_);
        packetHandler_ = handler;
    }

    bool UdpServer::sendTo(const uint8_t *data, size_t len, const SocketAddress &addr) {
        if (sockets_.empty()) return false;

        // 简单的负载均衡：轮询发送
        static std::atomic<size_t> current{0};
        size_t idx = current.fetch_add(1) % sockets_.size();

        ssize_t sent = sockets_[idx]->sendTo(data, len, addr);
        return sent == static_cast<ssize_t>(len);
    }
} // namespace hruft
