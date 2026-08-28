# Sphinx 源码与调用链导读（面向小白）

如果你学过 C++ 和基础的数据库开发（比如写过简单的内存 KV 数据库、了解哈希索引和线程），但对网络协议、异步并发和分布式一窍不通，请放心，本文档就是为你写的。

读完本文，你就能轻松看懂 Sphinx 的整体架构、每个目录是干什么的，以及一条命令是如何在代码中流转的。

---

## 1. 把它当成一个“高性能内存数据库”

抛开复杂的名词，Sphinx 本质就是一个 **支持网络访问的内存 KV 数据库**：
- 客户端发来：`set name 0 0 5\r\nhello\r\n`，它在内存里存下 `name = "hello"`；
- 客户端发来：`get name\r\n`，它查出 `"hello"` 并返回给客户端。

### 为什么它比一般的简单数据库快？
普通多线程数据库是多个线程抢着去读写同一个全局哈希表，必须到处加互斥锁（`std::mutex`），这在高并发下会非常慢。

Sphinx 采用的是 **无锁分片设计（Shared-Nothing / Partitioning）**：
- 假设启动 4 个工作线程（Worker 0 ~ 3），每个 Worker 内部都有一套完全独立的**小数据库**（自己的哈希索引 + 自己的内存池）；
- 数据存到哪个 Worker？按 `hash(key) % 4` 划分；
- **核心原则**：谁的数据谁读写，绝不跨线程直接改对方的内存。这样整个存储模块**一行锁都不需要加**，跑得飞快！

---

## 2. 核心四大组件（人话对照表）

| 模块名称 | 对应源码 | 相当于数据库里的什么？ | 用大白话解释它的工作 |
| --- | --- | --- | --- |
| **Reactor（前台接待）** | `reactor.h`<br>`reactor-epoll.h` | 网络连接接入层 | 负责和客户端连网、接听 socket 数据、发回响应。底层用 Linux 的 `epoll`，有数据时才叫醒线程，绝不傻等阻塞。 |
| **Parser（协议解析器）** | `protocol.h`<br>`protocol_types.h` | SQL / 命令词法分析器 | 把客户端发来的网络字符串（如 `set key ...`）翻译切片成 C++ 的强类型结构体 `Command`。 |
| **Log & Index（存储引擎）** | `logmem.h`<br>`index.h` | 内存池与索引 | **Log**：通过 Linux `mmap` 申请一块大内存，像写日志一样按顺序追加存数据；<br>**Index**：哈希表，记录 `key` 在哪块内存地址。 |
| **SPSC 信箱（跨线程通道）** | `spsc_queue.h` | 线程间消息通信 | 单生产者单消费者无锁队列。两个 Worker 之间互通有无的专用信箱。 |

---

## 3. 源码地图：每个文件是干啥的

进入 `sphinxd/` 目录，你会看到以下核心代码：

```text
sphinxd/
├── include/sphinx/
│   ├── protocol.h               # 协议解析器：把网络文本转为 Command 结构体
│   ├── logmem.h / index.h       # 存储引擎：内存分段管理、数据追加写与哈希索引
│   ├── memory.h                 # 内存管理：用 RAII 方式调用系统的 mmap 申请内存
│   ├── reactor.h                # 网络框架：Reactor 与 Socket 的抽象
│   ├── reactor-epoll.h          # 网络实现：基于 Linux epoll 的事件循环驱动
│   ├── spsc_queue.h             # 线程信箱：无锁单产单消循环队列
│   └── stats.h                  # 状态计数：原子记录 get/set 命中与未命中次数
├── src/
│   ├── sphinxd.cpp              # main() 函数入口：解析启动参数，创建多个 Worker
│   ├── server/
│   │   ├── server.cpp           # 业务枢纽：调度网络收包、计算数据分片、派发请求
│   │   ├── connection.cpp       # 连接管家：保存未拼完整的 TCP 数据包，并按顺序回包
│   │   ├── command_executor.cpp # 命令执行器：真正调用 Log::append / find_value 的地方
│   │   └── message.h            # 跨线程数据包定义（Command 与 Response）
│   ├── logmem.cpp               # 存储实现：数据写入、旧段循环覆盖回收（淘汰策略）
│   ├── reactor-epoll.cpp        # 网络实现：epoll_wait 等待网络读写事件
│   └── sphinx-cluster.cpp       # 集群客户端命令行工具
└── test/                        # 单元测试与网络自动化测试
```

---

## 4. 一条请求的完整旅程（故事化调用链）

假设我们启动了 4 个 Worker 线程。此时客户端连上来，发送了一句：
`get user:100\r\n`

整个系统内部发生了什么？

```text
[TCP 客户端]
     │ 发送 "get user:100\r\n"
     ▼
【Worker 0】（假设操作系统把连接分配给了 Worker 0）
  1. epoll 发现 socket 来了数据，触发 TcpSocket::on_pollin
  2. Connection::receive_buffer 把收到的字节存下
  3. Parser::parse 把它翻译成结构体：GetCommand(key = "user:100")
  4. 计算归属：MurmurHash3("user:100") % 4
     假设算出的结果是 2 ── 说明这个数据归【Worker 2】保管！
  5. Worker 0 不能直接查 Worker 2 的内存，于是它写了一封信 Message(Command)，
     塞进了 Worker 2 的专属信箱（SPSCQueue）。
     │
     │ （跨线程无锁信箱传递）
     ▼
【Worker 2】
  6. Worker 2 的 Reactor 被唤醒，从信箱里取出这封信；
  7. 交给 CommandExecutor::execute_get 执行；
  8. 调用 Worker 2 本地的 Index 查找 key，在 Log 内存中读出值 "Alice"；
  9. 把结果包装成回信 Message(Response: "VALUE user:100 ... Alice\r\n")，
     塞回 Worker 0 的信箱！
     │
     │ （跨线程无锁信箱传递）
     ▼
【Worker 0】
  10. Worker 0 收到回信；
  11. Connection::enqueue_response 检查请求序号（确保先后请求不乱序）；
  12. TcpSocket::send 通过网络发回给客户端。
     │
     ▼
[客户端收到结果]
```

> **同 Worker 的极速捷径**：
> 如果第 4 步算出来的目标刚好就是 Worker 0 自己，系统就不会走信箱发信，而是直接在本地调用 `CommandExecutor` 读写，耗时更短！

---

## 5. 小白常见疑问解答

### Q1：为什么不能让 Worker 0 直接去读 Worker 2 的哈希表？
在 C++ 里，如果两个线程同时访问同一个容器，必须加锁（比如 `std::mutex`），否则程序会崩溃或读到脏数据。但加锁会让 CPU 大量时间浪费在排队等锁上。
Sphinx 的设计理念是 **“用消息通信代替共享内存”**：每个 Worker 独享自己的地盘，永远单线程无锁跑，吞吐量能轻松跑满 CPU 多核。

### Q2：什么是所谓的“集群（Cluster）”？
- **单机 Worker 分片**：是一台电脑内部的多个 CPU 核心分工；
- **集群分片**：是一台电脑内存放不下（比如只有 64GB，而你有 1TB 数据），于是买 3 台电脑（Node A、Node B、Node C）。
- 客户端工具 `sphinx-cluster` 也是用哈希算法（一致性哈希），算出来这个 key 归哪台电脑管，就往哪台电脑发。这就是分布式数据库最基础的数据分片思想！

### Q3：内存如果被写满了会发生什么？
Sphinx 采用的是 **分段循环淘汰机制**：
内存不是每次 `new`，而是一开始就划分为若干个固定大小的“段（Segment）”。写数据时按顺序在当前段追加；当所有段都写满后，系统会**把最早、最旧的那个段整段清空回收**，并清理掉对应的哈希索引，腾出空间继续写入。

---

## 6. 小白读源码的最佳路线

建议用 CLion 或 VSCode 按以下顺序点击跳转，最容易理清头绪：

1. **`sphinxd/src/sphinxd.cpp`**：看 `main()` 函数，看服务是怎么开线程启动的；
2. **`sphinxd/src/server/server.cpp`**：看 `Server::recv` 和 `Server::process_one`，看收包与哈希路由；
3. **`sphinxd/src/server/command_executor.cpp`**：看 `execute_command`，看具体命令怎么被映射为数据库操作；
4. **`sphinxd/src/server/connection.cpp`**：看怎么解决 TCP 粘包拆包，以及怎么保证回包按顺序；
5. **`sphinxd/src/logmem.cpp`**：看数据库存储引擎如何做内存追加写和段回收。
