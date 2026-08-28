# Sphinx 编码与命名规范 (Coding Standards)

本文档记录 Sphinx 项目的 C++ 代码风格与命名约定，旨在保持代码库风格一致，降低协作与维护成本。

---

## 1. 命名规范总览

| 对象 | 风格 | 示例 | 说明 |
|---|---|---|---|
| 类型、类、结构体、枚举、类型别名 | `PascalCase` | `ClusterClient`、`LogConfig`、`DeleteStatus` | 统一采用首字母大写的驼峰命名 |
| 枚举值 | `PascalCase` | `Deleted`、`NotFound` | 与类型风格保持一致 |
| 函数和类方法 | `snake_case` | `parse_options`、`make_reactor`、`route` | 全部小写，单词间用下划线分隔 |
| 局部变量和函数参数 | `snake_case` | `thread_id`、`memory_limit`、`key_hash` | 全部小写，单词间用下划线分隔 |
| 私有和受保护成员变量 | `_snake_case` | `_nodes`、`_entries`、`_socket` | **必须统一使用前置下划线**，禁止后置下划线 |
| 公共结构体成员变量 | `snake_case` | `host`、`port`、`node_id` | POD / 数据载体结构体成员使用小写下划线 |
| 命名空间 | `snake_case` / 小写 | `sphinx` | 统一小写 |
| 宏定义与预处理标识符 | `UPPER_SNAKE_CASE` | `SPHINX_VERSION`、`SPHINX_PORT` | 全部大写，单词间用下划线分隔 |
| C++ 文件名 | `snake_case` | `cluster_client.cpp`、`logmem.h` | 统一小写下划线（历史文件见下文说明） |

---

## 2. 详细规范与历史过渡准则

### 2.1 私有和受保护成员变量
- 项目统一约定使用 **前置下划线**（`_member_name`），例如 `_socket`、`_memory_limit`、`_nodes`、`_entries`。
- 不再使用 Google 风格的后置下划线（如 `nodes_`、`entries_`）。
- 该规则已通过 `.clang-tidy` 中的 `readability-identifier-naming` 进行自动化强制检查。

### 2.2 常量命名 (Constants)
Sphinx 历史代码中存在 `lower_snake_case`（原有偏好）与部分公开接口 `kCamelCase` 的并存：
- **新代码规范**：所有新编写的文件内部常量、局部常量或静态常量统一采用 `lower_snake_case`（例如 `default_tcp_port`、`max_nr_threads`、`cache_line_size`）。
- **公开接口兼容**：已公开的头文件常量（例如 `kDefaultTimeout`、`kVirtualNodesPerNode`）为了保障调用方接口兼容性继续保留，不进行破坏性重命名。
- **演进策略**：内部私有常量在重构时逐步向 `lower_snake_case` 收敛，新代码禁止引入新的 `kCamelCase` 风格常量。

### 2.3 单元测试命名 (GoogleTest)
- **新测试规范**：新添加或重构的 GoogleTest 测试用例统一使用 `PascalCase`（例如 `TEST(ClusterClientTest, HandlesPartialResponsesAndBinaryValues)`），符合 GoogleTest 常见命名惯例。
- **既有测试兼容**：历史测试（如 `TEST(LogTest, append_expires)`）运行良好且不影响功能，不强制一次性批量重命名，平滑过渡。

### 2.4 文件命名与历史文件
- 新增头文件与实现文件统一使用 `snake_case.cpp` / `snake_case.h`。
- 历史遗留的文件名（如 `reactor-epoll.cpp`）和与可执行程序名称对应的实现文件（如 `sphinx-cluster.cpp`）保持原样，避免引起构建脚本、CMakeLists 或部署流程的不必要变动。

### 2.5 命名空间层级
- 项目对外统一使用单一顶层命名空间 `sphinx`。
- 项目类型、函数和类型别名直接声明在 `namespace sphinx` 中，禁止新增 `namespace sphinx::<module>` 作为模块分区。
- 模块边界通过头文件、源文件和目录组织，不通过嵌套命名空间表达。
- 仅文件内部使用的辅助实现放在匿名命名空间中，不作为公共 API 暴露。
- 不使用根命名空间类型别名来遮蔽项目类型原本的嵌套命名空间；如确需兼容旧 API，必须单独说明兼容范围和迁移计划。

---

## 3. 静态代码分析检查

项目在根目录下的 `.clang-tidy` 中配置了命名规则、并发安全与现代 C++ 规范检查：

```yaml
Checks: >
  -*,
  clang-analyzer-*,
  bugprone-*,
  performance-*,
  portability-*,
  readability-identifier-naming,
  concurrency-mt-unsafe,
  modernize-use-override,
  modernize-use-nullptr,
  modernize-make-unique,
  modernize-make-shared,
  modernize-loop-convert,
  modernize-deprecated-headers,
  modernize-use-equals-default,
  modernize-use-equals-delete,
  readability-redundant-smartptr-get,
  readability-redundant-string-cstr,
  readability-container-size-empty,
  readability-simplify-boolean-expr,
  readability-make-member-function-const,
  misc-redundant-expression,
  misc-unused-using-decls,
  -bugprone-easily-swappable-parameters,
  -performance-enum-size,
  -portability-avoid-pragma-once
```

本地执行全核并发静态检查：

```bash
# 自动探测 CPU 核心数并发检查 build 目录编译产物
./scripts/tidy build
```

---

## 4. 代码格式化规范 (Code Formatting)

项目使用根目录下的 `.clang-format` 统一格式化风格，基于 **Google C++ Style**（100 列宽，行内花括号）：

- **基准风格**：`Google`
- **缩进**：2 空格
- **列宽限制**：100 列 (`ColumnLimit: 100`)
- **花括号**：紧随同行（`Attach`，如 `void foo() {`、`if (...) {`）
- **指针与引用**：靠左对齐（`PointerAlignment: Left`，如 `std::string_view& str`）
- **单行函数**：禁止折叠为单行（`AllowShortFunctionsOnASingleLine: None`）

本地执行全量格式化：

```bash
./scripts/fmt
```
