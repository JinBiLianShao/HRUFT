#include "utils.h"

#include <iostream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <thread>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#else
#include <sys/statvfs.h>
#include <ifaddrs.h>
#include <unistd.h>
#include <pwd.h>
#endif

namespace hruft {
    namespace Platform {
        uint64_t getFreeDiskSpace(const std::string &path) {
#ifdef _WIN32
            ULARGE_INTEGER freeBytesAvailable;
            if (GetDiskFreeSpaceExA(path.c_str(), &freeBytesAvailable, NULL, NULL)) {
                return freeBytesAvailable.QuadPart;
            }
            return 0;
#else
            struct statvfs stat;
            if (statvfs(path.c_str(), &stat) == 0) {
                return static_cast<uint64_t>(stat.f_bavail) * stat.f_frsize;
            }
            return 0;
#endif
        }

        std::string getCurrentTimeString() {
            auto now = std::chrono::system_clock::now();
            auto in_time_t = std::chrono::system_clock::to_time_t(now);

            std::stringstream ss;
            ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d %H:%M:%S");
            return ss.str();
        }

        bool createDirectory(const std::string &path) {
#ifdef _WIN32
            return CreateDirectoryA(path.c_str(), NULL) != 0;
#else
            return mkdir(path.c_str(), 0755) == 0;
#endif
        }

        std::string getHomeDirectory() {
#ifdef _WIN32
            char path[MAX_PATH];
            if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PROFILE, NULL, 0, path))) {
                return std::string(path);
            }
            return "";
#else
            const char *home = getenv("HOME");
            if (home) return home;

            struct passwd *pw = getpwuid(getuid());
            if (pw) return pw->pw_dir;

            return "";
#endif
        }

        std::string getTempDirectory() {
#ifdef _WIN32
            char path[MAX_PATH];
            if (GetTempPathA(MAX_PATH, path)) {
                return std::string(path);
            }
            return "";
#else
            const char *tmp = getenv("TMPDIR");
            if (tmp) return tmp;

            return "/tmp";
#endif
        }
    } // namespace Platform

    namespace Network {
        std::string getLocalIP() {
#ifdef _WIN32
            PIP_ADAPTER_INFO adapterInfo = nullptr;
            ULONG bufferSize = 0;

            // 获取适配器信息大小
            if (GetAdaptersInfo(adapterInfo, &bufferSize) == ERROR_BUFFER_OVERFLOW) {
                adapterInfo = reinterpret_cast<PIP_ADAPTER_INFO>(new char[bufferSize]);

                if (GetAdaptersInfo(adapterInfo, &bufferSize) == ERROR_SUCCESS) {
                    for (PIP_ADAPTER_INFO adapter = adapterInfo; adapter != nullptr; adapter = adapter->Next) {
                        if (adapter->Type == MIB_IF_TYPE_ETHERNET || adapter->Type == IF_TYPE_IEEE80211) {
                            IP_ADDR_STRING *ipAddr = &adapter->IpAddressList;
                            while (ipAddr) {
                                std::string ip(ipAddr->IpAddress.String);
                                if (ip != "0.0.0.0" && ip != "127.0.0.1") {
                                    delete[] adapterInfo;
                                    return ip;
                                }
                                ipAddr = ipAddr->Next;
                            }
                        }
                    }
                }

                delete[] adapterInfo;
            }
#else
            struct ifaddrs *ifAddrStruct = nullptr;
            struct ifaddrs *ifa = nullptr;

            if (getifaddrs(&ifAddrStruct) == 0) {
                for (ifa = ifAddrStruct; ifa != nullptr; ifa = ifa->ifa_next) {
                    if (ifa->ifa_addr == nullptr) continue;

                    if (ifa->ifa_addr->sa_family == AF_INET) {
                        // IPv4
                        void *tmpAddrPtr = &((struct sockaddr_in *) ifa->ifa_addr)->sin_addr;
                        char addressBuffer[INET_ADDRSTRLEN];
                        inet_ntop(AF_INET, tmpAddrPtr, addressBuffer, INET_ADDRSTRLEN);

                        std::string interfaceName(ifa->ifa_name);
                        if (interfaceName != "lo") {
                            // 排除回环接口
                            freeifaddrs(ifAddrStruct);
                            return std::string(addressBuffer);
                        }
                    }
                }
                freeifaddrs(ifAddrStruct);
            }
#endif

            return "127.0.0.1";
        }

        uint16_t getAvailablePort(uint16_t startPort) {
            for (uint16_t port = startPort; port < startPort + 100; ++port) {
                if (isPortAvailable(port)) {
                    return port;
                }
            }
            return 0;
        }

        bool isPortAvailable(uint16_t port) {
#ifdef _WIN32
            SOCKET testSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (testSocket == INVALID_SOCKET) return false;

            sockaddr_in addr;
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = INADDR_ANY;
            addr.sin_port = htons(port);

            bool available = bind(testSocket, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0;
            closesocket(testSocket);
            return available;
#else
            int testSocket = socket(AF_INET, SOCK_STREAM, 0);
            if (testSocket < 0) return false;

            sockaddr_in addr;
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = INADDR_ANY;
            addr.sin_port = htons(port);

            bool available = bind(testSocket, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0;
            close(testSocket);
            return available;
#endif
        }

        std::vector<std::string> getNetworkInterfaces() {
            std::vector<std::string> interfaces;

#ifdef _WIN32
            PIP_ADAPTER_INFO adapterInfo = nullptr;
            ULONG bufferSize = 0;

            if (GetAdaptersInfo(adapterInfo, &bufferSize) == ERROR_BUFFER_OVERFLOW) {
                adapterInfo = reinterpret_cast<PIP_ADAPTER_INFO>(new char[bufferSize]);

                if (GetAdaptersInfo(adapterInfo, &bufferSize) == ERROR_SUCCESS) {
                    for (PIP_ADAPTER_INFO adapter = adapterInfo; adapter != nullptr; adapter = adapter->Next) {
                        interfaces.emplace_back(adapter->Description);
                    }
                }

                delete[] adapterInfo;
            }
#else
            struct ifaddrs *ifAddrStruct = nullptr;

            if (getifaddrs(&ifAddrStruct) == 0) {
                for (struct ifaddrs *ifa = ifAddrStruct; ifa != nullptr; ifa = ifa->ifa_next) {
                    if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET) {
                        interfaces.emplace_back(ifa->ifa_name);
                    }
                }
                freeifaddrs(ifAddrStruct);
            }
#endif

            return interfaces;
        }
    } // namespace Network
} // namespace hruft
