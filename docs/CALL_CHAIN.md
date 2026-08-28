# Sphinx 架构与核心调用链

本文档梳理 Sphinx 的核心设计、源码目录与请求生命周期，方便快速理解系统全貌。

---

## 1. 核心概念

| 概念 | 说明 |
| --- | --- |
| **Worker** | 工作线程。每个 Worker 独占一个 epoll 事件循环与一份无锁 Log 存储分片 |
| **Reactor** | 基于 `epoll` 的网络调度器，驱动连接接入、数据收发与跨线程唤醒 |
| **两级分片** | 客户端通过一致性哈希定位节点；服务端内部按 `MurmurHash3(key) % N` 路由到 Worker |
| **保序回包** | 连接始终归属接收 socket 的 Worker，跨线程异步处理通过递增 `sequence` 保证应答不乱序 |

---

## 2. 源码目录

```text
sphinxd/
├── include/sphinx/
│   ├── protocol.h               # 自包含状态机协议解析器（零外部生成依赖）
│   ├── protocol_types.h         # 解析器输出的类型化命令结构体
│   ├── reactor.h / reactor-epoll.h # Reactor 抽象与 Linux epoll 实现
│   ├── spsc_queue.h             # 跨线程无锁有界 SPSC 消息队列
│   ├── logmem.h / index.h       # Log 分段内存池与哈希索引
│   ├── memory.h                 # 基于 mmap 的内存 RAII 管理
│   ├── stats.h                  # 进程级原子计数器统计
│   └── cluster.h / cluster_client.h # 一致性哈希环与集群客户端
├── src/
│   ├── sphinxd.cpp              # 服务端启动入口（解析参数并启动 Worker）
│   ├── server/
│   │   ├── server.h/.cpp        # 网络收发、命令路由与跨线程协调
│   │   ├── connection.h/.cpp    # 连接缓冲区管理、请求流水线与保序队列
│   │   ├── command_executor.h/.cpp # 存储操作分发与执行
│   │   └── message.h            # 跨线程 Command / Response 消息载荷
│   ├── logmem.cpp               # 日志内存分配、对象追加、淘汰与过期
│   ├── reactor.cpp / reactor-epoll.cpp # 事件循环与 socket 读写驱动
│   └── sphinx-cluster.cpp       # 集群客户端 CLI 入口
└── test/                        # 单元测试与网络端到端测试
```

---

## 3. 架构流程图

```text
TCP 客户端
    │
    │  SO_REUSEPORT 随机分发
    ▼
连接所属 Worker (接收并持有 socket)
    │
    ├─ 1. epoll 触发可读 -> TcpSocket::on_pollin
    ├─ 2. 追加到 Connection::receive_buffer
    ├─ 3. Parser::parse 提取 ParsedCommand
    └─ 4. 计算 target = MurmurHash3(key) % N
           │
     ┌─────┴────────────────┐
     │ (目标为本地)          │ (目标为其他 Worker)
     ▼                      ▼
Server::handle_command    Reactor::send_msg (SPSC 队列)
     │                      │
     │                      ▼
     │            目标 Worker 收到消息并执行
     │                      │
     └─────────┬────────────┘
               ▼
   CommandExecutor::execute_command
               │
   操作本地存储分片 (Log::append / Log::find_value)
               │
               ▼
     生成 Response 并回传连接 Worker
               │
               ▼
 Connection::enqueue_response (按 sequence 严格保序)
               │
    TcpSocket::send (非阻塞发送至客户端)
```

---

## 4. 关键实现机制

### 4.1 服务端启动链
`main` $\to$ 解析命令行参数 $\to$ 创建共享 `ServerStats` 与 `ReactorGroup` $\to$ 为每个 Worker 线程 `mmap` 分配存储内存 $\to$ 初始化 `Log`、`Reactor` 与 `Server` $\to$ 绑定监听端口 $\to$ 进入 `Reactor::run` 事件循环。

### 4.2 跨线程异步通信与回压
- Worker 间通过无锁单产单消队列（`SPSCQueue`）与 `eventfd` 进行唤醒；
- 队列占满时触发回压或延迟排队，避免高并发下内存无限膨胀或消息丢失。

### 4.3 多键 Get 聚合
处理 `get a b c` 时，服务端通过 `Connection::begin_multi_get` 记录分片等待计数，按键分别路由到各个 Worker 并行读取，汇总集齐后按原键序组装单一响应发出。

### 4.4 存储与分段回收
- 内存预先划分为固定大小的 `Segment`；
- 写操作（`set/add`）在当前活跃 Segment 内通过 placement-new 构造 `Object` 并注册到 `Index`；
- 空间耗尽时，按轮转顺序整段回收最旧 Segment，并自动清除对应失效索引，杜绝悬空指针。

---

## 5. 源码导读推荐顺序

1. **`sphinxd/src/sphinxd.cpp`**：理解服务启动与 Worker 线程拓扑；
2. **`sphinxd/src/server/server.cpp`**：掌握连接生命周期与命令路由；
3. **`sphinxd/src/server/connection.cpp`**：理解分包、流水线与保序队列；
4. **`sphinxd/src/server/command_executor.cpp`**：掌握命令与存储操作的映射；
5. **`sphinxd/src/logmem.cpp`**：理解分段日志内存分配与回收算法；
6. **`sphinxd/src/reactor-epoll.cpp`**：理解底层 epoll 事件循环与非阻塞 IO。

---

## 6. 测试与模块对应

| 测试套件 | 覆盖模块 |
| --- | --- |
| `protocol_test.cpp` | 状态机协议解析、分包粘包与非法输入防护 |
| `reactor_test.cpp` | SPSC 跨线程消息传递、队列回压与非阻塞部分写 |
| `logmem_test.cpp` | 数据写入、覆盖、原子增减、过期失效与段回收 |
| `stats_test.cpp` | 原子计数器并发统计与 stats 输出 |
| `cluster_test.cpp` | 一致性哈希虚拟节点分布与路由算法 |
| `cluster_client_test.cpp` | 集群客户端连接复用、分包处理与超时重试 |
| `network_test.py` | 启动真实单节点服务进行全量端到端测试 |
| `cluster_network_test.py` | 启动三节点集群验证跨节点一致性哈希路由 |
