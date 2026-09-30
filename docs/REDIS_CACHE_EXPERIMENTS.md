# Redis 与 Sphinx 对照实验记录

这份记录保存一次可重复运行的本机基线。它用于核对请求路径、保护策略和内存行为，不代表生产容量或跨机器性能结论。

## 环境与条件

- 时间：2026-10-01；WSL2 Ubuntu 26.04，Linux 6.18.33.2，x86_64，14 个逻辑 CPU。
- 组件：MySQL 8.4.11、Redis 8.0.5、Sphinx `v0.1.0-9189822`。
- 主实验：每个组合 512 个商品、每个场景 100 个 HTTP 请求、客户端并发 8、HTTP Worker 8；缓存预算 64 MiB，基础 TTL 30 秒。
- 矩阵：Sphinx/Redis × basic/protected；分别以 Redis `allkeys-lru` 与 `noeviction` 运行，每个组合重复 3 次。
- 内存压力子实验：独立启动 8 MiB 缓存实例，写入 64 个 256 KiB、60 秒 TTL 的合成值；写后扫描 key 并采集两端缓存统计。合成值用于制造可观察压力，不代表商品值大小。
- MySQL 使用隔离临时库；Performance Schema 可读。原始每请求延迟样本、计数增量、缓存统计及进程快照保存在 JSON 中。

## HTTP 观察

下表的 P95 是每轮 100 个请求计算出的 P95，再对六轮结果取中位数；范围列显示六轮的最小值与最大值。六轮来自两个 Redis 淘汰策略各三轮。

| 后端/策略 | 单项全命中 P95（中位数；范围） | 32 项批量全命中 P95（中位数；范围） | 100 次热点不存在的 MySQL 读数 | 热点过期后的 MySQL 读数/轮 |
| --- | ---: | ---: | ---: | ---: |
| Sphinx / basic | 2.816 ms（2.598–3.188） | 3.015 ms（2.842–3.496） | 100 | 6–8 |
| Redis / basic | 2.764 ms（2.484–3.178） | 2.904 ms（2.750–3.205） | 100 | 5–8 |
| Sphinx / protected | 2.696 ms（2.491–2.917） | 3.025 ms（2.709–3.327） | 0 | 1 |
| Redis / protected | 2.894 ms（2.447–3.013） | 2.841 ms（2.568–3.493） | 0 | 1 |

单项与批量命中延迟范围明显重叠，这组 WSL 本机样本不支持“某个后端更快”的结论。protected 的负缓存让热点不存在请求在 5 秒 TTL 内不再访问 MySQL；热点过期后，进程内协调把回源压到每轮 1 次。basic 没有负缓存或回源合并承诺。

MySQL 读数来自 `performance_schema.prepared_statements_instances.COUNT_EXECUTE`，按测试用户和 schema 过滤，并排除 `SELECT ... FOR UPDATE`。如果预处理语句在统计窗口中被替换，造成执行计数不稳定，JSON 对应场景留空；例如 `partial_batch_8` 不用应用层调用数冒充 SQL 数。

## 内存压力观察

| 实现/策略 | 写入接受 | 写入拒绝 | 保留 key | Redis `evicted_keys` 增量 |
| --- | ---: | ---: | ---: | ---: |
| Sphinx（两种业务策略） | 64 | 0 | 22 | 不适用 |
| Redis `allkeys-lru` | 64 | 0 | 22 | 42 |
| Redis `noeviction` | 22 | 42 | 22 | 0 |

每种结果在三次重复中相同。Sphinx 接受新值并循环回收旧 segment；它没有精确的回收字节指标，因此用写入结果、保留 key 数和 `get_hits/get_misses` 观察。Redis `allkeys-lru` 在继续接受写入的同时淘汰旧 key；`noeviction` 到达 `maxmemory` 后拒绝写入。保留数相同不表示两种引擎具有相同的商品容量：Redis 与 Sphinx 的元数据开销、内存计量和回收单位不同。

## 原始结果与复现

- [Redis allkeys-lru 原始 JSON](redis-cache-results/product-cache-comparison-20261001-034318.json)
- [Redis noeviction 原始 JSON](redis-cache-results/product-cache-comparison-20261001-034354.json)

复现时先准备独立测试数据库并设置 `SPHINX_TEST_MYSQL_*`，再运行以下命令；把 `--redis-policy` 分别设为 `allkeys-lru`、`noeviction`：

```bash
python3 scripts/compare_product_cache.py \
  --build-dir build --products 512 --requests 100 --concurrency 8 \
  --cache-memory-mb 64 --pressure-cache-memory-mb 8 \
  --pressure-entries 64 --pressure-value-kib 256 \
  --redis-policy allkeys-lru --repeat 3 \
  --output-dir docs/redis-cache-results
```

要形成更稳的性能结论，应在目标机器上提高请求量和重复次数，分别报告数据库实例、缓存配置、TTL、淘汰策略、数据分布、并发和原生批量往返差异；不要从本地基线外推生产容量。
