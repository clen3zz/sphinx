# Sphinx

Sphinx 是一个运行在 Linux 上的 C++17 内存键值缓存。本仓库基于
[penberg/sphinx](https://github.com/penberg/sphinx) 继续开发，重点展示事件驱动网络、
多线程分片、日志结构化内存和一致性哈希在缓存系统中的组合方式。

## 核心能力

- 使用 Linux `epoll` 和每线程一个 Reactor 处理 TCP 连接；
- 按键哈希把数据分配到进程内不同存储线程，跨线程请求通过消息队列传递；
- 使用分段、日志结构化内存保存键值，并在空间不足时回收旧 Segment；
- 使用 Ragel 解析 Memcached ASCII 协议，支持分包、粘包、流水线和部分写；
- 支持 `flags`、相对或绝对过期时间，以及并发安全的原子计数；
- 提供进程级运行统计和基于 `memtier_benchmark` 的可复现端到端基准；
- 提供一致性哈希集群客户端，将键静态分片到多个独立 Sphinx 节点。

## 架构

```text
                         Sphinx 节点

TCP 客户端
    │
    ├── SO_REUSEPORT ──> Worker 0：epoll Reactor + 存储分片 0
    ├── SO_REUSEPORT ──> Worker 1：epoll Reactor + 存储分片 1
    └── SO_REUSEPORT ──> Worker N：epoll Reactor + 存储分片 N
                                  ▲
                                  │ hash(key) % worker_count
                         跨线程消息队列与响应回传
```

集群客户端在节点列表上建立一致性哈希环，每个键只路由到一个负责节点。节点内部再按相同的
键哈希完成线程级分片。

## 支持的命令

Sphinx 实现了以下 Memcached ASCII 协议子集：

| 命令 | 行为 |
| --- | --- |
| `set` | 写入或覆盖键值 |
| `add` | 仅在键不存在或已过期时写入 |
| `replace` | 仅在键存在且未过期时覆盖 |
| `get` | 按请求顺序读取一个或多个键 |
| `delete` | 删除键 |
| `incr` / `decr` | 对无符号十进制值进行原子增减 |
| `stats` | 返回进程级命令、命中和未命中统计 |
| `version` | 返回服务版本 |

## 构建与测试

Ubuntu 24.04 可安装以下依赖：

```bash
sudo apt update
sudo apt install -y build-essential cmake ragel python3 libgtest-dev
```

构建并运行全部测试：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

## 启动服务

下面的命令在 `127.0.0.1:11211` 启动一个 4 线程、64 MiB 的节点：

```bash
./build/sphinxd/sphinxd \
  --listen 127.0.0.1 \
  --port 11211 \
  --threads 4 \
  --memory-limit 64 \
  --segment-size 2
```

用 `nc` 写入并读取一个键：

```bash
printf 'set name 0 0 6\r\nsphinx\r\nget name\r\n' | nc -N 127.0.0.1 11211
```

## 静态集群客户端

分别启动多个 Sphinx 节点后，可以使用 `sphinx-cluster` 查询路由并读写负责节点：

```bash
NODES=127.0.0.1:11211,127.0.0.1:11212,127.0.0.1:11213

./build/sphinxd/sphinx-cluster --nodes "$NODES" route user:42
./build/sphinxd/sphinx-cluster --nodes "$NODES" set user:42 active
./build/sphinxd/sphinx-cluster --nodes "$NODES" get user:42
./build/sphinxd/sphinx-cluster --nodes "$NODES" delete user:42
```

## 文档

- [开发规格与测试矩阵](specs/README.md)
- [端到端基准方法与结果](BENCHMARK.md)

## 来源与许可

本项目基于 [penberg/sphinx](https://github.com/penberg/sphinx)，沿用 Apache License 2.0。
