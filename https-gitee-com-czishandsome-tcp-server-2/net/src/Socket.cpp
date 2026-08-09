#include "net/Socket.h"
#include "net/InetAddress.h"
#include <cassert>

#ifdef _WIN32
    #include <io.h>
    #define close closesocket
#else
    #include <unistd.h>
    #include <fcntl.h>
#endif
#include <cstring>

Socket::~Socket() { if (fd_ != INVALID_SOCK) close(fd_); }

void Socket::bind(const InetAddress& addr) {
    int ret = ::bind(fd_, (const sockaddr*)addr.sockaddrPtr(), addr.addrLen());
    assert(ret == 0);
}

void Socket::listen() { ::listen(fd_, SOMAXCONN); }

sockfd_t Socket::accept(InetAddress* peerAddr) {
    socklen_t len = peerAddr->addrLen();
    sockfd_t connfd = ::accept(fd_, (sockaddr*)peerAddr->sockaddrPtr(), &len);
    return connfd;
}

void Socket::setReuseAddr(bool on) {
    int opt = on ? 1 : 0;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
}

void Socket::setTcpNoDelay(bool on) {
    int opt = on ? 1 : 0;
    ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, (const char*)&opt, sizeof(opt));
}

void Socket::setNonBlocking() {
#ifdef _WIN32
    u_long mode = 1;
    ::ioctlsocket(fd_, FIONBIO, &mode);
#else
    int flags = ::fcntl(fd_, F_GETFL, 0);
    ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
#endif
}

sockfd_t Socket::createNonBlocking() {
    sockfd_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(fd != INVALID_SOCK);
    Socket s(fd);
    s.setReuseAddr(true);
    s.setNonBlocking();
    return fd;
}

void Socket::close(sockfd_t fd) {
#ifdef _WIN32
    ::closesocket(fd);
#else
    ::close(fd);
#endif
}
