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
import signal
import socket
import subprocess
import sys
import tempfile
import time
from collections import Counter
from pathlib import Path


METRIC_NAMES = (
    "cache_lookup_keys",
    "cache_hits",
    "negative_hits",
    "cache_misses",
    "cache_corrupt",
    "cache_read_failures",
    "cache_fill_failures",
    "cache_invalidation_failures",
    "cache_circuit_bypasses",
    "store_read_operations",
    "store_read_ids",
    "store_read_failures",
    "read_leaders",
    "read_followers",
    "read_rejected",
    "read_wait_timeouts",
    "read_admission_rejected",
)


def reserve_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def read_line(connection):
    line = bytearray()
    while not line.endswith(b"\r\n"):
        chunk = connection.recv(1)
        if not chunk:
            break
        line.extend(chunk)
    if not line.endswith(b"\r\n"):
        raise RuntimeError("incomplete cache protocol response")
    return bytes(line[:-2])


def read_exactly(connection, size):
    value = bytearray()
    while len(value) < size:
        chunk = connection.recv(size - len(value))
        if not chunk:
            raise RuntimeError("cache closed before a response payload ended")
        value.extend(chunk)
    return bytes(value)


def read_redis_reply(connection):
    line = read_line(connection)
    if not line:
        raise RuntimeError("Redis returned an empty response")
    kind, payload = line[:1], line[1:]
    if kind == b"+":
        return payload
    if kind == b"-":
        raise RuntimeError(f"Redis command failed: {payload.decode(errors='replace')}")
    if kind == b":":
        return int(payload)
    if kind == b"$":
        size = int(payload)
        if size == -1:
            return None
        value = read_exactly(connection, size)
        if read_exactly(connection, 2) != b"\r\n":
            raise RuntimeError("Redis returned an invalid bulk terminator")
        return value
    if kind == b"*":
        size = int(payload)
        if size == -1:
            return None
        return [read_redis_reply(connection) for _ in range(size)]
    raise RuntimeError(f"Redis returned an unsupported reply type: {kind!r}")


def redis_command(port, *arguments):
    encoded = [
        argument if isinstance(argument, bytes) else str(argument).encode("utf-8")
        for argument in arguments
    ]
    request = bytearray(f"*{len(encoded)}\r\n".encode("ascii"))
    for argument in encoded:
        request.extend(f"${len(argument)}\r\n".encode("ascii"))
        request.extend(argument)
        request.extend(b"\r\n")
    with socket.create_connection(("127.0.0.1", port), timeout=2) as connection:
        connection.sendall(request)
        return read_redis_reply(connection)


def wait_for_port(process, port, log_file, timeout=8):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            log_file.flush()
            log_file.seek(0)
            raise RuntimeError(log_file.read().decode(errors="replace"))
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("local cache or HTTP service did not start")


def stop_process(process, log_file, backend=None, port=None):
    if process is None:
        return
    if process.poll() is None and backend == "redis":
        try:
            redis_command(port, "SHUTDOWN", "NOSAVE")
        except (OSError, RuntimeError):
            pass
    if process.poll() is None:
        process.send_signal(signal.SIGTERM)
    try:
        process.wait(timeout=8)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=2)
    log_file.close()


class MySqlDatabase:
    def __init__(self, values):
        self.host = values["SPHINX_TEST_MYSQL_HOST"]
        self.port = values["SPHINX_TEST_MYSQL_PORT"]
        self.user = values["SPHINX_TEST_MYSQL_USER"]
        self.password = values["SPHINX_TEST_MYSQL_PASSWORD"]
        self.database = values["SPHINX_TEST_MYSQL_DATABASE"]
        self.environment = os.environ.copy()
        self.environment["MYSQL_PWD"] = self.password

    def run(self, statement=None, input_file=None, check=True):
        command = [
            shutil.which("mysql") or "mysql",
            "--protocol=tcp",
            f"--host={self.host}",
            f"--port={self.port}",
            f"--user={self.user}",
            f"--database={self.database}",
            "--batch",
            "--skip-column-names",
            "--raw",
        ]
        if statement is not None:
            command.extend(["--execute", statement])
        source = open(input_file, "rb") if input_file is not None else subprocess.DEVNULL
        try:
            result = subprocess.run(
                command,
                env=self.environment,
                stdin=source,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                check=False,
            )
        finally:
            if input_file is not None:
                source.close()
        if check and result.returncode != 0:
            raise RuntimeError("isolated MySQL benchmark operation failed")
        return result.stdout.strip() if result.returncode == 0 else None

    def seed(self, rows):
        for offset in range(0, len(rows), 300):
            group = rows[offset : offset + 300]
            values = ",".join(
                f"({product_id},CONVERT(0x{name.encode('utf-8').hex()} USING utf8mb4),"
                "100,1)"
                for product_id, name in group
            )
            self.run(
                "INSERT INTO products (id,name,price_cents,version) VALUES " + values
            )

    def delete_ids(self, product_ids):
        for offset in range(0, len(product_ids), 300):
            group = product_ids[offset : offset + 300]
            if group:
                self.run("DELETE FROM products WHERE id IN (" + ",".join(map(str, group)) + ")")

    def statement_count(self):
        escaped_database = self.database.replace("'", "''")
        query = (
            "SELECT COALESCE(SUM(COUNT_STAR),0) "
            "FROM performance_schema.events_statements_summary_by_digest "
            f"WHERE SCHEMA_NAME='{escaped_database}' "
            "AND DIGEST_TEXT LIKE 'SELECT%products%'"
        )
        result = self.run(query, check=False)
        try:
            return int(result) if result else None
        except ValueError:
            return None

    def version(self):
        return self.run("SELECT VERSION()", check=False)


class CacheServer:
    def __init__(self, backend, binary, memory_mb, redis_policy):
        self.backend = backend
        self.binary = binary
        self.memory_mb = memory_mb
        self.redis_policy = redis_policy
        self.port = reserve_port()
        self.redis_directory = None
        self.process = None
        self.log_file = None
        if backend == "sphinx":
            self.command = [
                binary,
                "--listen",
                "127.0.0.1",
                "--port",
                str(self.port),
                "--threads",
                "2",
                "--memory-limit",
                str(memory_mb),
                "--segment-size",
                "2",
            ]
        else:
            self.redis_directory = tempfile.TemporaryDirectory(prefix="sphinx-compare-redis-")
            self.command = [
                binary,
                "--bind",
                "127.0.0.1",
                "--port",
                str(self.port),
                "--save",
                "",
                "--appendonly",
                "no",
                "--dir",
                self.redis_directory.name,
                "--maxmemory",
                f"{memory_mb}mb",
                "--maxmemory-policy",
                redis_policy,
            ]

    def start(self):
        self.log_file = tempfile.TemporaryFile()
        self.process = subprocess.Popen(
            self.command,
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
        stop_process(process, log_file, self.backend, self.port)

    def close(self):
        self.stop()
        if self.redis_directory is not None:
            self.redis_directory.cleanup()


class ProductService:
    def __init__(
        self,
        binary,
        database,
        backend,
        policy,
        cache_server,
        workers,
        ttl_seconds,
        ttl_jitter_seconds=3,
    ):
        self.binary = binary
        self.database = database
        self.backend = backend
        self.policy = policy
        self.cache_server = cache_server
        self.workers = workers
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
    if process is None:
        return None
    try:
        stat = Path(f"/proc/{process.pid}/stat").read_text(encoding="utf-8")
        fields = stat[stat.rfind(")") + 2 :].split()
        ticks = os.sysconf("SC_CLK_TCK")
        cpu_seconds = (int(fields[11]) + int(fields[12])) / ticks
        status = Path(f"/proc/{process.pid}/status").read_text(encoding="utf-8")
        rss_line = next(line for line in status.splitlines() if line.startswith("VmRSS:"))
        rss_bytes = int(rss_line.split()[1]) * 1024
        return {"cpu_seconds": cpu_seconds, "rss_bytes": rss_bytes}
    except (OSError, ValueError, StopIteration, IndexError):
        return None


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


def run_workload(service, database, label, jobs, workers):
    before = service.metrics()["counters"]
    sql_before = database.statement_count()
    started_at = time.perf_counter()
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as executor:
        responses = list(executor.map(lambda job: service.request(*job), jobs))
    elapsed_seconds = time.perf_counter() - started_at
    after = service.metrics()["counters"]
    sql_after = database.statement_count()

    latencies = sorted(response[0] for response in responses)
    statuses = Counter(str(response[1]) for response in responses)
    cache_sources = Counter(response[2] or "NOT_REPORTED" for response in responses)
    product_errors = Counter()
    for response in responses:
        product_errors.update(response[3])
    metric_deltas = {
        name: after.get(name, 0) - before.get(name, 0) for name in METRIC_NAMES
    }
    return {
        "scenario": label,
        "request_count": len(jobs),
        "elapsed_seconds": elapsed_seconds,
        "throughput_requests_per_second": len(jobs) / elapsed_seconds if elapsed_seconds else 0,
        "http_status_counts": dict(statuses),
        "cache_source_counts": dict(cache_sources),
        "product_error_counts": dict(product_errors),
        "metrics_delta": metric_deltas,
        "mysql_product_select_digest_delta": (
            sql_after - sql_before if sql_before is not None and sql_after is not None else None
        ),
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
        if response[1] != 200:
            raise RuntimeError("cache warm-up failed")


def build_scenarios(service, cache_server, product_ids, requests, randomizer):
    jobs = []
    for index in range(requests):
        product_id = randomizer.choice(product_ids)
        jobs.append(("GET", f"/products/{product_id}", None))
    records = [run_workload(service, service.database, "single_hit", jobs, service.workers)]

    for size in (1, 8, 32):
        jobs = [
            ("GET", batch_path(randomizer.sample(product_ids, size)), None)
            for _ in range(requests)
        ]
        records.append(
            run_workload(service, service.database, f"batch_{size}_hit", jobs, service.workers)
        )

    missing_id = max(product_ids) + 1
    partial_jobs = []
    for _ in range(requests):
        partial_ids = randomizer.sample(product_ids, 7) + [missing_id]
        partial_jobs.append(("GET", batch_path(partial_ids), None))
    records.append(
        run_workload(service, service.database, "partial_batch_8", partial_jobs, service.workers)
    )

    missing_jobs = [("GET", f"/products/{missing_id}", None) for _ in range(requests)]
    records.append(
        run_workload(service, service.database, "hot_not_found", missing_jobs, service.workers)
    )

    cold_ids = product_ids[:requests]
    delete_cache_keys(cache_server, cold_ids)
    cold_jobs = [("GET", f"/products/{product_id}", None) for product_id in cold_ids]
    records.append(
        run_workload(service, service.database, "cold_miss", cold_jobs, service.workers)
    )

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
    records.append(
        run_workload(service, service.database, "mixed_get_put", mixed_jobs, service.workers)
    )
    return records


def run_hot_expiry(service, cache_server, database, product_ids, requests):
    service.stop()
    service.ttl_seconds = 1
    service.ttl_jitter_seconds = 0
    service.start()
    hot_id = product_ids[0]
    delete_cache_keys(cache_server, [hot_id])
    warm = service.request("GET", f"/products/{hot_id}")
    if warm[1] != 200:
        raise RuntimeError("hot-key expiration setup failed")
    time.sleep(1.05)
    jobs = [("GET", f"/products/{hot_id}", None) for _ in range(max(requests, service.workers))]
    return run_workload(service, database, "hot_key_expiry", jobs, service.workers)


def run_cache_fault(service, cache_server, database, requests):
    count = max(1, min(requests, service.workers * 2))
    product_ids = getattr(service, "product_ids")
    jobs = [("GET", f"/products/{product_ids[index % len(product_ids)]}", None) for index in range(count)]
    cache_server.stop()
    try:
        outage = run_workload(service, database, "cache_unavailable", jobs, service.workers)
    finally:
        cache_server.start()

    deadline = time.monotonic() + 4
    recovery = None
    while time.monotonic() < deadline:
        recovery = run_workload(
            service,
            database,
            "cache_recovery",
            jobs[:1],
            service.workers,
        )
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
    parser.add_argument("--cache-memory-mb", type=int, default=64)
    parser.add_argument(
        "--redis-policy", choices=("noeviction", "allkeys-lru"), default="allkeys-lru"
    )
    parser.add_argument("--ttl-seconds", type=int, default=30)
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--output-dir", default=None)
    args = parser.parse_args()
    if args.products < 32 or args.requests < 1 or args.concurrency < 1 or args.repeat < 1:
        parser.error("products must be >= 32; requests, concurrency and repeat must be positive")
    if args.cache_memory_mb < 4 or args.cache_memory_mb % 4 != 0:
        parser.error("cache-memory-mb must be a positive multiple of 4")
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


def metrics_metadata(database):
    try:
        return {"mysql_version": database.version()}
    except RuntimeError:
        return {"mysql_version": None}


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

                cache_server = CacheServer(
                    backend,
                    str(sphinx_binary) if backend == "sphinx" else redis_binary,
                    args.cache_memory_mb,
                    args.redis_policy,
                )
                service = ProductService(
                    str(service_binary),
                    database,
                    backend,
                    policy,
                    cache_server,
                    max(2, min(64, args.concurrency)),
                    args.ttl_seconds,
                )
                service.database = database
                service.product_ids = product_ids
                try:
                    cache_server.start()
                    service.start()
                    warm_cache(service, product_ids)
                    process_before = {
                        "service": process_snapshot(service.process),
                        "cache": process_snapshot(cache_server.process),
                    }
                    cache_before = cache_info(cache_server)
                    workloads = build_scenarios(
                        service,
                        cache_server,
                        product_ids,
                        args.requests,
                        rng,
                    )
                    hot_expiry = run_hot_expiry(
                        service,
                        cache_server,
                        database,
                        product_ids,
                        args.requests,
                    )
                    workloads.append(hot_expiry)
                    workloads.extend(
                        run_cache_fault(service, cache_server, database, args.requests)
                    )
                    process_after = {
                        "service": process_snapshot(service.process),
                        "cache": process_snapshot(cache_server.process),
                    }
                    cache_after = cache_info(cache_server)
                    runs.append(
                        {
                            "repeat": repeat + 1,
                            "backend": backend,
                            "policy": policy,
                            "dataset_size": len(product_ids),
                            "client_concurrency": args.concurrency,
                            "http_workers": service.workers,
                            "requested_requests_per_scenario": args.requests,
                            "cache_memory_budget_mb": args.cache_memory_mb,
                            "redis_maxmemory_policy": (
                                args.redis_policy if backend == "redis" else None
                            ),
                            "ttl_seconds": args.ttl_seconds,
                            "ttl_jitter_seconds": 3,
                            "hot_expiry_ttl_seconds": 1,
                            "workloads": workloads,
                            "process_before": process_before,
                            "process_after": process_after,
                            "cache_info_before": cache_before,
                            "cache_info_after": cache_after,
                        }
                    )
                finally:
                    service.stop()
                    cache_server.close()
    finally:
        database.delete_ids(all_product_ids)

    result = {
        "created_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "environment": {
            "platform": platform.platform(),
            "machine": platform.machine(),
            "logical_cpus": os.cpu_count(),
            "mysql_version": metrics_metadata(database)["mysql_version"],
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
            "http_workers": max(2, min(64, args.concurrency)),
            "cache_memory_budget_mb": args.cache_memory_mb,
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
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError) as error:
        print(f"comparison failed: {error}", file=sys.stderr)
        raise SystemExit(1) from None
