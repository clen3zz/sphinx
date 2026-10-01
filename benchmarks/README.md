# 商品 HTTP 与直接缓存路径实验

两组实验使用 Release 构建和固定 CPU 分区。HTTP 组测试单项命中、32 项批量命中、
`fresh=1`；直接组通过现有缓存适配器测试 GET、SET、32 项读取和 32 项回填。
直接组不经过 HTTP 或 MySQL，但仍包含客户端、协议、网络和返回值校验，不是引擎内部微基准。

## 运行

从仓库根目录执行，需要本机 `mysqld`、MySQL 命令行客户端和 Redis，以及至少 8 个可用逻辑 CPU。
脚本自行创建隔离 MySQL 数据目录和测试账号，结束时清理，不使用现有业务数据库。

```bash
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_BENCHMARKS=ON
cmake --build build-release -j4
python3 scripts/benchmark_cache_paths.py \
  --build-dir build-release \
  --redis-server /usr/bin/redis-server \
  --output-dir out/cache-benchmark
```

输出目录须没有已有 `samples.json`，防止覆盖实验。非系统安装的 Redis 若需要动态库路径，
应在调用前设置 `LD_LIBRARY_PATH`。默认参数是 4096 个 key、直接组 256 字节 value、
并发 1/4/16/64、每次预热 1 秒及测量 5 秒、每个条件重复 3 次，共 168 次采样。

## 比较口径

- 两个缓存各使用同一个逻辑 CPU；Sphinx 一个 Worker，Redis 一个 I/O 线程。
- HTTP 服务固定 4 个 Worker、2 个逻辑 CPU；MySQL 单独使用 2 个逻辑 CPU。
  客户端使用其余 CPU 中除观察进程所用最后一个 CPU 外的部分。
- HTTP 采用每请求连接，包含 TCP 建连和关闭成本；固定线程池在等待长连接下一次请求时
  仍占用 Worker，因此不能用超过 Worker 数的空闲长连接完成相同并发扫描。直接缓存组
  使用每客户端独占的长连接；商品服务到缓存和 MySQL 的连接也保持复用。
- 两个缓存内存预算均为 64 MiB，TTL 为 300 秒，数据集全部预热；Redis 关闭持久化，
  使用 noeviction。数据集小于预算；写入过程中 Sphinx 的日志回收属于实际写路径成本。
- HTTP 组固定 basic 策略。命中场景要求实际数据库 SELECT 为零；fresh 场景要求
  SELECT 次数与成功请求数一致。fresh 跳过缓存读取，但仍回填缓存，不是纯 MySQL 模式。
- 直接组 GET 校验 value，批量读校验长度与全部 value；写操作要求客户端确认成功。
  HTTP 预检校验数据与批量形状，测量期间校验状态码和 X-Cache，另核对服务指标和实际 SQL。
- 并发按客户端线程数计算，每个线程最多一个在途操作，属于闭环负载。完成测量后排空
  在途操作，吞吐分母包含排空时间；不是指定到达率的开放负载或严格 SLA 容量测试。
- 32 项批量吞吐同时记录 API 操作数和 item 数。Redis 批量写使用 pipeline，Sphinx
  使用逐条 set，差距包含不同往返次数，不能全部归因于存储引擎。
- 测量前后读取 `/proc` 得到缓存、HTTP、MySQL 的 CPU 时间和 RSS，缓存 CPU 核数为
  CPU 秒数除以实际墙钟秒数；负载程序另记录测量阶段自身的 CPU 时间。
- 条件顺序与每对后端顺序按固定随机种子打乱；每次采样独立启动服务，避免累计状态偏差。

## 结果

`samples.json` 持续保存原始样本、条件、环境与源码摘要；`complete=true` 才表示全套完成。
`SUMMARY.md` 展示每个条件三次采样的吞吐中位数、最小～最大范围，以及 P50/P99 中位数。
分位数中位数不等于合并所有请求后的分位数。不能用不同并发下的最高吞吐和最低延迟
拼成一个配置的性能结论。

WSL CPU 亲和性只限制客体逻辑 CPU，不能排除宿主机调度、功耗与其他应用影响。
本实验适合比较当前本机配置下的完整路径；跨机器网络、缓存淘汰、复制、持久化与 Sphinx
多 Worker 扩展需要另设条件，不能从单核结果外推。
