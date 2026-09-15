#pragma once

// Minimal hiredis stub — provides just enough type definitions to compile
// without the real hiredis library installed.
// All operations will return failure, and the system runs in degraded mode.

#ifdef __cplusplus
extern "C" {
#endif

#include <stdarg.h>
#include <stdint.h>

#define REDIS_REPLY_STRING  1
#define REDIS_REPLY_ARRAY   2
#define REDIS_REPLY_INTEGER 3
#define REDIS_REPLY_NIL     4
#define REDIS_REPLY_STATUS  5
#define REDIS_REPLY_ERROR   6

typedef struct redisReply {
    int type;
    long long integer;
    size_t len;
    char* str;
    size_t elements;
    struct redisReply** element;
} redisReply;

typedef struct redisContext {
    int err;
    char errstr[128];
    int fd;
} redisContext;

// Connection
redisContext* redisConnectWithTimeout(const char* ip, int port, const struct timeval tv);
int redisEnableKeepAlive(redisContext* c);
void redisFree(redisContext* c);

// Commands
void* redisCommand(redisContext* c, const char* format, ...);
void* redisvCommand(redisContext* c, const char* format, va_list ap);
void* redisCommandArgv(redisContext* c, int argc, const char** argv, const size_t* argvlen);

// Reply cleanup
void freeReplyObject(void* reply);

// Connect with timeout
#ifndef _WIN32
#include <sys/time.h>  // struct timeval
#else
// MinGW 的 <time.h>/<winsock2.h> 已通过 _timeval.h 提供 struct timeval（守卫 _TIMEVAL_DEFINED）；
// MSVC 的 CRT 没有，需自行定义。复用同一守卫避免重定义。
#ifndef _TIMEVAL_DEFINED
struct timeval {
    long tv_sec;
    long tv_usec;
};
#define _TIMEVAL_DEFINED
#endif
#endif

#ifdef __cplusplus
}
#endif
