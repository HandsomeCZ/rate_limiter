# Zero-Basics Guide to This Project

## 1. What Does This Project Do?

In one sentence: **A website backend with a smart security guard**.

### Restaurant Analogy

| Real World | Computer World | Code |
|------------|---------------|------|
| Restaurant door | Server port (8080) | TcpServer |
| Customer enters | Browser connects | Acceptor |
| Waiter takes order | HTTP request handling | TcpConnection |
| Guard checks crowd | Rate Limiter | RateLimiterFacade |
| Guard assesses risk | Risk Engine | DecisionEngine |
| Kitchen serves food | HTTP response | send() |

### Three Core Abilities

1. **HTTP Server**: Handles web requests
2. **Rate Limiting**: Limits how often each user/IP can access
3. **Risk Control**: Scores request risk level (safe/suspicious/dangerous)

---

## 2. Basic Networking Concepts

### IP Address = House Number
Each computer has a unique number. 127.0.0.1 always points to your own computer.

### Port = Room Number
```
127.0.0.1:8080
 building  room
```

### HTTP = Browser-Server Language
Browser sends: GET /health HTTP/1.1
Server replies: {"status":"ok"}

### TCP = Reliable Delivery
Like a courier service with signature confirmation.

### Event Loop = Waiter Constantly Checking
```
Waiter at door -> Anyone here? -> Take them in
    ^                              |
    +--- Keep watching <--- Done
```

### Redis = Super-Fast Notepad
In-memory database. Fast, survives restarts.

---

## 3. Code Structure

```
program/
|-- rate_limiter.sln          <- VS solution file
|
|-- https-gitee-com-...tcp-server-2/   <- HTTP server
|   |-- net/                  <- Network library
|   |   |-- include/net/      <- Headers (menus)
|   |   |-- src/              <- Source (kitchen)
|   |-- http/Http.hpp         <- HTTP parsing + routing
|   |-- src/main.cpp          <- Entry point
|
|-- rate_limiter/             <- Rate limiter + risk engine
    |-- include/              <- Headers
    |-- src/                  <- Implementation
```

.h = header = menu (declares what functions exist)
.cpp = source = kitchen (actual implementation)

---

## 4. Journey of One Request (8 Steps)

### Step 1: Accept Connection
File: net/src/Acceptor.cpp -> handleRead()
"Someone knocked! Open the door."

### Step 2: Event Loop Dispatch
File: net/src/EventLoop.cpp -> loop()
while(running) { check events; dispatch to handlers; }

### Step 3: Read Data
File: net/src/Buffer.cpp -> readFd()
"Write down what the guest said."

### Step 4: Parse HTTP
File: http/Http.hpp -> OnMessage()
"Translate: method=GET, path=/api/v1/user/info"

### Step 5: Rate Limit + Risk Check
File: rate_limiter/src/service/rate_limit_service.cpp -> process()
"Security guard checks:"
  Whitelist? -> Skip
  Blacklist? -> Reject
  Rate limit? -> Check frequency
  Risk score? -> Evaluate
  Decision: ALLOW / LIMIT / CHALLENGE / REJECT

### Step 6: Route Match
File: http/Http.hpp -> Route()
"What does the guest want?" Match URL to handler.

### Step 7: Generate Response
Handler returns JSON: {"user":{"id":"123","name":"demo"}}

### Step 8: Send Response
File: net/src/TcpConnection.cpp -> send()
"Dish is ready, serve to guest."

---

## 5. Module Details (11 Modules)

### 5.1 EventLoop - Heart of the Program
```while(running) { check events; dispatch; }```

### 5.2 SelectPoller / EpollPoller - Network Monitor
Windows: SelectPoller (ask each "are you ready?")
Linux: EpollPoller (wait for notification)

### 5.3 TcpServer / TcpConnection - Reception
TcpServer = Front desk (accept new guests)
TcpConnection = Waiter (one-on-one service)

### 5.4 Buffer - Notepad
Network data arrives in chunks. Buffer reassembles them.

### 5.5 HttpServer - Translator
Converts raw HTTP text into structured request objects.

### 5.6 RateLimiter - Frequency Guard
Two algorithms:
- Fixed Window: Reset at clock boundaries
- Sliding Window: Look back 1 second (more precise)

### 5.7 DecisionEngine - Risk Assessment
feature extraction -> rule matching -> scoring -> decision
Example: new IP (+10) + night access (+20) + sensitive API (+30) = 60 -> LIMIT

### 5.8 Decision Levels
ALLOW = Green light
LIMIT = Yellow light (tighten quota)
CHALLENGE = Orange light (need verification)
REJECT = Red light (blocked)

### 5.9 RedisClient - Remote Notepad
Shared counter storage, survives restart.

### 5.10 LocalCache - Local Cheat Sheet
Cache common rules in memory, no need to ask Redis every time.

### 5.11 ConfigManager - Configuration Center
Manages all rules: whitelist, blacklist, rate limits, risk rules.

---

## 6. Data Flow Diagram

```
Browser
  |  GET /api/v1/user/info
  v
[OS Network Stack] -- "Port 8080, someone knocked"
  v
[EventLoop + SelectPoller] -- "Event!"
  v
[Acceptor -> TcpConnection] -- "Assign waiter"
  v
[Buffer::readFd()] -- "Note: GET /api/v1/user/info"
  v
[HttpServer::OnMessage()] -- "Parse: method=GET, path=/api/v1/user/info"
  v
[RateLimitService::process()] -- "Guard check: ALLOW"
  v
[Route()] -- "/api/v1/user/info -> handler"
  v
[Handler] -- returns JSON response
  v
[TcpConnection::send()] -- "HTTP/1.1 200 OK..."
  v
Browser displays result
```

---

## 7. How to Run and Test

### Start
Press F5 in Visual Studio.

### Test URLs
http://127.0.0.1:8080/health         -> {"status":"ok"}
http://127.0.0.1:8080/api/v1/stats   -> rate limit statistics
http://127.0.0.1:8080/api/v1/order   -> order info

### curl
curl http://127.0.0.1:8080/health

---

## 8. Debugging: Where to Set Breakpoints

### Beginner: 5 Breakpoints

| # | File | Function | See |
|---|------|----------|-----|
| 1 | net/src/Acceptor.cpp | handleRead() | New connection |
| 2 | http/Http.hpp | OnMessage() | HTTP parsing |
| 3 | http/Http.hpp | CheckRateLimit() | Rate limit check |
| 4 | http/Http.hpp | Route() | URL routing |
| 5 | http/Http.hpp | route lambda | Business logic |

### Advanced: Rate Limit Engine

| # | File | Function |
|---|------|----------|
| 6 | rate_limiter/src/service/rate_limit_service.cpp | process() |
| 7 | rate_limiter/src/rate_limiter/rate_limiter.cpp | checkRule() |

### Advanced: Event Loop

| # | File | Function |
|---|------|----------|
| 8 | net/src/EventLoop.cpp | loop() while body |
| 9 | net/src/EventLoop.cpp | after poller->poll() |

---

## 9. Common Issues

Q: Crash on startup?
A: Port 8080 might be in use. Change port in main.cpp.

Q: [RedisPool] conn failed?
A: NORMAL. Redis is not installed, running in degraded mode.

Q: How to verify rate limiting works?
A: Rapidly access /api/v1/order, then check /api/v1/stats for rejected count.

Q: What is "degraded mode"?
A: Like using bottled water when tap is broken. Redis down -> use local memory.

---

## 10. Glossary

| Term | Meaning |
|------|---------|
| Socket | Network communication "phone line" |
| Port | Service "room number" |
| TCP | Reliable data delivery |
| HTTP | Web "language" |
| Request | Your question to server |
| Response | Its answer |
| JSON | Data format {"k":"v"} |
| EventLoop | Program heartbeat |
| RateLimit | Frequency control |
| Redis | In-memory database |
| Degradation | Fallback when something fails |
| Fail-open | Allow by default when uncertain |

---

> Think of this as a restaurant: front desk (TcpServer), waiters (TcpConnection), security guards (RateLimiter + DecisionEngine), notepads (Redis + Buffer). Data flows like guests: enter, get checked, get served, leave. Code just describes this flow in computer language.
