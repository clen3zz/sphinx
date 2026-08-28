#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""使用伪造的子程序测试 scripts/run_local_benchmark.py。"""

from __future__ import annotations

import contextlib
import io
import json
import os
import stat
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parents[2] / "scripts"
sys.path.insert(0, str(SCRIPT_DIR))
import run_local_benchmark as benchmark  # noqa: E402


class BenchmarkScriptTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory(prefix="sphinx-benchmark-test-")
        self.root = Path(self.temp_dir.name)
        self.server = self._write_executable(
            "fake-server.py",
            r'''#!/usr/bin/env python3
import os
import socket
import sys

port = int(sys.argv[sys.argv.index("--port") + 1])
pid_file = os.environ.get("FAKE_SERVER_PID_FILE")
if pid_file:
    with open(pid_file, "w", encoding="utf-8") as stream:
        stream.write(str(os.getpid()))
if os.environ.get("FAKE_SERVER_MODE") == "early-exit":
    sys.exit(4)
sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(("127.0.0.1", port))
sock.listen(16)
sock.settimeout(0.05)
while True:
    try:
        client, _ = sock.accept()
        client.close()
    except socket.timeout:
        pass
''',
        )
        self.memtier = self._write_executable(
            "fake-memtier.py",
            r'''#!/usr/bin/env python3
import json
import os
import sys
import time

log_file = os.environ.get("FAKE_MEMTIER_LOG")
if log_file:
    with open(log_file, "a", encoding="utf-8") as stream:
        stream.write(json.dumps(sys.argv[1:]) + "\n")
output = sys.argv[sys.argv.index("--json-out-file") + 1]
mode = os.environ.get("FAKE_MEMTIER_MODE", "ok")
is_prefill = "--requests" in sys.argv
key_minimum = int(sys.argv[sys.argv.index("--key-minimum") + 1])
key_maximum = int(sys.argv[sys.argv.index("--key-maximum") + 1])
if mode != "missing":
    if mode == "invalid-json":
        with open(output, "w", encoding="utf-8") as stream:
            stream.write("not json")
        sys.exit(0)
    protocol = 1 if mode == "protocol-error" or (mode == "prefill-error" and is_prefill) else 0
    connection = 2 if mode == "connection-error" else 0
    other = 3 if mode == "other-error" else 0
    prefill_count = key_maximum - key_minimum
    if mode == "short-prefill" and is_prefill:
        prefill_count -= 1
    document = {
        "ALL STATS": {
            "Sets": {"Count": prefill_count if is_prefill else 0},
            "Totals": {
                "Ops/sec": 123.5,
                "p50": 1.0,
                "p95": 2.0,
                "p99": 3.0,
                "Connection Errors": connection,
                "Errors": other,
            }
        },
        "protocol_errors": protocol,
    }
    with open(output, "w", encoding="utf-8") as stream:
        json.dump(document, stream)
if mode == "nonzero":
    sys.exit(7)
time.sleep(0.02)
''',
        )

    def tearDown(self) -> None:
        self.temp_dir.cleanup()

    def _write_executable(self, name: str, content: str) -> Path:
        path = self.root / name
        path.write_text(content, encoding="utf-8")
        path.chmod(path.stat().st_mode | stat.S_IXUSR)
        return path

    def _config(self, **overrides: object) -> benchmark.BenchmarkConfig:
        values: dict[str, object] = {
            "server_executable": str(self.server),
            "memtier_executable": str(self.memtier),
            "output_dir": self.root / "results",
            "workload": "mixed",
            "threads": 2,
            "memory_limit_mb": 8,
            "segment_size_mb": 1,
            "duration_seconds": 1,
            "client_threads": 2,
            "connections_per_thread": 3,
            "key_space": 20,
            "value_size": 16,
            "startup_timeout_seconds": 2.0,
            "shutdown_timeout_seconds": 1.0,
        }
        values.update(overrides)
        return benchmark.BenchmarkConfig(**values)

    def _run_with_fake_processes(
            self, mode: str = "ok", **config_overrides: object
    ) -> benchmark.BenchmarkResult:
        pid_file = self.root / "server.pid"
        log_file = self.root / "memtier.log"
        old_values = {
            "FAKE_MEMTIER_MODE": os.environ.get("FAKE_MEMTIER_MODE"),
            "FAKE_SERVER_PID_FILE": os.environ.get("FAKE_SERVER_PID_FILE"),
            "FAKE_MEMTIER_LOG": os.environ.get("FAKE_MEMTIER_LOG"),
            "FAKE_SERVER_MODE": os.environ.get("FAKE_SERVER_MODE"),
        }
        os.environ["FAKE_MEMTIER_MODE"] = mode
        os.environ["FAKE_SERVER_PID_FILE"] = str(pid_file)
        os.environ["FAKE_MEMTIER_LOG"] = str(log_file)
        try:
            result = benchmark.run_benchmark(self._config(**config_overrides))
            assert result is not None
            return result
        finally:
            for key, old_value in old_values.items():
                if old_value is None:
                    os.environ.pop(key, None)
                else:
                    os.environ[key] = old_value

    def test_rejects_invalid_configuration(self) -> None:
        with self.assertRaises(benchmark.BenchmarkError):
            benchmark.validate_config(self._config(memory_limit_mb=7))
        with self.assertRaises(benchmark.BenchmarkError):
            benchmark.validate_config(self._config(workload="scan"))
        with self.assertRaises(benchmark.BenchmarkError):
            benchmark.validate_config(self._config(duration_seconds=0))
        with self.assertRaises(benchmark.BenchmarkError):
            benchmark.validate_config(self._config(segment_size_mb=3))

    def test_workload_ratios_and_protocol_are_explicit(self) -> None:
        expected = {"read": "0:1", "mixed": "1:9", "write": "1:0"}
        for workload, ratio in expected.items():
            config = self._config(workload=workload)
            command = benchmark.build_memtier_command(config, 11211, Path("result.json"))
            self.assertIn("memcache_text", command)
            self.assertEqual(
                command[command.index("--print-percentiles") + 1], "50,95,99"
            )
            self.assertEqual(command[command.index("--ratio") + 1], ratio)
            self.assertEqual(command[command.index("--key-minimum") + 1], "1")
            self.assertEqual(command[command.index("--key-maximum") + 1], "21")
            self.assertEqual(command[command.index("--test-time") + 1], "1")
            prefill = benchmark.build_memtier_command(
                config, 11211, Path("prefill.json"), prefill=True
            )
            self.assertEqual(prefill[prefill.index("--ratio") + 1], "1:0")
            self.assertEqual(prefill[prefill.index("--requests") + 1], "allkeys")
            self.assertNotIn("--test-time", prefill)
            self.assertEqual(prefill[prefill.index("--threads") + 1], "1")
            self.assertEqual(prefill[prefill.index("--clients") + 1], "1")
            self.assertEqual(prefill[prefill.index("--key-maximum") + 1], "21")

    def test_dry_run_does_not_start_process_or_create_results(self) -> None:
        output_dir = self.root / "dry-run-results"
        config = self._config(
            server_executable=str(self.root / "does-not-exist-server"),
            memtier_executable=str(self.root / "does-not-exist-memtier"),
            output_dir=output_dir,
            dry_run=True,
        )
        stream = io.StringIO()
        with contextlib.redirect_stdout(stream):
            self.assertIsNone(benchmark.run_benchmark(config))
        output = stream.getvalue()
        self.assertIn("memcache_text", output)
        self.assertIn("<free-port>", output)
        self.assertFalse(output_dir.exists())

    def test_success_saves_raw_json_metadata_and_cleans_server(self) -> None:
        result = self._run_with_fake_processes()
        self.assertTrue(result.raw_result_path.is_file())
        self.assertTrue(result.metadata_path.is_file())
        self.assertEqual(result.protocol_errors, 0)
        self.assertEqual(result.metrics["qps"], 123.5)
        document = json.loads(result.raw_result_path.read_text(encoding="utf-8"))
        metadata = json.loads(result.metadata_path.read_text(encoding="utf-8"))
        self.assertEqual(document["protocol_errors"], 0)
        self.assertEqual(metadata["workload"], "mixed")
        self.assertEqual(metadata["client"]["protocol"], "memcache_text")
        self.assertEqual(metadata["raw_result"], result.raw_result_path.name)
        self.assertEqual(metadata["error_counts"], {"connection": 0, "other": 0, "protocol": 0})
        self.assertEqual(
            metadata["prefill_error_counts"], {"connection": 0, "other": 0, "protocol": 0}
        )
        self.assertEqual(metadata["prefill_count"], 20)
        self.assertIsInstance(metadata["git_dirty"], bool)
        self.assertTrue(metadata["source_state_id"])
        command_lines = (self.root / "memtier.log").read_text(encoding="utf-8").splitlines()
        self.assertEqual(len(command_lines), 2, "mixed workload must prefill then measure")
        self.assertFalse(self._pid_exists(self.root / "server.pid"))

    def test_write_workload_has_no_prefill(self) -> None:
        result = self._run_with_fake_processes()
        self.assertIsNotNone(result)
        old_log = os.environ.get("FAKE_MEMTIER_LOG")
        log_file = self.root / "memtier-write.log"
        os.environ["FAKE_MEMTIER_LOG"] = str(log_file)
        try:
            result = benchmark.run_benchmark(self._config(workload="write"))
            self.assertIsNotNone(result)
        finally:
            if old_log is None:
                os.environ.pop("FAKE_MEMTIER_LOG", None)
            else:
                os.environ["FAKE_MEMTIER_LOG"] = old_log
        command_lines = (self.root / "memtier.log").read_text(encoding="utf-8").splitlines()
        command_lines = (log_file.read_text(encoding="utf-8")).splitlines()
        self.assertEqual(len(command_lines), 1)
        last_command = json.loads(command_lines[-1])
        self.assertEqual(last_command[last_command.index("--ratio") + 1], "1:0")

    def test_nonzero_memtier_is_failure_and_server_is_cleaned(self) -> None:
        with self.assertRaises(benchmark.BenchmarkError):
            self._run_with_fake_processes(mode="nonzero")
        self.assertFalse(self._pid_exists(self.root / "server.pid"))

    def test_protocol_errors_are_failure(self) -> None:
        with self.assertRaisesRegex(benchmark.BenchmarkError, "reported errors"):
            self._run_with_fake_processes(mode="protocol-error", workload="write")
        self.assertFalse(self._pid_exists(self.root / "server.pid"))

    def test_connection_and_other_errors_are_failure(self) -> None:
        for mode, message in (("connection-error", "connection"), ("other-error", "other")):
            with self.subTest(mode=mode), self.assertRaisesRegex(benchmark.BenchmarkError, "reported errors"):
                self._run_with_fake_processes(mode=mode)
            self.assertFalse(self._pid_exists(self.root / "server.pid"))

    def test_prefill_errors_are_failure(self) -> None:
        with self.assertRaisesRegex(benchmark.BenchmarkError, "prefill reported errors"):
            self._run_with_fake_processes(mode="prefill-error")

    def test_incomplete_prefill_is_failure(self) -> None:
        with self.assertRaisesRegex(benchmark.BenchmarkError, "prefill wrote 19 keys"):
            self._run_with_fake_processes(mode="short-prefill")
        self.assertFalse(self._pid_exists(self.root / "server.pid"))

    def test_missing_result_is_failure(self) -> None:
        with self.assertRaisesRegex(benchmark.BenchmarkError, "missing or empty"):
            self._run_with_fake_processes(mode="missing")
        self.assertFalse(self._pid_exists(self.root / "server.pid"))

    def test_invalid_json_is_failure(self) -> None:
        with self.assertRaisesRegex(benchmark.BenchmarkError, "valid JSON"):
            self._run_with_fake_processes(mode="invalid-json")
        self.assertFalse(self._pid_exists(self.root / "server.pid"))

    def test_server_exit_before_ready_is_failure(self) -> None:
        old_mode = os.environ.get("FAKE_SERVER_MODE")
        os.environ["FAKE_SERVER_MODE"] = "early-exit"
        try:
            with self.assertRaisesRegex(benchmark.BenchmarkError, "before becoming ready"):
                self._run_with_fake_processes()
        finally:
            if old_mode is None:
                os.environ.pop("FAKE_SERVER_MODE", None)
            else:
                os.environ["FAKE_SERVER_MODE"] = old_mode

    def test_protocol_error_counter_supports_nested_layouts(self) -> None:
        self.assertEqual(benchmark.protocol_error_count({"protocol_errors": 2}), 2)
        self.assertEqual(benchmark.protocol_error_count({"Totals": {"Errors": 3}}), 3)
        self.assertEqual(benchmark.protocol_error_count({"Totals": {"Errors": 0}}), 0)
        self.assertEqual(
            benchmark.error_counts(
                {
                    "Totals": {
                        "Connection Errors": 2,
                        "Errors": 3,
                        "Failed Requests": 4,
                        "Errors/sec": 9,
                    }
                }
            ),
            {"connection": 2, "other": 7, "protocol": 0},
        )

    def test_metrics_come_from_totals_not_inactive_command_placeholders(self) -> None:
        document = {
            "ALL STATS": {
                "Sets": {
                    "Ops/sec": 0.0,
                    "Percentile Latencies": {"p50.00": 0.007, "p95.00": 0.007, "p99.00": 0.007},
                },
                "Gets": {
                    "Ops/sec": 80000.0,
                    "Percentile Latencies": {"p50.00": 0.3, "p95.00": 0.6, "p99.00": 0.8},
                },
                "Totals": {
                    "Ops/sec": 80000.0,
                    "Percentile Latencies": {"p50.00": 0.3, "p95.00": 0.6, "p99.00": 0.8},
                },
            }
        }
        self.assertEqual(
            benchmark.extract_metrics(document),
            {"qps": 80000.0, "p50": 0.3, "p95": 0.6, "p99": 0.8},
        )

    @staticmethod
    def _pid_exists(pid_file: Path) -> bool:
        if not pid_file.exists():
            return False
        pid = int(pid_file.read_text(encoding="utf-8"))
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return False
        except PermissionError:
            return True
        return True


if __name__ == "__main__":
    unittest.main()
