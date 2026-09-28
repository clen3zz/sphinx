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

## 相较原仓库的改进

原仓库提供了 epoll、按核分片、跨核消息传递和日志结构化内存的基础实现；本项目在此基础上：

- 修复 TCP 拆包、部分写、连接关闭、跨线程响应保序以及索引与对象生命周期等正确性问题；
- 补全 `delete`、`incr/decr`、`stats`、过期时间、multi-get 和客户端一致性哈希分片；
- 增加覆盖协议、网络、跨线程与集群行为的自动化测试和本地基准测试工具。

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

项目提供依赖安装脚本，支持 Ubuntu、Debian 和 Fedora：

```bash
./scripts/install_dependencies.sh
```

脚本会安装编译器、CMake、Ninja、ccache、GoogleTest、Python 和网络测试所需工具。
`memtier_benchmark` 仅用于可选的基准测试，不属于核心构建依赖。
MySQL 商品服务另需 MySQL 服务端、`libmysqlclient` 开发包；HTTP 和 JSON 头文件由启用该示例时的 CMake 获取。

### 编译与测试

```bash
# 编译并运行测试（支持 Ninja + ccache 加速）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

### MySQL + Sphinx 商品服务（可选）

MySQL 是商品记录的权威存储，Sphinx 是可重建的读取缓存。启用示例后会生成
`sphinx-product-service`。该服务通过 `SPHINX_MYSQL_USER`、`SPHINX_MYSQL_PASSWORD`、
`SPHINX_MYSQL_DATABASE` 连接数据库；可选配置包括 `SPHINX_MYSQL_HOST`、
`SPHINX_MYSQL_PORT`、`SPHINX_CACHE_NODES`、`SPHINX_HTTP_BIND` 和 `SPHINX_HTTP_PORT`。

```bash
cmake -S . -B build -G Ninja -DBUILD_MYSQL_SPHINX_DEMO=ON
cmake --build build -j"$(nproc)"
# 先启动 Sphinx，并配置上述 SPHINX_MYSQL_* 变量，再启动 HTTP 服务
./build/product-service/sphinx-product-service
```

真实数据库验收使用独立的临时库，库名需包含 `test`，并设置
`SPHINX_TEST_MYSQL_HOST`、`SPHINX_TEST_MYSQL_PORT`、`SPHINX_TEST_MYSQL_USER`、
`SPHINX_TEST_MYSQL_PASSWORD`、`SPHINX_TEST_MYSQL_DATABASE`。随后运行：

```bash
./scripts/verify_mysql_sphinx.sh build
```

普通 `ctest` 在未配置数据库时会跳过相应集成用例；只有上面的严格验收脚本成功，才表示数据库和 HTTP 集成用例实际执行通过。

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

- [快速架构导读](docs/ARCHITECTURE.md)
- [核心调用链导读](docs/CALL_CHAIN.md)
- [性能基准测试报告](docs/BENCHMARK.md)
- [团队代码与命名规范](docs/CODING_STANDARDS.md)

## License

Apache-2.0
