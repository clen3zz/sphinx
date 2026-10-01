#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run comparable HTTP workloads against the Sphinx and Redis cache backends."""

import argparse
import concurrent.futures
import datetime
import http.client
import json
import os
import platform
import random
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from collections import Counter
from pathlib import Path

from product_cache_tools import (
    CacheServer, MySqlDatabase, RedisCommandError, read_exactly, read_line,
    redis_command, reserve_port, stop_process, wait_for_port,
)


class ProductService:
    def __init__(
        self,
        binary,
        database,
        backend,
        policy,
        cache_server,
        workers,
        client_concurrency,
        ttl_seconds,
        ttl_jitter_seconds=3,
    ):
        self.binary = binary
        self.database = database
        self.backend = backend
        self.policy = policy
        self.cache_server = cache_server
        self.workers = workers
        self.client_concurrency = client_concurrency
        self.ttl_seconds = ttl_seconds
        self.ttl_jitter_seconds = ttl_jitter_seconds
        self.port = reserve_port()
        self.process = None
        self.log_file = None

    def start(self):
        environment = os.environ.copy()
        environment.update(
            {
                "SPHINX_MYSQL_HOST": self.database.host,
                "SPHINX_MYSQL_PORT": str(self.database.port),
                "SPHINX_MYSQL_USER": self.database.user,
                "SPHINX_MYSQL_PASSWORD": self.database.password,
                "SPHINX_MYSQL_DATABASE": self.database.database,
                "SPHINX_HTTP_BIND": "127.0.0.1",
                "SPHINX_HTTP_PORT": str(self.port),
                "SPHINX_HTTP_WORKERS": str(self.workers),
                "SPHINX_CACHE_BACKEND": self.backend,
                "SPHINX_CACHE_POLICY": self.policy,
                "SPHINX_CACHE_TTL_SECONDS": str(self.ttl_seconds),
                "SPHINX_CACHE_TIMEOUT_MS": "200",
                "SPHINX_NEGATIVE_TTL_SECONDS": "5",
                "SPHINX_TTL_JITTER_SECONDS": str(self.ttl_jitter_seconds),
                "SPHINX_CACHE_FAILURE_THRESHOLD": "3",
                "SPHINX_CACHE_OPEN_INTERVAL_MS": "250",
            }
        )
        if self.backend == "sphinx":
            environment["SPHINX_CACHE_NODES"] = f"127.0.0.1:{self.cache_server.port}"
        else:
            environment["SPHINX_REDIS_HOST"] = "127.0.0.1"
            environment["SPHINX_REDIS_PORT"] = str(self.cache_server.port)
            environment["SPHINX_REDIS_DATABASE"] = "0"
            environment.pop("SPHINX_REDIS_USERNAME", None)
            environment.pop("SPHINX_REDIS_PASSWORD", None)

        self.log_file = tempfile.TemporaryFile()
        self.process = subprocess.Popen(
            [self.binary],
            env=environment,
            stdout=self.log_file,
            stderr=self.log_file,
        )
        try:
            wait_for_port(self.process, self.port, self.log_file)
        except Exception:
            self.stop()
            raise

    def stop(self):
        process, log_file = self.process, self.log_file
        self.process = None
        self.log_file = None
        stop_process(process, log_file)

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, *_exception):
        self.stop()

    def request(self, method, path, body=None):
        started_at = time.perf_counter_ns()
        status = 0
        cache_source = ""
        product_errors = Counter()
        try:
            connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
            headers = {}
            request_body = None
            if body is not None:
                headers["Content-Type"] = "application/json"
                request_body = json.dumps(body, separators=(",", ":")).encode("utf-8")
            try:
                connection.request(method, path, body=request_body, headers=headers)
                response = connection.getresponse()
                status = response.status
                cache_source = response.headers.get("X-Cache", "")
                content = response.read()
            finally:
                connection.close()
            if content:
                parsed = json.loads(content.decode("utf-8"))
                if isinstance(parsed, dict) and isinstance(parsed.get("error"), str):
                    product_errors[parsed["error"]] += 1
                if isinstance(parsed, dict) and isinstance(parsed.get("items"), list):
                    for item in parsed["items"]:
                        if isinstance(item, dict) and isinstance(item.get("error"), str):
                            product_errors[item["error"]] += 1
        except (OSError, ValueError, RuntimeError):
            status = 0
            product_errors["client_or_protocol_error"] += 1
        elapsed_ms = (time.perf_counter_ns() - started_at) / 1_000_000
        return elapsed_ms, status, cache_source, product_errors

    def metrics(self):
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        try:
            connection.request("GET", "/metrics")
            response = connection.getresponse()
            body = response.read()
            if response.status != 200:
                raise RuntimeError("metrics endpoint failed during comparison")
            return json.loads(body.decode("utf-8"))
        finally:
            connection.close()


def process_snapshot(process):
    if process is None or process.poll() is not None:
        return None
    try:
        stat = Path(f"/proc/{process.pid}/stat").read_text(encoding="utf-8")
        fields = stat[stat.rfind(")") + 2 :].split()
        ticks = os.sysconf("SC_CLK_TCK")
        cpu_seconds = (int(fields[11]) + int(fields[12])) / ticks
        status = Path(f"/proc/{process.pid}/status").read_text(encoding="utf-8")
        rss_line = next(line for line in status.splitlines() if line.startswith("VmRSS:"))
        rss_bytes = int(rss_line.split()[1]) * 1024
        return {
            "pid": process.pid,
            "start_time_ticks": int(fields[19]),
            "cpu_seconds": cpu_seconds,
            "rss_bytes": rss_bytes,
        }
    except (OSError, ValueError, StopIteration, IndexError):
        return None


def process_resource_interval(before, after):
    cpu_delta = None
    if before is not None and after is not None:
        same_process = (
            before["pid"] == after["pid"]
            and before["start_time_ticks"] == after["start_time_ticks"]
        )
        difference = after["cpu_seconds"] - before["cpu_seconds"]
        if same_process and difference >= 0:
            cpu_delta = difference
    return {"before": before, "after": after, "cpu_seconds_delta": cpu_delta}


def redis_info(port):
    result = {}
    for section in ("memory", "stats", "keyspace"):
        content = redis_command(port, "INFO", section)
        if not isinstance(content, bytes):
            continue
        for line in content.decode("utf-8", errors="replace").splitlines():
            if not line or line.startswith("#") or ":" not in line:
                continue
            key, value = line.split(":", 1)
            try:
                result[key] = int(value)
            except ValueError:
                result[key] = value
    return result


def sphinx_stats(port):
    with socket.create_connection(("127.0.0.1", port), timeout=2) as connection:
        connection.sendall(b"stats\r\n")
        result = {}
        while True:
            line = read_line(connection)
            if line == b"END":
                return result
            fields = line.split(b" ", 2)
            if len(fields) == 3 and fields[0] == b"STAT":
                key = fields[1].decode("ascii", errors="replace")
                value = fields[2].decode("ascii", errors="replace")
                try:
                    result[key] = int(value)
                except ValueError:
                    result[key] = value


def cache_info(cache_server):
    try:
        return (
            redis_info(cache_server.port)
            if cache_server.backend == "redis"
            else sphinx_stats(cache_server.port)
        )
    except (OSError, RuntimeError):
        return None


def count_sphinx_keys(port, keys):
    present = 0
    for offset in range(0, len(keys), 32):
        group = keys[offset : offset + 32]
        request = "get " + " ".join(group) + "\r\n"
        with socket.create_connection(("127.0.0.1", port), timeout=2) as connection:
            connection.sendall(request.encode("ascii"))
            while True:
                line = read_line(connection)
                if line == b"END":
                    break
                fields = line.split()
                if len(fields) != 4 or fields[0] != b"VALUE":
                    raise RuntimeError("Sphinx returned an invalid pressure-probe reply")
                value_size = int(fields[3])
                read_exactly(connection, value_size)
                if read_exactly(connection, 2) != b"\r\n":
                    raise RuntimeError("Sphinx returned an invalid pressure-probe terminator")
                present += 1
    return present


def run_memory_pressure(cache_server, entry_count, value_bytes):
    """Compare write failure, eviction, and segment reclamation under a bounded cache."""
    cache_before = cache_info(cache_server)
    payload = b"x" * value_bytes
    keys = [f"compare:pressure:{index}" for index in range(entry_count)]
    accepted = 0
    rejected = 0

    for key in keys:
        if cache_server.backend == "redis":
            try:
                reply = redis_command(cache_server.port, "SET", key, payload, "EX", 60)
            except RedisCommandError:
                rejected += 1
                continue
            if reply != b"OK":
                raise RuntimeError("Redis returned an unexpected pressure-write reply")
            accepted += 1
            continue

        request = (
            f"set {key} 0 60 {len(payload)}\r\n".encode("ascii")
            + payload
            + b"\r\n"
        )
        with socket.create_connection(("127.0.0.1", cache_server.port), timeout=2) as connection:
            connection.sendall(request)
            reply = read_line(connection)
        if reply == b"STORED":
            accepted += 1
        elif reply in (b"NOT_STORED", b"SERVER_ERROR out of memory storing object"):
            rejected += 1
        else:
            raise RuntimeError(f"Sphinx returned an unexpected pressure-write reply: {reply!r}")

    cache_after_fill = cache_info(cache_server)
    if cache_server.backend == "redis":
        retained = 0
        for offset in range(0, len(keys), 32):
            reply = redis_command(cache_server.port, "EXISTS", *keys[offset : offset + 32])
            if not isinstance(reply, int):
                raise RuntimeError("Redis returned an unexpected pressure-probe reply")
            retained += reply
    else:
        retained = count_sphinx_keys(cache_server.port, keys)
    cache_after_probe = cache_info(cache_server)

    evicted_before = cache_before.get("evicted_keys") if cache_before else None
    evicted_after = cache_after_fill.get("evicted_keys") if cache_after_fill else None
    evicted_delta = (
        evicted_after - evicted_before
        if isinstance(evicted_before, int) and isinstance(evicted_after, int)
        else None
    )
    if cache_server.backend == "sphinx":
        pressure_observed = retained < accepted
    elif cache_server.redis_policy == "noeviction":
        pressure_observed = rejected > 0
    else:
        pressure_observed = evicted_delta is not None and evicted_delta > 0
    return {
        "scenario": "memory_pressure",
        "configured_memory_mb": cache_server.memory_mb,
        "redis_maxmemory_policy": (
            cache_server.redis_policy if cache_server.backend == "redis" else None
        ),
        "offered_entries": entry_count,
        "value_bytes_each": value_bytes,
        "offered_value_bytes": entry_count * value_bytes,
        "write_accepted": accepted,
        "write_rejected": rejected,
        "retained_entries_after_fill": retained,
        "redis_evicted_keys_delta": evicted_delta,
        "pressure_observed": pressure_observed,
        "cache_info_before": cache_before,
        "cache_info_after_fill": cache_after_fill,
        "cache_info_after_probe": cache_after_probe,
        "interpretation": (
            "Redis write rejection/eviction and Sphinx retained-key loss are observed separately; "
            "Sphinx exposes no exact reclaimed-byte counter."
        ),
    }


def delete_cache_keys(cache_server, product_ids):
    for product_id in product_ids:
        key = f"product:v3:{product_id}"
        if cache_server.backend == "redis":
            redis_command(cache_server.port, "DEL", key)
            continue
        with socket.create_connection(("127.0.0.1", cache_server.port), timeout=2) as connection:
            connection.sendall(f"delete {key}\r\n".encode("ascii"))
            read_line(connection)


def percentile(samples, percent):
    if not samples:
        return None
    index = max(0, min(len(samples) - 1, int((percent / 100) * len(samples) + 0.999999) - 1))
    return samples[index]


def run_workload(service, label, jobs):
    database = service.database
    before = service.metrics()["counters"]
    sql_before = database.statement_count()
    cache_stats_before = cache_info(service.cache_server)
    service_before = process_snapshot(service.process)
    cache_before = process_snapshot(service.cache_server.process)
    started_at = time.perf_counter()
    with concurrent.futures.ThreadPoolExecutor(max_workers=service.client_concurrency) as executor:
        responses = list(executor.map(lambda job: service.request(*job), jobs))
    elapsed_seconds = time.perf_counter() - started_at
    service_after = process_snapshot(service.process)
    cache_after = process_snapshot(service.cache_server.process)
    cache_stats_after = cache_info(service.cache_server)
    after = service.metrics()["counters"]
    sql_after = database.statement_count()

    latencies = sorted(response[0] for response in responses)
    statuses = Counter(str(response[1]) for response in responses)
    cache_sources = Counter(response[2] or "NOT_REPORTED" for response in responses)
    product_errors = Counter()
    for response in responses:
        product_errors.update(response[3])
    metric_deltas = {
        name: after.get(name, 0) - before.get(name, 0) for name in before
    }
    mysql_select_delta = (
        sql_after - sql_before if sql_before is not None and sql_after is not None else None
    )
    if mysql_select_delta is not None and (
        mysql_select_delta < 0
        or (
            metric_deltas["store_read_operations"] > mysql_select_delta
            and metric_deltas["store_read_failures"] < metric_deltas["store_read_operations"]
        )
    ):
        mysql_select_delta = None
    return {
        "scenario": label,
        "request_count": len(jobs),
        "client_concurrency": service.client_concurrency,
        "elapsed_seconds": elapsed_seconds,
        "throughput_requests_per_second": len(jobs) / elapsed_seconds if elapsed_seconds else 0,
        "http_status_counts": dict(statuses),
        "cache_source_counts": dict(cache_sources),
        "product_error_counts": dict(product_errors),
        "metrics_delta": metric_deltas,
        "mysql_product_select_execute_delta": mysql_select_delta,
        "cache_info_before": cache_stats_before,
        "cache_info_after": cache_stats_after,
        "process_resources": {
            "service": process_resource_interval(service_before, service_after),
            "cache": process_resource_interval(cache_before, cache_after),
        },
        "latency_ms": {
            "p50": percentile(latencies, 50),
            "p95": percentile(latencies, 95),
            "p99": percentile(latencies, 99),
        },
        "latency_samples_ms": latencies,
    }


def batch_path(product_ids):
    return "/products?ids=" + ",".join(map(str, product_ids))


def warm_cache(service, product_ids):
    for offset in range(0, len(product_ids), 32):
        response = service.request("GET", batch_path(product_ids[offset : offset + 32]))
        if response[1] != 200 or response[3]:
            raise RuntimeError("cache warm-up failed")


def build_scenarios(service, product_ids, requests, randomizer):
    jobs = []
    for index in range(requests):
        product_id = randomizer.choice(product_ids)
        jobs.append(("GET", f"/products/{product_id}", None))
    records = [run_workload(service, "single_hit", jobs)]

    for size in (1, 8, 32):
        jobs = [
            ("GET", batch_path(randomizer.sample(product_ids, size)), None)
            for _ in range(requests)
        ]
        records.append(run_workload(service, f"batch_{size}_hit", jobs))

    missing_id = max(product_ids) + 1
    partial_jobs = []
    for _ in range(requests):
        partial_ids = randomizer.sample(product_ids, 7) + [missing_id]
        partial_jobs.append(("GET", batch_path(partial_ids), None))
    records.append(run_workload(service, "partial_batch_8", partial_jobs))

    missing_jobs = [("GET", f"/products/{missing_id}", None) for _ in range(requests)]
    records.append(run_workload(service, "hot_not_found", missing_jobs))

    cold_ids = product_ids[:requests]
    delete_cache_keys(service.cache_server, cold_ids)
    cold_jobs = [("GET", f"/products/{product_id}", None) for product_id in cold_ids]
    records.append(run_workload(service, "cold_miss", cold_jobs))

    write_count = min(requests, max(1, requests // 10))
    write_ids = product_ids[-write_count:]
    mixed_jobs = [
        (
            "PUT",
            f"/products/{product_id}",
            {"name": f"updated-{product_id}", "price_cents": 101, "expected_version": 1},
        )
        for product_id in write_ids
    ]
    read_pool = product_ids[:-write_count] or product_ids
    mixed_jobs.extend(
        ("GET", f"/products/{randomizer.choice(read_pool)}", None)
        for _ in range(requests - write_count)
    )
    randomizer.shuffle(mixed_jobs)
    records.append(run_workload(service, "mixed_get_put", mixed_jobs))
    return records


def run_hot_expiry(service, product_ids, requests):
    service.stop()
    service.ttl_seconds = 1
    service.ttl_jitter_seconds = 0
    service.start()
    hot_id = product_ids[0]
    delete_cache_keys(service.cache_server, [hot_id])
    warm = service.request("GET", f"/products/{hot_id}")
    if warm[1] != 200:
        raise RuntimeError("hot-key expiration setup failed")
    time.sleep(1.05)
    jobs = [("GET", f"/products/{hot_id}", None) for _ in range(max(requests, service.workers))]
    return run_workload(service, "hot_key_expiry", jobs)


def run_cache_fault(service, product_ids, requests):
    cache_server = service.cache_server
    count = max(1, min(requests, service.workers * 2))
    jobs = [("GET", f"/products/{product_ids[index % len(product_ids)]}", None) for index in range(count)]
    cache_server.stop()
    try:
        outage = run_workload(service, "cache_unavailable", jobs)
    finally:
        cache_server.start()

    deadline = time.monotonic() + 4
    recovery = None
    while time.monotonic() < deadline:
        recovery = run_workload(service, "cache_recovery", jobs[:1])
        if recovery["cache_source_counts"].get("BYPASS", 0) == 0:
            break
        time.sleep(0.05)
    if recovery is None or recovery["cache_source_counts"].get("BYPASS", 0) != 0:
        raise RuntimeError("cache did not recover after restart")
    return [outage, recovery]


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default="build")
    parser.add_argument("--products", type=int, default=512)
    parser.add_argument("--requests", type=int, default=200)
    parser.add_argument("--concurrency", type=int, default=16)
    parser.add_argument("--http-workers", type=int, default=None)
    parser.add_argument("--cache-memory-mb", type=int, default=64)
    parser.add_argument("--pressure-cache-memory-mb", type=int, default=8)
    parser.add_argument("--pressure-entries", type=int, default=64)
    parser.add_argument("--pressure-value-kib", type=int, default=256)
    parser.add_argument(
        "--redis-policy", choices=("noeviction", "allkeys-lru"), default="allkeys-lru"
    )
    parser.add_argument("--ttl-seconds", type=int, default=30)
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--output-dir", default=None)
    args = parser.parse_args()
    if args.products < 32 or args.requests < 1 or args.concurrency < 1 or args.repeat < 1:
        parser.error("products must be >= 32; requests, concurrency and repeat must be positive")
    if args.http_workers is None:
        args.http_workers = min(64, args.concurrency)
    if not 1 <= args.http_workers <= 64:
        parser.error("http-workers must be in 1..64")
    if args.cache_memory_mb < 4 or args.cache_memory_mb % 4 != 0:
        parser.error("cache-memory-mb must be a positive multiple of 4")
    if args.pressure_cache_memory_mb < 8 or args.pressure_cache_memory_mb % 4 != 0:
        parser.error("pressure-cache-memory-mb must be a multiple of 4 and at least 8")
    if args.pressure_entries < 1 or not 1 <= args.pressure_value_kib <= 1024:
        parser.error("pressure-entries must be positive and pressure-value-kib must be in 1..1024")
    if not 1 <= args.ttl_seconds <= 2592000:
        parser.error("ttl-seconds must be in 1..2592000")
    if not re_database(os.environ.get("SPHINX_TEST_MYSQL_DATABASE", "")):
        parser.error("provide a simple isolated database name through SPHINX_TEST_MYSQL_DATABASE")
    return args


def re_database(value):
    return bool(value) and all(character.isalnum() or character in "_$" for character in value)


def required_database_environment():
    names = (
        "SPHINX_TEST_MYSQL_HOST",
        "SPHINX_TEST_MYSQL_PORT",
        "SPHINX_TEST_MYSQL_USER",
        "SPHINX_TEST_MYSQL_PASSWORD",
        "SPHINX_TEST_MYSQL_DATABASE",
    )
    values = {name: os.environ.get(name) for name in names}
    if any(value is None for value in values.values()):
        raise RuntimeError("configure all SPHINX_TEST_MYSQL_* values for an isolated test database")
    if "test" not in values["SPHINX_TEST_MYSQL_DATABASE"].lower():
        raise RuntimeError("SPHINX_TEST_MYSQL_DATABASE must contain 'test'")
    if shutil.which("mysql") is None:
        raise RuntimeError("mysql client is required")
    return values


def run_combination(args, database, binaries, backend, policy, product_ids, randomizer):
    cache_server = CacheServer(backend, binaries[backend], args.cache_memory_mb, args.redis_policy)
    service = ProductService(
        binaries["service"], database, backend, policy, cache_server,
        args.http_workers, args.concurrency, args.ttl_seconds,
    )
    with cache_server, service:
        warm_cache(service, product_ids)
        workloads = build_scenarios(service, product_ids, args.requests, randomizer)
        workloads.append(run_hot_expiry(service, product_ids, args.requests))
        workloads.extend(run_cache_fault(service, product_ids, args.requests))

    with CacheServer(
        backend, binaries[backend], args.pressure_cache_memory_mb, args.redis_policy
    ) as pressure_cache:
        before = process_snapshot(pressure_cache.process)
        memory_pressure = run_memory_pressure(
            pressure_cache, args.pressure_entries, args.pressure_value_kib * 1024
        )
        memory_pressure["cache_process_resources"] = process_resource_interval(
            before, process_snapshot(pressure_cache.process)
        )

    return {
        "backend": backend,
        "policy": policy,
        "dataset_size": len(product_ids),
        "client_concurrency": args.concurrency,
        "http_workers": service.workers,
        "requested_requests_per_scenario": args.requests,
        "cache_memory_budget_mb": args.cache_memory_mb,
        "redis_maxmemory_policy": args.redis_policy if backend == "redis" else None,
        "ttl_seconds": args.ttl_seconds,
        "ttl_jitter_seconds": 3,
        "hot_expiry_ttl_seconds": 1,
        "workloads": workloads,
        "memory_pressure": memory_pressure,
    }


def main():
    args = parse_args()
    values = required_database_environment()
    build_dir = Path(args.build_dir)
    sphinx_binary = build_dir / "sphinxd" / "sphinxd"
    service_binary = build_dir / "product-service" / "sphinx-product-service"
    if not sphinx_binary.is_file() or not service_binary.is_file():
        raise RuntimeError("build sphinxd and sphinx-product-service before comparing backends")
    redis_binary = os.environ.get("SPHINX_TEST_REDIS_SERVER") or shutil.which("redis-server")
    if redis_binary is None or not Path(redis_binary).is_file():
        raise RuntimeError("redis-server is required for the Redis comparison")

    binaries = {"sphinx": str(sphinx_binary), "redis": redis_binary, "service": str(service_binary)}
    database = MySqlDatabase(values)
    schema_path = Path(__file__).resolve().parent.parent / "product-service" / "schema.sql"
    database.run(input_file=schema_path)
    output_dir = Path(args.output_dir or (build_dir / "product-cache-results"))
    output_dir.mkdir(parents=True, exist_ok=True)
    run_id = time.time_ns()
    product_count = max(args.products, args.requests)
    combo_width = product_count + 100
    start_id = 8_500_000_000_000_000 + run_id % 1_000_000_000
    existing = database.run(
        "SELECT COUNT(*) FROM products WHERE id BETWEEN "
        f"{start_id} AND {start_id + args.repeat * 4 * combo_width}"
    )
    if existing and int(existing) != 0:
        raise RuntimeError("generated benchmark ID range is already occupied")

    all_product_ids = []
    runs = []
    rng = random.Random(run_id)
    try:
        for repeat in range(args.repeat):
            for combo_index, (backend, policy) in enumerate(
                (("sphinx", "basic"), ("sphinx", "protected"), ("redis", "basic"), ("redis", "protected"))
            ):
                base = start_id + (repeat * 4 + combo_index) * combo_width
                products = [
                    (base + index, f"{backend}-{policy}-{index:08d}")
                    for index in range(product_count)
                ]
                product_ids = [product_id for product_id, _ in products]
                all_product_ids.extend(product_ids)
                database.seed(products)

                run = run_combination(args, database, binaries, backend, policy, product_ids, rng)
                runs.append(dict(run, repeat=repeat + 1))
    finally:
        database.delete_ids(all_product_ids)

    result = {
        "created_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "environment": {
            "platform": platform.platform(),
            "machine": platform.machine(),
            "logical_cpus": os.cpu_count(),
            "mysql_version": database.version(),
            "sphinx_version": subprocess.run(
                [str(sphinx_binary), "--version"], capture_output=True, text=True, check=False
            ).stdout.strip(),
            "redis_version": subprocess.run(
                [redis_binary, "--version"], capture_output=True, text=True, check=False
            ).stdout.strip(),
            "performance_schema_product_select_count_available": (
                database.statement_count() is not None
            ),
        },
        "conditions": {
            "products_per_combination": product_count,
            "requests_per_scenario": args.requests,
            "client_concurrency": args.concurrency,
            "http_workers": args.http_workers,
            "cache_memory_budget_mb": args.cache_memory_mb,
            "pressure_cache_memory_mb": args.pressure_cache_memory_mb,
            "pressure_entries": args.pressure_entries,
            "pressure_value_kib": args.pressure_value_kib,
            "redis_maxmemory_policy": args.redis_policy,
            "ttl_seconds": args.ttl_seconds,
            "repeat_count": args.repeat,
            "backends": ["sphinx", "redis"],
            "policies": ["basic", "protected"],
        },
        "runs": runs,
    }
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    output_file = output_dir / f"product-cache-comparison-{stamp}.json"
    output_file.write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(output_file)
    for run in runs:
        highlights = []
        for workload in run["workloads"]:
            if workload["scenario"] in {
                "single_hit",
                "batch_32_hit",
                "hot_not_found",
                "hot_key_expiry",
            }:
                latency = workload["latency_ms"]
                highlights.append(
                    f"{workload['scenario']} P95={latency['p95']:.3f}ms "
                    f"store_reads={workload['metrics_delta']['store_read_operations']}"
                )
        print(f"{run['backend']}/{run['policy']}: " + ", ".join(highlights))
        pressure = run["memory_pressure"]
        print(
            f"  memory pressure: accepted={pressure['write_accepted']} "
            f"rejected={pressure['write_rejected']} "
            f"retained={pressure['retained_entries_after_fill']} "
            f"evicted={pressure['redis_evicted_keys_delta']} "
            f"observed={pressure['pressure_observed']}"
        )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError) as error:
        print(f"comparison failed: {error}", file=sys.stderr)
        raise SystemExit(1) from None
