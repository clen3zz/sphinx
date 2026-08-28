# Sphinx

Sphinx 是一个 Linux 上的 C++17 内存键值缓存服务，支持 Memcached 文本协议，重点展示 **事件驱动网络**、 **多线程分片**、
**日志结构化内存**与 **一致性哈希集群**的设计实现。

## 架构

```text
                         Sphinx 节点

TCP 客户端
    │
    ├── SO_REUSEPORT ──> Worker 0：epoll + 存储分片 0
    ├── SO_REUSEPORT ──> Worker 1：epoll + 存储分片 1
    └── SO_REUSEPORT ──> Worker N：epoll + 存储分片 N
                                  ▲
                                  │ hash(key) % worker_count
                         跨线程 SPSC 队列按序通信
```

- **两级分片**：客户端基于一致性哈希环选节点，节点内按键哈希（MurmurHash3）分发到 Worker；
- **无锁存储**：每个 Worker 独占一份 Log 分片，无锁读写；跨线程请求通过 SPSC 队列异步传递并严格保序回包。

## 支持的命令

| 命令                      | 行为                     |
|---------------------------|--------------------------|
| `set` / `add` / `replace` | 写入、条件添加、条件覆盖 |
| `get`                     | 单键或多键聚合读取       |
| `delete`                  | 删除键                   |
| `incr` / `decr`           | 原子增减 64 位整数       |
| `stats` / `version`       | 统计信息、版本查询       |

## 快速上手

### 依赖安装

- **Ubuntu/Debian**: `sudo apt install -y build-essential cmake ninja-build ccache python3 libgtest-dev netcat-openbsd`
- **Fedora**: `sudo dnf install -y gcc-c++ cmake ninja-build ccache python3 gtest-devel nc`

### 编译与测试

```bash
# 编译并运行测试（支持 Ninja + ccache 加速）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

### 运行体验

```bash
# 启动服务
./build/sphinxd/sphinxd --listen 127.0.0.1 --port 11211 --threads 4

# 读写测试
printf 'set key 0 0 5\r\nhello\r\nget key\r\n' | nc -N 127.0.0.1 11211

# 集群客户端测试
./build/sphinxd/sphinx-cluster --nodes 127.0.0.1:11211,127.0.0.1:11212 set user:1 Alice
./build/sphinxd/sphinx-cluster --nodes 127.0.0.1:11211,127.0.0.1:11212 get user:1
```

## 相关文档

- [核心调用链导读](docs/CALL_CHAIN.md)
- [性能基准测试报告](docs/BENCHMARK.md)
- [团队代码与命名规范](docs/CODING_STANDARDS.md)

## License

Apache-2.0
