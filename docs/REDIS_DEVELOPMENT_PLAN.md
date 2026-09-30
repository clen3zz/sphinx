# MySQL + Redis 商品缓存开发计划

本文是待实施的开发规格。目标是在现有 MySQL + Sphinx 商品服务中增加 Redis 对照路径，
通过真实商品请求学习 Redis，同时保持代码可读、架构简单。本文中的类型、接口和测试均为
计划；已有接口会明确标注。编码依据是仓库的 [CODING_STANDARDS.md](CODING_STANDARDS.md)、
[.clang-format](../.clang-format) 和 [.clang-tidy](../.clang-tidy)。

## 1. 目标、范围和已确定的选择

MySQL 保存商品事实；缓存减少重复读取 MySQL 的成本。两种缓存后端共用业务服务、JSON 编码、
失效规则和保护策略，便于比较后端自身的性能、连接行为、过期和内存回收机制。

```text
HTTP 请求
  → 当前 HTTP Worker 的 ProductService
    → ProductCache：SphinxProductCache 或 RedisProductCache
    → 缓存未命中或不可用：当前 Worker 的 MySqlProductStore
    → 尽力回填缓存

PUT：MySQL 事务明确提交成功 → 尽力删除所选后端中的商品缓存
```

| 决策 | 本次方案 |
| --- | --- |
| 缓存后端 | `sphinx`、`redis`；默认 `sphinx`，不增加纯 MySQL 后端 |
| Redis 部署 | 单实例，使用同步 hiredis 客户端 |
| 学习范围 | 商品缓存、连接与协议、批量命令、过期、淘汰、故障与观测 |
| 默认策略 | `basic`；`protected` 显式启用读保护 |
| 缓存失效 | 保留提交后尽力删除和 TTL 收敛 |
| 热点回源 | 同一商品服务进程内合并，不使用 Redis 分布式锁 |
| 批量查询 | 每项返回结果，保持输入顺序和重复 ID |
| 暂不开发 | Outbox、删除重试队列、Sentinel、Cluster、Lua、排行榜、限流、消息业务 |

正常的缓存故障回源仍访问 MySQL；这属于两条缓存路径的行为，不是新增运行模式。
不为学习某个命令强行添加没有商品业务需求的类。

### 1.1 当前实现必须保留的行为

- 单商品 GET 和 PUT 的 JSON 格式、版本检查和 HTTP 状态语义保持兼容。
- `fresh=1` 跳过缓存读取，但成功读取 MySQL 后仍尽力回填。
- 数据库明确没有记录才返回 `NotFound`；数据库错误不能写成不存在的负缓存。
- `CommitUnknown` 不自动重试事务、不删除缓存，保留现有错误映射。
- 缓存读取失败不直接使商品请求失败；MySQL 结果决定商品响应。
- 缓存回填失败不改变已成功读取的商品结果；删除失败不撤销已提交的更新。

### 1.2 一致性边界

仍允许以下顺序：GET 从 MySQL 读到 v1 → PUT 提交 v2 并删除缓存 → 旧 GET 回填 v1。
旧值从实际回填时开始计时，直到自身 TTL 到期；不能声称“提交后最多 30 秒就一定新鲜”。
进程内合并只减少重复回源，不修复这类读写竞争，也不合并多个服务进程的请求。

`protected` 中正缓存默认 TTL 为 27～33 秒，负缓存默认 5 秒。持续写入、删除失败和持续旧读
回填时，不能仅凭一个 TTL 推导全局的新鲜度上界。商品 `version` 用于数据校验和更新冲突，
本次不把它扩展为缓存写入栅栏。

## 2. Redis 与 Sphinx 的比较边界

| 维度 | 现有 Sphinx 路径及本次扩展 | 新 Redis 路径 |
| --- | --- | --- |
| 客户端 | `ClusterClient`，Memcached 文本协议 | hiredis，RESP |
| 路由 | 客户端一致性哈希，节点内再分到 Worker | 一个 Redis 地址，不做分片路由 |
| 连接 | HTTP Worker 独占客户端，按缓存节点复用连接 | HTTP Worker 独占一个惰性连接 |
| 单项操作 | `get`、`set`、`delete` | `GET`、`SET ... EX`、`DEL` |
| 批量读取 | 新增按节点分组的原生 multi-get，节点组依次执行 | 一个 `MGET` |
| 批量回填 | 复用已有 `set`，逐条执行 | 有界 pipeline 发送多个 `SET ... EX` |
| 过期 | 现有 TTL、惰性检查及索引清理 | Redis 的 TTL 与过期机制 |
| 内存回收 | 现有 Log segment 回收，不等于 LRU | `maxmemory` 与淘汰策略实验 |
| 复制和故障转移 | 本项目未实现 | 本方案也不配置 |
| 业务保护 | 两个后端共用 `ProductService` 的策略 | 同左 |

当前 `ClusterClient` 只有单项 `get`，虽然 Sphinx 服务端已支持多 key `get`，仍需补客户端
跨节点批量归并。不要将已有服务端能力当作商品服务已经具有的批量能力。

比较时分别报告单项读取、批量读取和批量回填。Sphinx 的逐条回填与 Redis pipeline 的往返
次数不同，这是本次适配器的实际差异；不能把这部分收益全归因于缓存存储引擎。

## 3. 运行模式、配置和请求契约

### 3.1 `basic` 与 `protected`

| 行为 | `basic` | `protected` |
| --- | --- | --- |
| 缓存读取、故障回源、尽力回填、提交后删除 | 开启 | 开启 |
| 批量接口和原生批量读取 | 开启 | 开启 |
| 正缓存 TTL | 配置值，默认 30 秒 | 配置值加有界抖动 |
| 不存在商品的负缓存 | 关闭 | 默认 5 秒 |
| 同进程同 ID 回源合并 | 关闭 | 开启 |
| MySQL 读并发准入 | 关闭 | 开启 |
| 缓存读取与回填熔断 | 关闭 | 开启 |
| 公共指标 | 开启 | 开启 |

保护功能通过一个模式选择，避免多个布尔开关产生难以验证的组合。`basic` 接触到合法负缓存
标记时把它作为未命中回源，不使用该标记直接回答不存在。

### 3.2 配置默认值

所有环境变量在启动阶段读取；Worker 只使用已校验的配置副本。默认值如下。

| 环境变量 | 默认值 | 校验或用途 |
| --- | --- | --- |
| `SPHINX_CACHE_BACKEND` | `sphinx` | 仅接受 `sphinx`、`redis` |
| `SPHINX_CACHE_POLICY` | `basic` | 仅接受 `basic`、`protected` |
| `SPHINX_CACHE_NODES` | `127.0.0.1:11211` | 保留，只在 Sphinx 模式解析 |
| `SPHINX_CACHE_TIMEOUT_MS` | `200` | 正数，最大 10000；新开放环境变量 |
| `SPHINX_CACHE_TTL_SECONDS` | `30` | 保留，范围 1～2592000 |
| `SPHINX_REDIS_HOST` | `127.0.0.1` | 非空，只在 Redis 模式解析 |
| `SPHINX_REDIS_PORT` | `6379` | 1～65535 |
| `SPHINX_REDIS_DATABASE` | `0` | 0～15；实例未配置该 DB 时连接初始化失败 |
| `SPHINX_REDIS_USERNAME` | 空 | 非空用户名要求非空密码 |
| `SPHINX_REDIS_PASSWORD` | 空 | 可选；禁止输出到日志和指标 |
| `SPHINX_NEGATIVE_TTL_SECONDS` | `5` | protected 使用，1～30 |
| `SPHINX_TTL_JITTER_SECONDS` | `3` | protected 使用，0～30 |
| `SPHINX_READ_MAX_INFLIGHT_KEYS` | `1024` | protected 使用，1～65536 |
| `SPHINX_READ_MAX_CONCURRENT_LOADS` | `min(2, worker_count)` | protected 使用，1～HTTP Worker 数 |
| `SPHINX_READ_WAIT_TIMEOUT_MS` | `500` | protected 使用，1～10000 |
| `SPHINX_CACHE_FAILURE_THRESHOLD` | `3` | protected 使用，1～100 |
| `SPHINX_CACHE_OPEN_INTERVAL_MS` | `2000` | protected 使用，1～60000 |

保留现有 HTTP、MySQL 环境变量。`basic` 不读取保护参数；未选择的缓存后端不读取其专有参数。
这样不会因为无关配置阻止另一条路径启动。有效配置打印后端、模式和非敏感边界即可。

Sphinx timeout 是现有客户端的单节点操作期限。Redis 将相同配置分别用于连接超时和阻塞
I/O 超时；同步 hiredis 不提供整个 HTTP 请求或整个 pipeline 的统一 200 ms 截止时间，
不要把二者宣传为相同的端到端超时保证。

### 3.3 HTTP 接口

保留 `GET /products/{id}`、`PUT /products/{id}`，新增：

```text
GET /products?ids=42,999,42
GET /products?ids=42,999,42&fresh=1
GET /metrics
```

批量 IDs 的上限为 32，重复 ID 也占输入名额。仅接受一个 `ids` 参数；每项必须是正十进制
`uint64_t`。空列表、空 token、空白、符号、溢出和超过上限均在访问依赖前返回 400。
`fresh` 的合法性沿用单项 GET；多个 `fresh` 参数拒绝。内部去重，输出恢复原顺序。

批量只接受 `ids` 和可选 `fresh=1`，两者顺序不限；其他参数拒绝。解析原始 query，保持
单项接口对编码/额外参数的严格行为，不悄悄把 URL 解码后的空白或编码数字当合法 ID。
单项现有 parse_fresh_query 保持不变，批量另用自己的参数解析器。

合法批量请求的外层 HTTP 状态为 200，每项包含 `id` 和 `product` 或 `error`：

```json
{
  "items": [
    {"id": 42, "product": {"id": 42, "name": "键盘", "price_cents": 9900, "version": 1}},
    {"id": 999, "error": "not_found"},
    {"id": 42, "product": {"id": 42, "name": "键盘", "price_cents": 9900, "version": 1}}
  ]
}
```

某部分回源失败时保留已命中的成功项。`GetProductsResult.status == Ok` 表示批量结构可用，
不表示所有商品成功。无效请求外层 400；无法形成批量结果的初始化或异常沿用现有 503/500
处理。新增 `ProductStatus::ReadBusy`：单项映射 503、`error=read_busy`、`Retry-After: 1`；
批量则只在受影响项写 `read_busy`。容量拒绝和等待超时不伪装成不存在。

沿用 `Cache-Control: no-store` 和 JSON Content-Type。单项 `X-Cache` 保留；负缓存命中
使用 `HIT`。批量全部来源相同时用对应值，来源不同时用 `MIXED`。
PUT 删除失败仍通过现有 `X-Cache-Invalidation: failed` 表达。

## 4. 文件组织和依赖

下面列出新文件；已有文件的修改在各组件章节中说明。

| 新文件，相对仓库根目录 | 负责内容 |
| --- | --- |
| `product-service/include/sphinx/product_cache_options.h` | 两种后端和客户端配置、创建函数 |
| `product-service/src/product_cache_options.cpp` | 枚举解析、配置校验、适配器创建 |
| `product-service/src/product_cache.cpp` | 缓存默认批量实现和统一输入校验 |
| `product-service/src/product_store.cpp` | Store 默认批量实现 |
| `product-service/include/sphinx/redis_product_cache.h` | Redis 适配器声明 |
| `product-service/src/redis_product_cache.cpp` | hiredis RAII、连接、命令、pipeline |
| `product-service/include/sphinx/product_read_coordinator.h` | 回源合并和读准入 |
| `product-service/src/product_read_coordinator.cpp` | flight、票据、许可的并发实现 |
| `product-service/include/sphinx/cache_circuit_breaker.h` | 熔断状态和操作许可 |
| `product-service/src/cache_circuit_breaker.cpp` | 熔断状态机 |
| `product-service/include/sphinx/product_metrics.h` | 指标、快照、计时器 |
| `product-service/src/product_metrics.cpp` | 原子计数、固定桶统计 |
| `product-service/include/sphinx/product_shared_state.h` | 进程级共享状态的组合 |
| `product-service/test/redis_product_cache_test.cpp` | Redis 协议及连接行为 |
| `product-service/test/product_read_coordinator_test.cpp` | 合并、准入、清理、等待 |
| `product-service/test/cache_circuit_breaker_test.cpp` | 确定性状态机测试 |
| `product-service/test/product_metrics_test.cpp` | 计数与分桶规则 |
| `product-service/test/redis_cache_integration_test.py` | 真实 Redis 命令与过期验收 |
| `scripts/verify_product_cache.sh` | 两种后端、两种策略的严格集成验收 |
| `scripts/compare_product_cache.py` | HTTP 端到端对照实验与结果输出 |
| `docs/REDIS_LEARNING_ROUTE.md` | 按实现阶段安排学习与观察 |

构建增加 `pkg_check_modules(HIREDIS REQUIRED IMPORTED_TARGET hiredis>=1.0.0)`。
在 `sphinx_product_core` 中加入新缓存、协调器、熔断和指标实现，链接
`PkgConfig::HIREDIS`；静态库最终使用者需要获得正确的链接依赖。
hiredis 头文件仅出现在适配器 `.cpp`，公共头文件不传播 C 客户端细节。

构建需要 hiredis 开发包，运行 Redis 路径和真实 Redis 测试才需要 Redis 服务。
依赖安装脚本补 Ubuntu/Debian 的 `libhiredis-dev`、Fedora 的 `hiredis-devel`；
学习环境另行记录 `redis-server`、`redis-tools` 的安装和版本检查。
若包版本不足，明确报告依赖版本问题，不降低要求或偷偷替换客户端。

头文件只包含自身需要的依赖。`product_codec.h` 前置声明 `ProductCachePolicy`，实现文件再
包含其定义，避免 codec 与 service 循环包含。协调器头文件先声明 coordinator/flight，再
定义票据和许可，最后定义 coordinator；含不完整类型的析构在 `.cpp` 中定义。

保留现有 C++17、CMake 最低版本、GTest 和 Python 检测方式。严格验收脚本必须检查所需测试
目标真实存在，不能把“没有安装测试依赖，因此没生成测试”当作成功。

## 5. 公共数据结构和编解码

### 5.1 配置数据

这些数据载体直接定义在 `namespace sphinx`，不增加嵌套模块命名空间。

```cpp
enum class CacheBackend : std::uint8_t { Sphinx, Redis };
enum class CachePolicyMode : std::uint8_t { Basic, Protected };

struct SphinxCacheOptions {
  std::string nodes = "127.0.0.1:11211";
  std::chrono::milliseconds timeout{200};
};

struct RedisOptions {
  std::string host = "127.0.0.1";
  std::uint16_t port = 6379;
  std::uint32_t database = 0;
  std::string username;
  std::string password;
  std::chrono::milliseconds connect_timeout{200};
  std::chrono::milliseconds io_timeout{200};
};

struct ProductCacheOptions {
  CacheBackend backend = CacheBackend::Sphinx;
  SphinxCacheOptions sphinx;
  RedisOptions redis;
};

struct ProductCachePolicy {
  CachePolicyMode mode = CachePolicyMode::Basic;
  std::uint32_t ttl_seconds = 30;
  std::uint32_t negative_ttl_seconds = 5;
  std::uint32_t ttl_jitter_seconds = 3;
};

struct ProductReadOptions {
  std::size_t max_inflight_keys = 1024;
  std::size_t max_concurrent_loads = 2;
  std::chrono::milliseconds wait_timeout{500};
};

struct CacheBreakerOptions {
  std::uint32_t failure_threshold = 3;
  std::chrono::milliseconds open_interval{2000};
};
```

`ProductCachePolicy` 扩展现有类型，定义保留在 `product_service.h`。
读参数和熔断参数分别放各自头文件。HTTP 配置校验负责默认读并发数与 Worker 数的关系。

配置辅助函数固定如下，不增加 `CacheFactory` 类：

```cpp
CacheBackend parse_cache_backend(std::string_view text);
CachePolicyMode parse_cache_policy_mode(std::string_view text);
void validate_product_cache_options(const ProductCacheOptions& options);
std::unique_ptr<ProductCache> make_product_cache(const ProductCacheOptions& options);
```

解析或校验失败抛 `std::invalid_argument`，错误不包含凭据。创建函数只创建所选适配器；
Redis 网络连接保持惰性，不在创建对象时执行网络 I/O。

### 5.2 缓存、回源和批量结果数据

```cpp
struct CacheWriteEntry {
  std::string key;
  std::string value;
  std::uint32_t ttl_seconds = 0;
};

enum class CacheEntryKind : std::uint8_t { Product, NotFound, Corrupt };

struct DecodedProductCacheEntry {
  CacheEntryKind kind = CacheEntryKind::Corrupt;
  std::optional<Product> product;
};

struct ProductLoadResult {
  ProductStatus status = ProductStatus::InternalError;
  std::optional<Product> product;
};

struct BatchProductItem {
  std::uint64_t id = 0;
  GetProductResult result;
};

struct GetProductsResult {
  ProductStatus status = ProductStatus::InternalError;
  std::vector<BatchProductItem> items;
};
```

`CacheWriteEntry` 放 `product_cache.h`，包含拥有所有权的字符串，避免 pipeline 使用已失效的
`string_view`。解码类型放 `product_codec.h`；回源和批量结果放 `product.h`。
`ProductLoadResult` 不含 `CacheSource`，因为同一回源结果可能被不同读取来源的请求共享。

在 `product.h` 新增 `inline constexpr std::size_t max_product_batch_size = 32;`，商品服务、
两种商品缓存适配器和 MySQL 批量接口共用它，不把同一业务上限散写成多个魔法数字。

所有成功商品结果必须有有效 `product`；不存在及错误结果必须没有商品。
不存在的缓存 key 用 `nullopt`，合法负缓存用 `NotFound`，损坏字节用 `Corrupt`，三者不混用。

### 5.3 Key、正缓存、负缓存

现有 key 是 `product:v2:{id}`。为了声明新增负缓存格式，统一切换为 `product:v3:{id}`；
两种后端和策略都使用相同版本的 key。正缓存继续使用现有四字段 JSON：

```json
{"id":42,"name":"键盘","price_cents":9900,"version":1}
```

负缓存仅接受以下两个字段，`id` 必须匹配查询，`not_found` 必须严格为 `true`：

```json
{"id":999,"not_found":true}
```

在现有 codec 中保留 `encode_product_cache`、`decode_product_cache`，新增：

```cpp
std::string encode_product_not_found(std::uint64_t id);
DecodedProductCacheEntry decode_product_cache_entry(std::string_view payload,
                                                    std::uint64_t expected_id);
std::uint32_t product_cache_ttl(std::uint64_t id, const ProductCachePolicy& policy) noexcept;
```

`decode_product_cache_entry` 复用现有正商品校验，包括 JSON 长度上限、字段数、UTF-8、价格、
正 ID 和版本号。多字段、字段类型错误、ID 不符、负缓存含商品字段全部视为损坏。
`product_cache_ttl` 在 basic 返回原 TTL；protected 用固定 ID 哈希产生 `[-jitter,+jitter]`
的偏移，用足够宽的整数计算并裁剪到 1～2592000。使用稳定哈希而非实现不保证稳定的
`std::hash` 或全局随机数：采用固定的 uint64_t 混合运算，不分配字符串、不调用 rand，
保证该 noexcept 计算不会因字符串分配失败终止进程。它分散不同 key 的到期时间，同一 ID
仍依赖回源合并应对热点。
负缓存 TTL 不叠加该抖动。

升级时 v2 key 自行过期，不扫描或批量删除。滚动运行旧服务时，它仍可能使用自己的 v2 缓存
直到 TTL；不能承诺新服务 PUT 会使旧命名空间立即失效。对照实验按相同版本顺序运行。

## 6. 缓存接口和两个适配器

### 6.1 `ProductCache`：只统一缓存语义

保留现有三个纯虚接口，增加可覆写的批量接口。默认批量实现顺序调用单项接口，便于已有
Fake 和简单调用方迁移；两个正式适配器必须按下面的规格覆写读取。

```cpp
class ProductCache {
 public:
  virtual ~ProductCache() = default;
  virtual std::optional<std::string> get(std::string_view key) = 0;
  virtual void put(std::string_view key, std::string_view value, std::uint32_t ttl_seconds) = 0;
  virtual void erase(std::string_view key) = 0;
  virtual std::vector<std::optional<std::string>> get_many(const std::vector<std::string>& keys);
  virtual void put_many(const std::vector<CacheWriteEntry>& entries);
};
```

成员数据：无。实例属于一个 HTTP Worker，接口不承担跨线程同步。空批量返回空结果或直接
结束，不访问网络；非空批量最大 32 项，所有参数必须在发送第一条命令前校验。
`get_many` 输出长度和顺序必须与输入一致，重复 key 重复输出。单项缺失是 `nullopt`。
缓存操作失败抛 `CacheError`；批量读取异常不返回一个无法判定完整性的结果。
`put_many` 没有原子性保证，失败时部分 key 可能已写入，调用者不得整体重试。

默认批量方法的定义放 `product_cache.cpp`。新增自由函数
`bool valid_product_cache_key(std::string_view key) noexcept`，校验非空、最多 250 字节、
无空白和控制字符；这是两条商品路径统一约定的文本 key 域。
value 仍是二进制安全的拥有长度字节串。单项和批量 TTL 都使用已有 TTL 校验函数。

### 6.2 `RedisProductCache`：同步、线程独占、二进制安全

```cpp
class RedisProductCache final : public ProductCache {
 public:
  explicit RedisProductCache(RedisOptions options);
  ~RedisProductCache() override;
  RedisProductCache(const RedisProductCache&) = delete;
  RedisProductCache& operator=(const RedisProductCache&) = delete;
  RedisProductCache(RedisProductCache&&) = delete;
  RedisProductCache& operator=(RedisProductCache&&) = delete;

  std::optional<std::string> get(std::string_view key) override;
  void put(std::string_view key, std::string_view value, std::uint32_t ttl_seconds) override;
  void erase(std::string_view key) override;
  std::vector<std::optional<std::string>> get_many(const std::vector<std::string>& keys) override;
  void put_many(const std::vector<CacheWriteEntry>& entries) override;

 private:
  struct Impl;
  std::unique_ptr<Impl> _impl;
};
```

外层只做委托，C 句柄和协议解析都收在 `.cpp` 的 `Impl` 内。`Impl` 的成员数据和内部接口：

| 成员或接口 | 类型或签名 | 责任 |
| --- | --- | --- |
| `_options` | `RedisOptions` | 已校验的连接配置 |
| `_owner_thread` | `std::thread::id` | 构造线程；每次操作检查 |
| `_context` | `RedisContextPtr` | `unique_ptr<redisContext, RedisContextDeleter>` |
| 构造 | `explicit Impl(RedisOptions options)` | 保存配置及线程，不立即连接 |
| `check_owner` | `void check_owner() const` | 错误线程使用抛 `CacheError` |
| `ensure_connection` | `void ensure_connection()` | 连接、设置 timeout、认证、选择 DB |
| `reset_connection` | `void reset_connection() noexcept` | 丢弃连接和未消费缓冲 |
| `command` | `RedisReplyPtr command(const std::vector<std::string_view>& args)` | argv + 长度调用 |
| `append_command` | `void append_command(const std::vector<std::string_view>& args)` | pipeline 追加 |
| `read_reply` | `RedisReplyPtr read_reply()` | 消费一条回复，保管其内存 |
| `parse_value` | `std::optional<std::string> parse_value(const redisReply& reply) const` | 只接受 String/Nil |
| `expect_ok` | `void expect_ok(const redisReply& reply) const` | 严格验证 `OK` |
| `expect_deleted` | `void expect_deleted(const redisReply& reply) const` | 单 key DEL 只接受 0/1 |

`Impl` 使用 class 风格的 private 字段；作为外层私有类型不导出。
匿名命名空间内只增加两个 RAII 删除器：
`RedisContextDeleter::operator()(redisContext*) const noexcept` 调 `redisFree`，
`RedisReplyDeleter::operator()(redisReply*) const noexcept` 调 `freeReplyObject`；
类型别名分别为 `RedisContextPtr`、`RedisReplyPtr`。

操作与连接规则：

1. 使用 `redisConnectWithTimeout` 和 `redisSetTimeout`；初始化失败也释放 context。
   有用户名时执行双参数 AUTH，仅密码时执行单参数 AUTH；非零 DB 执行 SELECT。
2. GET 接受 String/Nil；SET 使用 `EX`，成功回复必须为 `OK`；DEL 的 0 是幂等成功。
3. 使用 `redisCommandArgv`、`redisAppendCommandArgv` 和显式参数长度，包含 `\0` 的 value
   也要完整往返。禁止拼接命令字符串或使用用户数据作为格式串。
   这一选择依据 [hiredis 官方接口说明](https://github.com/redis/hiredis)。
4. `get_many` 使用 MGET，验证 Array、元素数量和每个元素类型；回复按输入位置归并。
   Redis 对缺失 key 和非 String key 都返回 Nil，本适配器按未命中处理并回源。
   依据 [MGET 文档](https://redis.io/docs/latest/commands/mget/)。
5. `put_many` 最多追加 32 个 SET，再按相同数量取回复。参数和 TTL 先全量校验。
   已收到普通服务器 Error 时继续消费本批剩余回复，再抛一次 `CacheError`；协议或传输失败
   立即丢弃连接。pipeline 减少往返但不保证原子性，不能承诺失败时都没有写入。
   依据 [Redis pipeline 文档](https://redis.io/docs/latest/develop/using-commands/pipelining/)。
6. 任一无法确认连接协议位置的失败都 reset；不在当前操作内重新发命令。
   下一个独立请求才尝试重连。错误信息只带操作和错误类别，不带密码或完整 value。

不新增连接池、异步事件循环、通用 Redis 命令框架或自写 RESP 解析器。
正式商品访问固定使用上述命令；测试可借助独立测试进程观察原始命令。

### 6.3 `SphinxProductCache` 和 `ClusterClient`

`SphinxProductCache` 保留 `_client: ClusterClient` 及现有构造、get、put、erase；新增覆写：

```cpp
std::vector<std::optional<std::string>> get_many(const std::vector<std::string>& keys) override;
void put_many(const std::vector<CacheWriteEntry>& entries) override;
```

`get_many` 委托 `ClusterClient::get_many`，`ClientError` 转成 `CacheError`；`put_many`
校验所有输入后逐项 set。无需为 Sphinx 重写一套 pipeline。

在 `sphinxd/include/sphinx/cluster_client.h` 和对应 `.cpp` 扩展 `ClusterClient`：

- 新公共接口：`std::vector<std::optional<std::string>> get_many(const std::vector<std::string>& keys)`。
- 保留成员 `_ring`、`_timeout`、`_connections`，连接仍由当前线程独占。
- 新私有辅助：`group_get_keys(const std::vector<std::string>& keys) const`，返回
  `std::vector<NodeGetBatch>`；分组用 `route`，顺序采用节点第一次出现的顺序。
- `NodeGetBatch` 是类的私有数据结构：`Node node`、`vector<string> keys`、
  `vector<vector<size_t>> input_positions`，每个去重 key 对应所有原始位置。
- 使用临时节点索引 map 和 key 索引 map 去重，不把批次状态存成长期成员。
- 逐节点发送 multi-get，恢复原顺序；任何节点失败都清除该节点连接并抛 `ClientError`。
  本次缓存读整体回源，其他请求中已经完成的缓存命中不受影响。

内部 `MemcachedConnection` 保留 `_transport: TcpTransport`，新增
`get_many(const std::vector<std::string>& keys)`；现有 `get` 可以调用一个 key 的批量实现。
协议解析循环读 `VALUE`，最后必须读到 `END`；校验返回 key 属于请求、无重复 VALUE、
flags/长度合法、value 后 CRLF 完整。按长度读取，保留二进制字节，不依赖 VALUE 返回顺序。
调整现有 VALUE header 解析辅助函数，返回拥有 key 的 `ValueHeader`：
`string key`、`size_t value_size`。不要只按第一项的 expected key 验证整批回复。

保持现有节点路由、Worker 分片和服务端 Reactor，无需改索引或 Log 数据结构。

## 7. MySQL 批量读取

### 7.1 `ProductStore`

保留 find/update；新增带默认逐项实现的虚接口：

```cpp
virtual std::vector<std::optional<Product>> find_many(const std::vector<std::uint64_t>& ids);
```

成员数据：无。输出与输入长度及顺序一致；不存在用 `nullopt`；任何 SQL/连接/数据错误抛
`StoreError`。空批量不访问数据库，非空不超过 32，禁止零 ID。正式 MySQL 适配器覆写它，
用一条数据 SELECT 读取本次尚未解决的 ID，不能声称默认循环也减少了 SQL 往返。

默认方法定义放 `product_store.cpp`；开始循环前校验整批 ID 和上限，确保后部参数非法时
不会先查询前面的商品。MySQL 正式覆写采用相同输入契约。

### 7.2 `MySqlProductStore` 与 `Impl`

外层增加 `find_many` override；保留 `_impl`、find、update、构造、析构和不可复制约束。
`Impl` 保留现有连接、owner 检查、事务状态及单项/锁行/更新语句；新增：

| 成员或接口 | 类型或签名 | 责任 |
| --- | --- | --- |
| `find_many_statement` | `MYSQL_STMT*`，初始 nullptr | 与现有 Impl 公共数据成员命名一致 |
| `find_many_parameter_count` | `std::size_t`，初始 0 | 当前缓存语句的 IN 参数个数 |
| `prepare_find_many_statement` | `MYSQL_STMT* prepare_find_many_statement(std::size_t count)` | 参数数改变时重建这一条语句 |
| `select_products` | `vector<optional<Product>> select_products(const vector<uint64_t>& ids)` | 绑定、执行、逐行读取、恢复顺序 |
| `reset_connection` | 扩展已有清理接口 | 关闭批量语句并将计数归零 |

SQL 固定字段 `id,name,price_cents,version`，只根据 count 生成 `IN (?,...,?)` 的占位符；
商品 ID 通过 `MYSQL_BIND` 绑定。最多缓存一条批量语句，避免为不同列表维护无界 map。
输入可去重后查询，结果用临时 `unordered_map<uint64_t, Product>` 按 ID 归并再恢复位置。
返回非请求 ID、重复行、截断、NULL、无效 UTF-8、价格/版本非法均为 `InvalidData`。

为复用现有行读取校验，在 `.cpp` 匿名命名空间抽取 `ProductRowBuffer` 数据载体：
`uint64_t id/price_cents/version`、`array<char, max_product_name_bytes + 1> name`、
`array<unsigned long, 4> lengths`、MySQL 兼容布尔类型的 `null_flags/error_flags`、
`array<MYSQL_BIND, 4> bindings`。`bindings` 指向该对象自己的稳定地址，禁止复制/移动；
构造初始化绑定，`reset_flags()` 清理每行状态，`to_product() const` 验证并形成拥有字符串的商品。
保持与既有 MySQL 客户端头文件兼容，不假定所有发行版都用相同的 `my_bool` 定义。

执行后包括异常路径都释放结果并 reset statement；无法确认连接可继续使用时丢弃连接。
仍然不在当前数据库操作内自动重试。批量只改读取，不改写事务和提交不确定性处理。

### 7.3 MySQL 运行时守卫

`MySqlRuntime` 和 `MySqlThreadGuard` 不新增接口或数据成员；沿用现有构造、析构、不可复制
约束。进程运行时先于工作线程创建；每个线程的 guard 先于 MySQL 连接创建、晚于连接销毁。

## 8. 进程内回源合并与读准入

这些类只协调 ID、结果和读名额，不持有 Redis、Sphinx 或 MySQL 客户端。
每个 HTTP Worker 仍用自己的连接执行自己的 Leader 任务。所有这些协调对象不可复制。

### 8.1 `ProductReadFlight`：一次 ID 回源的共享状态

在协调器头文件中前置声明，定义放 `.cpp`，不作为业务调用接口。
它是公共字段的内部数据载体，成员为：

```cpp
struct ProductReadFlight {
  std::mutex mutex;
  std::condition_variable ready;
  std::optional<ProductLoadResult> result;
};
```

`result.has_value()` 就是完成标志，不额外维护容易不一致的 `done`。
只有 Leader 发布结果；Follower 只等待和复制结果，不取消 Leader。
完成结果包括成功、明确不存在、读繁忙和内部错误。

### 8.2 `ProductReadCoordinator`：拥有有界 flight 表与读计数

| 接口 | 签名 | 契约 |
| --- | --- | --- |
| 构造 | `explicit ProductReadCoordinator(ProductReadOptions options)` | 校验正容量和等待配置 |
| 领取 | `vector<ProductReadTicket> acquire_many(const vector<uint64_t>& ids)` | 输入已去重；一次加锁认领整批 |
| 读准入 | `optional<ProductLoadPermit> try_acquire_load()` | 立即得到一个名额或失败，不排第二层队列 |
| 活跃 key | `size_t active_key_count() const` | 加锁快照 |
| 活跃读 | `size_t active_load_count() const` | 加锁快照 |
| 发布，private | `void complete(uint64_t id, const shared_ptr<ProductReadFlight>& flight, ProductLoadResult result) noexcept` | 发布并移除仍指向该 flight 的 map 项 |
| 放弃，private | `void abandon(uint64_t id, const shared_ptr<ProductReadFlight>& flight) noexcept` | 发布 InternalError 并清理 |
| 释放名额，private | `void release_load() noexcept` | 归还一个活跃读计数 |

成员数据：

- `_options: ProductReadOptions`。
- `_mutex: mutable std::mutex`。
- `_flights: std::unordered_map<std::uint64_t, std::shared_ptr<ProductReadFlight>>`。
- `_active_loads: std::size_t = 0`。

`acquire_many` 的结果与输入对齐。已有 flight 生成 Follower；新 key 在容量允许时生成
Leader；容量不足的新 key 生成 Rejected，已有 Follower 仍可领取。
批量一个 MySQL SELECT 占一个读名额，名额约束的是数据库读操作并发，不是返回行数。

领取前 reserve 票据 vector，再持 map 锁。map 插入成功后票据构造/移动必须不抛异常；
vector 声明在锁守卫之前，确保后续分配异常时先释放 map 锁，再析构已领取的 Leader。
不得让票据析构在同一线程仍持有 map 锁时调用 abandon，造成递归锁死。

`complete` 移动结果到 flight，先释放 flight 锁再处理 map，不持两把锁执行 I/O。
删除 map 项前比较 shared_ptr 身份，防止删除同 ID 的新 flight。flight 被移出 map 后，
已有票据持有 shared_ptr，等待者仍能获取结果。不得在 map 锁中查询数据库、读取缓存或等待。

### 8.3 `ProductReadTicket`：Leader/Follower 的 RAII 票据

```cpp
enum class ReadRole : std::uint8_t { Leader, Follower, Rejected };
```

| 接口 | 签名 | 契约 |
| --- | --- | --- |
| 移动 | move constructor/assignment，`noexcept` | 转移发布责任；源票据失效 |
| 析构 | `~ProductReadTicket()` | 未完成 Leader 自动 abandon，不让 Follower 永久等待 |
| 角色 | `ReadRole role() const noexcept` | 不触发 I/O |
| ID | `uint64_t id() const noexcept` | 对应唯一 ID |
| 等待 | `ProductLoadResult wait_until(steady_clock::time_point deadline) const` | Follower 谓词等待；超时返回 ReadBusy |
| 完成 | `void complete(ProductLoadResult result) noexcept` | 仅 Leader 有发布责任，完成后票据失效 |
| 构造，private | coordinator、ID、role、flight 四个参数 | 仅协调器创建，使用 friend 授权 |

私有成员：`_owner: ProductReadCoordinator*`、`_id: uint64_t`、`_role: ReadRole`、
`_flight: shared_ptr<ProductReadFlight>`。`_owner == nullptr` 表示没有未履行的发布责任，
不再加一个重复的布尔状态。不可复制；移动赋值先清理当前票据的未履行责任。
Rejected 的等待结果为 ReadBusy，不能通过它绕开准入自行回源。

### 8.4 `ProductLoadPermit`：MySQL 读名额的 RAII 守卫

接口为 move constructor/assignment、析构；私有构造由协调器调用。
唯一成员 `_owner: ProductReadCoordinator*`，移出后置空；析构释放名额。
无需公开 acquire/release 成对手动调用。正常、SQL 异常、商品验证失败都自动释放。

### 8.5 批量合并的执行顺序

1. 第一次缓存读后，把尚未解决的唯一 ID 整批领取票据。
2. 收集本请求所有 Leader，先二次检查这些 key 的缓存，避免首次 miss 与领取之间的重复回源。
3. 对仍缺失的 Leader 获取一个读名额，批量查询数据库；不能获取则这些 Leader 发布 ReadBusy。
4. SQL 及结果清理结束即释放数据库名额，再执行尽力回填，随后完成所有 Leader 票据。
5. 本请求所有 Leader 已发布后才等待 Follower。整批 Follower 共用一个从进入等待阶段开始的
   500 ms deadline，不能给每个 ID 各等 500 ms。
6. 等待超时只使该请求对应项返回 ReadBusy，不取消另一个请求的 Leader，也不重新回源。

例如 A 同时拥有 ID 1、等待 ID 2，B 同时拥有 ID 2、等待 ID 1。两者都先完成自己拥有的
Leader，再等 Follower，因此不形成相互等待环。异常退出由票据析构完成清理。

`fresh=1` 不读取缓存、不参与 flight 合并，以避免等待较早的普通读；在 protected 中仍须
获取 MySQL 读名额，并在成功读取后尽力回填。单个 Leader 的 SQL 等待受 MySQL timeout
约束，500 ms 只约束 Follower 的等待阶段，不是整个请求期限。

HTTP Worker 固定数量仍是总线程边界；Follower 会占用 Worker。本次不宣称 PUT 可以绕开
HTTP 队列，读并发准入也不预留专门的写线程。

## 9. 缓存熔断

熔断属于商品服务进程的缓存访问保护，两个适配器共用，不写进 Redis 客户端内部。

### 9.1 状态和数据

```cpp
enum class CacheBreakerState : std::uint8_t { Closed, Open, HalfOpen };

struct CacheBreakerSnapshot {
  CacheBreakerState state = CacheBreakerState::Closed;
  std::uint32_t consecutive_failures = 0;
  std::chrono::milliseconds retry_after{0};
};

using CacheNowFunction = std::function<std::chrono::steady_clock::time_point()>;
```

时钟只注入这一处状态机；默认使用 steady_clock，不建立全项目时钟框架。

默认自由函数签名为 `std::chrono::steady_clock::time_point default_cache_now() noexcept`。
注入的测试时钟也必须不抛异常，保证许可析构和 finish 的 noexcept 契约。

### 9.2 `CacheCircuitBreaker`

| 接口 | 签名 | 契约 |
| --- | --- | --- |
| 构造 | `explicit CacheCircuitBreaker(CacheBreakerOptions options, CacheNowFunction now = default_cache_now)` | 默认 now 是对应的自由函数 |
| 访问准入 | `optional<CacheOperationPermit> try_acquire()` | Closed 可访问，Open 拒绝，到期只放一个探测 |
| 快照 | `CacheBreakerSnapshot snapshot() const` | 加锁，计算非负 retry_after |
| 完成，private | `void finish(uint64_t generation, bool succeeded, bool probe) noexcept` | 更新与该代匹配的状态 |

私有成员为 `_options: CacheBreakerOptions`、`_now: CacheNowFunction`、
`_mutex: mutable std::mutex`、`_state: CacheBreakerState`、
`_consecutive_failures: uint32_t = 0`、`_retry_at: steady_clock::time_point`、
`_generation: uint64_t = 0`。HalfOpen 状态本身表示已有一个探测，不额外维护 probe_active。

Closed 连续 3 个缓存操作失败后 Open；2000 ms 后一个操作进入 HalfOpen；探测成功 Closed，
探测失败重新 Open。状态转换递增 generation，使上一轮延迟完成的操作不能关闭新一轮 Open。
Closed 中成功操作清零失败计数，“连续”按完成报告顺序计算。

Nil/miss 是缓存操作成功；正确收到损坏商品字节也是协议读取成功，不因商品 JSON 损坏熔断。
连接、timeout、协议异常和服务器命令 Error 是缓存操作失败。一次 MGET 或整个回填批次
算一次操作，不能把一个失败的 32 项批次直接当作 32 次失败。

### 9.3 `CacheOperationPermit`

接口为 move constructor/assignment（noexcept）、析构、`void succeed() noexcept`、
`void fail() noexcept`。私有构造由 breaker 调用；不可复制。
成员 `_owner: CacheCircuitBreaker*`、`_generation: uint64_t`、`_probe: bool`。
成功或失败上报后将 owner 置空，防止重复报告；异常退出未报告的许可按失败释放探测。
失败上报和析构不记录带敏感值的日志，也不做网络操作。

### 9.4 服务中的使用规则

- basic 直接访问缓存；protected 读取、二次检查、读路径损坏项清理和回填先领取许可。
- 同一 GET 中一次 CacheError 后停止其后续缓存尝试，避免读失败后再次等待回填 timeout。
- Open 时跳过缓存，回源仍须服从 MySQL 读准入，不能因熔断把数据库并发保护也关闭。
- **明确提交后的 PUT 删除始终尝试**，不受读熔断开关抑制；失败按现有结果和指标上报。
  PUT 删除单独记指标，不参与本次读/回填熔断的失败计数。
- 不重试失败的回填或失效，不增删重试后台线程。

## 10. 指标和共享对象

### 10.1 `ProductMetrics`、快照和计时器

使用进程内原子计数和固定延迟桶，不加入监控 SDK。枚举、数组大小和标签一一对应，尾部
`Count` 只用于数组长度，不作为可记录指标。新常量采用 lower_snake_case。

`ProductMetric` 枚举值及明确口径：

| 枚举值 | 增量口径 |
| --- | --- |
| `GetRequests` / `BatchRequests` / `UpdateRequests` | 进入对应商品服务接口的请求数 |
| `RequestUniqueIds` | 每次有效 GET/批量请求的唯一 ID 数，重复输入只算一次 |
| `CacheLookupKeys` | 每次调用缓存读接口的唯一 key 数，Leader 二次检查另算一次 |
| `CacheHits` / `NegativeHits` / `CacheMisses` / `CacheCorrupt` | 每次实际检查的对应分类；正命中与负命中分开 |
| `CacheReadFailures` | 失败读取操作数，批量一次失败记一次 |
| `CacheFillFailures` / `CacheInvalidationFailures` | 失败回填批次/失败 PUT 删除操作数 |
| `CacheCleanupFailures` | 读路径删除损坏值失败操作数 |
| `StoreReadOperations` | 进入 find/find_many 的回源调用数，包含发 SQL 前的连接失败 |
| `StoreReadIds` | 本次提交给 store 的唯一 ID 数 |
| `StoreReadFailures` | 失败的数据读取调用数 |
| `ReadLeaders` / `ReadFollowers` / `ReadRejected` | 每次领取的唯一 ID 票据角色数 |
| `ReadWaitTimeouts` | 等待超时的 Follower 项数 |
| `ReadAdmissionRejected` | 获取数据库读名额失败次数 |
| `CacheCircuitBypasses` | 被熔断拒绝的缓存操作次数 |

`ProductLatency` 枚举值：`CacheRead`、`StoreRead`、`CacheFill`、`CacheErase`、`ReadWait`、
`GetRequest`、`BatchRequest`、`UpdateRequest`、`Count`。
桶上界为微秒 `100,250,500,1000,2500,5000,10000,50000,200000,1000000`，再加溢出桶。

数据结构：

- `ProductLatencySnapshot`：`array<uint64_t, latency_bucket_count> buckets`、
  `uint64_t count`、`uint64_t total_microseconds`，全零初始化。
- `ProductMetricsSnapshot`：`array<uint64_t, product_metric_count> counters`、
  `array<ProductLatencySnapshot, product_latency_count> latencies`。
- `ProductLatencyCounters`：内部非复制数据结构，上述桶、count、total 改为 `atomic<uint64_t>`；
  构造时逐一显式初始化为零，不能依赖 C++17 atomic 默认构造的值。

`ProductMetrics` 接口及私有成员：

```cpp
class ProductMetrics final {
 public:
  ProductMetrics();
  void increment(ProductMetric metric, std::uint64_t amount = 1) noexcept;
  void observe(ProductLatency latency, std::chrono::microseconds elapsed) noexcept;
  ProductMetricsSnapshot snapshot() const noexcept;

 private:
  std::array<std::atomic<std::uint64_t>, product_metric_count> _counters;
  std::array<ProductLatencyCounters, product_latency_count> _latencies;
};
```

原子更新使用 relaxed，因为指标不承担业务同步。快照允许不同计数来自稍有差异的瞬间，
明确为近似观测，不能当作事务一致快照。负持续时间裁剪为零，并做安全整数转换。
延迟桶不是精确 P99；端到端分位数由压测客户端记录实际请求耗时后计算。

basic 忽略负缓存标记时计 CacheMisses，只有 protected 用其回答不存在才计 NegativeHits。
CacheLookupKeys 包含失败尝试，分类计数只统计成功取回并检查的结果；计算分类命中率时用
`(CacheHits + NegativeHits) / (CacheHits + NegativeHits + CacheMisses + CacheCorrupt)`，
另外报告失败操作数，不把连接失败当作普通 miss 隐去。

`ScopedProductTimer` 构造接口为 `ScopedProductTimer(ProductMetrics&, ProductLatency)`，
析构采集耗时；私有成员 `_metrics: ProductMetrics&`、`_latency: ProductLatency`、
`_started_at: steady_clock::time_point`。不可复制或移动，避免重复计时。

### 10.2 `ProductSharedState`

```cpp
struct ProductSharedState {
  ProductSharedState(ProductReadOptions read_options, CacheBreakerOptions breaker_options);
  ProductMetrics metrics;
  ProductReadCoordinator reads;
  CacheCircuitBreaker breaker;
};
```

只在 `ProductHttpServer::Impl` 中创建一个实例，由全部 Worker 的 ProductService 引用。
这是显式组合，不用全局 singleton、静态 cache client 或动态插件注册机制。
共享状态因含 mutex/atomic 自然不可复制，显式删除复制接口以清楚表达意图。

构造函数直接在该头文件中用成员初始化列表定义，只创建 metrics/reads/breaker；没有网络
行为，不需要另建 product_shared_state.cpp。

`GET /metrics` 输出 backend、policy、计数器、桶边界、延迟快照，以及 coordinator/breaker
快照。只返回非敏感运行信息；不读取 MySQL、不读取 Redis，也不初始化当前线程的服务。
指标路由的 JSON 形成由现有 routes 实现承担，无需新增 MetricsServer 类。

## 11. `ProductService` 的接口、成员与读写流程

### 11.1 对外接口

```cpp
class ProductService {
 public:
  ProductService(ProductStore& store, ProductCache& cache, ProductSharedState& shared,
                 ProductCachePolicy policy = {});
  GetProductResult get(std::uint64_t id, bool bypass_cache = false);
  GetProductsResult get_many(const std::vector<std::uint64_t>& ids, bool bypass_cache = false);
  UpdateProductResult update(const UpdateProductRequest& request);

 private:
  struct ReadWorkItem;
  struct ReadBatch;

  std::vector<GetProductResult> read_products(const std::vector<std::uint64_t>& ids,
                                              bool bypass_cache);
  ReadBatch make_read_batch(const std::vector<std::uint64_t>& ids, bool bypass_cache) const;
  void read_cache(ReadBatch& batch, const std::vector<std::size_t>& positions);
  void erase_corrupt(ReadBatch& batch, std::size_t position);
  void load_basic(ReadBatch& batch, const std::vector<std::size_t>& positions);
  void load_protected(ReadBatch& batch, const std::vector<std::size_t>& positions);
  std::vector<ProductLoadResult> load_from_store(const std::vector<std::uint64_t>& ids);
  void fill_cache(ReadBatch& batch, const std::vector<std::size_t>& positions);
  std::vector<GetProductResult> restore_results(const ReadBatch& batch) const;

  ProductStore& _store;
  ProductCache& _cache;
  ProductSharedState& _shared;
  ProductCachePolicy _policy;
};
```

改造已有构造调用和测试，显式传 shared；不提供隐藏静态共享对象的兼容构造。
单项和批量共用 private 读取流程，单项 get 不调用公共 get_many，避免重复请求指标。
单项使用 get/find/put，多个唯一 ID 使用 get_many/find_many/put_many；二者属于同一策略流程。

### 11.2 内部工作数据

以下两个私有类型定义在 `product_service.cpp`，数据随单次请求存在：

```cpp
struct ProductService::ReadWorkItem {
  std::uint64_t id = 0;
  std::string key;
  CacheSource source = CacheSource::NotChecked;
  std::optional<GetProductResult> result;
};

struct ProductService::ReadBatch {
  std::vector<ReadWorkItem> work_items;
  std::vector<std::size_t> input_positions;
  bool bypass_cache = false;
  bool cache_failed = false;
};
```

`work_items` 按唯一 ID 第一次出现的顺序保存；`input_positions` 将每个输入位置映射到唯一项。
临时去重 map 在 make_read_batch 中使用，不保留为长期成员。
未解决使用 `result == nullopt`；明确不存在用有值的 NotFound 结果，避免多层 optional。
`cache_failed` 只在 protected 阻止当前 GET 后续缓存 I/O；它不影响 MySQL 结果。

每个 helper 只负责下面一件事：

| 方法 | 职责和错误处理 |
| --- | --- |
| `make_read_batch` | 建 key、去重、映射原位置；fresh 来源初始化为 Bypass |
| `read_cache` | 领取可选许可、批量读、解码、记录来源；捕获 CacheError 后回源 |
| `erase_corrupt` | 尽力清理损坏项；basic 保留原处理，protected 服从本请求失败标记和读熔断 |
| `load_basic` | 一次读取未解决子集，逐项写结果，再尽力回填；不领取保护票据 |
| `load_protected` | 领取整批 flight、完成自身 Leader、等待 Follower；fresh 只使用读准入 |
| `load_from_store` | 实际 find/find_many、验证结果长度/商品 ID、映射 StoreError；不接触缓存 |
| `fill_cache` | 仅成功商品和 protected 的真实 NotFound；计算 TTL；一次回填失败不改商品结果 |
| `restore_results` | 按 input_positions 恢复重复项；未解决项防御性映射 InternalError |

结果长度、商品字段和返回 ID 在业务层再次校验，避免 Fake 或未来适配器破坏业务契约。
已有 `status_from_store_error` 保留为匿名命名空间函数；新解析、枚举标签辅助函数也放匿名
命名空间，不扩大公共接口。

### 11.3 读取错误如何影响批量响应

首次缓存读取成功时，先保存命中项，再对其余项回源。数据库批量调用失败只覆盖交给该次
调用的 ID，不能重写已有成功项。protected 中 Rejected、Leader 读准入失败、Follower 等待
超时分别写 ReadBusy；其他已解决项不受影响。

一次 get_many CacheError 表示这次缓存批次没有完整结果，此批所有未解决 key 回源；不猜测
失败前哪些缓存值可用。已经完成的单项检查或二次检查结果继续保留。
StoreError 映射保持现有约定：Unavailable → StoreUnavailable；InvalidData/Unexpected →
InternalError；不向 HTTP 泄露 SQL、凭据和底层协议内容。

只把真实 MySQL nullopt 写成负缓存。Conflict、ReadBusy、StoreUnavailable、CommitUnknown
和 InternalError 都禁止负缓存。负缓存同样是可丢弃的优化。

### 11.4 更新流程

继续使用现有事务、行锁和 expected_version。仅在 store 返回 Updated、事务结果明确时
删除 v3 商品 key；删除存在或不存在都算成功。读熔断 Open 时也执行这次删除。
记录更新请求计时、删除耗时和删除失败次数，保留更新返回体及失败 header。
冲突、NotFound 和提交不确定性不额外操作缓存。

跨后端运行两个服务同时更新同一数据库时，各自只删除所选缓存，另一份缓存仍可能旧到 TTL。
本次用途是顺序运行的对照路径，不承诺双缓存同时部署的广播失效。

## 12. HTTP 对象生命周期及路由接入

### 12.1 `ProductHttpConfig`

保留 `bind_address`、`port`、`worker_count`、`mysql`；将原 `cache_nodes/cache_timeout`
替换为 `ProductCacheOptions cache`，保留扩展后的 `ProductCachePolicy cache_policy`，
新增 `ProductReadOptions read_options`、`CacheBreakerOptions breaker_options`。
修改现有 C++ 构造使用处和测试；环境变量名称按第 3 节兼容。

load_config 根据 Worker 数计算未指定的读并发默认值。直接在 C++ 中构造 config 时，调用方
显式保证读并发数不超过 Worker 数；checked_config 校验最终配置，不吞掉错误配置。

### 12.2 `WorkerContext`

仍是 `product_http.cpp` 匿名命名空间中的内部 struct，构造接收
`const ProductHttpConfig& config, ProductSharedState& shared`。
成员声明顺序如下，均为公开数据字段：

```cpp
MySqlThreadGuard thread_guard;
MySqlProductStore store;
std::unique_ptr<ProductCache> cache;
ProductService service;
```

用 make_product_cache 创建所选缓存，service 引用 store、`*cache` 和 shared。
声明顺序保证 service 先销毁，随后缓存和 store，最后 thread_guard。
保留 thread_local 惰性 WorkerContext；所有连接只在创建它们的线程使用。

### 12.3 `ProductHttpServer` 与 `Impl`

外层 serve/stop/析构和 `_impl` 保持现有接口。`Impl` 保留 `config`、`mysql_runtime`、
`server`、`state_mutex`、`stopping`、`serve_called`，新增 `ProductSharedState shared`。
`shared` 声明在 `server` 前，mysql_runtime 也在 server 前，保证 HTTP 线程及线程局部对象
全部退出后才销毁共享状态和客户端运行时。不得让 service 引用已释放的 shared。
进程停机由现有控制线程机制处理，不在信号处理函数中销毁句柄或加业务锁。

### 12.4 路由安装和辅助函数

现有内部头文件是 `product-service/src/product_http_routes.h`，修改为：

```cpp
void install_product_routes(httplib::Server& server,
                            const std::function<ProductService&()>& current_service,
                            const ProductSharedState& shared, CacheBackend backend,
                            CachePolicyMode policy_mode);
```

在 `product_http_routes.cpp` 保留既有商品请求解析和错误映射，新增匿名命名空间辅助：

- `parse_product_ids(string_view text) -> optional<vector<uint64_t>>`：严格解析及 32 项上限。
- `ProductBatchQuery`：`vector<uint64_t> ids`、`bool bypass_cache = false`。
- `parse_batch_query(const httplib::Request&) -> optional<ProductBatchQuery>`：只解析 ids/fresh，
  拒绝重复和未知参数；保持现有单项 parse_fresh_query。
- `make_batch_response(const GetProductsResult&) -> nlohmann::json`：逐项 product/error。
- `batch_cache_source(const GetProductsResult&) -> string_view`：相同来源或 MIXED。
- `make_metrics_response(const ProductSharedState&, CacheBackend, CachePolicyMode)`：只读快照 JSON。

先验证查询参数，再调用 current_service，避免无效列表触发连接初始化。
metrics handler 直接访问 shared；不得调用 current_service，Redis/MySQL 故障时仍可观察指标。
新增路由匹配必须与 `/products/{id}` 分开，回归现有路径和非法 ID 行为。

## 13. 开发顺序和每阶段完成条件

按下面顺序开发，每阶段先形成能运行的最小闭环再继续。阶段中的源文件列表包含第 4 节的新
文件以及所需已有文件修改；不在一次提交里同时重构所有历史代码。
`protected` 的完整承诺在 P4 结束后成立，早期阶段不将部分保护实现作为完成版发布。

### P0：单商品 Redis 路径

1. 建立 options 类型、解析函数、make_product_cache，保留 Sphinx 默认路径。
2. 配置 hiredis 依赖和 `RedisProductCache` 的 RAII、惰性连接、GET/SET/DEL。
3. 改 WorkerContext 使用 `unique_ptr<ProductCache>`，保持线程守卫和连接的析构顺序。
4. 开放 backend 和 Redis 配置；只校验选中的后端，凭据不进入错误字符串。
5. 保留当前正缓存格式与单项接口行为，先在 basic 跑通 Redis 商品读写闭环。

完成条件：单项 miss→MySQL→fill→hit、fresh→MySQL→fill、PUT→提交→删除均可在 Redis
观察；连接拒绝和截断响应能够回源；原 Sphinx 测试仍通过；没有纯 MySQL 开关。

### P1：批量闭环

1. 加 CacheWriteEntry 和 ProductCache/ProductStore 默认批量实现，定义放各自新 `.cpp`。
2. 增加 Redis MGET、有界 pipeline、错误回复消费和连接重置测试。
3. 扩展 ClusterClient/MemcachedConnection，完成按节点分组、去重和位置恢复。
4. 增加 MySQL IN 预处理查询及 ProductRowBuffer，保持结果清理和连接失效规则。
5. 增加 BatchProductItem/GetProductsResult；将单项和批量读取收敛到同一业务流程。
6. 增加 `/products?ids=...`，严格输入校验、逐项结果、MIXED 来源和部分成功。

完成条件：重复 ID 只做一次业务读取但按输入重复输出；32 项接受、33 项拒绝且无依赖 I/O；
多个 miss 使用一条数据 SELECT；Redis 一条 MGET，Sphinx 每个相关节点一条 get；混合 hit
与 SQL 失败时 hit 商品仍返回；pipeline 后下一次 GET 不误读剩余回复。

### P2：负缓存和 TTL 策略

1. 增加 CachePolicyMode，扩展 ProductCachePolicy。
2. 定义 v3 key、负缓存标记和类型化解码，保留正商品 codec 接口。
3. 实现纯函数 TTL 偏移和边界裁剪，不使用全局 rand。
4. protected 对真实不存在写 5 秒负缓存；basic 不写、不使用负缓存回答。
5. 回归 fresh、损坏值、ID 不符、旧命名空间和提交不确定性。

完成条件：不存在热点在 TTL 内减少数据库访问；SQL 错误无负缓存；正 TTL 在配置边界内；
负缓存过期后重新回源；v2/v3 迁移边界写入 README；basic 保持原读取策略。

### P3：进程内合并和 MySQL 读准入

1. 增加 flight、Coordinator、Ticket、LoadPermit，并先用独立测试验证生命周期。
2. 创建进程级 ProductSharedState，所有 Worker 借用同一协调器。
3. protected 加首次读后整批认领、Leader 二次检查、批量回源、回填、发布结果。
4. 全部本方 Leader 发布后等待 Follower，使用一次等待阶段 deadline。
5. 加 flight 容量限制、读操作并发准入、ReadBusy HTTP 映射和 fresh 准入。

完成条件：同进程同一 flight 只有一个数据回源；交叉批次无等待环；Leader 抛错/退出不会遗留
flight；Follower 超时不取消 Leader；容量和读名额始终有界；更新事务不占读名额。

### P4：熔断、指标和完整 protected

1. 增加 generation 熔断状态机、操作许可及可注入时钟。
2. 将 protected 缓存读、二次检查、清理、回填接入许可；一次 GET 失败后跳过后续缓存尝试。
3. 明确 PUT 提交后删除绕过读熔断，不引入重试。
4. 增加 ProductMetrics、ScopedProductTimer、指标标签与 `/metrics`。
5. 把请求、实际缓存操作、实际数据读、Leader/Follower 的计数口径落实到调用点。

完成条件：连续故障进入 Open、到期一个探测、恢复后 Closed；旧 generation 的回复不篡改
新状态；熔断时数据库仍受准入；缓存故障下 metrics 可读取；basic 的保护对象不改变业务
行为；两后端通过相同的业务测试矩阵。

### P5：严格验收、对照实验和学习文档

1. 参数化现有 Python HTTP 集成测试，支持 backend/policy，避免复制两套业务测试。
2. 加真实 Redis 验收、严格两后端验证脚本，保留原 MySQL + Sphinx 脚本入口。
3. 加 HTTP 压测脚本，输出请求分位数、数据库读取和内存/CPU 数据。
4. 扩展格式/静态检查覆盖，执行第 15 节的全部最终检查。
5. 更新 README 的启动配置和边界，新增学习路线、实验说明和结果模板。

完成条件：两个后端 × 两个模式的严格验收均实际执行；测试无跳过冒充通过；有可重复运行的
对照命令和原始结果；结论附带环境、数据、并发、TTL 和原生批量差异；未实现能力没有写成
项目已支持的特性。

## 14. 测试数据、测试类和必须覆盖的场景

### 14.1 测试辅助类的边界

复用 `product_service_test.cpp` 中的 Fake，按接口扩展，不向生产代码暴露测试开关。

| 类/结构 | 接口与数据 | 使用方式 |
| --- | --- | --- |
| `FakeStore` | find/find_many/update overrides；`map<uint64_t, Product>` 商品、调用记录、可选 StoreErrorCode | 单线程业务测试；记录批量实际交给 store 的 IDs |
| `FakeCache` | 五个缓存接口 overrides；key/value map、TTL 记录、调用记录、按调用次序失败注入 | 验证 hit、负缓存、失败回填及输入校验前无操作 |
| `SharedFakeProductDatabase` | `find_many(ids)`、`update(request)`、受锁保护的数据/读取计数 | 多个线程各自的 FakeStore 共享数据库状态，避免共享真实客户端 |
| `ReadBarrier` | `arrive_and_wait()`、`wait_until_arrived()`、`release()`；mutex、condition_variable、到达/释放标志 | 精确控制读到旧值、提交更新、放行回填等顺序 |
| `ManualCacheClock` | `now() const noexcept`、`advance(milliseconds) noexcept`；`_now: steady_clock::time_point` | 注入 breaker；状态机测试不实际等 2 秒 |
| `ScriptedRedisServer` | 构造脚本、`port()`、`commands()`、`release_reply()`、`stop()`；不可复制，`_impl` | 本地回环监听端口 0、记录参数，按脚本返回字节或断连 |

`ScriptedRedisServer::Impl` 的数据为监听 socket、服务线程、mutex、condition_variable、
已录命令和回复脚本、释放/停止标志；按是否 private 使用对应成员命名。
`TestRedisCommand` 只含 `vector<string> args`；`TestRedisReply` 含 `string bytes`、
`bool close_connection`、`bool wait_for_release`。测试服务器只实现够记录 argv 的最小 RESP
输入解析，不模拟 Redis 存储或过期。析构关闭自有连接并 join；等待有测试总期限。
连接正常行为和 TTL/淘汰由真实 Redis 测试证明，故障字节由脚本服务器证明。

Fake 的单线程已有字段不强制改成共享；并发测试把数据库状态共享，把 Worker 适配器分开。
新增测试 suite/case 一律 PascalCase，检查相应生产 `.cpp` 与测试都进入编译数据库。

### 14.2 单元和协议测试矩阵

| 组件 | 触发与必须观察的结果 |
| --- | --- |
| Redis 连接 | 惰性首次连接；正确 AUTH/SELECT；初始化失败释放句柄；错误线程拒绝使用 |
| Redis 值 | 含 NUL/CRLF 的 value 完整往返；Nil 与空字符串不同；GET 错误类型回源 |
| Redis SET/DEL | EX 参数正确；0/1 删除均成功；非 OK/非法整数为 CacheError |
| Redis MGET | 顺序、重复 key、Nil、非 String key；非法回复长度和类型导致连接重置 |
| Redis pipeline | 多回复对应；中间 Error 后消费剩余回复；断连后不重发；后续独立操作重连 |
| ClusterClient | 多节点分组、同 key 去重、乱序 VALUE、遗漏 key、二进制 value、异常节点连接清除 |
| Memcached 解析 | 重复/陌生 key、非法长度、缺 CRLF/END、截断均明确失败 |
| MySQL 批量 | 一条 SELECT、乱序行、缺行、重复输入、参数数改变后语句重建、坏数据与清理 |
| codec/policy | 正负格式、ID 错误、额外字段、JSON 上限、UTF-8、TTL 最小/最大边界 |
| Coordinator | 相同 ID 合并、不同 ID 独立、交叉批次、容量满、名额满、Follower 超时 |
| Ticket/Permit | 移动后仅一个释放者、Leader 异常放弃唤醒、所有名额和 flight 被清理 |
| Breaker | Closed 连续失败、Open 无读/回填 I/O、到期单探测、成功/失败恢复、过期 generation 回复被忽略 |
| Service | 两模式下 hit/miss/fresh/corrupt/fill error；SQL error 不缓存为 NotFound |
| 批量响应 | 保留顺序/重复；hit 与 miss 混合；部分 SQL 失败；ReadBusy 仅覆盖对应项 |
| 更新 | expected_version 冲突、CommitUnknown 不删除不重试、Open 仍尝试提交后删除 |
| 一致性边界 | 保留并扩展 `ConcurrentStaleFillExpiresByOwnTtl`，证明保护没有消除旧读回填 |
| 指标 | 一批失败计一次；二次检查另计；重复输入不膨胀唯一 ID 数；跨线程无丢计数 |

并发顺序用 barrier/condition_variable 或明确事件控制，禁止靠固定 sleep 猜测顺序。
真实过期测试允许有总期限的轮询，并观察首次写入后的 TTL，避免与并发时序测试混用。

### 14.3 严格集成验收

现有 `scripts/verify_mysql_sphinx.sh` 保持原用法。新增 `verify_product_cache.sh [build-dir]`
负责两种 backend、两种 policy 的完整矩阵，复用现有 MySQL 凭据约定和隔离数据库检查。
测试 DB 名仍必须包含 `test`；缺凭据、缺 Redis 可执行文件、缺测试目标都失败退出。

Redis 集成测试启动自己拥有的实例：绑定 `127.0.0.1`，选独立端口和临时目录，
`save ""`、`appendonly no`；使用短测试 TTL，结束时 finally 只清理自有进程和目录。
禁止对配置中的共享 Redis 执行 FLUSHALL/FLUSHDB。测试启动服务和运行应用时不打印凭据。

HTTP 集成检查新增：

- Redis 命中时暂时使 MySQL 读不可用，仍能得到已缓存商品；fresh 正确暴露数据库失败。
- 缓存断连、重启、协议失败时回源与 protected 熔断；数据库也不可用时返回明确错误。
- 批量逐项结果、重复 ID、最大项数、非法输入无后端操作。
- 正负缓存实际到期、故障中不生成错误负缓存、提交后删除、删除失败 header。
- 并发热点的回源调用数减少，真实 MySQL 批量测试验证一条数据 SELECT；两个不同服务进程
  不声称合并。
- 进程正常停机后无自有子进程和临时资源遗留；metrics 不依赖当前 Worker 初始化成功。

原有 mysql_commit 包装测试保留。普通 ctest 允许环境不足时跳过现有集成测试，但这类结果
只表示局部验证；最终验收必须由严格脚本实际跑完依赖测试。

## 15. 可读性、格式与静态检查要求

### 15.1 写代码时的准则

- 所有新类型直接位于 `namespace sphinx`，文件内 helper 用匿名命名空间。
- 类型/枚举/别名 PascalCase；函数/参数 snake_case；private/protected 字段 `_snake_case`；
  公开数据字段 snake_case；新常量 lower_snake_case。保留历史公开常量不作无关重命名。
- 不新增通用 Repository、泛型缓存策略引擎、插件注册表或按命令划分的一组微型类。
  helper 按“读缓存、读数据库、回填、等待、恢复输出”命名，阅读顺序就是请求顺序。
- 只用 C++17。头文件自包含，显式 include 用到的 string_view、vector、optional 等类型。
- C 资源 RAII；析构不抛业务异常；不复用失败的协议连接；不将 `string_view` 存过所属字符串
  生命周期；MySQL binding 的指针不能随 vector 扩容或对象移动失效。
- 网络/SQL 操作不放在业务 mutex 内。共享的只有纯结果和保护状态，客户端绝不共享。
- 指标更新不承担同步；真正的完成/等待由 mutex 与 condition_variable 的谓词保证。
- 环境变量仅启动读取，沿用已有局部、说明原因的 concurrency NOLINT；新代码不加入全局
  抑制，不关闭 analyzer/bugprone/concurrency 检查。
- 尽力失败捕获后记录指标或设置结果状态，避免新增无意义空 catch 和为它添加 NOLINT。
- 边界处才把异常映射为业务结果；内部 helper 不一层层重复 catch/rethrow。
- 单文件过大时按既定组件拆文件，不靠“读起来很短”的高度模板化写法压缩逻辑。
  注释说明事务不确定性、TTL 竞争、所有权和批量不原子，不逐句翻译显然的代码。

### 15.2 检查脚本接入

`scripts/fmt` 已覆盖商品服务与 sphinxd 的 C++ 文件，无需另造格式规则。
`scripts/tidy` 要把第 4 节新增生产 `.cpp` 加入 required 集合，确保旧编译数据库会报错；
现有选择目录增加 `sphinxd/test`，使新增 ClusterClient 批量测试实际进入检查。
保持最多四个分析进程、既有 HeaderFilter 和第三方/生成协议排除规则。
测试环境需要 GTest，不能靠不生成测试规避相关诊断。

最终执行顺序：

```bash
./scripts/fmt
git diff --check

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./scripts/verify_product_cache.sh build

cmake -S . -B cmake-build-debug-wsl -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON
cmake --build cmake-build-debug-wsl
./scripts/tidy cmake-build-debug-wsl
```

格式化后复查变更范围，避免把无关历史文件格式变化混进功能提交。
按项目现有 ENABLE_ASAN 配置另外运行受改动的协议/生命周期/并发测试，重点检查句柄、回复、
绑定缓冲和票据销毁；ASAN 不证明没有数据竞争，并发正确性还要靠时序测试和代码检查。
记录实际执行、跳过和失败项；不能把静态阅读或文档代码示例当作编译/tidy 已通过。

## 16. 对照实验计划

`compare_product_cache.py` 是外部实验驱动，不进入商品运行时。输入包含 build 目录、隔离测试
数据库配置、数据规模、客户端并发、请求数、输出目录；不提供纯 MySQL backend。
脚本按四种 backend/policy 组合顺序运行，创建自有服务/缓存进程，关闭后继续下一组合。
数据初始化沿用现有集成测试访问 MySQL 的方式，不通过本来不支持插入的 PUT 冒充建数接口。

### 16.1 保持相同的实验条件

- 商品数、名字/JSON 大小、价格与版本分布、ID 请求分布和不存在 ID 比例。
- HTTP Worker 数、客户端并发、请求总数、预热方式、CPU 预算和机器位置。
- 正负 TTL、抖动范围、读准入、回源合并、熔断参数；比较时明确 basic 或 protected。
- MySQL 实例、表结构、索引和连接 timeout；记录其实际数据 SELECT 次数。
- 缓存内存预算与实际 RSS/内部使用量都记录，不把相同配置字节数视为相同可用容量。

先比较一个 Sphinx 节点与一个 Redis 实例；多 Sphinx 节点单独做扩展实验，记录节点数与
Worker 数，不能直接用多节点资源对单 Redis 实例宣布性能胜负。

### 16.2 必做场景

| 场景 | 应观察什么 |
| --- | --- |
| 单项全命中 | HTTP 吞吐/延迟和实际零回源，不只测原生命令 |
| 批量 1/8/32 项全命中 | MGET 与跨节点 multi-get、去重及协议往返成本 |
| 冷缓存和部分命中 | 数据 SELECT 数、回填时间、缓存命中比例 |
| 热点过期 | basic 与 protected 的数据库读数、Follower 等待、ReadBusy 比例 |
| 不存在热点 | 负缓存效果、NotFound 比例、负缓存到期后的再回源 |
| 缓存断连/恢复 | 延迟恶化、Open/HalfOpen、受控数据库并发、恢复速度 |
| 内存压力 | Redis noeviction / allkeys-lru 的写失败或淘汰，以及 Sphinx segment 回收 |
| 混合 GET/PUT | 更新冲突、删除失败、版本观测；保留旧读回填边界 |

每组输出配置与版本、运行时间、总请求/成功/业务错误/系统错误、吞吐、P50/P95/P99、
缓存分类、回源调用数和 ID 数、CPU、RSS、缓存内部内存、淘汰/回收指标。
将正常 NotFound/Conflict 与故障类错误分开报告，避免通过丢弃失败请求虚增吞吐。
重复运行保存原始记录及分布，不用单次最好结果代替一般表现。

Redis INFO 中的内存、命中、过期、淘汰数据与商品层 metrics 一起观察；两者口径不同，
商品层的二次检查、负缓存和批量重复项尤其需要按第 10 节解释。
回源调用数包含连接阶段就失败的调用，不能无条件称为已执行 SELECT 的次数。需要 SQL
执行量时，在隔离 MySQL 上读取 Performance Schema 的商品 SELECT 摘要前后增量，并记录
权限和其他流量条件；不能观测时 SQL 次数字段留空并说明，不用应用调用数冒充数据库统计。
Redis maxmemory 实验按 [官方淘汰说明](https://redis.io/docs/latest/develop/reference/eviction/)
解释策略；缓存回填 OOM 时商品读取仍可成功，不能将它误算成商品不存在。

## 17. Redis 学习覆盖与学习顺序

这个方案覆盖的是 Redis 缓存工程的常见核心问题，适合作为项目第二条实现路径。
Redis 的全部数据类型和高可用运维不在本次开发范围；学习文档要明确这个覆盖边界。

| 顺序 | 学习点 | 对应实现或观察方式 |
| --- | --- | --- |
| 1 | String、key 设计、序列化、RESP、Nil/空串、AUTH/SELECT | RedisProductCache 单项路径与协议测试 |
| 2 | Cache-Aside、MySQL 权威性、先提交后删除、提交不确定性 | ProductService 读写与失败测试 |
| 3 | TTL、过期、负缓存、穿透、TTL 抖动 | v3 codec、protected 策略、真实 TTL 观察 |
| 4 | MGET、pipeline、RTT、批量不等于事务 | 批量读/回填协议与端到端对照 |
| 5 | 热点击穿、同进程合并、背压、故障时数据库保护 | Coordinator、ReadBusy、Breaker 场景 |
| 6 | maxmemory、淘汰、缓存重建、OOM 与降级 | 内存压力与重启实验 |
| 7 | INFO、延迟分位数、命中率、过期/淘汰计数的口径 | metrics 和对照脚本原始结果 |

Hash/List/Set/ZSet、Streams、Pub/Sub、Lua、MULTI/EXEC、WATCH、分布式锁、复制、Sentinel、
Cluster，以及持久化参数调优列为后续独立学习主题，不为“覆盖知识点”加入商品服务代码。
可以读 RDB/AOF 原理并观察重启后的缓存重建；本次默认关闭持久化，缓存不是事实存储。

新增学习路线按每阶段的请求链列源码入口、动手方法和观察问题，不填写一次性测试通过数量。
学完本次应能解释：为什么缓存可丢、什么时候回源、批量为何省往返、哪些保护是应用行为、
Redis 淘汰与 Sphinx segment 回收有何区别，以及为什么这些机制仍不保证强一致性。

## 18. 最终交付清单

- 两种后端、两种模式共用商品业务与格式，默认 Sphinx + basic；配置可重复启动。
- 每 Worker 独占客户端；共享协调状态生命周期明确；所有失败路径释放句柄、票据和读名额。
- 单项读写兼容；批量顺序/重复/逐项失败符合约定；fresh 和 CommitUnknown 保持原边界。
- 负缓存只表示真实不存在，回源合并/准入/熔断有界且无后台重试。
- 格式、静态检查、有效测试目标、严格 MySQL/Redis/Sphinx 验收实际完成并保留记录。
- README、Redis 学习路线、对照脚本、实验条件和原始数据齐全；性能结论可追溯。
- 未实现的高可用、跨实例合并、可靠失效和强一致性明确记录，项目介绍与代码能力一致。
