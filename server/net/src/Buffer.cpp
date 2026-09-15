#include "net/Buffer.h"
#include <algorithm>

#ifdef _WIN32
    #include <winsock2.h>
#else
    #include <sys/socket.h>
    #include <sys/uio.h>
#endif

void Buffer::makeSpace(size_t len) {
    if (prependableBytes() + writableBytes() < len) {
        buffer_.resize(writeIndex_ + len);
    } else {
        size_t readable = readableBytes();
        std::copy(begin() + readIndex_, begin() + writeIndex_, begin());
        readIndex_ = 0;
        writeIndex_ = readable;
    }
}

ssize_t Buffer::readFd(sockfd_t fd, int* savedErrno) {
    // 用栈上 64KB 作为第二缓冲，配合 readv/WSARecv 一次系统调用读满：
    //   1. 避免 writable==0 时 recv(fd,buf,0,0) 返回 0 被误判为对端关闭（原 bug）
    //   2. 大报文也能一次读入，减少 read 系统调用次数
    char extrabuf[65536];

#ifdef _WIN32
    const size_t writable = writableBytes();
    WSABUF wsabuf[2];
    DWORD bytesRecv = 0;
    DWORD flags = 0;
    wsabuf[0].buf = beginWrite();
    wsabuf[0].len = static_cast<ULONG>(writable);
    wsabuf[1].buf = extrabuf;
    wsabuf[1].len = sizeof(extrabuf);
    const int iovcnt = (writable < sizeof(extrabuf)) ? 2 : 1;

    int ret = ::WSARecv(fd, wsabuf, iovcnt, &bytesRecv, &flags, nullptr, nullptr);
    if (ret == SOCKET_ERROR) {
        *savedErrno = WSAGetLastError();
        return -1;
    }
    const ssize_t n = static_cast<ssize_t>(bytesRecv);
    if (n == 0) return 0;
    if (static_cast<size_t>(n) <= writable) {
        writeIndex_ += n;
    } else {
        writeIndex_ = buffer_.size();
        append(extrabuf, n - writable);
    }
    return n;
#else
    struct iovec vec[2];
    const size_t writable = writableBytes();
    vec[0].iov_base = begin() + writeIndex_;
    vec[0].iov_len  = writable;
    vec[1].iov_base = extrabuf;
    vec[1].iov_len  = sizeof(extrabuf);
    const int iovcnt = (writable < sizeof(extrabuf)) ? 2 : 1;

    const ssize_t n = ::readv(fd, vec, iovcnt);
    if (n < 0) {
        *savedErrno = errno;
    } else if (static_cast<size_t>(n) <= writable) {
        writeIndex_ += n;
    } else {
        writeIndex_ = buffer_.size();
        append(extrabuf, n - writable);
    }
    return n;
#endif
}
