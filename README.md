# Sphinx：MySQL + 分片缓存商品服务

这是一个 C++17 后端项目 Demo：MySQL 保存商品的权威数据，HTTP 商品服务采用缓存旁路模式，Sphinx 节点保存可过期、可重建的查询结果。商品服务按一致性哈希选择缓存节点；每个节点再按 key 将请求分发给独占存储的 Worker。

```text
HTTP 客户端 → sphinx-product-service ──→ MySQL（权威数据）
                         │
                         └─ ClusterClient（一致性哈希选节点）
                              ├─ sphinxd :11211 ─ Worker 0..N
                              └─ sphinxd :11212 ─ Worker 0..N
```

`GET /products/{id}` 先查缓存，未命中或缓存故障时查询 MySQL，并尽力回填；`PUT /products/{id}` 在 MySQL 中按 `expected_version` 检查并提交更新，然后尽力删除缓存。缓存命中、未命中和旁路原因会写入 `X-Cache` 响应头。另有 Redis 缓存对照实现，见[架构说明](docs/ARCHITECTURE.md)。

## 构建

项目运行于 Linux。`scripts/install_dependencies.sh` 支持 Ubuntu、Debian 和 Fedora；默认构建包含 MySQL 商品服务，构建时需要 `libmysqlclient` 和 `hiredis` 开发包。CMake 会获取 `cpp-httplib` 和 `nlohmann/json` 的头文件。

```bash
./scripts/install_dependencies.sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

没有配置测试数据库时，普通 `ctest` 会跳过 MySQL 与 HTTP 集成用例；“通过”不代表真实数据库链路已经验收。

## 跑通一个商品

先准备 MySQL 8.4 数据库和有读写权限的用户。在之后要运行商品服务的终端设置连接配置、建表并插入一条商品：

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

在另外两个终端分别启动缓存节点；然后回到已设置 MySQL 环境变量的终端启动商品服务。以下命令从仓库根目录执行：

```bash
# 缓存节点终端 1
./build/sphinxd/sphinxd --listen 127.0.0.1 --port 11211 --threads 2
```

```bash
# 缓存节点终端 2
./build/sphinxd/sphinxd --listen 127.0.0.1 --port 11212 --threads 2
```

```bash
# 商品服务终端；沿用本终端上面设置的 SPHINX_MYSQL_* 环境变量
export SPHINX_CACHE_NODES=127.0.0.1:11211,127.0.0.1:11212
./build/product-service/sphinx-product-service
```

在第四个终端依次请求：

```bash
curl -i http://127.0.0.1:8080/products/42
curl -i http://127.0.0.1:8080/products/42
curl -i -X PUT http://127.0.0.1:8080/products/42 \
  -H 'Content-Type: application/json' \
  -d '{"name":"tea-v2","price_cents":299,"expected_version":1}'
curl -i http://127.0.0.1:8080/products/42
```

正常情况下，三次 GET 的 `X-Cache` 依次是 `MISS`、`HIT`、`MISS`；最后一次返回版本 2。`GET /products/42?fresh=1` 可绕过缓存核对 MySQL 中的值。可用下面的命令查看商品缓存 key 会路由到哪一个节点：

```bash
./build/sphinxd/sphinx-cluster \
  --nodes 127.0.0.1:11211,127.0.0.1:11212 route product:v3:42
```

节点列表是静态配置；本项目没有副本、自动故障转移或数据迁移。MySQL 是权威数据，缓存节点不可用时商品查询会尝试回源。

## 严格集成验收

准备名称包含 `test` 的独立临时数据库，并配置 `SPHINX_TEST_MYSQL_HOST`、`SPHINX_TEST_MYSQL_PORT`、`SPHINX_TEST_MYSQL_USER`、`SPHINX_TEST_MYSQL_PASSWORD`、`SPHINX_TEST_MYSQL_DATABASE` 后执行：

```bash
./scripts/verify_mysql_sphinx.sh build
```

脚本缺少数据库凭据时会报错，不会把跳过测试当作验收成功。

## 文档

- [Redis 对照架构](docs/ARCHITECTURE.md)
- [代码与命名规范](docs/CODING_STANDARDS.md)

## License

Apache-2.0
