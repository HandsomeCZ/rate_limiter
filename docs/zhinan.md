# 零基础看懂这个项目 —— 从"什么是服务器"到"限流引擎"

> 本文假设你**完全没有编程经验**。用日常生活中的比喻来解释每一个概念。

---

## 目录

1. [这项目是干什么的？](#1)
2. [三分钟理解计算机网络](#2)
3. [代码骨架](#3)
4. [一条请求的完整旅程](#4)
5. [逐模块详解](#5)
6. [数据流向图](#6)
7. [如何运行和测试](#7)
8. [调试指南：在哪打断点](#8)
9. [常见问题排查](#9)
10. [术语速查表](#10)

---

<a name="1"></a>

## 1. 这项目是干什么的？

一句话：**带智能保安的网站后台**。

### 用餐厅来比喻

| 现实世界 | 计算机世界 | 对应代码 |
|----------|-----------|----------|
| 餐厅大门 | 服务器端口（8080） | TcpServer |
| 顾客进门 | 浏览器连接 | Acceptor |
| 服务员接单 | HTTP 请求处理 | TcpConnection |
| 保安检查人数 | 限流器（Rate Limiter） | RateLimiterFacade |
| 保安评估风险 | 风控引擎（Decision Engine） | DecisionEngine |
| 厨房出菜 | HTTP 响应 | send() |

### 三个核心能力

- **HTTP 服务器**：处理网页请求
- **限流**：限制每个用户/IP 的访问频率（每秒最多 N 次）
- **风控**：评估请求的风险等级，分级处理

---

<a name="2"></a>
## 2. 三分钟理解计算机网络

### 2.1 什么是 IP 地址？

就像每家每户有门牌号，计算机在网络上有 **IP 地址**（如 `192.168.1.1`）。
`127.0.0.1` 是一个特殊地址，指向**你自己的电脑**（叫回环地址）。

### 2.2 什么是端口？

一个 IP 地址就像一栋楼，**端口**就像楼里的房间号。

```
127.0.0.1:8080
 楼栋号  房间号
```

- 8080 端口：我们的程序在这里监听
- 80 端口：标准网页端口
- 443 端口：加密网页（HTTPS）

### 2.3 什么是 HTTP 请求？

你在浏览器输入 `http://127.0.0.1:8080/health`，浏览器发送一段文本：

```
GET /health HTTP/1.1
Host: 127.0.0.1:8080
```

服务器收到后返回：

```
HTTP/1.1 200 OK
Content-Type: application/json

{"status":"ok"}
```

这就是 **请求-响应模型**：你问一句，它答一句。

### 2.4 什么是 TCP？

TCP 就像**快递公司的签收服务**——保证数据完整、顺序正确地送达，不会丢字也不会乱序。

### 2.5 什么是事件循环（Event Loop）？

想象餐厅服务员不停张望：

```
服务员站门口 -> 有人来？-> 带进去点菜
    ^                        |
    +--- 继续等下个人 <--- 点完菜
```

程序里也是一样：**一个循环**不断检查有没有新连接、有没有数据到达。这就是 `EventLoop::loop()` 在做的事。

### 2.6 什么是 Redis？

Redis 是一个**超快临时记事本**。数据存在内存里（不是硬盘），极快。

在这个项目中用于记录：用户 A 在过去 1 秒内来了多少次。超过限制就拒绝。

---

<a name="3"></a>
## 3. 代码骨架：文件怎么组织的？

```
program/
├── rate_limiter.sln          ← Visual Studio 解决方案文件
├── rate_limiter.vcxproj      ← 项目配置文件
│
├── https-gitee-com-...tcp-server-2/   ← TCP/HTTP 服务器
│   ├── net/                  ← 网络库（底层通信）
│   │   ├── include/net/      ← 头文件（说明书）
│   │   │   ├── TcpServer.h   ← TCP 服务器
│   │   │   ├── EventLoop.h   ← 事件循环
│   │   │   ├── SelectPoller.h← Windows 网络监控
│   │   │   ├── EpollPoller.h ← Linux 网络监控
│   │   │   └── ...
│   │   └── src/              ← 源文件（真正干活的代码）
│   ├── http/
│   │   └── Http.hpp          ← HTTP 解析和路由
│   └── src/
│       └── main.cpp          ← 程序入口
│
└── rate_limiter/             ← 限流 & 风控引擎
    ├── include/              ← 头文件
    │   ├── common/common.h   ← 通用定义
    │   ├── service/          ← 核心业务层
    │   ├── rate_limiter/     ← 限流器
    │   ├── decision_engine/  ← 风控决策引擎
    │   ├── redis_client/     ← Redis 客户端
    │   └── ...
    └── src/                  ← 源文件
```

### 关键概念：.h 和 .cpp 是什么？

| 文件类型 | 比喻 | 作用 |
|---------|------|------|
| `.h`（头文件） | 菜单 | 声明：有什么功能，需要什么参数 |
| `.cpp`（源文件） | 厨房 | 实现：具体怎么做 |

---

<a name="4"></a>
## 4. 一条请求的完整旅程（8 步）

下面跟踪一个真实 HTTP 请求，从网络数据包到最终响应。

```
浏览器 -> http://127.0.0.1:8080/api/v1/user/info
```

### Step 1：接受连接
- 文件：`net/src/Acceptor.cpp`
- 函数：`handleRead()`
- 含义：有人敲门了，开门让客人进来

### Step 2：事件循环调度
- 文件：`net/src/EventLoop.cpp`
- 函数：`loop()`
- 含义：有活了，安排给对应的处理者

### Step 3：读取数据
- 文件：`net/src/Buffer.cpp`
- 函数：`readFd()`
- 含义：记下客人说的话（GET /api/v1/user/info HTTP/1.1 ...）

### Step 4：解析 HTTP
- 文件：`http/Http.hpp`
- 函数：`OnMessage()`
- 含义：翻译——方法=GET, 路径=/api/v1/user/info

### Step 5：限流 + 风控检查 ⭐
- 文件：`rate_limiter/src/service/rate_limit_service.cpp`
- 函数：`process()`
- 含义：保安按顺序检查
  1. 白名单？→ 直接放行
  2. 黑名单？→ 直接拒绝
  3. 限流检查？→ 超过频率就拒绝
  4. 风控评分？→ 风险高就挑战/拒绝
  5. 都没问题 → 放行

### Step 6：路由分发
- 文件：`http/Http.hpp`
- 函数：`Route()`
- 含义：匹配 URL 到处理函数

### Step 7：生成响应
- Handler 执行业务逻辑，返回 JSON 数据

### Step 8：发送响应
- 文件：`net/src/TcpConnection.cpp`
- 函数：`send()`
- 含义：通过网络把结果发回浏览器

---

<a name="5"></a>
## 5. 逐模块详解（11 个模块）

### 5.1 EventLoop —— 程序的"心脏"

就像餐厅服务员在门口不停张望：

```
while (没打烊) {
    看门口（有没有新客人？）
    看桌子（有没有客人举手？）
    处理能处理的事
}
```

它是整个程序的调度中心。

### 5.2 SelectPoller / EpollPoller —— 网络监控

| 平台 | 实现 | 工作方式 |
|------|------|----------|
| Windows | SelectPoller | 挨个问"你好了吗？" |
| Linux | EpollPoller | 等通知"有人好了" |

### 5.3 TcpServer / TcpConnection —— 接待客人

- TcpServer = 餐厅前台（接受新客人，分配服务员）
- TcpConnection = 服务员（一对一服务，直到客人离开）

### 5.4 Buffer —— 记事本

网络数据分块到达："你"..."好"..."我"..."要"...
Buffer 负责拼成完整句子："你好，我要点菜"

### 5.5 HttpServer —— 翻译官

把原始 HTTP 文本翻译成结构化的请求对象。

### 5.6 RateLimiter —— 频率控制保安 ⭐

两种算法：

| 算法 | 比喻 | 精确度 |
|------|------|--------|
| 固定窗口 | 钟表整点清零 | 一般（边界会漏） |
| 滑动窗口 | 往前看 1 秒内的次数 | 精确 |

### 5.7 DecisionEngine —— 风险评估保安 ⭐

```
特征提取 -> 规则匹配 -> 打分 -> 决策

示例：
  IP 是新的？       -> +10 分
  半夜访问？        -> +20 分
  访问了敏感 API？  -> +30 分
                       ----
              总分 60 -> 中风险 -> LIMIT
```

### 5.8 决策等级

| 等级 | 含义 | 比喻 |
|------|------|------|
| ALLOW | 正常放行 | 绿灯 |
| LIMIT | 收紧配额 | 黄灯（能进不能快） |
| CHALLENGE | 需要验证 | 橙灯（出示验证码） |
| REJECT | 直接拒绝 | 红灯 |

### 5.9 RedisClient —— 远程记事本

本地内存 -> 程序重启就没了。Redis -> 重启还在，多台机器共享。

### 5.10 LocalCache —— 本地小抄

缓存常用规则到本地内存。不用每次都问 Redis。

### 5.11 ConfigManager —— 配置中心

管理所有规则：白名单、黑名单、限流规则、风控规则。

---

<a name="6"></a>
## 6. 数据流向全景图

```
浏览器
  │ HTTP 请求
  ▼
[操作系统网络栈] ── "8080 有人敲门"
  │
  ▼
[EventLoop + SelectPoller] ── "有事件！"
  │
  ▼
[Acceptor -> TcpConnection] ── "分配服务员"
  │
  ▼
[Buffer::readFd()] ── "记下客人说的话"
  │
  ▼
[HttpServer::OnMessage()] ── "翻译 HTTP"
  │
  ▼
[RateLimitService::process()] ── "保安检查"
  │  白名单? 黑名单? 限流? 风控?
  │  决策: ALLOW
  ▼
[Route()] ── "/api/v1/user/info -> handler"
  │
  ▼
[Handler] ── 返回 {"user":{"id":"123"}}
  │
  ▼
[TcpConnection::send()] ── "HTTP/1.1 200 OK..."
  │
  ▼
浏览器显示结果
```

---

<a name="7"></a>
## 7. 如何运行和测试

### 启动程序

在 Visual Studio 中按 **F5**（调试模式）。

程序输出：

```
=== Initializing Rate Limit System ===
[LocalCache] started, interval=1000ms
[RedisPool] conn 1/32 failed          <- 正常！Redis 没装，降级运行
[WARN] Redis not available
=== Rate Limit System Ready ===
====================================
  Listening on port: 8080
====================================
```

### 浏览器测试

```
http://127.0.0.1:8080/health         -> {"status":"ok"}
http://127.0.0.1:8080/api/v1/stats   -> 限流统计
http://127.0.0.1:8080/api/v1/order   -> 订单数据
```

### curl 命令行测试

```powershell
curl http://127.0.0.1:8080/health
curl http://127.0.0.1:8080/api/v1/stats
```

---

<a name="8"></a>
## 8. 调试指南：在哪打断点

### 入门：5 个断点就够了

| # | 文件 | 函数/位置 | 看什么 |
|---|------|----------|--------|
| 1 | net/src/Acceptor.cpp | handleRead() | 新连接来了 |
| 2 | http/Http.hpp | OnMessage() | HTTP 解析 |
| 3 | http/Http.hpp | CheckRateLimit() | 限流判断 |
| 4 | http/Http.hpp | Route() | 路由匹配 |
| 5 | http/Http.hpp | 路由 lambda | 业务逻辑 |

按 F5 启动，浏览器访问 `http://127.0.0.1:8080/health`，断点会按 1-2-3-4-5 命中。

### 进阶：深入限流引擎

| # | 文件 | 函数 |
|---|------|------|
| 6 | rate_limiter/src/service/rate_limit_service.cpp | process() |
| 7 | rate_limiter/src/rate_limiter/rate_limiter.cpp | checkRule() |

### 进阶：理解事件循环

| # | 文件 | 函数 |
|---|------|------|
| 8 | net/src/EventLoop.cpp | loop() 内的 while |
| 9 | net/src/EventLoop.cpp | poller->poll() 之后 |

---

<a name="9"></a>
## 9. 常见问题排查

### Q: 程序启动就崩溃？
A: 端口 8080 可能被占用。改 `main.cpp` 里的 `port` 变量。

### Q: [RedisPool] conn 1/32 failed 是什么意思？
A: **正常的**。Redis 没装，程序自动降级运行，不用 Redis 也能工作。

### Q: 怎么确认限流在工作？
A: 快速多次访问 `/api/v1/order`，然后查看 `/api/v1/stats` 的 `rejected` 数字。

### Q: 降级是什么意思？
A: 就像水管坏了用瓶装水——功能差一点，但还能开业。Redis 不可用就用本地内存计数。

---

<a name="10"></a>
## 10. 术语速查表

| 术语 | 解释 |
|------|------|
| Socket | 网络通信的"电话线" |
| Port（端口） | 服务的"房间号" |
| TCP | 保证数据完整送达的协议 |
| HTTP | 网页的"语言" |
| Request（请求） | 你问服务器的问题 |
| Response（响应） | 服务器给你的回答 |
| JSON | 数据格式 {"key":"value"} |
| Event Loop | 程序的"心跳"，不停检查有无活要干 |
| Rate Limit | 限流——"你太快了，慢一点" |
| Risk Score | 风险评分，数字越大越危险 |
| Redis | 超快内存数据库 |
| 降级 (Degradation) | 部分功能坏了，用备用方案替代 |
| Fail-open | 出问题时选择放行（宁可错放也不错杀） |

---

> **最后的话**：你不用理解每一行代码。把这个项目想象成一个餐厅——
> 前台（TcpServer）、服务员（TcpConnection）、保安（RateLimiter + DecisionEngine）、
> 记事本（Redis + Buffer）。数据就像客人，进来、被检查、被服务、离开。
> 代码只是用计算机语言描述这个流程而已。
