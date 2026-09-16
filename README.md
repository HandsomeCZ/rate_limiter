# 高并发 API 网关（Reactor + 风控限流双引擎）

一个基于 C++17 的高并发 API 网关，整合两大模块：

| 模块 | 目录 | 职责 |
|------|------|------|
| **网络层** | `server/` | 基于 Reactor 模式（Epoll/Select）+ one-loop-per-thread 的高并发 TCP/HTTP 服务器 |
| **业务层** | `rate_limiter/` | 风控评分（Feature → Score → Decision）+ 限流（固定窗口 / 滑动窗口）双引擎 |

单机目标吞吐 **10w QPS**，P99 判定延迟 **< 2ms**，Redis 故障自动 fail-open 降级。

---

## 特性

### 网络层（Reactor 服务器）
- **Reactor 多线程模型**：`EventLoop` + `Channel` + `Poller`（Linux 用 epoll，Windows 用 Select/WSAPoll），one-loop-per-thread
- **非阻塞 I/O**：`Buffer` 用 readv/WSARecv 一次系统调用读满，避免大报文多次 read
- **连接管理**：`TcpServer`/`TcpConnection`/`Acceptor`（循环 accept 应对建连风暴）
- **时间轮**：`TimerWheel` 管理连接空闲超时，绝对 deadline 支持任意超时
- **HTTP 层**：请求解析 + 正则路由 + 静态文件服务 + keep-alive

### 业务层（风控 + 限流）
- **双引擎串联**：风控评分（多特征 → 评分 → 分级）+ 限流（频率控制）
- **限流算法**：固定窗口（INCR+EXPIRE）、滑动窗口（Redis ZSet + Lua 原子判定）
- **本地限流**：64 分片锁 + 本地滑动窗口，零网络开销，支撑 10w QPS
- **热点 Key 分片**：16 分片把热点压力降到 1/16
- **三级降级**：Redis 不可用 fail-open、单特征失败填默认值、无引擎退化为纯限流
- **熔断自愈**：连续失败触发熔断，冷却超时后半开探测恢复
- **异步数据回流**：有界队列 + 后台消费，不阻塞主链路

---

## 架构

```
Browser
   │  HTTP 请求
   ▼
[Acceptor] ──accept──▶ [TcpConnection]（分派到 worker EventLoop）
   │                        │
   │                        ▼
   │                 [HttpServer::OnMessage]
   │                   ├─ 解析 HTTP 请求
   │                   ├─ 风控评分（DecisionEngine）
   │                   ├─ 限流检查（LocalRateLimiter + Redis）
   │                   └─ 路由 → 生成响应 → send
   │
   └─ 连接空闲超时（TimerWheel，默认 60s）

风控/限流处理流程：
  Phase 0  黑白名单 FastPath（0 网络开销）
  Phase 1  特征提取（Static + Velocity，本地优先 + Redis 兜底）
  Phase 2  规则匹配（责任链）
  Phase 3  风险评分（AdditiveScorer → ALLOW/LIMIT/CHALLENGE/REJECT）
  Phase 4  动作分派（LIMIT 收紧配额，REJECT 直接拒绝）
```

---

## 目录结构

```
.
├── server/                  # 网络层（Reactor HTTP 服务器）
│   ├── net/                 #   Reactor 网络库
│   │   ├── include/net/     #     头文件（EventLoop/Channel/Poller/TcpServer/...）
│   │   └── src/             #     实现
│   ├── http/Http.hpp        #   HTTP 解析 + 路由
│   ├── src/main.cpp         #   入口（组装网络层 + 业务层）
│   └── CMakeLists.txt       #   集成构建（net + http + rate_limiter）
├── rate_limiter/            # 业务层（风控 + 限流）
│   ├── include/             #   头文件
│   ├── src/                 #   实现
│   ├── test/                #   单元测试 + 压测
│   ├── scripts/             #   Lua 脚本（滑动窗口）
│   └── CMakeLists.txt       #   独立构建（demo/unit_test/benchmark）
├── rate_limiter.sln         # VS2022 解决方案（可选，CMake 亦可生成）
└── .gitignore
```

---

## 快速开始

### 依赖

- C++17（GCC 8+ / Clang 10+ / MSVC 2019+）
- CMake 3.14+
- [hiredis](https://github.com/redis/hiredis)（可选：不安装则自动用 stub，Redis 功能降级）
- Redis（可选：不启动则限流系统降级运行）

### 编译并运行主服务器

主服务器 = 网络层 + 业务层整合版，**不依赖 hiredis 也能编译**（自动用 stub 降级）：

```bash
# Linux / MinGW-w64
cd server
mkdir build && cd build
cmake .. && make -j$(nproc)
./tcp_server_http 8080 8          # 端口 8080，8 个 worker 线程
```

Windows (MSVC) 可直接打开根目录 `rate_limiter.sln` 按 F5，或用 CMake 生成：

```bash
cd server
cmake -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
```

### 运行单元测试（10 个用例，无需 Redis）

```bash
# 需先安装 hiredis（rate_limiter 独立构建依赖真实 hiredis）
cd rate_limiter
mkdir build && cd build
cmake .. && make -j$(nproc)
./unit_test          # 预期 10/10 passed
```

### 接口示例

启动后可用 `curl` 验证：

```bash
curl http://127.0.0.1:8080/health              # {"status":"ok"}
curl http://127.0.0.1:8080/api/v1/user/info     # 用户信息
curl http://127.0.0.1:8080/api/v1/order         # 订单（严格限流：20 req/s）
curl -X POST -d '{"a":1}' http://127.0.0.1:8080/api/v1/order
curl http://127.0.0.1:8080/api/v1/stats         # 限流统计
```

---

## 核心设计

### Feature → Score → Decision 三层解耦（风控）

```
Layer 1 Feature  → "你是谁？"    IFeatureExtractor 接口（静态/频率）
Layer 2 Score    → "多危险？"    IScorer 接口（累加/加权/ML 可替换）
Layer 3 Decision → "怎么处理？"  ThresholdConfig（ALLOW/LIMIT/CHALLENGE/REJECT）
```

任一层可独立替换，符合开闭原则。

### 限流算法

| 算法 | 实现 | 说明 |
|------|------|------|
| 固定窗口 | `FixedWindowLimiter` | INCR + EXPIRE，O(1)，边界突刺是已知缺陷 |
| 滑动窗口 | `SlidingWindowLimiter` | Redis ZSet + Lua 原子判定，1 次往返 |

### 本地限流 + Redis 兜底

三档判定：

```
本地计数 < 80% 配额  →  直接放行（纯本地滑动窗口，零网络开销）
本地计数 ≥ 80%      →  查 Redis 精确校验（跨机共享计数）
Redis 不可用        →  fail-open 放行
```

计数语义统一（这是本项目的关键设计点）：

| 计数环节 | 数据结构 | 角色 |
|---------|---------|------|
| 本地快路径 | `LocalSlidingWindow`（deque 时间戳，滑动窗口） | 单机精确计数 |
| Redis 兜底校验 | `slidingWindowCount`（ZSet + Lua，**只读** ZCARD） | 跨机精确校验 |
| 后台同步 | `slidingWindowRecord`（**批量 ZADD** 到同一 ZSet） | 把本地增量写入跨机共享计数 |

三者**共用同一个 Redis ZSet key、同一种滑动窗口语义**：后台同步是唯一写入方，兜底校验是纯读，每请求只被计数一次，最终一致（同步周期 100ms）。本地用 **64 分片锁**降低锁争用，Redis I/O 在锁外执行。

### 降级策略（三级）

1. Redis 不可用 → fail-open 放行
2. 单个 Extractor 失败 → 该特征填 defaultSafe，不阻断
3. DecisionEngine 为 null → 退化为纯限流

---

## 性能优化要点

本项目在网络层与业务层做了系统性的正确性修复与性能优化，要点包括：

- `Buffer` 用 readv/WSARecv + 64KB 栈缓冲，避免大报文多次 read 与缓冲满误判关闭
- `Acceptor` 循环 accept 直到 EAGAIN，应对突发建连
- `LocalRateLimiter` 64 分片锁 + 锁外做 Redis I/O + 增量同步
- `TimerWheel` 绝对 deadline（修 delay ≥ capacity 环绕）+ 原子计数器 ID
- `KeyBuilder` reserve+append 替代 ostringstream，热路径去堆分配
- HTTP 请求行/头/body 大小上限，防内存 DoS

---

## License

MIT
