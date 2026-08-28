# Sphinx 项目结构与调用链

这是一份面向初学者的源码导读。读完后，你应该能回答三个问题：

1. 项目的核心代码分别放在哪里？
2. 一条缓存命令从客户端进来后，会经过哪些模块？
3. 多线程分片和集群分片有什么区别？

## 1. 先用一句话理解项目

Sphinx 是一个 Linux 上的 C++17 内存缓存服务，支持一部分 Memcached 文本协议，例如：

- `set`：写入键值；
- `get`：读取一个或多个键；
- `add`、`replace`、`delete`：条件写入或删除；
- `incr`、`decr`：修改数字值；
- `stats`、`version`：查看状态和版本。

它有两个可执行程序：

| 程序 | 用途 |
| --- | --- |
| `sphinxd` | 真正保存数据并处理 TCP 请求的缓存服务端 |
| `sphinx-cluster` | 把请求路由到不同 Sphinx 节点的命令行客户端 |

## 2. 四个必须先懂的词

| 名称 | 用人话解释 |
| --- | --- |
| Worker | 一个工作线程。每个 Worker 都有自己的网络事件循环和一份存储空间 |
| Reactor | 网络调度器。它用 `epoll` 等待连接、收包、发包和线程消息 |
| 分片 | 把不同的键交给不同 Worker 或节点保存，避免所有请求挤在一起 |
| Message | Worker 之间传递的类型安全消息，内容只能是 `Command` 或 `Response` |

最重要的一点是：

> 接收 TCP 连接的 Worker 不一定是保存这个键的 Worker。

因此项目要同时处理两件事：

- socket 始终留在接收连接的 Worker；
- 命令按 key 的哈希值转发到数据所属的 Worker。

## 3. 项目目录

```text
sphinx/
├── CMakeLists.txt                   # 顶层构建配置
├── README.md                        # 使用说明
├── docs/
│   ├── BENCHMARK.md                 # 性能测试说明
│   └── CALL_CHAIN.md                # 本文档
├── scripts/                         # 格式化和基准测试脚本
└── sphinxd/
    ├── CMakeLists.txt               # 服务端、客户端和测试目标
    ├── include/sphinx/
    │   ├── reactor.h                # ReactorGroup、Reactor、TCP socket
    │   ├── reactor-epoll.h          # Linux epoll 后端
    │   ├── protocol_types.h         # Parser 输出的类型化命令
    │   ├── logmem.h                 # Object、Segment、Log
    │   ├── index.h                  # key 到 Object* 的索引
    │   ├── memory.h                 # mmap 内存的 RAII 封装
    │   ├── stats.h                  # 进程级统计
    │   ├── cluster.h                # 一致性哈希环
    │   └── cluster_client.h         # 集群客户端接口
    ├── src/
    │   ├── sphinxd.cpp              # 服务端入口，只负责启动
    │   ├── server/
    │   │   ├── config.h             # 启动配置
    │   │   ├── options.cpp          # 命令行解析和校验
    │   │   ├── server.h/.cpp        # 网络、解析、路由和跨线程协调
    │   │   ├── connection.h/.cpp    # 单连接缓冲、顺序和多键聚合
    │   │   ├── message.h            # Command、Response、Message
    │   │   └── command_executor.*   # 真正执行缓存命令
    │   ├── protocol.rl              # Ragel 协议解析器
    │   ├── reactor.cpp              # TCP 和线程消息公共逻辑
    │   ├── reactor-epoll.cpp        # epoll 事件循环
    │   ├── logmem.cpp               # 日志结构化内存
    │   ├── stats.cpp                # 统计实现
    │   ├── cluster.cpp              # 节点解析和一致性哈希
    │   ├── cluster_client.cpp       # TCP 传输和集群命令
    │   └── sphinx-cluster.cpp       # 集群客户端入口
    ├── test/                        # 单元测试和网络测试
    └── perf/                        # 可选微基准
```

## 4. 整体架构

```text
                         一个 Sphinx 节点

TCP 客户端
    │
    │  Linux 通过 SO_REUSEPORT 选择一个 Worker 接收连接
    ▼
连接所属 Worker
    ├── EpollReactor：负责 socket
    ├── Connection：负责收包、请求序号和回包顺序
    └── Server：解析命令并计算 key 属于哪个 Worker
                         │
               ┌─────────┴─────────┐
               │                   │
          key 属于本 Worker    key 属于其他 Worker
               │                   │
               ▼                   ▼
       execute_command       Message(Command)
               │                   │
               ▼                   ▼
          本地 Log 分片       目标 Worker 的 Log
               │                   │
               └─────────┬─────────┘
                         ▼
                      Response
                         │
                         ▼
             回到连接所属 Worker 按序发出
```

每个 Worker 都拥有：

- 一个 `Reactor`；
- 一个 `Server`；
- 一个独占的 `Log` 存储分片。

所有 Worker 共享：

- 一个 `ReactorGroup`，里面保存线程间队列、eventfd 和唤醒状态；
- 一个 `ServerStats`，使用原子计数器保存进程级统计。

因为每个 key 只由一个数据 Worker 操作，所以 `Log` 不需要为普通读写加锁。

## 5. 服务端怎么启动

```text
main
  -> parse_options                 读取并校验命令行参数
  -> 创建 ServerStats             所有 Worker 共享统计
  -> 创建 ReactorGroup            所有 Worker 共享消息通道
  -> 为每个 Worker 创建线程
       -> Memory::mmap             分配该 Worker 的存储内存
       -> 构造 Log                 把内存切成多个 Segment
       -> 构造 Server              连接 Reactor、Log 和 Stats
       -> Server::serve
            -> 创建 TCP listener
            -> 注册到 epoll
            -> Reactor::run        进入事件循环
```

`sphinxd.cpp` 只负责这条启动链。真正的服务逻辑位于 `src/server/`。

## 6. 一条普通请求怎么走

以 `get user:42` 为例：

```text
客户端发送 "get user:42\r\n"
  -> epoll 发现 socket 可读
  -> TcpSocket::on_pollin
  -> Server::recv
  -> 追加到 Connection::receive_buffer
  -> memcache::Parser::parse
  -> 得到 ParsedCommand(GetCommand)
  -> Server::process_one
  -> 计算 hash(key) % Worker 数量
  -> Server::dispatch_command
  -> execute_command
  -> Log::find_value
  -> 生成 Response
  -> Connection::enqueue_response
  -> TcpSocket::send
```

这里有三个容易忽略的细节。

### 6.1 分包和流水线

TCP 不保证一次 `recv` 就得到一条完整命令：

- 命令不完整时，字节继续留在 `Connection` 的接收缓冲中；
- 一次收到多条命令时，服务端会循环解析；
- `set` 等写命令必须等命令头、value 和结尾 `\r\n` 全部到齐。

Parser 不会把内部临时指针暴露给服务端。它通过 `command()` 返回拥有自己字符串数据的 `ParsedCommand`。

### 6.2 本地执行和远程执行

目标 Worker 的计算方式是：

```text
MurmurHash3(key) % Worker 数量
```

如果目标就是当前 Worker：

```text
dispatch_command -> handle_command -> execute_command
```

如果目标是其他 Worker：

```text
dispatch_command
  -> Message(Command)
  -> Reactor::send_msg
  -> 目标 Worker::on_message
  -> handle_command
  -> execute_command
```

无论本地还是远程，最终都调用同一个 `execute_command`，不会维护两套命令逻辑。

### 6.3 响应为什么不会乱序

同一连接上的请求都有递增的 `sequence`：

```text
请求 A: sequence 0
请求 B: sequence 1
请求 C: sequence 2
```

即使 B 比 A 更早执行完，`Connection` 也会先保存 B 的结果，等 A 到达后再按 `0、1、2` 的顺序发送。

跨 Worker 响应使用 `send_msg_deferred`。如果溢出邮箱无法分配内存，服务端再尝试一次有界队列；两次都失败时直接终止进程，避免悄悄丢失一个响应后让连接永久等待。

## 7. 多键 get 怎么走

例如：

```text
get a b c
```

三个 key 可能属于三个不同 Worker：

```text
Server::process_one
  -> Connection::begin_multi_get
  -> 分别派发 a、b、c
  -> 每个数据 Worker 返回一个 Response 片段
  -> Connection::add_multi_get_piece
  -> 按 a、b、c 的原顺序拼接
  -> 最后只添加一次 END
  -> enqueue_response
```

多键 `get` 只是结果聚合，不是事务。读取不同 key 时，其他请求仍可能修改其中的值。

## 8. 命令最终在哪里执行

所有缓存命令集中进入：

```text
execute_command(Log&, ServerStats&, const Command&)
```

| 命令 | 调用的存储操作 |
| --- | --- |
| `set` | `Log::append` |
| `add` | 先 `Log::find_value`，不存在才 `append` |
| `replace` | 先 `Log::find_value`，存在才 `append` |
| `get` | `Log::find_value` |
| `delete` | `Log::remove` |
| `incr/decr` | `Log::update_counter` |
| `stats` | `ServerStats::render` |
| `version` | 直接生成版本响应 |

统计也在这里更新，所以命令执行和计数逻辑不会散落在网络层。

## 9. 数据怎么保存在内存里

每个 Worker 启动时通过 `mmap` 得到一块连续内存，`Log` 再把它切成固定大小的 Segment：

```text
mmap 内存
┌──────── Segment 0 ────────┐
│ Object │ Object │ ...     │
├──────── Segment 1 ────────┤
│ Object │ Object │ ...     │
└───────────────────────────┘
```

一次写入的大致调用链：

```text
Log::append
  -> Segment::append
  -> 在 Segment 内 placement-new 一个 Object
  -> Index::insert_or_assign(key, Object*)
  -> 如果是覆盖，标记旧 Object 已过期
```

查询时：

```text
Log::find_value
  -> Index::find
  -> 检查对象是否删除或到期
  -> 返回 value、flags 和 expiration
```

当前 Segment 放不下，而且没有干净 Segment 时，`Log` 会从最旧 Segment 开始整段回收。回收前会清理仍指向该 Segment 的索引项，避免留下悬空指针。

## 10. 非阻塞发送怎么处理

socket 一次不一定能把整个响应发完：

```text
TcpSocket::send
  -> 全部写完：结束
  -> 只写了一部分或遇到 EAGAIN：
       -> 剩余字节放入 _tx_buf
       -> Reactor 开始监听 EPOLLOUT
       -> socket 再次可写时继续发送
       -> 缓冲清空后取消 EPOLLOUT
```

因此 Worker 不会为了等待一个慢客户端而阻塞整个事件循环。

## 11. 集群客户端怎么路由

`sphinx-cluster` 处理的是“节点之间”的分片，而服务端内部处理的是“Worker 之间”的分片。

```text
ClusterClient
  -> ConsistentHashRing::route(key)
  -> 选择一个 Sphinx 节点
  -> connection_for(node)
  -> MemcachedConnection
  -> TcpTransport
  -> 向该节点发送 Memcached 命令
```

一致性哈希环为每个真实节点建立 64 个虚拟节点。增加或删除节点时，通常只需要重新映射一部分 key。

每个 `ClusterClient` 会复用到各节点的连接。如果一次操作发生连接、超时或协议错误，它会删除对应连接；下一次操作会重新连接。

完整的两级路由是：

```text
key
  -> 一致性哈希：选择 Sphinx 节点
  -> Linux：选择接收 TCP 连接的 Worker
  -> hash(key) % Worker 数量：选择数据 Worker
```

## 12. 建议阅读顺序

第一次看源码时，按下面顺序最容易理解：

```text
1. sphinxd/src/sphinxd.cpp
2. sphinxd/src/server/server.h
3. sphinxd/src/server/server.cpp
4. sphinxd/src/server/connection.cpp
5. sphinxd/src/server/command_executor.cpp
6. sphinxd/src/server/message.h
7. sphinxd/src/protocol.rl
8. sphinxd/include/sphinx/reactor.h
9. sphinxd/src/reactor.cpp
10. sphinxd/src/reactor-epoll.cpp
11. sphinxd/src/logmem.cpp
12. sphinxd/src/cluster.cpp
13. sphinxd/src/cluster_client.cpp
```

## 13. 测试对应什么功能

| 测试 | 主要验证内容 |
| --- | --- |
| `protocol_test.cpp` | 协议解析、分包、流水线和非法输入 |
| `reactor_test.cpp` | 跨线程消息、队列回压和 TCP 部分写 |
| `logmem_test.cpp` | 写入、覆盖、过期、回收和数字操作 |
| `stats_test.cpp` | 原子计数和 stats 输出 |
| `cluster_test.cpp` | 一致性哈希 |
| `cluster_client_test.cpp` | 连接复用、分包、超时和错误处理 |
| `network_test.py` | 启动真实服务端进行端到端协议测试 |
| `cluster_network_test.py` | 启动三个节点验证集群路由和读写 |

把一条请求记成下面这句话，就抓住了整个项目：

> Reactor 收包，Parser 解析，Server 路由，CommandExecutor 操作 Log，Connection 按顺序回包。
