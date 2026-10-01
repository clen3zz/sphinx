# Sphinx：C++ 分片缓存与 MySQL 商品服务

Sphinx 是一个 C++17 缓存项目：客户端通过一致性哈希选择缓存节点，节点使用 Reactor/epoll
处理网络请求，再按 key 分发给独占 Log + Index 存储的 Worker。节点支持 Memcached 文本
协议、TTL 和多 key 查询；存储通过 segment 回收控制内存占用。

项目还提供商品 HTTP 服务，展示 Sphinx 在缓存旁路模式中的用法：MySQL 保存权威数据，
Sphinx 缓存可过期、可重建的查询结果。

```text
HTTP 客户端 → sphinx-product-service ──→ MySQL（权威数据）
                         │
                         └─ ClusterClient（一致性哈希选节点）
                              ├─ sphinxd :11211 ─ Worker 0..N ─ Log + Index
                              └─ sphinxd :11212 ─ Worker 0..N ─ Log + Index
```

`GET /products/{id}` 先查 Sphinx，未命中或缓存故障时查询 MySQL 并尽力回填；
`PUT /products/{id}` 在 MySQL 中检查 expected_version 并提交更新，然后尽力删除缓存。
缓存来源通过 X-Cache 响应头体现。另有 Redis 对照实现与实验工具，两条路径共用商品业务
和保护策略，详见 [Redis 对照架构](docs/ARCHITECTURE.md)。

## 构建

项目运行于 Linux。依赖安装脚本支持 Ubuntu、Debian 和 Fedora；默认构建包含商品服务，
需要 libmysqlclient 和 hiredis 开发包。CMake 获取 cpp-httplib 与 nlohmann/json 头文件。

```bash
./scripts/install_dependencies.sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

未配置测试数据库时，普通 ctest 会跳过 MySQL 与 HTTP 集成用例；完整链路需执行下方严格验证。

## 跑通一个商品

准备 MySQL 8.4 数据库和有读写权限的用户，在商品服务终端设置连接配置并建表：

```bash
export SPHINX_MYSQL_HOST=127.0.0.1
export SPHINX_MYSQL_PORT=3306
export SPHINX_MYSQL_USER=your_user
export SPHINX_MYSQL_PASSWORD=your_password
export SPHINX_MYSQL_DATABASE=your_database

MYSQL_PWD="$SPHINX_MYSQL_PASSWORD" mysql --protocol=tcp \
  -h "$SPHINX_MYSQL_HOST" -P "$SPHINX_MYSQL_PORT" \
  -u "$SPHINX_MYSQL_USER" "$SPHINX_MYSQL_DATABASE" < product-service/schema.sql
MYSQL_PWD="$SPHINX_MYSQL_PASSWORD" mysql --protocol=tcp \
  -h "$SPHINX_MYSQL_HOST" -P "$SPHINX_MYSQL_PORT" \
  -u "$SPHINX_MYSQL_USER" "$SPHINX_MYSQL_DATABASE" \
  -e "INSERT INTO products (id, name, price_cents) VALUES (42, 'tea', 199);"
```

从仓库根目录，在另外两个终端分别启动 Sphinx 缓存节点：

```bash
# 缓存节点终端 1
./build/sphinxd/sphinxd --listen 127.0.0.1 --port 11211 --threads 2
# 缓存节点终端 2
./build/sphinxd/sphinxd --listen 127.0.0.1 --port 11212 --threads 2
```

回到已设置 MySQL 配置的终端启动商品服务：

```bash
export SPHINX_CACHE_BACKEND=sphinx
export SPHINX_CACHE_NODES=127.0.0.1:11211,127.0.0.1:11212
./build/product-service/sphinx-product-service
```

Sphinx 是默认缓存后端。另开终端依次请求：

```bash
curl -i http://127.0.0.1:8080/products/42
curl -i http://127.0.0.1:8080/products/42
curl -i -X PUT http://127.0.0.1:8080/products/42 \
  -H 'Content-Type: application/json' \
  -d '{"name":"tea-v2","price_cents":299,"expected_version":1}'
curl -i http://127.0.0.1:8080/products/42
curl -i 'http://127.0.0.1:8080/products?ids=42,999,42'
curl -s http://127.0.0.1:8080/metrics
```

正常情况下，三次单项 GET 的 X-Cache 依次是 MISS、HIT、MISS，最后一次返回版本 2。
单项或批量加 fresh=1 可绕过缓存读取，核对 MySQL 权威值。查看商品 key 对应的缓存节点：

```bash
./build/sphinxd/sphinx-cluster \
  --nodes 127.0.0.1:11211,127.0.0.1:11212 route product:v3:42
```

节点列表是静态配置，没有缓存副本、自动故障转移或在线迁移。节点故障时商品服务会尝试
回源，但仍受数据库可用性与读容量限制。提交后删除和 TTL 不提供缓存强一致保证。

## 严格验证

准备名称包含 test 的独立测试数据库，配置 SPHINX_TEST_MYSQL_HOST、SPHINX_TEST_MYSQL_PORT、
SPHINX_TEST_MYSQL_USER、SPHINX_TEST_MYSQL_PASSWORD、SPHINX_TEST_MYSQL_DATABASE 后执行：

```bash
./scripts/verify_mysql_sphinx.sh build
```

脚本连接真实 MySQL 并启动临时缓存节点与商品服务；缺少凭据会报错，跳过测试不能算成功。
测试会建表和修改表结构或数据，请使用隔离数据库。

完整缓存对照验证入口为 `scripts/verify_product_cache.sh`，需安装 redis-server；
可重复实验入口为 `scripts/compare_product_cache.py`，结果输出到构建目录。

## 源码入口

- [集群客户端与一致性哈希路由](sphinxd/src/cluster_client.cpp)
- [网络服务与 Worker 分发](sphinxd/src/server/server.cpp)
- [连接与回包保序](sphinxd/src/server/connection.cpp)
- [Log、Index 与 segment 回收](sphinxd/src/logmem.cpp)
- [商品缓存旁路与版本更新](product-service/src/product_service.cpp)
- [编码与命名规范](docs/CODING_STANDARDS.md)

## License

Apache-2.0
