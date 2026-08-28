# Sphinx 端到端基准

本文件只记录由 `scripts/run_local_benchmark.py` 保存的 memtier 原始 JSON， 不把微基准耗时或手工压测输出当作端到端性能结果。QPS
和延迟没有预设门槛， 不同机器、不同提交的数字也不直接用来判断功能回归。

## 当前状态

截至 2026-08-24，脚本单元测试和官方 `memtier_benchmark` 2.5.1 冒烟均已通过。 同一源码状态下的 read、mixed、write 已分别完成三次
20 秒正式运行，共保存 9 份 原始 JSON 和 9 份 metadata。九次运行的协议、连接和其他错误计数均为 0。

本批结果的 `source_state_id` 为 `88ca18e34d078a95`。运行时工作树尚未提交， metadata 因此同时记录 `git_commit=11a75a5` 和
`git_dirty=true`，不能只用提交号 替代源码状态标识。

## 固定配置

正式基线需要对每种工作负载分别运行三次。每次运行都会生成一份原始 JSON 和一份 同名 `.metadata.json`，结果文件名包含 UTC
时间、负载名和进程标识：

metadata 同时记录 `git_commit`、`git_dirty` 和 `source_state_id`。因此即使工作树 尚未提交，也能知道结果对应的源码状态；正式基线不要求为了运行脚本先创建提交。

| 参数                          |  固定值 |
|-------------------------------|--------:|
| 服务线程                      |       4 |
| 缓存内存                      |  64 MiB |
| Segment                       |   2 MiB |
| Key space                     | 100,000 |
| Value size                    |   256 B |
| Client threads                |       4 |
| Connections per client thread |       8 |
| 测量时间                      |    20 s |
| 重复次数                      |       3 |

三个负载的含义分别是：

- `read`：先用 SET 预填充，再只执行 GET；
- `mixed`：先预填充，再按 90% GET、10% SET（memtier 的 `1:9` SET:GET）运行；
- `write`：只执行 SET，不做预填充。

read/mixed 的预填充使用 memtier 的 `--requests allkeys`，配合完整 key range 和 单个客户端逐键写入，因此不依赖固定秒数，也会检查预填充
JSON 中的协议、连接和 其他错误计数。

## 运行命令

先构建并确认功能测试通过，然后为每种负载执行三次。下面的命令把结果直接 保存到仓库的 `benchmark-results/`；重复执行同一命令不会覆盖已有
JSON。

```bash
cmake --build build -j"$(nproc)" --target sphinxd
ctest --test-dir build --output-on-failure

MEMTIER=/path/to/memtier_benchmark
export LD_LIBRARY_PATH="$(dirname "$MEMTIER")/../lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

for workload in read mixed write; do
  for run in 1 2 3; do
    python3 scripts/run_local_benchmark.py \
      --server ./build/sphinxd/sphinxd \
      --memtier "$MEMTIER" \
      --output-dir benchmark-results \
      --workload "$workload" \
      --threads 4 \
      --memory-limit 64 \
      --segment-size 2 \
      --duration 20 \
      --client-threads 4 \
      --connections 8 \
      --key-space 100000 \
      --value-size 256
  done
done
```

正式运行前可以用 `--dry-run` 查看将执行的服务端、预填充和 memtier 命令； dry-run 不启动进程，也不创建结果目录。脚本会拒绝非法参数、等待服务
TCP 端口可连接，并在成功、失败或中断时只停止自己创建的服务进程。

## 正式结果

QPS 的单位是 operations/s，延迟的单位是 ms。“错误”是协议、连接和其他错误之和。 每行证据链接到对应 metadata，metadata 中保存原始
JSON 文件名、完整命令和参数。

| 负载  | Run |       QPS |   P50 |   P95 |   P99 | 错误 | 证据                                                                                |
|-------|----:|----------:|------:|------:|------:|-----:|-------------------------------------------------------------------------------------|
| read  |   1 | 74,406.71 | 0.415 | 0.735 | 0.927 |    0 | [metadata](../benchmark-results/20260824-075457-read-61335-7ba7976f.metadata.json)  |
| read  |   2 | 72,853.55 | 0.415 | 0.743 | 0.935 |    0 | [metadata](../benchmark-results/20260824-075537-read-62229-2efcccb4.metadata.json)  |
| read  |   3 | 73,844.37 | 0.407 | 0.719 | 0.887 |    0 | [metadata](../benchmark-results/20260824-075616-read-61334-7d2a5156.metadata.json)  |
| mixed |   1 | 73,142.36 | 0.415 | 0.719 | 0.879 |    0 | [metadata](../benchmark-results/20260824-075714-mixed-62352-58cbd476.metadata.json) |
| mixed |   2 | 73,447.12 | 0.415 | 0.719 | 0.903 |    0 | [metadata](../benchmark-results/20260824-075751-mixed-62437-63b40f91.metadata.json) |
| mixed |   3 | 75,577.14 | 0.415 | 0.719 | 0.895 |    0 | [metadata](../benchmark-results/20260824-075827-mixed-62351-8a9fa3a9.metadata.json) |
| write |   1 | 76,623.90 | 0.407 | 0.703 | 0.879 |    0 | [metadata](../benchmark-results/20260824-075918-write-62574-93976f4c.metadata.json) |
| write |   2 | 76,024.96 | 0.407 | 0.663 | 0.815 |    0 | [metadata](../benchmark-results/20260824-075938-write-62609-3ad18933.metadata.json) |
| write |   3 | 74,478.23 | 0.423 | 0.703 | 0.895 |    0 | [metadata](../benchmark-results/20260824-075958-write-62573-350d21ad.metadata.json) |

每个指标分别取三次运行的中位数，不挑选单次最好结果：

| 负载  | QPS 中位数 | P50 中位数 | P95 中位数 | P99 中位数 |
|-------|-----------:|-----------:|-----------:|-----------:|
| read  |  73,844.37 |      0.415 |      0.735 |      0.927 |
| mixed |  73,447.12 |      0.415 |      0.719 |      0.895 |
| write |  76,024.96 |      0.407 |      0.703 |      0.879 |

运行环境如下：

| 项目     | 值                                                              |
|----------|-----------------------------------------------------------------|
| CPU      | 13th Gen Intel(R) Core(TM) i5-13490F                            |
| 逻辑 CPU | 16                                                              |
| 内存     | 16,264,848 KiB                                                  |
| 操作系统 | Ubuntu 24.04.4 LTS                                              |
| 编译器   | GCC 13.3.0                                                      |
| 构建类型 | Release                                                         |
| Git 状态 | `11a75a5`，`git_dirty=true`，`source_state_id=88ca18e34d078a95` |
| memtier  | 2.5.1，memcache_text                                            |

客户端和服务端运行在同一台机器上，结果会受到客户端 CPU、后台进程和 WSL 调度的影响。这些数字只用于本项目的可复现观察，不代表生产容量、稳定性或跨
机器比较结论。
