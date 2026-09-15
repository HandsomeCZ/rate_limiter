// hiredis stub implementation — all operations fail gracefully.
// This allows the project to compile without the real hiredis library.
// The rate limiter system runs in degraded mode without Redis.

#include "hiredis/hiredis.h"
#include <cstdlib>
#include <cstring>
#include <cstdarg>

redisContext* redisConnectWithTimeout(const char* ip, int port, const struct timeval tv) {
    (void)ip; (void)port; (void)tv;
    redisContext* c = (redisContext*)malloc(sizeof(redisContext));
    if (c) {
        std::memset(c, 0, sizeof(redisContext));
        c->err = 1;
        c->fd = -1;
    }
    return c;
}

int redisEnableKeepAlive(redisContext* c) {
    (void)c;
    return -1;
}

void redisFree(redisContext* c) {
    free(c);
}

void* redisCommand(redisContext* c, const char* format, ...) {
    (void)c; (void)format;
    return nullptr;
}

void* redisvCommand(redisContext* c, const char* format, va_list ap) {
    (void)c; (void)format; (void)ap;
    return nullptr;
}

void* redisCommandArgv(redisContext* c, int argc, const char** argv, const size_t* argvlen) {
    (void)c; (void)argc; (void)argv; (void)argvlen;
    return nullptr;
}

void freeReplyObject(void* reply) {
    (void)reply;
}
