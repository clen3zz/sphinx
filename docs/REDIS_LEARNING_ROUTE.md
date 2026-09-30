# Redis 商品缓存学习路线

本文按一次商品请求的调用顺序组织源码和练习，帮助从已有的 Sphinx 路径迁移到 Redis。
目标是理解“关系数据库做权威存储、Redis 做可丢弃的读缓存”，不是覆盖 Redis 的全部功能。

## 先画出两条请求路径

```text
HTTP GET
  → ProductHttpServer 的当前 Worker
  → ProductService
  → ProductCache 接口
      → RedisProductCache：hiredis、RESP、MGET、pipeline
      → SphinxProductCache：ClusterClient、一致性哈希、memcached 文本协议
  → miss / cache error 时回到 MySqlProductStore
  → 尽力回填缓存
```

`ProductSharedState` 由 HTTP 进程持有，Worker 共享回源协调器、熔断器和指标；MySQL 连接、
Redis 客户端及 Sphinx 集群客户端仍由各 Worker 独占。MySQL 是权威数据，缓存项过期或整个
缓存丢失后都可以重建。当前设计没有跨服务进程的回源合并，也没有缓存副本或强一致性协议。

## 1. 从接口边界开始

阅读 [product_cache.h](../product-service/include/sphinx/product_cache.h)、
[product_cache_options.cpp](../product-service/src/product_cache_options.cpp) 和
[product_http.cpp](../product-service/src/product_http.cpp)。先确认商品服务依赖的只有
`get`、`put`、`erase`、`get_many`、`put_many`；环境配置选择实现，`ProductService` 不知道
正在访问哪个缓存协议。

练习：沿 `SPHINX_CACHE_BACKEND` 找到 `make_product_cache`，说明它改变的是缓存适配器，为什么
没有改变商品数据模型和 MySQL 写入流程。

## 2. 看一次 Redis 命令的完整生命周期

按顺序阅读 [redis_product_cache.h](../product-service/include/sphinx/redis_product_cache.h)、
[redis_product_cache.cpp](../product-service/src/redis_product_cache.cpp) 和
[redis_product_cache_test.cpp](../product-service/test/redis_product_cache_test.cpp)。重点跟踪：

- 客户端在 Worker 首次请求时才建立连接；一次客户端不能跨线程共享。
- `GET` 将 Nil 映射成 miss，空字符串保留为空值；缓存值是有长度的字节序列。
- `SET key value EX ttl` 写入相对过期时间；`DEL` 的 0 和 1 都代表删除成功。
- AUTH 与 SELECT 在连接建立后发送；连接断开后丢弃旧句柄，后续独立操作再连接。
- Redis 命令错误与无效协议回复是缓存故障，交给业务层回源；不把错误翻译成不存在。

练习：在协议测试中找一个包含二进制字节的 value、一个 Nil 回复和一个错误回复，沿 hiredis
的 argv 调用观察请求参数与结果分类。

## 3. 把缓存旁路流程连回权威数据

阅读 [product_service.cpp](../product-service/src/product_service.cpp)、
[mysql_product_store.cpp](../product-service/src/mysql_product_store.cpp) 与
[product_http_routes.cpp](../product-service/src/product_http_routes.cpp)。普通 GET 先查缓存；
miss 或 cache error 回源 MySQL；成功商品和真实 NotFound 可按策略回填；MySQL 错误不得缓存成
NotFound。`fresh=1` 跳过缓存，作为直接读取权威值的入口。

PUT 的顺序是 MySQL 事务和版本检查成功提交，再删除对应缓存 key。检查
`expected_version`、`CommitUnknown` 和 `X-Cache-Invalidation`：结果不确定时不能假设提交失败，
也不能盲目重试或提前删除缓存。

练习：分别让缓存不可达、MySQL 不可达，再对照 `X-Cache`、HTTP 错误和 `/metrics`。回答哪个
错误会回源，哪个错误能被缓存命中遮住，哪种情况绝不能回答 NotFound。

## 4. 理解 key、值和过期

阅读 [product_codec.cpp](../product-service/src/product_codec.cpp) 与
[product_cache_options.h](../product-service/include/sphinx/product_cache_options.h)。商品 key
使用 `product:v3:<id>`；缓存值严格校验 ID、字段集合、JSON 类型和大小。protected 模式用
短 TTL 表示真实不存在，并用商品 ID 的稳定散列分散正缓存过期时间；basic 模式忽略负缓存。

练习：分别改变正 TTL、TTL 抖动与负 TTL，读代码计算裁剪边界；运行严格验收观察正值和负值
到期后重新回源。区分“Redis 过期”和“应用认为数据变新”：TTL 只限制缓存数据的最长存活，
不提供事务一致性。

## 5. 比较批量操作的往返成本

阅读 `RedisProductCache::get_many` / `put_many` 和
[sphinx_product_cache.cpp](../product-service/src/sphinx_product_cache.cpp)。Redis 对一批 key
使用一次 `MGET`，回填用 pipeline 发送多个 `SET`；Sphinx 按一致性哈希先把 key 分组，再向每个
相关节点发送一次 multi-get。pipeline 降低往返次数，但多个 `SET` 不组成事务；Sphinx 批量读也
可能跨多个节点。

练习：分别请求 1、8、32 项；检查重复 ID 是否只读一次并按输入顺序返回，比较商品层
`cache_lookup_keys`、`store_read_operations` 和 HTTP 延迟。不要把批量条目数当作网络请求数。

## 6. 看热点保护和故障降级

阅读 [product_read_coordinator.h](../product-service/include/sphinx/product_read_coordinator.h)、
[product_read_coordinator.cpp](../product-service/src/product_read_coordinator.cpp)、
[cache_circuit_breaker.h](../product-service/include/sphinx/cache_circuit_breaker.h) 和
[cache_circuit_breaker.cpp](../product-service/src/cache_circuit_breaker.cpp)。Coordinator 合并
同进程相同 key 的 miss，并限制 flight 容量、MySQL 并发及 follower 等待时间；breaker 保护
缓存读、损坏项清理和回填。缓存熔断时仍回 MySQL，但 MySQL 读仍受并发准入限制。PUT 提交后
删除继续尝试，不参加读熔断，也没有后台重试队列。

练习：运行 Redis 停机/恢复测试与 protected 热点测试；观察 `read_leaders`、`read_followers`、
`read_admission_rejected`、`cache_circuit_bypasses`。再检查跨服务进程的边界：两个进程各有自己
的一份协调器，因此这不是分布式 single-flight。

## 7. 读观测数据而不是猜测

访问 `GET /metrics`。它只读取进程内原子计数和协调器/熔断器快照，不会创建 Worker、连接
MySQL 或访问缓存。延迟使用固定桶，不能当作精确的 P50/P99；实验端应记录每个 HTTP 请求的
耗时，再计算分位数。应用 `store_read_operations` 是调用次数，不保证每次都已执行 SQL；有权限
时再用 Performance Schema 对照数据库语句数。

练习：比较 `cache_lookup_keys`、`cache_misses` 和 `store_read_operations`。protected leader
二次检查会增加一次缓存读取；重复输入只计一次唯一 ID；一次失败的批量缓存操作按一个操作
计数。

## 8. 做可重复的对照实验

先运行 [verify_product_cache.sh](../scripts/verify_product_cache.sh)，确认 Sphinx/Redis 与
basic/protected 四种组合共用同一 HTTP 业务测试。再运行
[compare_product_cache.py](../scripts/compare_product_cache.py)，固定 MySQL、商品数量、请求
并发、批量大小、TTL 和缓存内存预算，保存每次请求样本、服务指标及进程 CPU/RSS。

先比较一个 Sphinx 节点和一个 Redis 实例；多 Sphinx 节点作为单独实验。报告必须说明 Redis
淘汰策略、Sphinx 内存段限制、机器与请求分布。只报告观察到的结果，不预设哪种后端更快。

## 本次代码没有覆盖的主题

本项目使用 Redis String、key/TTL、RESP、AUTH/SELECT、MGET、pipeline、INFO、内存和淘汰观察。
Hash/List/Set/ZSet、Streams、Pub/Sub、Lua、事务命令、分布式锁、复制、Sentinel、Cluster 和持久化
调优适合之后独立学习，不需要为了覆盖名词加入商品服务代码。Redis 不是此项目的事实存储；
丢弃缓存不应丢失商品数据。
