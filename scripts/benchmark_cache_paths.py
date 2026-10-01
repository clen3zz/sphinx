#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run paired Release HTTP and direct-cache experiments using disposable services."""

import argparse
import contextlib
import datetime
import getpass
import hashlib
import itertools
import json
import os
from pathlib import Path
import platform
import random
import secrets
import shutil
import socket
import statistics
import subprocess
import tempfile
import time

from compare_product_cache import ProductService, cache_info, process_snapshot
from product_cache_tools import (
    CacheServer, MySqlDatabase, read_line, reserve_port,
)


@contextlib.contextmanager
def launch_on(cpus):
    """Child threads inherit affinity from birth; restore the runner after startup."""
    previous = os.sched_getaffinity(0)
    os.sched_setaffinity(0, cpus)
    try:
        yield
    finally:
        os.sched_setaffinity(0, previous)


@contextlib.contextmanager
def private_mysql(repo, cpus):
    root = Path(tempfile.mkdtemp(prefix="sphinx-formal-mysql-", dir="/tmp"))
    process = None
    try:
        subprocess.run(
            ["mysqld", "--no-defaults", "--initialize-insecure", f"--datadir={root}/data",
             f"--user={getpass.getuser()}"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True,
        )
        port = reserve_port()
        with (root / "server.log").open("wb") as log, launch_on(cpus):
            process = subprocess.Popen(
                ["mysqld", "--no-defaults", f"--datadir={root}/data",
                 f"--socket={root}/mysql.sock", f"--port={port}", "--bind-address=127.0.0.1",
                 f"--pid-file={root}/mysql.pid", f"--log-error={root}/error.log",
                 f"--user={getpass.getuser()}", "--mysqlx=OFF", "--performance-schema=ON"],
                stdout=log, stderr=log,
            )
        admin = ["mysql", "--protocol=socket", f"--socket={root}/mysql.sock", "--user=root"]
        for _ in range(200):
            if process.poll() is not None:
                raise RuntimeError("private MySQL exited during startup")
            probe = subprocess.run(
                admin + ["--execute", "SELECT 1"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            )
            if probe.returncode == 0:
                break
            time.sleep(0.05)
        else:
            raise RuntimeError("private MySQL startup timed out")
        password = secrets.token_hex(16)
        subprocess.run(
            admin + ["--execute",
                     "CREATE DATABASE sphinx_formal_test CHARACTER SET utf8mb4; "
                     f"CREATE USER formal_test@localhost IDENTIFIED BY '{password}'; "
                     "GRANT ALL ON sphinx_formal_test.* TO formal_test@localhost; "
                     "GRANT SELECT ON performance_schema.* TO formal_test@localhost;"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True,
        )
        database = MySqlDatabase({
            "SPHINX_TEST_MYSQL_HOST": "127.0.0.1", "SPHINX_TEST_MYSQL_PORT": str(port),
            "SPHINX_TEST_MYSQL_USER": "formal_test", "SPHINX_TEST_MYSQL_PASSWORD": password,
            "SPHINX_TEST_MYSQL_DATABASE": "sphinx_formal_test",
        })
        database.run(input_file=repo / "product-service/schema/mysql/schema.sql")
        database.process = process
        yield database
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        if root.resolve().parent != Path("/tmp") or not root.name.startswith("sphinx-formal-mysql-"):
            raise RuntimeError("unexpected private MySQL cleanup path")
        shutil.rmtree(root)


def seed_direct(cache, keys, value_bytes):
    value = b"x" * value_bytes
    # This setup uses one persistent connection and is excluded from measured time.
    with socket.create_connection(("127.0.0.1", cache.port), timeout=5) as connection:
        for product_id in range(1, keys + 1):
            key = f"benchmark:{product_id}".encode()
            if cache.backend == "sphinx":
                connection.sendall(b"set " + key + f" 0 300 {len(value)}\r\n".encode() + value + b"\r\n")
                if read_line(connection) != b"STORED":
                    raise RuntimeError("Sphinx direct warmup failed")
            else:
                arguments = [b"SET", key, value, b"EX", b"300"]
                request = b"*5\r\n" + b"".join(
                    f"${len(argument)}\r\n".encode() + argument + b"\r\n"
                    for argument in arguments
                )
                connection.sendall(request)
                if read_line(connection) != b"+OK":
                    raise RuntimeError("Redis direct warmup failed")


def cpu_delta(before, after, elapsed):
    if before is None or after is None or before["pid"] != after["pid"]:
        return None
    seconds = after["cpu_seconds"] - before["cpu_seconds"]
    return {"seconds": seconds, "average_cores": seconds / elapsed,
            "rss_before_bytes": before["rss_bytes"], "rss_after_bytes": after["rss_bytes"]}


def run_sample(args, repo, database, backend, group, scenario, concurrency, repeat, cpus):
    binary = args.build_dir / "sphinxd/sphinxd" if backend == "sphinx" else args.redis_server
    cache = CacheServer(backend, str(binary), 64, "noeviction")
    if backend == "sphinx":
        cache.command[cache.command.index("--threads") + 1] = "1"
    else:
        cache.command.extend(["--io-threads", "1"])
    driver = None
    service = None
    with contextlib.ExitStack() as stack:
        with launch_on(cpus["cache"]):
            stack.enter_context(cache)
        if group == "http":
            service = ProductService(
                str(args.build_dir / "product-service/sphinx-product-service"), database,
                backend, "basic", cache, 4, concurrency, 300, ttl_jitter_seconds=0,
            )
            with launch_on(cpus["http"]):
                stack.enter_context(service)
            for offset in range(0, args.keys, 32):
                ids = range(offset + 1, min(offset + 33, args.keys + 1))
                response = service.request("GET", "/products?ids=" + ",".join(map(str, ids)))
                if response[1] != 200 or response[3]:
                    raise RuntimeError("HTTP dataset warmup failed")
            port = service.port
        else:
            seed_direct(cache, args.keys, args.value_bytes)
            port = cache.port
        command = [str(args.build_dir / "benchmarks/sphinx_cache_path_benchmark"), group,
                   backend, scenario, str(port), str(concurrency), str(args.seconds),
                   str(args.warmup_seconds), str(args.keys), str(args.value_bytes)]
        with launch_on(cpus["client"]):
            driver = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                      stderr=subprocess.PIPE, text=True)
        try:
            if driver.stdout.readline().strip() != "READY":
                raise RuntimeError("benchmark driver failed before measurement: " + driver.stderr.read())
            before = {"cache": process_snapshot(cache.process)}
            if service:
                before["http"] = process_snapshot(service.process)
                before["mysql"] = process_snapshot(database.process)
                metrics_before = service.metrics()["counters"]
                sql_before = database.statement_count()
            cache_before = cache_info(cache)
            driver.stdin.write("start\n")
            driver.stdin.flush()
            stdout, stderr = driver.communicate(timeout=args.seconds + 30)
            after = {"cache": process_snapshot(cache.process)}
            if service:
                after["http"] = process_snapshot(service.process)
                after["mysql"] = process_snapshot(database.process)
            result = json.loads(stdout.strip())
            result.update(group=group, backend=backend, scenario=scenario,
                          concurrency=concurrency, repeat=repeat)
            result["process_resources"] = {
                name: cpu_delta(snapshot, after[name], result["elapsed_seconds"])
                for name, snapshot in before.items()
            }
            result["cache_info_before"] = cache_before
            result["cache_info_after"] = cache_info(cache)
            if service:
                metrics_after = service.metrics()["counters"]
                result["metrics_delta"] = {
                    name: metrics_after[name] - value for name, value in metrics_before.items()
                }
                sql_after = database.statement_count()
                result["mysql_select_delta"] = (
                    sql_after - sql_before if sql_before is not None and sql_after is not None else None
                )
                expected_reads = result["completed"] if scenario == "fresh_get" else 0
                if result["metrics_delta"]["store_read_operations"] != expected_reads:
                    raise RuntimeError("HTTP source isolation failed")
                if result["mysql_select_delta"] != expected_reads:
                    raise RuntimeError("actual SQL count does not match expected reads")
            if driver.returncode != 0 or result["errors"] or result["warmup_errors"]:
                raise RuntimeError("invalid benchmark sample: " + json.dumps(result) + stderr)
            return result
        finally:
            if driver.poll() is None:
                driver.kill()
                driver.wait()


def write_report(report, output):
    temporary = output.with_suffix(".tmp")
    temporary.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    temporary.replace(output)


def summarize(report, destination):
    lines = ["# 商品 HTTP 与直接缓存路径性能实验", "",
             "本表为每个条件多次采样的中位数。吞吐按成功的 API/HTTP 操作数计算；批量一次含 32 项。",
             "P99 是各次采样 P99 的中位数，不是合并全部请求后的 P99。完整条件和原始样本见同目录 JSON。", "",
             "| 组 | 场景 | 并发 | 后端 | ops/s 中位数 | ops/s 最小～最大 | P50 μs | P99 μs | 缓存 CPU 核数 |",
             "| --- | --- | --- | --- | ---: | --- | ---: | ---: | ---: |"]
    samples = sorted(report["samples"], key=lambda sample: (
        sample["group"], sample["scenario"], sample["concurrency"], sample["backend"]
    ))
    for key, values in itertools.groupby(samples, key=lambda sample: (
        sample["group"], sample["scenario"], sample["concurrency"], sample["backend"]
    )):
        values = list(values)
        rates = [value["operations_per_second"] for value in values]
        p50 = statistics.median(value["latency_us"]["p50"] for value in values)
        p99 = statistics.median(value["latency_us"]["p99"] for value in values)
        cache_cpu = statistics.median(value["process_resources"]["cache"]["average_cores"] for value in values)
        lines.append(f"| {' | '.join(map(str, key))} | {statistics.median(rates):.0f} | "
                     f"{min(rates):.0f}～{max(rates):.0f} | {p50:.1f} | {p99:.1f} | {cache_cpu:.2f} |")
    destination.write_text("\n".join(lines) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--redis-server", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--seconds", type=float, default=5)
    parser.add_argument("--warmup-seconds", type=float, default=1)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--concurrency", default="1,4,16,64")
    parser.add_argument("--keys", type=int, default=4096)
    parser.add_argument("--value-bytes", type=int, default=256)
    args = parser.parse_args()
    args.build_dir = args.build_dir.resolve()
    # Redis selects server/checker mode from argv[0]; retain the redis-server symlink name.
    args.redis_server = Path(os.path.abspath(args.redis_server))
    args.output_dir = args.output_dir.resolve()
    concurrency = [int(value) for value in args.concurrency.split(",")]
    if (not 0 < args.seconds <= 60 or not 0 <= args.warmup_seconds <= 60 or args.repeats < 1
            or not 32 <= args.keys <= 65536 or not 1 <= args.value_bytes <= 4096
            or not concurrency or any(not 1 <= value <= 256 for value in concurrency)):
        parser.error("invalid experiment dimensions")
    repo = Path(__file__).resolve().parents[1]
    for binary in (args.redis_server, args.build_dir / "sphinxd/sphinxd",
                   args.build_dir / "product-service/sphinx-product-service",
                   args.build_dir / "benchmarks/sphinx_cache_path_benchmark"):
        if not binary.is_file():
            parser.error(f"missing executable: {binary}")
    build_cache = (args.build_dir / "CMakeCache.txt").read_text()
    if "CMAKE_BUILD_TYPE:STRING=Release" not in build_cache:
        parser.error("formal experiments require a Release build")
    available = sorted(os.sched_getaffinity(0))
    if len(available) < 8:
        parser.error("this fixed CPU partition needs at least eight available logical CPUs")
    cpus = {"cache": available[:1], "http": available[1:3], "mysql": available[3:5],
            "client": available[5:-1], "runner": available[-1:]}
    os.sched_setaffinity(0, cpus["runner"])
    args.output_dir.mkdir(parents=True, exist_ok=True)
    output = args.output_dir / "samples.json"
    if output.exists():
        parser.error("output already exists; choose a fresh output directory")
    source_hashes = {
        str(path.relative_to(repo)): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in (repo / "benchmarks/cache_path_benchmark.cpp", Path(__file__).resolve())
    }
    report = {
        "created_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "complete": False,
        "environment": {"platform": platform.platform(), "cpu_affinity": cpus,
                        "cpu_model": next((line.split(":", 1)[1].strip()
                                           for line in Path("/proc/cpuinfo").read_text().splitlines()
                                           if line.startswith("model name")), "unknown"),
                        "compiler": subprocess.check_output(["c++", "--version"], text=True).splitlines()[0],
                        "git_head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip(),
                        "tracked_diff_sha256": hashlib.sha256(subprocess.check_output(
                            ["git", "diff", "HEAD"], cwd=repo)).hexdigest(),
                        "source_sha256": source_hashes,
                        "redis_version": subprocess.check_output([str(args.redis_server), "--version"], text=True).strip()},
        "conditions": {"build": "Release", "seconds": args.seconds, "warmup_seconds": args.warmup_seconds,
                       "repeats": args.repeats, "concurrency": concurrency, "keys": args.keys,
                       "direct_value_bytes": args.value_bytes, "ttl_seconds": 300, "http_workers": 4,
                       "cache_threads": 1, "cache_memory_mb": 64, "redis_policy": "noeviction",
                       "redis_persistence": False, "http_policy": "basic",
                       "http_connections": "new connection per request; fixed HTTP worker pool",
                       "direct_connections": "persistent per client worker",
                       "load_model": "closed-loop, one operation in flight per client worker",
                       "pair_order_seed": 20261001},
        "samples": [],
    }
    randomizer = random.Random(20261001)
    workloads = [("http", value) for value in ("get", "batch_get", "fresh_get")]
    workloads += [("direct", value) for value in ("get", "put", "batch_get", "batch_put")]
    jobs = list(itertools.product(workloads, concurrency, range(1, args.repeats + 1)))
    randomizer.shuffle(jobs)
    total = len(jobs) * 2
    with private_mysql(repo, cpus["mysql"]) as database:
        database.seed([(product_id, "benchmark-product-" + str(product_id))
                       for product_id in range(1, args.keys + 1)])
        report["environment"]["mysql_version"] = database.version()
        for (group, scenario), clients, repeat in jobs:
            backends = ["sphinx", "redis"]
            randomizer.shuffle(backends)
            for backend in backends:
                sample = run_sample(args, repo, database, backend, group, scenario, clients, repeat, cpus)
                report["samples"].append(sample)
                write_report(report, output)
                print(f"{len(report['samples'])}/{total} {group}/{scenario} c={clients} "
                      f"{backend} repeat={repeat}: {sample['operations_per_second']:.0f} ops/s, "
                      f"P99={sample['latency_us']['p99']:.1f}us, errors={sample['errors']}", flush=True)
    report["complete"] = True
    write_report(report, output)
    summarize(report, args.output_dir / "SUMMARY.md")
    print("Experiments complete; private MySQL and all cache/HTTP processes cleaned", flush=True)


if __name__ == "__main__":
    main()
