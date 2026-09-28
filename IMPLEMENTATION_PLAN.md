# MySQL + Sphinx 商品详情服务：架构与逐项实现计划

## 0. 需求基线与已经固定的选择

本次扩展以“按 ID 查询与修改商品详情”为具体场景：MySQL 8.4/InnoDB 是唯一权威数据源，Sphinx 只保存可丢弃的商品详情缓存。`GET /products/{id}` 走 cache-aside；`PUT /products/{id}` 使用 MySQL 乐观版本更新，提交后删除缓存。只更新已有商品，不新增商品；商品预置/迁移由数据库运维完成。此场景让接口、事务和一致性边界可检验，后续不能把 MySQL 当作 Sphinx 的持久化层。

当前项目的 `sphinxd/src/sphinxd.cpp` 在每个服务器线程创建 `Server`、Reactor 与内存 Log；`Server` 在节点内按 key hash 分发；`ClusterClient` 在客户端按一致性哈希环选节点，内部复用有状态 TCP 连接。Log 采用 mmap 内存段，过期为逻辑失效，段淘汰可能提前删除仍有效的 key。集群没有节点间复制/故障转移。因此业务进程使用缓存失效后回源，不能依赖 Sphinx 保存权威数据或提供强一致事务。

已经完成并应保持稳定：

- `ClusterClient::set(key, value, ttl_seconds)`：只接受 0..2,592,000 秒；0 与旧 API 行为相同。服务配置必须是 1..2,592,000 秒。超过 30 天会被 Sphinx 当成 UNIX 绝对时间，禁止传入。
- `product.h`、`product_store.h`、`product_cache.h`、`product_codec.h`、`product_service.h`、`sphinx_product_cache.h` 的 public API；`product_codec.cpp`、`product_service.cpp`、`sphinx_product_cache.cpp` 已实现。
- 默认构建只增加 `sphinx_product_core` 与其测试；`BUILD_MYSQL_SPHINX_DEMO=OFF`，原 `sphinxd`、协议、存储和集群路由不变。

禁止在后续任务中修改上述 public API，除非实测证明存在无法实现的错误并先单独提交设计修订。不要把同步 MySQL 调用放进 `sphinxd` 的 Reactor 线程，不要在 Sphinx 节点之间加入“同步副本”来解决本场景。

## 1. 模块、依赖和数据流

| 模块 | 文件 | 职责 | 依赖 |
| --- | --- | --- | --- |
| Sphinx 协议客户端 | `sphinxd/include/sphinx/cluster_client.h`、`sphinxd/src/cluster_client.cpp` | 路由、连接复用、Memcached 命令、相对 TTL | 原有 `sphinx_core` |
| 领域模型/编解码 | `examples/mysql_sphinx/include/sphinx/product*.h`、`src/product_codec.cpp` | 商品字段约束、`product:v1:{id}`、稳定二进制值 | C++17 标准库 |
| 业务服务 | `src/product_service.cpp` | cache-aside、读主库、乐观更新后失效、错误归类 | 抽象 `ProductStore`、`ProductCache` |
| Sphinx 缓存适配器 | `src/sphinx_product_cache.cpp` | 把 `ClusterClient` 结果/异常映射到 `ProductCache` | `sphinx_core` |
| MySQL 适配器 | `src/mysql_product_store.cpp` | 每线程一连接、预处理语句、事务、错误分类 | MySQL C API；仅可选目标 |
| HTTP 工作池 | `src/product_http.cpp` | 固定线程池、每线程对象生命周期、启动/停止 | cpp-httplib、MySQL/缓存适配器 |
| HTTP 路由 | `src/product_http_routes.h/.cpp` | HTTP/JSON 校验与结果映射 | cpp-httplib、nlohmann/json、`ProductService` |
| 进程入口 | `src/product_main.cpp` | 环境配置、信号控制、退出码 | `ProductHttpServer` |

依赖方向只能是 HTTP → service → store/cache 抽象 → 适配器。适配器之间不能相互调用。`sphinxd` 对这些新模块零依赖。

读流程：HTTP worker → `ProductService::get` → Sphinx GET → 命中且有效则返回；缺失/损坏/缓存故障时查 MySQL 主库 → 读到有效行后 best-effort 写回 Sphinx → 返回。不存在行时不写负缓存。`fresh=1` 跳过缓存读，直接查主库，供不确定提交后的人工/客户端核对。

写流程：HTTP worker → `ProductService::update` → MySQL 事务内锁行、比较版本、更新、提交 → Sphinx DELETE → 返回。DELETE 失败仍返回已提交的成功结果，并以响应头提示。提交结果不明时绝不能自行重试或删缓存。

## 2. 数据模型和必须保持的 invariant

- `Product.id > 0`，`version > 0`，`price_cents <= 1,000,000,000,000`；`name` 是 1..128 **字节**有效 UTF-8，不含 C0/C1 控制码。HTTP、MySQL 行、缓存值在边界上都要校验。MySQL schema 施加相同检查；若旧数据库不符合，`find` 报 `InvalidData`，不可误判为不存在。
- `UpdateProductRequest.expected_version` 在 1..`UINT64_MAX-1`；每次已确认成功的更新只增加 1。数据库行的 `version` 是唯一并发控制字段；缓存 `version` 仅供诊断，不作为冲突裁决。
- 缓存 key 精确为 `product:v1:<十进制正整数 ID>`。缓存值精确为 `SPC1` + 三个大端 u64（id、price_cents、version）+ 大端 u16 名称字节数 + 名称字节；长度必须完全相符，不接受额外字节。格式变化必须使用新 key 命名空间和 magic。
- 命中的缓存值还必须满足 payload.id == URL id；否则按损坏处理：尽力删除，然后查 DB。绝不把另一商品的缓存值返回给调用方。
- Sphinx 的 TTL 最多 30 天，默认 30 秒。Sphinx 可提前淘汰数据；未失效的旧值也可能在并发读写下出现。TTL 约束的是**单条缓存值从写入起的寿命**，不能推导出“提交后固定时间内全局变新”。Sphinx 使用墙上时钟计算过期，时钟回拨也会影响实际时长。不保证线性一致性或立即读到最新值。
- 已确认 COMMIT 之前不碰缓存；Conflict/NotFound/CommitUnknown 都不删缓存。已确认 COMMIT 后即使适配器返回行异常，也尽力删缓存，然后将返回视为内部错误。
- 任何 `ProductStore`/`ProductCache` 实例及其底层连接只属于创建它的工作线程。`ProductService` 借用二者，不能晚于二者析构；不需要在服务层加锁。不同工作线程可并发处理同一商品，MySQL 行锁与版本谓词决定结果。

## 3. 所有权、线程和停机

一个进程只创建一个 `ProductHttpServer`。其 `Impl` 保存配置、**先构造**的 `MySqlRuntime`、`httplib::Server`。`MySqlRuntime` 在任何工作线程启动前执行 `mysql_library_init`，在全部线程退出后执行 `mysql_library_end`。不要将 `MYSQL*` 放进 `Impl` 或全局变量。

设置 `server.new_task_queue = [n] { return new httplib::ThreadPool(n, n, 256); };`，其中 `n=worker_count`，范围 1..64；第二个 `n` 禁止动态扩容，256 是最多排队请求数。设置 `set_payload_max_length(65536)`，并设置 2 秒读/写超时。每个 HTTP worker 第一次处理请求时构造一个 `thread_local WorkerContext`，成员**按下面顺序声明**：`MySqlThreadGuard`、`MySqlProductStore`、`SphinxProductCache`、`ProductService`。析构逆序保证 service 先于连接，thread guard 最后。`MySqlProductStore` 构造时只保存/验证配置，不连接；`find`/`update` 第一次使用时建立连接，因而 MySQL 暂时不可用时已命中的缓存仍可服务。`current_service()` 回调只返回当前 worker 的引用，路由函数不得缓存这个引用跨线程使用。

`stop()` 只唤醒/停止监听，不在 POSIX signal handler 中调用；`serve()` 在 worker 全部退出后返回，随后才能析构 `Impl`/`MySqlRuntime`。使用 `Impl::state_mutex` 同步启动与停止：`serve()` 持锁检查 `stopping` 并调用 `bind_to_port`，放锁后才进入 `listen_after_bind`；`stop()` 持同一锁设置 `stopping=true` 并调用 `server.stop()`。这样“停止先于绑定”不会遗漏，绑定后停止会关闭已持有的监听 fd。真正绑定失败返回 false；主动停止返回 true。入口进程在启动线程前屏蔽 SIGINT/SIGTERM，单独的控制线程用 `sigwait` 接信号并调用 `stop()`；如果 `serve()` 因绑定失败先返回，要唤醒并 join 控制线程，避免悬挂。重复 `stop()` 必须安全。禁止通过 detached 线程延长任何连接寿命。

入口的具体控制流程：`pthread_sigmask(SIG_BLOCK, {SIGINT,SIGTERM})` → 创建 server → 启动一个继承此 mask 的控制线程，循环 `sigwait` 直到收到目标信号，然后仅在 `done=false` 时调用 `server.stop()` → 主线程调用 `serve()` → 返回后设 `done=true`，用 `pthread_kill(control.native_handle(), SIGTERM)` 唤醒可能仍在 `sigwait` 的控制线程（已退出时允许返回 `ESRCH`）→ `join()` → 恢复原 signal mask → 根据 `serve()` 结果退出。构造 server 抛异常时尚未启动控制线程，直接恢复 mask 并返回错误。

## 4. MySQL 适配器：逐句实现规则

只在 `.cpp` 引入 `<mysql.h>`。使用 `mysql_options` 设置连接/读/写超时，禁用自动重连；TCP 连接到 `MySqlOptions.host:port`，`mysql_set_character_set(..., "utf8mb4")`。首次业务调用或上次连接被丢弃后的下一次业务调用才建连接。每个连接保存或按需创建三个数据预处理语句：

```sql
SELECT id, name, price_cents, version FROM products WHERE id = ?
SELECT id, name, price_cents, version FROM products WHERE id = ? FOR UPDATE
UPDATE products SET name = ?, price_cents = ?, version = version + 1
 WHERE id = ? AND version = ?
```

所有用户数据只通过 `MYSQL_BIND` 绑定，整数用无符号 64 位绑定，名称用显式字节长度绑定。结果名称缓冲 129 字节，记录返回长度、NULL 与 truncation；超过 128 字节、NULL、非法 UTF-8、非法域值是 `StoreError(InvalidData)`。每次使用完语句都调用 `mysql_stmt_free_result`/`mysql_stmt_reset` 或等价 RAII 清理，避免下一次请求的结果串入。无行仅在成功执行并读取到 `MYSQL_NO_DATA` 时返回 `nullopt`。`find` 是对主库的单条自动提交读取，不使用只读副本。

`update` 的**唯一允许算法**：

1. 检查请求域值。在同一 `MYSQL*` 上执行固定的 `START TRANSACTION`；设 `transaction_active=true`。
2. 执行带 `FOR UPDATE` 的预处理 SELECT。无行：`mysql_rollback`，返回 NotFound。读到行但版本不等于 `expected_version`：rollback，返回 Conflict。若数据库版本为 `UINT64_MAX`，rollback 并抛 InvalidData。
3. 绑定新名称、价格、id、预期版本，执行带版本谓词的 UPDATE。`mysql_stmt_affected_rows` 必须恰为 1；0 或 >1 是内部不变量失败，rollback，抛 Unexpected。任何数据 SQL 错误都先尽力 rollback。
4. **只调用一次** `mysql_commit`。成功响应后 `transaction_active=false`，返回 Updated 和 `{id, request.name, request.price_cents, expected_version+1}`。此时才允许上层删缓存。
5. 若 `mysql_commit` 返回失败，不论本地猜测是否已执行，都关闭语句/连接、抛 `StoreError(CommitUnknown)`；不再 rollback、不在本次请求重新连接/重试。客户端可通过 `GET ...?fresh=1` 从主库观察版本，但紧随错误的一次旧版本读取**不能证明**那个 COMMIT 最终未生效；应等待并再次核对/由上层人工或幂等业务流程处理，不能盲目重复 PUT。

在 COMMIT 前，明确可归类的连接/超时/锁故障为 Unavailable；schema/语句编程错误为 Unexpected；坏行/数据越界为 InvalidData。rollback 失败时丢弃连接，仍保留原始错误类别。已关闭连接的下一次**独立**调用可以重连，不能在同一次调用里自动重放 UPDATE。异常字符串/日志不得包含密码、完整 SQL、用户输入。`mysql_library_init`/`mysql_thread_init` 的前后顺序见第 3 节。

## 5. HTTP、配置与错误契约

URL：`GET /products/{id}`、`PUT /products/{id}`，其中 id 是无符号十进制、完整解析、1..`UINT64_MAX`。`GET` 只允许无查询参数或精确的 `fresh=1`。`PUT` 必须有 `Content-Type: application/json`（可带 `; charset=utf-8`）和**恰好**三个 JSON 字段：`name` 字符串、`price_cents` 无符号整数、`expected_version` 无符号整数。拒绝重复/未知字段、浮点、负数、溢出、空名、非法 UTF-8、超出 128 字节的名称。请求体最大 65,536 字节；超出返回 413；错误 Content-Type 返回 415。所有结果加 `Cache-Control: no-store`。

解析 JSON 时使用 nlohmann/json 的 `parser_callback_t`，在每个 `parse_event_t::key` 上把键加入局部 `unordered_set<string>`，重复则直接拒绝；解析完再要求顶层对象恰有三个指定字段且没有嵌套对象。用完整整数类型检查和范围检查后才转为 `uint64_t`，不允许库的浮点或负数隐式转换。查询参数检查原始 query 字符串是否**恰好**为 `fresh=1`，以免参数容器合并重复参数。

成功 GET/PUT 均返回 200，body 为精确 `{ "id": <u64>, "name": <string>, "price_cents": <u64>, "version": <u64> }`。GET 增加 `X-Cache: HIT|MISS|BYPASS|CORRUPT`；若参数/路径检查未进入服务则为 `NOT_CHECKED`。已提交 PUT 的缓存删除失败仍 200，并加 `X-Cache-Invalidation: failed`；不暴露缓存异常文本。

| `ProductStatus`/边界错误 | HTTP | JSON body |
| --- | ---: | --- |
| InvalidArgument、无效 URL/查询/JSON/域值 | 400 | `{"error":"invalid_argument"}` |
| NotFound | 404 | `{"error":"not_found"}` |
| Conflict | 409 | `{"error":"conflict"}` |
| StoreUnavailable、连接主库失败 | 503 | `{"error":"store_unavailable"}` |
| CommitUnknown | 503 | `{"error":"commit_unknown"}` |
| InternalError、线程初始化/非预期异常 | 500 | `{"error":"internal_error"}` |
| Body > 65,536 字节 | 413 | `{"error":"payload_too_large"}` |
| 不支持的 Content-Type | 415 | `{"error":"unsupported_media_type"}` |

未匹配的其他路径/方法遵循 httplib 的 404/405 行为，但不得触碰数据库。HTTP 边界只输出固定错误码，不返回原始异常/SQL/密码；日志最多记录类别、商品 id、缓存是否失效，不能记录密码或完整请求体。无自动 HTTP 重试，尤其是 `commit_unknown`。

入口环境变量：`SPHINX_MYSQL_USER`、`SPHINX_MYSQL_PASSWORD`、`SPHINX_MYSQL_DATABASE` 必须存在；`SPHINX_MYSQL_HOST` 默认 `127.0.0.1`、`SPHINX_MYSQL_PORT` 默认 3306；`SPHINX_CACHE_NODES` 默认 `127.0.0.1:11211`；`SPHINX_HTTP_BIND` 默认 `127.0.0.1`、`SPHINX_HTTP_PORT` 默认 8080；`SPHINX_HTTP_WORKERS` 默认 4；`SPHINX_CACHE_TTL_SECONDS` 默认 30。超时使用头文件默认值（MySQL 2 秒，Sphinx 200 毫秒）。全部数字用 `from_chars` 完整解析并限制到头文件域；绝不输出密码或包含密码的异常。示例服务默认只绑定本地回环地址；开放到网络前由部署层配置鉴权/TLS，本示例 HTTP API 自身没有用户身份模型。

## 6. 一致性、可用性及明确限制

- 缓存坏值：尽力删除并回源；删失败也回源。缓存读/写故障：不影响已有的 MySQL 读写结果。DB miss 时绝不写负缓存。
- MySQL 故障：缓存命中可继续返回已有值；缓存 miss 或 `fresh=1` 返回 503。Sphinx 故障：直接读 MySQL，写操作照常提交，失效失败通过响应头暴露。
- 同一 id 的两个并发 PUT：MySQL 行锁串行化；相同 `expected_version` 只能有一个成功，另一个 Conflict。
- 读写竞态：GET 可能在 PUT 提交前读到旧行，并在 PUT 删缓存后才写入旧缓存。这是允许的。旧条目通常在**回填后**相对 TTL 到期；若旧 GET 被任意长时间暂停，提交到最后一次旧回填的间隔也可能任意长，且服务器时钟回拨会影响到期。若需要从 COMMIT 起严格有界或强一致，必须另立需求设计版本栅栏/锁或读主库策略，不能让实现 Agent 临时加一个局部锁。
- `ClusterClient` 节点环是静态的；增删节点会改变路由并导致命中率下降，回源会恢复。Sphinx 内存淘汰可早于 TTL。首次版本不做 singleflight、负缓存、CDC/binlog、Raft、Redis、MySQL 复制或跨进程分布式锁。

## 7. 验证矩阵

| 层 | 正常路径 | 边界/错误 | 并发/兼容 |
| --- | --- | --- | --- |
| `ClusterClient` TTL | 精确检查 Memcached `set` 帧 | >30 天 I/O 前拒绝，0 旧行为 | 旧 CLI 与全部原测试通过 |
| 编解码 | golden 大端帧、中文 UTF-8 | 截断、尾字节、错误版本/名称/ID/价格 | 旧 Sphinx key 不受影响 |
| `ProductService` | HIT、MISS+回填、PUT 成功、fresh 读主库 | 坏缓存、缓存/DB 故障、404/409/commit unknown、删除失败 | 独立 worker 的旧读延迟回填竞态，TTL 限制 |
| MySQL adapter | 主库读取、事务更新和版本递增 | 无行、坏行、超时、SQL 错误、rollback、模拟 COMMIT 响应丢失 | 两线程各一连接同时改同一版本，只成功一次 |
| HTTP | GET/PUT JSON 与响应头 | id/JSON/类型/大小边界、503/500、启动失败 | 并发请求不共享连接，原 `sphinxd` 网络测试继续通过 |

默认 `cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build --output-on-failure` 不依赖 MySQL。可选目标使用 CMake 中固定的 cpp-httplib v0.56.0、nlohmann/json v3.12.0 与 `pkg-config mysqlclient`；需要本机 MySQL C 开发包和首次构建下载第三方源码。MySQL/HTTP 集成测试只可对**一次性测试数据库**运行；缺测试凭据时明确 skip，不能连接生产库。后续 Agent 完成对应实现时要移除各测试文件中的无条件 skip，不能只靠“可编译”验收。

## 8. 后续 coding agent 任务（按依赖顺序）

所有任务共同约束：遵守 `docs/CODING_STANDARDS.md`，只修改本任务列出的文件；不得修改 `examples/mysql_sphinx/include/sphinx/*.h`、`sphinxd/include/sphinx/cluster_client.h` 或任何既定 public API；不得往 `sphinxd` Reactor 增加 MySQL/HTTP 依赖；不产生新的编译 warning。遇到接口无法满足实现时暂停该任务并报告具体矛盾，不自行扩大 API。

### Task 1：MySQL 线程与连接生命周期

修改：`examples/mysql_sphinx/src/mysql_product_store.cpp`。实现 `MySqlRuntime`、`MySqlThreadGuard`、`MySqlProductStore::Impl`、构造/析构以及私有连接与预处理语句 RAII；`find/update` 暂保持原签名骨架。不得修改头文件、CMake 或业务服务。验收：可选目标编译；初始化顺序可通过一次性测试记录验证；一个 worker 只开自己的连接；构造 store 不连接；连接/语句关闭次序正确；凭据不进入异常文本。

### Task 2：MySQL 单行读取

依赖 Task 1。修改：`examples/mysql_sphinx/src/mysql_product_store.cpp`。实现 `MySqlProductStore::find()` 及其本文件内的绑定/解码辅助函数。不得修改 public API 或 `update()` 的事务流程。验收：integration test 读到已有/缺失 id；非法行是 InvalidData，断连是 Unavailable；一次请求不自动重放；恢复连接只发生于下一独立请求。使用主库与预处理语句，测试 64 位边界、UTF-8/NULL/truncation。

### Task 3：MySQL 乐观事务更新

依赖 Task 2。修改：`examples/mysql_sphinx/src/mysql_product_store.cpp`。实现 `MySqlProductStore::update()`，严格翻译第 4 节 5 步事务。不得修改 public API、`ProductService` 或引入自动重试。验收：已确认成功后版本恰增 1；不存在行/版本冲突不提交；双线程竞争一成功一冲突；已知失败 rollback；COMMIT 响应丢失返回 CommitUnknown 且更新仅执行一次。

### Task 4：固定 HTTP 工作池与所有权

可与 Task 1–3 并行。修改：`examples/mysql_sphinx/src/product_http.cpp`。实现 `Impl`、构造/析构、`serve/stop` 和 `thread_local WorkerContext`；调用已定义的 `install_product_routes`。不得修改 `product_http_routes.h/.cpp`、任何 public API 或 CMake。验收：1..64 固定 worker、队列上限 256、body 上限 64 KiB、stop 多次安全、`serve()` 返回后 worker 全部销毁、MySqlRuntime 最后析构；两个 worker 的连接地址不同。`ProductService` 和连接不得跨线程传递。

### Task 5：HTTP 路由及 JSON 映射

可与 Task 4 和 Task 1–3 并行。修改：`examples/mysql_sphinx/src/product_http_routes.cpp`。实现 `install_product_routes`，严格按第 5 节解析、调用与响应。不得修改 `product_http_routes.h`、`product_http.cpp` 或 public API。验收：成功/404/409/503/500/413/415 响应与固定 JSON 错误码符合表格；`fresh=1` 不读缓存；未知字段/错误类型/溢出在调用 service 前拒绝；任何异常文本都不进响应。

### Task 6：入口配置与信号控制

可与 Task 4–5 并行。修改：`examples/mysql_sphinx/src/product_main.cpp`。实现第 5 节全部环境变量、数值校验及第 3 节 `sigwait` 停机流程。不得修改 public API 或路由逻辑。验收：缺必需配置/非法数字时退出码 1 且不打印密码；监听失败不挂起；SIGINT/SIGTERM 后 `stop()` 生效并退出码 0；无 detached 线程。

### Task 7：MySQL 集成与故障注入测试

依赖 Task 3。修改：`examples/mysql_sphinx/test/mysql_product_store_integration_test.cpp`。补齐三个已命名测试 case；提供仅对一次性数据库运行的 fixture；无测试环境时带原因 skip。不得修改生产代码或 public API。COMMIT 故障按测试文件内已固定的 GNU linker `--wrap=mysql_commit` 实现：wrapper 调用真实 COMMIT 一次，成功后对本次调用伪造非零返回；生产适配器必须归类 CommitUnknown、丢弃连接且不重试。验收：真实 MySQL 8.4 上读/写/冲突/rollback/提交未知测试通过；wrapper 计数只增加 1，新连接读取到已提交版本。这个测试验证“服务端已提交、客户端认为未知”的分支；真实网络断连另作部署演练，不由低能力 Agent 另设计代理。

### Task 8：HTTP 黑盒测试与联调

依赖 Task 4–6；可与 Task 7 并行。修改：`examples/mysql_sphinx/test/product_http_integration_test.py`。用临时端口启动本地 `sphinxd` 与商品服务，对一次性 MySQL schema 建立 fixture，补齐现有 6 个测试；无测试凭据时带原因 skip。不得修改 public API 或生产代码。验收：第 7 节 HTTP 行全部覆盖；GET 第一次 MISS、第二次 HIT；PUT 后 miss 读到新版本；MySQL/缓存分别断开时响应正确；并发 worker 无连接共享；SIGTERM 与监听失败进程退出；原 `ctest` 全量通过。

### Task 9：确定性竞态验证

可与 Task 7–8 并行。修改：`examples/mysql_sphinx/test/product_service_test.cpp`，只补齐 `ConcurrentStaleFillExpiresByOwnTtl`，不改 production API。验收：同步屏障强制“旧 GET 读库 → 新 PUT 提交并删缓存 → 旧 GET 回填”；证明结果在该缓存项自身的 TTL 到期前可为旧值，到期后下一次回源新版本。若 fake cache 无时钟能力，在测试文件内增加私有虚拟时钟辅助类，不扩大生产接口。

最终合并顺序：Task 1 → 2 → 3；Task 4/5/6 可独立提交；再合并 Task 7/8/9。最终门槛：默认构建与 `ctest` 全过；启用可选目标可编译、无新增 warning；一次性 MySQL 8.4 + 本地 Sphinx 的黑盒测试通过；未运行的故障注入/真实数据库测试必须如实列出。
