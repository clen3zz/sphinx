# MySQL + Redis 商品服务架构

MySQL 保存商品的权威数据，Redis 保存有 TTL、可丢弃和重建的查询结果。商品服务采用
Cache-Aside：读请求先查缓存，未命中或缓存故障时回源；更新先提交数据库，再尽力删除缓存。
Sphinx 作为可切换的缓存对照，两条路径共用业务逻辑、数据格式和保护策略。

```text
HTTP 请求
  → ProductHttpServer 的当前 Worker
  → ProductService
      ├─ ProductCache → RedisProductCache → hiredis → Redis 单实例
      └─ ProductStore → MySqlProductStore → MySQL products

GET：缓存命中 → 返回；未命中 / 损坏 / 不可用 → 查 MySQL → 尽力回填
PUT：MySQL 锁行、检查版本、更新并提交 → 尽力删除缓存
```

## 对象边界与数据结构

每个 HTTP Worker 独占自己的业务服务、数据库客户端和缓存客户端，使用同步 I/O；连接按需
建立并复用。进程持有一个 ProductSharedState，通过内部同步让 Worker 共享读协调器、
熔断器和指标。客户端连接不跨线程共享，也没有额外连接池。

| 组件 | 接口与职责 | 主要数据 |
| --- | --- | --- |
| [ProductHttpServer](../product-service/src/product_http.cpp) | 创建 Worker 上下文；[路由](../product-service/src/product_http_routes.cpp)解析请求、映射状态和响应头 | 运行配置、每个 Worker 的 store/cache/service、进程共享状态 |
| [ProductService](../product-service/include/sphinx/product_service.h) | get、get_many、update；统一缓存旁路和保护流程 | 借用 store/cache/shared，持有 ProductCachePolicy |
| [ProductCache](../product-service/include/sphinx/product_cache.h) | `get`、`put`、`erase`、`get_many`、`put_many` | 读返回 `optional<string>`；写用 `CacheWriteEntry{key, value, ttl_seconds}` |
| [RedisProductCache](../product-service/src/redis_product_cache.cpp) | 实现缓存接口，管理命令、回复和重连 | Pimpl 持有配置、所属线程 ID、RAII 管理的 hiredis context；回复也由 RAII 释放 |
| [MySqlProductStore](../product-service/src/mysql_product_store.cpp) | find、find_many、update；预处理查询、事务和错误分类 | Worker 独占 MYSQL 连接及语句句柄；批量查询复用对应参数数量的语句 |
| [ProductReadCoordinator](../product-service/include/sphinx/product_read_coordinator.h) | acquire_many、try_acquire_load、活动数量查询 | 按商品 ID 索引的 flight 表、活动回源数、互斥锁；flight 含条件变量与结果 |
| [CacheCircuitBreaker](../product-service/include/sphinx/cache_circuit_breaker.h) | try_acquire、snapshot；操作许可报告成功或失败 | Closed/Open/HalfOpen 状态、失败数、重试时间、generation、互斥锁 |
| [ProductMetrics](../product-service/include/sphinx/product_metrics.h) | increment、snapshot | 原子事件计数数组；指标不参与业务同步 |

ProductService 内部用 ReadBatch 串起读流程：work_items 存去重后的商品，input_positions
恢复原始顺序与重复项，两个标志记录主动旁路和缓存失败。每个 ReadWorkItem 含 id、key、
缓存来源、可选结果和回源票据。单项与批量读取共享这套逻辑，结果直接写入工作项。

ProductReadTicket 标识 Leader、Follower 或 Rejected，负责发布或等待结果；Leader 未完成
就析构时发布错误，避免留下悬空 flight。ProductLoadPermit 用 RAII 归还数据库读名额。
CacheOperationPermit 携带 generation，旧操作的结果不能修改新一轮熔断状态。

## 商品与缓存格式

[Product](../product-service/include/sphinx/product.h) 包含 id、name、price_cents、version。
ID 和版本为正数，名称是 1～128 字节的有效 UTF-8，价格使用整数分。更新请求另带
expected_version，成功后版本加一。

| 内容 | 格式与规则 |
| --- | --- |
| 缓存 key | `product:v3:<id>`；接口统一要求非空、最多 250 字节、无空白和控制字符 |
| 正缓存 | JSON：`{"id":42,"name":"tea","price_cents":199,"version":1}` |
| 负缓存 | JSON：`{"id":42,"not_found":true}`；仅 protected 使用 |
| 解码 | [codec](../product-service/src/product_codec.cpp)限制 payload 为 512 字节，严格检查字段、类型、领域约束及请求 ID；非法值按损坏缓存处理 |
| TTL | 正缓存默认 30 秒；protected 按 ID 确定性抖动，默认 27～33 秒；负缓存默认 5 秒 |

写缓存时把值和 TTL 放进同一条 SET ... EX 命令，避免先写值再设置过期的中间状态。
正缓存 TTL 统一限定为 1 秒～30 天；负缓存为 1～30 秒，抖动幅度为 0～30 秒。
抖动结果也受正缓存 TTL 上下界约束。同一 ID 的抖动固定，用于分散不同商品的过期时间。

## 读请求

### 基础路径

`GET /products/{id}` 先读取并校验缓存。正缓存命中直接返回；没有 key 则查 MySQL；
缓存值损坏时先尽力删除，再回源。MySQL 返回商品后尽力回填，明确没有记录才返回 404。
数据库故障不会被解释成商品不存在，缓存回填失败不改变已经取得的数据库结果。

X-Cache 标明 HIT、MISS、CORRUPT 或 BYPASS。负缓存命中使用 HIT；basic 遇到合法负缓存
标记时仍回源，不直接回答不存在。

### 保护路径

SPHINX_CACHE_POLICY 默认是 basic；设置为 protected 后，在相同读流程上加入：

| 保护 | 行为与默认边界 |
| --- | --- |
| 负缓存 | 仅缓存 MySQL 明确返回的 NotFound，默认 5 秒，减少不存在 ID 的重复回源 |
| TTL 抖动 | 正缓存默认 ±3 秒，分散不同商品的集中失效 |
| 同 ID 回源合并 | 同一进程中一个 Leader 加多个 Follower；最多 1024 个活动 ID |
| 数据库读准入 | 最多 min(2, HTTP Worker 数) 个并发回源操作；用尽时立即返回 ReadBusy |
| 等待期限 | 同一批 Follower 共用一个默认 500 ms 的等待截止时间；超时不取消 Leader |
| 缓存熔断 | 默认连续失败 3 次后 Open 2000 ms，之后只放行一个 HalfOpen 探测；成功恢复，失败重新打开 |

缓存未命中后先领取回源票据。Leader 再查一次缓存，仍未命中才申请数据库读名额并回源；
数据库读取完成即归还名额，随后尽力回填并发布结果。批量请求先完成本批 Leader，再等待
Follower，避免批次交叉持有 Leader 时互相等待。容量拒绝、准入拒绝和等待超时都返回
ReadBusy，单项 HTTP 状态为 503，批量则在受影响项中标明错误。

protected 中一次读请求发生缓存操作失败或被熔断旁路后，跳过该请求后续缓存操作。
熔断打开时数据库读准入仍生效；这些保护只在单个商品服务进程内协调。

### 批量与主动核实

`GET /products?ids=42,999,42` 最多接受 32 个输入项，重复 ID 也占名额。内部去重后使用 Redis
MGET；未命中的商品合并为 MySQL IN 查询，回填使用 pipeline。只有一个唯一 ID 时走单项
缓存和数据库接口。输出按原始顺序恢复，保留重复 ID，以及每项的商品或业务错误。
合法批量的外层状态为 200，不能据此认为每一项成功；缓存来源不同则 X-Cache 为 MIXED。

单项或批量加 fresh=1 会绕过缓存读取和回源合并，直接读 MySQL，之后仍尽力回填。
protected 的数据库读准入继续生效。它用于核实权威值，不绕过读容量限制。

## 更新与一致性

`PUT /products/{id}` 接收 name、price_cents 和 expected_version：

1. 开启 MySQL 事务，用 SELECT ... FOR UPDATE 锁定商品。
2. 检查版本，再执行带版本条件的更新并将版本加一；不存在返回 404，冲突返回 409。
3. 确认 COMMIT 成功后，尽力 DEL 商品缓存。删除独立于读熔断，即使熔断打开也会尝试。
4. 删除失败不撤销已提交更新，通过 X-Cache-Invalidation: failed 和指标体现。

CommitUnknown 表示提交结果无法确认：不自动重试事务，不据此删除缓存，丢弃故障连接；
调用方可以用 fresh=1 核对数据库，再决定后续操作。

版本检查防止更新者无声覆盖彼此，但不保证缓存强一致。仍可能出现：

```text
GET 读 MySQL 得到 v1 → PUT 提交 v2 并删除缓存 → 旧 GET 回填 v1
```

旧值从实际回填时开始计算自己的 TTL。删除失败也可能让旧缓存继续被读取；因此不能承诺
“提交后固定 30 秒一定新鲜”。当前没有版本写入栅栏、分布式锁或失效重试队列，依靠缓存项
过期收敛，fresh=1 则绕过缓存读取权威值。

## Redis 客户端与故障处理

Redis 地址、数据库号和可选认证在启动时校验。Worker 第一次实际缓存操作时连接，设置
连接与 I/O 超时，再按需 AUTH 和 SELECT。默认两种超时都是 200 ms，它们不构成整个
HTTP 请求或 pipeline 的统一截止时间。hiredis 的 argv 接口按指针和长度传参，支持空值、
零字节等二进制内容；公共头文件不暴露 hiredis 类型。

| 操作 | 命令与回复契约 |
| --- | --- |
| get | GET；String 是值，Nil 是未命中，空字符串不等于未命中 |
| put | SET key value EX ttl；严格收到 OK 才算成功 |
| erase | DEL；返回 0 或 1 都成功，删除不存在的 key 是幂等操作 |
| get_many | 一个 MGET；校验数组长度和每项 String/Nil，保持输入顺序与重复项 |
| put_many | 先校验整个批次，再追加多个 SET ... EX，按顺序读完回复；pipeline 减少往返，不提供事务原子性 |

Redis 错误回复转为 CacheError。pipeline 中即使某条命令被拒绝，也会读完剩余普通回复，
再统一报告失败；其中部分写入可能已经生效。传输失败、畸形回复或 pipeline 中途异常会
丢弃连接，防止未读回复串入下一次操作。当前操作不自动重放，后续独立操作可重新连接。
GET 遇到缓存故障尝试回源；回源失败按数据库错误返回，而不是伪造 404。

## 与 Sphinx 路径的对照

通过 SPHINX_CACHE_BACKEND=redis|sphinx 选择 [适配器](../product-service/src/product_cache_options.cpp)，
程序默认仍为 Sphinx。业务层不依赖缓存协议，两条路径都连接 MySQL，没有纯 MySQL 运行模式。

| 维度 | Redis | Sphinx |
| --- | --- | --- |
| 客户端与协议 | 同步 hiredis、RESP | [ClusterClient](../sphinxd/src/cluster_client.cpp)、Memcached 文本协议 |
| 路由 | 一个 Redis 实例 | 客户端一致性哈希选节点；节点内按 key 选独占存储的 Worker |
| 连接 | 每个 HTTP Worker 一个惰性连接 | 每个 HTTP Worker 独占客户端，按节点复用连接 |
| 批量读取 | 原生 MGET | 按节点分组执行原生 multi-get，再恢复结果顺序 |
| 批量回填 | pipeline SET ... EX | 默认逐条 set |
| 过期与容量 | Redis TTL；对照工具配置 maxmemory 和淘汰策略 | TTL 检查与索引清理；[Log segment 回收](../sphinxd/src/logmem.cpp)，不等于 LRU |
| 业务规则 | 共用编码、basic/protected、回源、版本更新及提交后删除 | 同左 |

Sphinx 节点列表是静态配置，改变节点列表或 Worker 数量后不迁移旧缓存，可通过 MySQL
重新回填。两条路径都没有配置缓存副本、自动故障转移或在线迁移；Redis 也未接入 Sentinel
或 Cluster。批量回填的往返次数不同，不能把对照结果中的全部差异都归因于存储引擎。

## 观测与源码入口

`GET /metrics` 返回 backend/policy、商品和缓存事件计数、协调器活动数量及熔断快照；
该路由不初始化 Worker，也不访问 MySQL 或缓存。计数是并发观测值，不是一个事务快照。
HTTP P50/P95/P99 由 [对照客户端](../scripts/compare_product_cache.py)采样计算；实验同时记录
阶段指标、进程 CPU/RSS、缓存统计及可用的真实 SQL 计数。观测条件不足的 SQL 计数留空，
RSS 是快照而非峰值。对照需同时说明并发、Worker 数、TTL、内存预算、淘汰策略与重复样本。

按请求链阅读：先看 [HTTP 路由](../product-service/src/product_http_routes.cpp)和
[ProductService](../product-service/src/product_service.cpp)，再看 Redis 适配器、codec 与
MySQL 存储，最后看读协调器和熔断状态机。运行与严格验证入口见 [README](../README.md)。

这条路径覆盖 Redis String、RESP/二进制安全、连接与认证、TTL、MGET、pipeline、淘汰与
拒写，以及缓存穿透、热点回源、集中失效和故障观测。Hash/List/Set/ZSet/Stream、Lua、
Redis 事务、持久化恢复、复制和集群不在当前商品缓存架构中。
