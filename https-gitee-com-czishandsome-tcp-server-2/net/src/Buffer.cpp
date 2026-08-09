#include "net/Buffer.h"
#include <algorithm>

#ifdef _WIN32
    #include <winsock2.h>
#else
    #include <sys/socket.h>
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
    char extrabuf[65536];
    size_t writable = writableBytes();
    ssize_t n = ::recv(fd, beginWrite(), static_cast<int>(writable), 0);
    if (n > 0) {
        writeIndex_ += n;
    } else if (n == 0) {
        return 0;
    } else {
        *savedErrno = 
#ifdef _WIN32
            WSAGetLastError();
#else
            errno;
#endif
    }
    return n;
}
