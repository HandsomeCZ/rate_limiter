#pragma once
#include <string>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    using socklen_t = int;
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
#endif

class InetAddress {
public:
    InetAddress();
    explicit InetAddress(uint16_t port, bool loopbackOnly = false);
    InetAddress(const std::string& ip, uint16_t port);

    const sockaddr_in& addr() const { return addr_; }
    sockaddr_in* sockaddrPtr() { return &addr_; }
    const sockaddr_in* sockaddrPtr() const { return &addr_; }
    socklen_t addrLen() const { return sizeof(addr_); }

    std::string ip() const;
    uint16_t    port() const;

private:
    sockaddr_in addr_;
};
