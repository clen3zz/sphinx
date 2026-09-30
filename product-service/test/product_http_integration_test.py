#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""MySQL + Sphinx 商品服务的黑盒验收测试。"""

import concurrent.futures
import http.client
import itertools
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path


SPHINXD_BINARY = None
PRODUCT_SERVICE_BINARY = None
PRODUCT_ID_BASE = 8_000_000_000_000_000
PRODUCT_IDS = itertools.count(PRODUCT_ID_BASE)


def reserve_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def wait_for_port(process, port, log_file, timeout=5.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            log_file.flush()
            log_file.seek(0)
            output = log_file.read().decode(errors="replace")
            raise RuntimeError(f"service exited during startup: {output}")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("service did not bind its local test port")


class ProductHttpIntegrationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        mysql_values = {
            name: os.environ.get(name)
            for name in (
                "SPHINX_TEST_MYSQL_HOST",
                "SPHINX_TEST_MYSQL_PORT",
                "SPHINX_TEST_MYSQL_USER",
                "SPHINX_TEST_MYSQL_PASSWORD",
                "SPHINX_TEST_MYSQL_DATABASE",
            )
        }
        if any(value is None for value in mysql_values.values()):
            raise unittest.SkipTest("requires SPHINX_TEST_MYSQL_* for a disposable test database")
        if "test" not in mysql_values["SPHINX_TEST_MYSQL_DATABASE"].lower():
            raise unittest.SkipTest("disposable MySQL database name must include test")
        if shutil.which("mysql") is None:
            raise unittest.SkipTest("requires the mysql client for disposable fixture setup")
        if not SPHINXD_BINARY.is_file() or not PRODUCT_SERVICE_BINARY.is_file():
            raise RuntimeError("optional service binaries are missing")

        try:
            cls.mysql_port = int(mysql_values["SPHINX_TEST_MYSQL_PORT"], 10)
        except ValueError as error:
            raise RuntimeError("SPHINX_TEST_MYSQL_PORT is invalid") from error
        if not 1 <= cls.mysql_port <= 65535:
            raise RuntimeError("SPHINX_TEST_MYSQL_PORT is outside 1..65535")
        cls.mysql_host = mysql_values["SPHINX_TEST_MYSQL_HOST"]
        cls.mysql_user = mysql_values["SPHINX_TEST_MYSQL_USER"]
        cls.mysql_password = mysql_values["SPHINX_TEST_MYSQL_PASSWORD"]
        cls.mysql_database = mysql_values["SPHINX_TEST_MYSQL_DATABASE"]
        cls.base_environment = os.environ.copy()
        cls.base_environment.update(
            {
                "SPHINX_MYSQL_HOST": cls.mysql_host,
                "SPHINX_MYSQL_PORT": str(cls.mysql_port),
                "SPHINX_MYSQL_USER": cls.mysql_user,
                "SPHINX_MYSQL_PASSWORD": cls.mysql_password,
                "SPHINX_MYSQL_DATABASE": cls.mysql_database,
                "SPHINX_HTTP_BIND": "127.0.0.1",
                "SPHINX_HTTP_WORKERS": "2",
                "SPHINX_CACHE_BACKEND": "sphinx",
                "SPHINX_CACHE_TIMEOUT_MS": "200",
                "SPHINX_CACHE_TTL_SECONDS": "30",
            }
        )
        cls.sphinx_port = reserve_port()
        cls.sphinx_log = tempfile.TemporaryFile()
        cls.sphinx_process = subprocess.Popen(
            [
                str(SPHINXD_BINARY),
                "--listen",
                "127.0.0.1",
                "--port",
                str(cls.sphinx_port),
                "--threads",
                "2",
            ],
            stdout=cls.sphinx_log,
            stderr=cls.sphinx_log,
        )
        try:
            wait_for_port(cls.sphinx_process, cls.sphinx_port, cls.sphinx_log)
        except Exception:
            cls._stop_process(cls.sphinx_process, cls.sphinx_log, expected=None)
            raise

    @classmethod
    def tearDownClass(cls):
        process = getattr(cls, "sphinx_process", None)
        if process is not None:
            cls._stop_process(process, cls.sphinx_log, expected=None)

    @staticmethod
    def _stop_process(process, log_file, expected):
        if process.poll() is None:
            process.send_signal(signal.SIGTERM)
        try:
            status = process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=2)
            log_file.flush()
            log_file.seek(0)
            output = log_file.read().decode(errors="replace")
            log_file.close()
            raise AssertionError(f"server did not stop within the test timeout: {output}")
        log_file.flush()
        log_file.seek(0)
        output = log_file.read().decode(errors="replace")
        log_file.close()
        if expected is not None and status != expected:
            raise AssertionError(f"server exited with {status}: {output}")
        return status, output

    def setUp(self):
        self.rows = []
        self.cache_keys = set()
        self.service_process = None
        self.service_log = None
        self.http_port = None
        self.start_service()

    def tearDown(self):
        if self.service_process is not None:
            self.stop_service(expected=0)
        if self.rows:
            ids = ",".join(str(product_id) for product_id in self.rows)
            self.run_mysql(f"DELETE FROM products WHERE id IN ({ids})")
        for key in self.cache_keys:
            self.delete_cache(key)

    def mysql_environment(self):
        environment = self.base_environment.copy()
        environment["MYSQL_PWD"] = self.mysql_password
        return environment

    def run_mysql(self, statement):
        command = [
            "mysql",
            "--protocol=tcp",
            f"--host={self.mysql_host}",
            f"--port={self.mysql_port}",
            f"--user={self.mysql_user}",
            f"--database={self.mysql_database}",
            "--batch",
            "--skip-column-names",
            "--raw",
            "--execute",
            statement,
        ]
        result = subprocess.run(
            command,
            env=self.mysql_environment(),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        if result.returncode != 0:
            raise AssertionError(f"disposable MySQL fixture command failed: {result.stderr.strip()}")
        return result.stdout.strip()

    def new_product_id(self):
        product_id = next(PRODUCT_IDS)
        self.rows.append(product_id)
        self.cache_keys.add(f"product:v2:{product_id}")
        return product_id

    def insert_product(self, name="product-v1", price_cents=100, version=1):
        product_id = self.new_product_id()
        name_hex = name.encode("utf-8").hex()
        self.run_mysql(
            "INSERT INTO products (id, name, price_cents, version) VALUES "
            f"({product_id}, CONVERT(0x{name_hex} USING utf8mb4), {price_cents}, {version})"
        )
        return product_id

    def update_database_product(self, product_id, name, price_cents, version):
        name_hex = name.encode("utf-8").hex()
        self.run_mysql(
            "UPDATE products SET "
            f"name=CONVERT(0x{name_hex} USING utf8mb4), price_cents={price_cents}, "
            f"version={version} WHERE id={product_id}"
        )

    def database_product(self, product_id):
        result = self.run_mysql(
            "SELECT id, name, price_cents, version FROM products "
            f"WHERE id={product_id}"
        )
        if not result:
            return None
        product_id, name, price_cents, version = result.split("\t")
        return {
            "id": int(product_id),
            "name": name,
            "price_cents": int(price_cents),
            "version": int(version),
        }

    def start_service(self, cache_nodes=None, mysql_port=None, workers=2, port=None):
        if self.service_process is not None:
            self.stop_service(expected=0)
        self.http_port = reserve_port() if port is None else port
        environment = self.base_environment.copy()
        environment["SPHINX_HTTP_PORT"] = str(self.http_port)
        environment["SPHINX_HTTP_WORKERS"] = str(workers)
        environment["SPHINX_CACHE_NODES"] = cache_nodes or f"127.0.0.1:{self.sphinx_port}"
        environment["SPHINX_MYSQL_PORT"] = str(self.mysql_port if mysql_port is None else mysql_port)
        self.service_log = tempfile.TemporaryFile()
        self.service_process = subprocess.Popen(
            [str(PRODUCT_SERVICE_BINARY)],
            env=environment,
            stdout=self.service_log,
            stderr=self.service_log,
        )
        try:
            wait_for_port(self.service_process, self.http_port, self.service_log)
        except Exception:
            process = self.service_process
            log_file = self.service_log
            self.service_process = None
            self.service_log = None
            self._stop_process(process, log_file, expected=None)
            raise

    def stop_service(self, expected=0):
        process = self.service_process
        log_file = self.service_log
        self.service_process = None
        self.service_log = None
        if process is None:
            return
        status, output = self._stop_process(process, log_file, expected=None)
        if expected is not None and status != expected:
            raise AssertionError(f"product service exited with {status}: {output}")

    def http_request(self, method, path, body=None, content_type="application/json"):
        if isinstance(body, dict):
            body = json.dumps(body, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        headers = {}
        if content_type is not None:
            headers["Content-Type"] = content_type
        connection = http.client.HTTPConnection("127.0.0.1", self.http_port, timeout=5)
        try:
            connection.request(method, path, body=body, headers=headers)
            response = connection.getresponse()
            response_body = response.read()
            return response.status, response.headers, response_body
        finally:
            connection.close()

    def assert_no_store(self, headers):
        self.assertEqual(headers.get("Cache-Control"), "no-store")

    def assert_json_error(self, response, status, error):
        response_status, headers, body = response
        self.assertEqual(response_status, status)
        self.assertEqual(json.loads(body.decode("utf-8")), {"error": error})
        self.assert_no_store(headers)

    def encode_cached_product(self, product_id, name, price_cents, version):
        return json.dumps({
            "id": product_id,
            "name": name,
            "price_cents": price_cents,
            "version": version,
        }, ensure_ascii=False).encode("utf-8")

    def set_cache(self, product_id, name, price_cents, version, ttl=60):
        key = f"product:v2:{product_id}"
        value = self.encode_cached_product(product_id, name, price_cents, version)
        with socket.create_connection(("127.0.0.1", self.sphinx_port), timeout=2) as client:
            client.sendall(f"set {key} 0 {ttl} {len(value)}\r\n".encode() + value + b"\r\n")
            response = self.read_line(client)
        self.assertEqual(response, b"STORED\r\n")
        self.cache_keys.add(key)

    @staticmethod
    def read_line(connection):
        line = bytearray()
        while not line.endswith(b"\r\n"):
            chunk = connection.recv(1)
            if not chunk:
                break
            line.extend(chunk)
        return bytes(line)

    def delete_cache(self, key):
        try:
            with socket.create_connection(("127.0.0.1", self.sphinx_port), timeout=1) as client:
                client.sendall(f"delete {key}\r\n".encode())
                self.read_line(client)
        except OSError:
            pass

    def test_get_miss_then_hit_and_fresh_primary_read(self):
        product_id = self.insert_product("primary-v1", 110, 1)
        first = self.http_request("GET", f"/products/{product_id}")
        self.assertEqual(first[0], 200)
        self.assertEqual(first[1].get("X-Cache"), "MISS")
        self.assertEqual(json.loads(first[2]), {
            "id": product_id, "name": "primary-v1", "price_cents": 110, "version": 1
        })
        self.assert_no_store(first[1])

        second = self.http_request("GET", f"/products/{product_id}")
        self.assertEqual(second[0], 200)
        self.assertEqual(second[1].get("X-Cache"), "HIT")
        self.assertEqual(json.loads(second[2])["version"], 1)

        self.update_database_product(product_id, "primary-v2", 220, 2)
        stale = self.http_request("GET", f"/products/{product_id}")
        self.assertEqual(stale[1].get("X-Cache"), "HIT")
        self.assertEqual(json.loads(stale[2])["version"], 1)
        fresh = self.http_request("GET", f"/products/{product_id}?fresh=1")
        self.assertEqual(fresh[0], 200)
        self.assertEqual(fresh[1].get("X-Cache"), "BYPASS")
        self.assertEqual(json.loads(fresh[2]), {
            "id": product_id, "name": "primary-v2", "price_cents": 220, "version": 2
        })

    def test_put_success_conflict_and_missing_row(self):
        product_id = self.insert_product("before-put", 10, 1)
        missing_id = self.new_product_id()
        initial = self.http_request("GET", f"/products/{product_id}")
        self.assertEqual(initial[1].get("X-Cache"), "MISS")
        updated = self.http_request(
            "PUT",
            f"/products/{product_id}",
            {"name": "after-put", "price_cents": 20, "expected_version": 1},
            "application/json; charset=utf-8",
        )
        self.assertEqual(updated[0], 200)
        self.assertEqual(json.loads(updated[2]), {
            "id": product_id, "name": "after-put", "price_cents": 20, "version": 2
        })
        self.assertIsNone(updated[1].get("X-Cache-Invalidation"))
        self.assertEqual(self.database_product(product_id)["version"], 2)

        after_put = self.http_request("GET", f"/products/{product_id}")
        self.assertEqual(after_put[1].get("X-Cache"), "MISS")
        self.assertEqual(json.loads(after_put[2])["version"], 2)
        conflict = self.http_request(
            "PUT",
            f"/products/{product_id}",
            {"name": "stale", "price_cents": 30, "expected_version": 1},
        )
        self.assert_json_error(conflict, 409, "conflict")
        cached_after_conflict = self.http_request("GET", f"/products/{product_id}")
        self.assertEqual(cached_after_conflict[1].get("X-Cache"), "HIT")
        self.assertEqual(json.loads(cached_after_conflict[2])["version"], 2)

        missing_get = self.http_request("GET", f"/products/{missing_id}")
        self.assert_json_error(missing_get, 404, "not_found")
        missing_put = self.http_request(
            "PUT",
            f"/products/{missing_id}",
            {"name": "absent", "price_cents": 1, "expected_version": 1},
        )
        self.assert_json_error(missing_put, 404, "not_found")

    def test_cache_outage_and_mysql_outage(self):
        product_id = self.insert_product("cache-fallback", 50, 3)
        dead_cache_port = reserve_port()
        self.start_service(cache_nodes=f"127.0.0.1:{dead_cache_port}")
        cache_outage = self.http_request("GET", f"/products/{product_id}")
        self.assertEqual(cache_outage[0], 200)
        self.assertEqual(cache_outage[1].get("X-Cache"), "BYPASS")
        self.assertEqual(json.loads(cache_outage[2])["version"], 3)

        cached_id = self.insert_product("cached-during-db-outage", 60, 4)
        missing_id = self.new_product_id()
        self.set_cache(cached_id, "cached-during-db-outage", 60, 4)
        dead_mysql_port = reserve_port()
        self.start_service(mysql_port=dead_mysql_port)
        cache_hit = self.http_request("GET", f"/products/{cached_id}")
        self.assertEqual(cache_hit[0], 200)
        self.assertEqual(cache_hit[1].get("X-Cache"), "HIT")
        self.assertEqual(json.loads(cache_hit[2])["version"], 4)
        cache_miss = self.http_request("GET", f"/products/{missing_id}")
        self.assert_json_error(cache_miss, 503, "store_unavailable")
        self.assertEqual(cache_miss[1].get("X-Cache"), "MISS")

    def test_invalid_json_body_and_oversize_payload(self):
        product_id = self.insert_product("unchanged", 70, 1)
        invalid_bodies = [
            b'{"name":"a","price_cents":-1,"expected_version":1}',
            b'{"name":"a","price_cents":1.0,"expected_version":1}',
            b'{"name":"a","price_cents":1,"expected_version":1,"extra":0}',
            b'{"name":"\\u0001","price_cents":1,"expected_version":1}',
        ]
        for body in invalid_bodies:
            response = self.http_request("PUT", f"/products/{product_id}", body)
            self.assert_json_error(response, 400, "invalid_argument")
        wrong_type = self.http_request(
            "PUT", f"/products/{product_id}", b"{}", "text/plain"
        )
        self.assert_json_error(wrong_type, 415, "unsupported_media_type")
        oversized = self.http_request(
            "PUT", f"/products/{product_id}", b"x" * 70000, "application/json"
        )
        self.assert_json_error(oversized, 413, "payload_too_large")
        self.assertEqual(self.database_product(product_id), {
            "id": product_id, "name": "unchanged", "price_cents": 70, "version": 1
        })

    def test_two_workers_use_independent_mysql_transactions(self):
        product_id = self.insert_product("worker-race", 1, 1)
        lock_name = f"sphinx_http_lock_{product_id}"
        lock_sql = (
            "START TRANSACTION; "
            f"SELECT id FROM products WHERE id={product_id} FOR UPDATE; "
            f"SELECT GET_LOCK('{lock_name}', 0); "
            "DO SLEEP(1.5); ROLLBACK; "
            f"SELECT RELEASE_LOCK('{lock_name}')"
        )
        locker = subprocess.Popen(
            [
                "mysql",
                "--protocol=tcp",
                f"--host={self.mysql_host}",
                f"--port={self.mysql_port}",
                f"--user={self.mysql_user}",
                f"--database={self.mysql_database}",
                "--execute",
                lock_sql,
            ],
            env=self.mysql_environment(),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
        try:
            deadline = time.monotonic() + 2
            while time.monotonic() < deadline:
                if self.run_mysql(
                    f"SELECT IS_USED_LOCK('{lock_name}') IS NOT NULL"
                ) == "1":
                    break
                if locker.poll() is not None:
                    error = locker.stderr.read().decode(errors="replace")
                    raise AssertionError(f"fixture row lock failed: {error}")
                time.sleep(0.03)
            else:
                raise AssertionError("fixture row lock was not established")

            body = {"name": "worker-winner", "price_cents": 2, "expected_version": 1}
            with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
                requests = [
                    executor.submit(self.http_request, "PUT", f"/products/{product_id}", body)
                    for _ in range(2)
                ]
                concurrent.futures.wait(requests, timeout=0.2, return_when=concurrent.futures.FIRST_COMPLETED)
                self.assertFalse(
                    any(request.done() for request in requests),
                    "both HTTP updates should remain pending behind the fixture's row lock",
                )
                locker.wait(timeout=2)
                responses = [request.result(timeout=4) for request in requests]
            self.assertEqual(sorted(response[0] for response in responses), [200, 409])
            self.assertEqual(self.database_product(product_id)["version"], 2)
        finally:
            if locker.poll() is None:
                locker.kill()
            locker.wait(timeout=2)
            if locker.stderr is not None:
                locker.stderr.close()

    def test_signal_shutdown_and_bind_failure(self):
        product_id = self.insert_product("shutdown", 5, 1)
        response = self.http_request("GET", f"/products/{product_id}")
        self.assertEqual(response[0], 200)

        bind_blocker = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        bind_blocker.bind(("127.0.0.1", 0))
        bind_blocker.listen(1)
        second_log = tempfile.TemporaryFile()
        second_environment = self.base_environment.copy()
        second_environment["SPHINX_HTTP_BIND"] = "127.0.0.1"
        second_environment["SPHINX_HTTP_PORT"] = str(bind_blocker.getsockname()[1])
        second_environment["SPHINX_CACHE_NODES"] = f"127.0.0.1:{self.sphinx_port}"
        second = subprocess.Popen(
            [str(PRODUCT_SERVICE_BINARY)],
            env=second_environment,
            stdout=second_log,
            stderr=second_log,
        )
        try:
            try:
                bind_status = second.wait(timeout=3)
            except subprocess.TimeoutExpired:
                second.kill()
                second.wait(timeout=2)
                second_log.flush()
                second_log.seek(0)
                bind_log = second_log.read().decode(errors="replace")
                second_log.close()
                self.fail(f"service did not exit after bind failure: {bind_log}")
            second_log.flush()
            second_log.seek(0)
            bind_log = second_log.read().decode(errors="replace")
            second_log.close()
        finally:
            bind_blocker.close()
        self.assertEqual(bind_status, 1, bind_log)

        process = self.service_process
        self.service_process = None
        log_file = self.service_log
        self.service_log = None
        process.send_signal(signal.SIGTERM)
        status, output = self._stop_process(process, log_file, expected=None)
        self.assertEqual(status, 0, output)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: product_http_integration_test.py SPHINXD PRODUCT_SERVICE")
    SPHINXD_BINARY = Path(sys.argv[1])
    PRODUCT_SERVICE_BINARY = Path(sys.argv[2])
    unittest.main(argv=[sys.argv[0]])
