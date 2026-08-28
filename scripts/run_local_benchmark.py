#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""使用 memtier_benchmark 运行一次可复现的本地 Sphinx 基准测试。

该脚本保持精简：负责管理 Sphinx 子进程，等待 TCP 监听端口，调用 memtier，
并记录结果和元数据。它不实现基准客户端，也不设置性能阈值。
"""

from __future__ import annotations

import argparse
import datetime as _datetime
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import uuid
from dataclasses import dataclass, field
from typing import Any, Iterable, Mapping, Optional, Sequence


DEFAULT_HOST = "127.0.0.1"
DEFAULT_THREADS = 4
DEFAULT_MEMORY_LIMIT_MB = 64
DEFAULT_SEGMENT_SIZE_MB = 2
DEFAULT_DURATION_SECONDS = 20
DEFAULT_CLIENT_THREADS = 4
DEFAULT_CONNECTIONS_PER_THREAD = 8
DEFAULT_KEY_SPACE = 100_000
DEFAULT_VALUE_SIZE = 256
DEFAULT_STARTUP_TIMEOUT_SECONDS = 5.0
DEFAULT_SHUTDOWN_TIMEOUT_SECONDS = 3.0


class BenchmarkError(RuntimeError):
    """基准测试无法产生可信结果时抛出的异常。"""


@dataclass(frozen=True)
class BenchmarkConfig:
    """一次测量负载的已校验输入。"""

    server_executable: str
    workload: str = "read"
    threads: int = DEFAULT_THREADS
    memory_limit_mb: int = DEFAULT_MEMORY_LIMIT_MB
    segment_size_mb: int = DEFAULT_SEGMENT_SIZE_MB
    duration_seconds: int = DEFAULT_DURATION_SECONDS
    client_threads: int = DEFAULT_CLIENT_THREADS
    connections_per_thread: int = DEFAULT_CONNECTIONS_PER_THREAD
    key_space: int = DEFAULT_KEY_SPACE
    value_size: int = DEFAULT_VALUE_SIZE
    output_dir: Path = Path("benchmark-results")
    memtier_executable: str = "memtier_benchmark"
    host: str = DEFAULT_HOST
    startup_timeout_seconds: float = DEFAULT_STARTUP_TIMEOUT_SECONDS
    shutdown_timeout_seconds: float = DEFAULT_SHUTDOWN_TIMEOUT_SECONDS
    dry_run: bool = False


@dataclass(frozen=True)
class BenchmarkResult:
    """成功完成测量后生成的路径和汇总值。"""

    raw_result_path: Path
    metadata_path: Path
    protocol_errors: int
    metrics: Mapping[str, Optional[float]]
    error_counts: Mapping[str, int] = field(default_factory=dict)


def _positive(name: str, value: int) -> None:
    if value <= 0:
        raise BenchmarkError(f"{name} must be positive")


def validate_config(config: BenchmarkConfig) -> None:
    """在创建任何进程前校验脚本和 Sphinx 参数。"""

    if not config.server_executable:
        raise BenchmarkError("server executable must not be empty")
    if config.workload not in {"read", "mixed", "write"}:
        raise BenchmarkError("workload must be one of: read, mixed, write")
    _positive("threads", config.threads)
    _positive("memory limit", config.memory_limit_mb)
    _positive("segment size", config.segment_size_mb)
    _positive("duration", config.duration_seconds)
    _positive("client threads", config.client_threads)
    _positive("connections per client thread", config.connections_per_thread)
    _positive("key space", config.key_space)
    _positive("value size", config.value_size)
    if config.memory_limit_mb % config.threads != 0:
        raise BenchmarkError("memory limit must be divisible by threads")
    per_thread_memory = config.memory_limit_mb // config.threads
    if config.segment_size_mb > per_thread_memory or per_thread_memory % config.segment_size_mb != 0:
        raise BenchmarkError("per-thread memory must contain whole segments")
    if not config.host:
        raise BenchmarkError("host must not be empty")
    if config.startup_timeout_seconds <= 0:
        raise BenchmarkError("startup timeout must be positive")
    if config.shutdown_timeout_seconds <= 0:
        raise BenchmarkError("shutdown timeout must be positive")


def _executable_path(value: str, label: str) -> str:
    """解析可执行文件，不静默接受缺失的程序。"""

    candidate = Path(value).expanduser()
    if candidate.parent != Path(".") or candidate.is_absolute():
        if not candidate.is_file() or not os.access(candidate, os.X_OK):
            raise BenchmarkError(f"{label} is not an executable file: {value}")
        return str(candidate.resolve())
    resolved = shutil.which(value)
    if resolved is None:
        raise BenchmarkError(f"{label} was not found: {value}")
    return resolved


def find_free_port(host: str = DEFAULT_HOST) -> int:
    """返回当前可用于绑定的临时本地 TCP 端口。"""

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind((host, 0))
        return int(sock.getsockname()[1])


def build_server_command(config: BenchmarkConfig, port: int) -> list[str]:
    """构造一次运行所使用的精确 Sphinx 命令。"""

    return [
        config.server_executable,
        "--listen",
        config.host,
        "--port",
        str(port),
        "--threads",
        str(config.threads),
        "--memory-limit",
        str(config.memory_limit_mb),
        "--segment-size",
        str(config.segment_size_mb),
    ]


def _ratio_for(workload: str) -> str:
    # memtier 使用 SET:GET 表示比例，因此 1:9 即 10% SET 和 90% GET。
    return {"read": "0:1", "mixed": "1:9", "write": "1:0"}[workload]


def build_memtier_command(
    config: BenchmarkConfig,
    port: int,
    result_path: Path,
    *,
    prefill: bool = False,
) -> list[str]:
    """构造预填充或测量流量所使用的 memtier 命令。"""

    workload = "write" if prefill else config.workload
    command = [
        config.memtier_executable,
        "--server",
        config.host,
        "--port",
        str(port),
        "--protocol",
        "memcache_text",
        "--threads",
        str(config.client_threads),
        "--clients",
        str(1 if prefill else config.connections_per_thread),
        "--ratio",
        _ratio_for(workload),
        "--data-size",
        str(config.value_size),
        "--print-percentiles",
        "50,95,99",
        "--key-pattern",
        "S:S",
        "--key-minimum",
        "1",
        "--key-maximum",
        str(config.key_space + 1),
        "--json-out-file",
        str(result_path),
    ]
    if prefill:
        # memtier 的 allkeys 模式会为半开区间 [minimum, maximum) 中的每个键发送
        # 一个请求。单个客户端可避免分片重复，也不依赖任意的时间预算。
        command[command.index("--threads") + 1] = "1"
        command.extend(["--requests", "allkeys"])
    else:
        command[command.index("--clients") + 1] = str(config.connections_per_thread)
        command.extend(["--test-time", str(config.duration_seconds)])
    return command


def _command_text(command: Sequence[str]) -> str:
    return shlex.join([str(part) for part in command])


def wait_for_server(
    process: subprocess.Popen[Any],
    host: str,
    port: int,
    timeout_seconds: float,
    *,
    poll_interval: float = 0.02,
) -> None:
    """等待子进程接受 TCP 连接，否则带启动诊断信息失败。"""

    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        return_code = process.poll()
        if return_code is not None:
            diagnostics = _process_diagnostics(process)
            raise BenchmarkError(f"server exited before becoming ready ({return_code}): {diagnostics}")
        try:
            with socket.create_connection((host, port), timeout=min(poll_interval, 0.2)):
                return
        except OSError:
            time.sleep(poll_interval)
    diagnostics = _process_diagnostics(process)
    raise BenchmarkError(f"server did not become ready within {timeout_seconds:g}s: {diagnostics}")


def _process_diagnostics(process: subprocess.Popen[Any]) -> str:
    """读取子进程已有的输出，且不阻塞仍在运行的进程。"""

    if process.poll() is None:
        return ""
    try:
        output, error = process.communicate(timeout=0.2)
    except (subprocess.TimeoutExpired, OSError):
        return ""
    text = ""
    if output:
        text += str(output)
    if error:
        text += str(error)
    return text.strip()[-1000:]


def stop_process(process: Optional[subprocess.Popen[Any]], timeout_seconds: float) -> None:
    """如果 *process* 仍在运行，只停止它所代表的子进程。"""

    if process is None:
        return
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=timeout_seconds)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=timeout_seconds)
    # 关闭 PIPE 对重复调用脚本的调用方很重要。在最终清理前退出的服务进程
    # 已经在这里完成回收。
    try:
        process.communicate(timeout=0.2)
    except (subprocess.TimeoutExpired, OSError):
        pass


def _run_client(
    command: Sequence[str],
    server_process: subprocess.Popen[Any],
    timeout_seconds: float,
) -> None:
    """运行 memtier，同时检测服务进程是否在其完成前退出。"""

    try:
        client = subprocess.Popen(
            list(command), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
        )
    except OSError as error:
        raise BenchmarkError(f"could not start memtier: {error}") from error

    while True:
        if server_process.poll() is not None:
            stop_process(client, timeout_seconds)
            diagnostics = _process_diagnostics(server_process)
            raise BenchmarkError(f"server exited while memtier was running: {diagnostics}")
        try:
            output, error = client.communicate(timeout=0.1)
            break
        except subprocess.TimeoutExpired:
            continue
    if client.returncode != 0:
        detail = (error or output or "").strip()[-1000:]
        raise BenchmarkError(f"memtier exited with status {client.returncode}: {detail}")
    if server_process.poll() is not None:
        diagnostics = _process_diagnostics(server_process)
        raise BenchmarkError(f"server exited before memtier completed: {diagnostics}")


def _normal_key(value: str) -> str:
    return "".join(character for character in value.lower() if character.isalnum())


def _numeric(value: Any) -> Optional[float]:
    if isinstance(value, bool):
        return None
    if isinstance(value, (int, float)):
        return float(value)
    if isinstance(value, str):
        try:
            return float(value.strip())
        except ValueError:
            return None
    return None


def _walk_dicts(value: Any) -> Iterable[tuple[str, Any]]:
    if isinstance(value, Mapping):
        for key, child in value.items():
            yield str(key), child
            yield from _walk_dicts(child)
    elif isinstance(value, list):
        for child in value:
            yield from _walk_dicts(child)


def _error_category(normalized_key: str) -> Optional[str]:
    """将 memtier 错误计数键映射到少量稳定的类别。"""

    if normalized_key.endswith("sec") or normalized_key.endswith("rate"):
        return None
    if "protocolerror" in normalized_key:
        return "protocol"
    if "connectionerror" in normalized_key:
        return "connection"
    if "error" in normalized_key or "failure" in normalized_key or "failed" in normalized_key:
        return "other"
    return None


def _sum_error_value(value: Any) -> int:
    """汇总错误映射下的数值叶节点，不把文本当作错误。"""

    number = _numeric(value)
    if number is not None:
        return int(number)
    if isinstance(value, Mapping):
        return sum(_sum_error_value(child) for child in value.values())
    if isinstance(value, list):
        return sum(_sum_error_value(child) for child in value)
    return 0


def _error_counts_in(document: Any) -> dict[str, int]:
    counts = {"protocol": 0, "connection": 0, "other": 0}

    def visit(value: Any) -> None:
        if isinstance(value, Mapping):
            for key, child in value.items():
                category = _error_category(_normal_key(str(key)))
                if category is not None:
                    counts[category] += _sum_error_value(child)
                else:
                    visit(child)
        elif isinstance(value, list):
            for child in value:
                visit(child)

    visit(document)
    return counts


def error_counts(document: Any) -> dict[str, int]:
    """从 memtier JSON 返回协议、连接和其他错误计数。

    Memtier 会输出命令专用计数和 ``Totals`` 汇总对象。如果存在 Totals 对象，
    只使用该对象，避免重复统计 Sets、Gets 和 Totals；否则使用文档中的所有计数。
    """

    totals: list[Any] = []

    def find_totals(value: Any) -> None:
        if isinstance(value, Mapping):
            for key, child in value.items():
                if _normal_key(str(key)) == "totals" and isinstance(child, Mapping):
                    totals.append(child)
                else:
                    find_totals(child)
        elif isinstance(value, list):
            for child in value:
                find_totals(child)

    find_totals(document)
    counts = _error_counts_in(totals[0] if totals else document)
    if totals and isinstance(document, Mapping):
        # 某些 memtier 构建版本会把协议级计数放在文档根部，
        # 而命令/连接计数位于 ALL STATS/Totals 下。
        # 保留这些根部计数，但不重复加入 Sets 和 Gets。
        for key, child in document.items():
            category = _error_category(_normal_key(str(key)))
            if category is not None:
                counts[category] += _sum_error_value(child)
    return counts


def protocol_error_count(document: Any) -> int:
    """为兼容现有调用方而保留的总错误数视图。"""

    return sum(error_counts(document).values())


def _find_metric(document: Any, candidates: set[str], contains: Sequence[str] = ()) -> Optional[float]:
    for key, value in _walk_dicts(document):
        normalized = _normal_key(key)
        if normalized in candidates or any(part in normalized for part in contains):
            number = _numeric(value)
            if number is not None:
                return number
    return None


def _find_named_mapping(document: Any, name: str) -> Optional[Mapping[str, Any]]:
    if isinstance(document, Mapping):
        for key, child in document.items():
            if _normal_key(str(key)) == name and isinstance(child, Mapping):
                return child
        for child in document.values():
            result = _find_named_mapping(child, name)
            if result is not None:
                return result
    elif isinstance(document, list):
        for child in document:
            result = _find_named_mapping(child, name)
            if result is not None:
                return result
    return None


def _operation_count(document: Any, operation: str) -> Optional[int]:
    section = _find_named_mapping(document, operation)
    if section is None:
        return None
    for key, value in section.items():
        if _normal_key(str(key)) == "count":
            number = _numeric(value)
            return int(number) if number is not None else None
    return None


def extract_metrics(document: Any) -> dict[str, Optional[float]]:
    """提取写入 BENCHMARK.md 的少量字段。"""

    # 官方 memtier JSON 包含 Sets、Gets 和 Totals。未激活的命令区段仍会包含
    # 0.007 ms 的百分位占位值，因此遍历整个文档可能静默报告错误的延迟。
    # 优先使用 Totals 汇总，仅在旧客户端使用的通用布局中找不到时再回退。
    totals = _find_named_mapping(document, "totals")
    metrics_source: Any = totals if totals is not None else document
    percentiles = _find_named_mapping(metrics_source, "percentilelatencies")
    percentile_source: Any = percentiles if percentiles is not None else metrics_source
    qps = _find_metric(metrics_source, {"opssec", "opspersec", "qps", "throughput"})
    return {
        "qps": qps,
        "p50": _find_metric(percentile_source, {"p50", "p5000", "p50latency"}, ("p50",)),
        "p95": _find_metric(percentile_source, {"p95", "p9500", "p95latency"}, ("p95",)),
        "p99": _find_metric(percentile_source, {"p99", "p9900", "p99latency"}, ("p99",)),
    }


def _git_commit() -> Optional[str]:
    repository = Path(__file__).resolve().parents[1]
    try:
        completed = subprocess.run(
            ["git", "-C", str(repository), "rev-parse", "--short", "HEAD"],
            check=True,
            capture_output=True,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError):
        return None
    return completed.stdout.strip() or None


def _git_source_state() -> dict[str, Any]:
    """记录工作树状态，不要求在基准测试前创建提交。"""

    repository = Path(__file__).resolve().parents[1]
    try:
        status_result = subprocess.run(
            ["git", "-C", str(repository), "status", "--porcelain=v1", "--untracked-files=all"],
            check=True,
            capture_output=True,
            text=True,
        )
        diff_result = subprocess.run(
            [
                "git",
                "-C",
                str(repository),
                "diff",
                "--no-ext-diff",
                "--binary",
                "HEAD",
                "--",
                "CMakeLists.txt",
                "FindRAGEL.cmake",
                "sphinxd",
                "scripts",
            ],
            check=True,
            capture_output=True,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError):
        return {"dirty": None, "source_state_id": None}

    status = status_result.stdout
    source_roots = {"CMakeLists.txt", "FindRAGEL.cmake", "sphinxd", "scripts"}
    source_status_lines: list[str] = []
    for line in status.splitlines():
        relative = line[3:] if len(line) >= 3 else ""
        top_level = relative.split("/", 1)[0]
        if top_level not in source_roots:
            continue
        source_status_lines.append(line)
    source_status = "\n".join(source_status_lines)
    hasher = hashlib.sha256()
    hasher.update(source_status.encode("utf-8", errors="replace"))
    hasher.update(b"\0")
    hasher.update(diff_result.stdout.encode("utf-8", errors="replace"))
    # 状态行只会列出未跟踪文件名，不包含文件内容。加入类似源码的未跟踪文件，
    # 让标识符在首次提交前仍然有用；跳过构建产物和 IDE 状态。
    source_suffixes = {".c", ".cc", ".cpp", ".h", ".hpp", ".py", ".rl", ".cmake"}
    for line in source_status_lines:
        if not line.startswith("?? "):
            continue
        relative = line[3:]
        path = repository / relative
        if path.is_file() and path.suffix.lower() in source_suffixes:
            hasher.update(relative.encode("utf-8", errors="replace"))
            hasher.update(b"\0")
            try:
                hasher.update(path.read_bytes())
            except OSError:
                pass
    return {
        "dirty": bool(status),
        "source_state_id": hasher.hexdigest()[:16],
    }


def _utc_now() -> str:
    return _datetime.datetime.now(_datetime.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def _run_stem(workload: str, started_at: str) -> str:
    stamp = started_at.replace("-", "").replace(":", "").replace("T", "-").replace("Z", "")
    return f"{stamp}-{workload}-{os.getpid()}-{uuid.uuid4().hex[:8]}"


def _load_json(path: Path) -> Any:
    if not path.is_file() or path.stat().st_size == 0:
        raise BenchmarkError(f"memtier result file is missing or empty: {path}")
    try:
        with path.open("r", encoding="utf-8") as stream:
            return json.load(stream)
    except (OSError, json.JSONDecodeError) as error:
        raise BenchmarkError(f"memtier result is not valid JSON: {path}: {error}") from error


def _write_json(path: Path, document: Any) -> None:
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    try:
        with temporary.open("w", encoding="utf-8") as stream:
            json.dump(document, stream, indent=2, sort_keys=True)
            stream.write("\n")
        temporary.replace(path)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def run_benchmark(config: BenchmarkConfig) -> Optional[BenchmarkResult]:
    """运行一次负载，仅在所有检查成功后返回结果路径。"""

    validate_config(config)
    server_command = build_server_command(config, 0)
    measured_command = build_memtier_command(config, 0, Path("RESULT.json"))
    prefill_command = build_memtier_command(config, 0, Path("PREFILL.json"), prefill=True)
    if config.dry_run:
        print("server: " + _command_text(server_command).replace(" --port 0", " --port <free-port>"))
        if config.workload in {"read", "mixed"}:
            print("prefill: " + _command_text(prefill_command).replace(" --port 0", " --port <free-port>"))
        print("measure: " + _command_text(measured_command).replace(" --port 0", " --port <free-port>"))
        return None

    server_path = _executable_path(config.server_executable, "server executable")
    memtier_path = _executable_path(config.memtier_executable, "memtier executable")
    output_dir = Path(config.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    port = find_free_port(config.host)
    started_at = _utc_now()
    stem = _run_stem(config.workload, started_at)
    git_commit = _git_commit()
    git_state = _git_source_state()
    raw_result_path = output_dir / f"{stem}.json"
    metadata_path = output_dir / f"{stem}.metadata.json"
    server_command = build_server_command(config, port)
    server_command[0] = server_path
    server_process: Optional[subprocess.Popen[Any]] = None
    prefill_errors = {"protocol": 0, "connection": 0, "other": 0}
    prefill_count: Optional[int] = None
    try:
        try:
            server_process = subprocess.Popen(
                server_command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
            )
        except OSError as error:
            raise BenchmarkError(f"could not start server: {error}") from error
        wait_for_server(
            server_process,
            config.host,
            port,
            config.startup_timeout_seconds,
        )
        with tempfile.TemporaryDirectory(prefix="sphinx-benchmark-") as temporary:
            temporary_path = Path(temporary)
            if config.workload in {"read", "mixed"}:
                prefill_path = temporary_path / "prefill.json"
                prefill_command = build_memtier_command(
                    config, port, prefill_path, prefill=True
                )
                prefill_command[0] = memtier_path
                _run_client(prefill_command, server_process, config.shutdown_timeout_seconds)
                prefill_document = _load_json(prefill_path)
                prefill_errors = error_counts(prefill_document)
                if sum(prefill_errors.values()):
                    raise BenchmarkError(
                        "memtier prefill reported errors: "
                        + json.dumps(prefill_errors, sort_keys=True)
                    )
                prefill_count = _operation_count(prefill_document, "sets")
                if prefill_count != config.key_space:
                    raise BenchmarkError(
                        f"memtier prefill wrote {prefill_count!r} keys; "
                        f"expected {config.key_space}"
                    )
            measured_command = build_memtier_command(config, port, raw_result_path)
            measured_command[0] = memtier_path
            _run_client(measured_command, server_process, config.shutdown_timeout_seconds)

        document = _load_json(raw_result_path)
        measured_errors = error_counts(document)
        total_errors = sum(measured_errors.values())
        if total_errors:
            raise BenchmarkError(
                "memtier reported errors: " + json.dumps(measured_errors, sort_keys=True)
            )
        metrics = extract_metrics(document)
        metadata = {
            "schema_version": 1,
            "started_at": started_at,
            "finished_at": _utc_now(),
            "git_commit": git_commit,
            "git_dirty": git_state["dirty"],
            "source_state_id": git_state["source_state_id"],
            "workload": config.workload,
            "server": {
                "executable": server_path,
                "host": config.host,
                "port": port,
                "threads": config.threads,
                "memory_limit_mb": config.memory_limit_mb,
                "segment_size_mb": config.segment_size_mb,
                "command": server_command,
            },
            "client": {
                "executable": memtier_path,
                "threads": config.client_threads,
                "connections_per_thread": config.connections_per_thread,
                "duration_seconds": config.duration_seconds,
                "key_space": config.key_space,
                "value_size": config.value_size,
                "protocol": "memcache_text",
                "command": measured_command,
            },
            "prefill_command": (
                prefill_command if config.workload in {"read", "mixed"} else None
            ),
            "prefill_error_counts": prefill_errors,
            "prefill_count": prefill_count,
            "raw_result": raw_result_path.name,
            "protocol_errors": measured_errors["protocol"],
            "error_counts": measured_errors,
            "metrics": metrics,
        }
        _write_json(metadata_path, metadata)
        return BenchmarkResult(
            raw_result_path,
            metadata_path,
            measured_errors["protocol"],
            metrics,
            measured_errors,
        )
    finally:
        stop_process(server_process, config.shutdown_timeout_seconds)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", "--server-executable", dest="server_executable", required=True)
    parser.add_argument("--workload", choices=("read", "mixed", "write"), default="read")
    parser.add_argument("--threads", type=int, default=DEFAULT_THREADS)
    parser.add_argument("--memory-limit", "--memory", dest="memory_limit_mb", type=int, default=DEFAULT_MEMORY_LIMIT_MB)
    parser.add_argument("--segment-size", "--segment", dest="segment_size_mb", type=int, default=DEFAULT_SEGMENT_SIZE_MB)
    parser.add_argument("--duration", "--test-time", dest="duration_seconds", type=int, default=DEFAULT_DURATION_SECONDS)
    parser.add_argument("--client-threads", type=int, default=DEFAULT_CLIENT_THREADS)
    parser.add_argument("--connections", "--connections-per-thread", dest="connections_per_thread", type=int, default=DEFAULT_CONNECTIONS_PER_THREAD)
    parser.add_argument("--key-space", type=int, default=DEFAULT_KEY_SPACE)
    parser.add_argument("--value-size", type=int, default=DEFAULT_VALUE_SIZE)
    parser.add_argument("--memtier", "--memtier-executable", dest="memtier_executable", default="memtier_benchmark")
    parser.add_argument("--output-dir", type=Path, default=Path("benchmark-results"))
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--startup-timeout", type=float, default=DEFAULT_STARTUP_TIMEOUT_SECONDS)
    parser.add_argument("--shutdown-timeout", type=float, default=DEFAULT_SHUTDOWN_TIMEOUT_SECONDS)
    parser.add_argument("--dry-run", action="store_true")
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = _parser().parse_args(argv)
    config = BenchmarkConfig(
        server_executable=args.server_executable,
        workload=args.workload,
        threads=args.threads,
        memory_limit_mb=args.memory_limit_mb,
        segment_size_mb=args.segment_size_mb,
        duration_seconds=args.duration_seconds,
        client_threads=args.client_threads,
        connections_per_thread=args.connections_per_thread,
        key_space=args.key_space,
        value_size=args.value_size,
        output_dir=args.output_dir,
        memtier_executable=args.memtier_executable,
        host=args.host,
        startup_timeout_seconds=args.startup_timeout,
        shutdown_timeout_seconds=args.shutdown_timeout,
        dry_run=args.dry_run,
    )
    try:
        result = run_benchmark(config)
    except BenchmarkError as error:
        print(f"benchmark failed: {error}", file=sys.stderr)
        return 1
    if result is not None:
        print(f"raw result: {result.raw_result_path}")
        print(f"metadata: {result.metadata_path}")
        print(f"protocol errors: {result.protocol_errors}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
