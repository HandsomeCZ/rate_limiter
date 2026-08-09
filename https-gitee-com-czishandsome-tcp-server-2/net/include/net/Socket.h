#pragma once
#ifdef _WIN32
    #include <winsock2.h>
    using sockfd_t = SOCKET;
    constexpr sockfd_t INVALID_SOCK = INVALID_SOCKET;
#else
    using sockfd_t = int;
    constexpr sockfd_t INVALID_SOCK = -1;
#endif

class InetAddress;

class Socket {
public:
    explicit Socket(sockfd_t fd) : fd_(fd) {}
    ~Socket();

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    sockfd_t fd() const { return fd_; }
    void bind(const InetAddress& addr);
    void listen();
    sockfd_t accept(InetAddress* peerAddr);

    void setReuseAddr(bool on);
    void setTcpNoDelay(bool on);
    void setNonBlocking();

    static sockfd_t createNonBlocking();
    static void close(sockfd_t fd);

private:
    sockfd_t fd_;
};
