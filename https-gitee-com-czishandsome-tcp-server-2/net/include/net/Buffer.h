#pragma once
#include <vector>
#include <string>
#include <cstring>
#include <cassert>

class Buffer {
public:
    static const size_t kInitialSize = 4096;

    explicit Buffer(size_t initialSize = kInitialSize)
        : buffer_(initialSize), readIndex_(0), writeIndex_(0) {}

    size_t readableBytes() const { return writeIndex_ - readIndex_; }
    size_t ReadAbleSize() const { return readableBytes(); }  // alias for Http.hpp compat
    void MoveReadOffset(size_t len) { retrieve(len); }       // alias for Http.hpp compat
    size_t writableBytes() const { return buffer_.size() - writeIndex_; }
    size_t prependableBytes() const { return readIndex_; }

    const char* peek() const { return begin() + readIndex_; }
    char* beginWrite() { return begin() + writeIndex_; }
    const char* beginWrite() const { return begin() + writeIndex_; }

    void retrieve(size_t len) {
        assert(len <= readableBytes());
        readIndex_ += len;
        if (readIndex_ == writeIndex_) { readIndex_ = writeIndex_ = 0; }
    }

    void retrieveAll() { readIndex_ = writeIndex_ = 0; }

    std::string retrieveAsString(size_t len) {
        std::string result(peek(), len);
        retrieve(len);
        return result;
    }

    std::string retrieveAllAsString() { return retrieveAsString(readableBytes()); }

    void append(const char* data, size_t len) {
        ensureWritable(len);
        std::copy(data, data + len, beginWrite());
        writeIndex_ += len;
    }

    void append(const std::string& str) { append(str.data(), str.size()); }

    void ensureWritable(size_t len) {
        if (writableBytes() < len) makeSpace(len);
    }

    ssize_t readFd(sockfd_t fd, int* savedErrno);

    const char* findCRLF() const {
        const char* crlf = static_cast<const char*>(std::memchr(peek(), '\n', readableBytes()));
        return crlf;
    }

private:
    char* begin() { return buffer_.data(); }
    const char* begin() const { return buffer_.data(); }
    void makeSpace(size_t len);

    std::vector<char> buffer_;
    size_t readIndex_;
    size_t writeIndex_;
};
