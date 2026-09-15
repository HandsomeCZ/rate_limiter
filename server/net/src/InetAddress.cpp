#include "net/InetAddress.h"
#include <cstring>

InetAddress::InetAddress() {
    std::memset(&addr_, 0, sizeof(addr_));
}

InetAddress::InetAddress(uint16_t port, bool loopbackOnly) {
    std::memset(&addr_, 0, sizeof(addr_));
    addr_.sin_family = AF_INET;
    addr_.sin_port = htons(port);
    addr_.sin_addr.s_addr = htonl(loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
}

InetAddress::InetAddress(const std::string& ip, uint16_t port) {
    std::memset(&addr_, 0, sizeof(addr_));
    addr_.sin_family = AF_INET;
    addr_.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr_.sin_addr);
}

std::string InetAddress::ip() const {
    char buf[INET_ADDRSTRLEN] = {0};
    // MinGW 的 inet_ntop 是 InetNtopA 的宏，第三参是 PVOID（非 const），
    // 而 POSIX/MSVC 是 const void*。const_cast 让三种平台都能通过编译。
    const char* res = inet_ntop(AF_INET,
                                const_cast<in_addr*>(&addr_.sin_addr),
                                buf, sizeof(buf));
    return res ? buf : "";
}

uint16_t InetAddress::port() const { return ntohs(addr_.sin_port); }
