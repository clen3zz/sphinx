# Sphinx

Sphinx 是一个运行在 Linux 上的 C++17 内存键值缓存。本仓库基于
[penberg/sphinx](https://github.com/penberg/sphinx) 继续开发，重点展示事件驱动网络、
多线程分片、日志结构化内存和一致性哈希在缓存系统中的组合方式。

## 核心架构

- 使用 Linux `epoll` 和每线程一个 Reactor 处理 TCP 连接；
- 按键哈希把数据分配到进程内不同存储线程，跨线程请求通过消息队列传递；
- 使用分段、日志结构化内存保存键值，并在空间不足时回收旧 Segment；
- 使用 Ragel 解析 Memcached ASCII 协议。

## 正确性修复

- 修复非阻塞 TCP 的分包、粘包、流水线、部分写及断开连接处理；
- 修复跨线程请求与响应回传、响应顺序和队列满时的错误处理；
- 修复键覆盖和 Segment 回收时的索引悬空、内存与 socket 生命周期问题。

## 新增功能

- 扩展 Memcached ASCII 协议，支持多键 `get`、`delete`、`incr` / `decr`、`flags` 和相对或绝对过期时间；
- 新增进程级 `stats`，统计数据命令数、缓存命中和未命中；
- 新增一致性哈希集群客户端，实现多节点静态分片、按节点复用连接及连接与 I/O 超时处理；
- 新增自动化测试、三节点集成测试，以及基于 `memtier_benchmark` 的可复现端到端基准。

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
| `stats` | 返回进程级数据命令、命中和未命中统计 |
| `version` | 返回兼容的 Memcached 版本字符串 |

## 构建与测试

Ubuntu 24.04 可安装以下依赖：

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build ccache python3 libgtest-dev netcat-openbsd
```

构建并运行默认测试（支持 Ninja 与 ccache 自动加速）：

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

`memtier_benchmark` 是可选依赖；安装后重新配置，CTest 会自动增加基准冒烟测试。

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

- [调用链说明](docs/CALL_CHAIN.md)
- [端到端基准方法与结果](docs/BENCHMARK.md)
- [编码与命名规范](docs/CODING_STANDARDS.md)

## 来源与许可

本项目基于 [penberg/sphinx](https://github.com/penberg/sphinx)，项目代码采用 Apache License 2.0；第三方文件遵循各自的许可声明。
