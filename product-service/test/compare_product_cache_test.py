#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Regression tests for benchmark concurrency and process resource accounting."""

import importlib.util
import os
import sys
import threading
import unittest
from collections import Counter
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "compare_product_cache.py"
sys.path.insert(0, str(SCRIPT.parent))
SPEC = importlib.util.spec_from_file_location("cache_comparison", SCRIPT)
comparison = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(comparison)


class CoordinatedService:
    def __init__(self, client_concurrency):
        self.client_concurrency = client_concurrency
        self.workers = 2
        self.database = SimpleNamespace(statement_count=lambda: 0)
        self.process = None
        self.cache_server = SimpleNamespace(process=None)
        self.condition = threading.Condition()
        self.started = 0
        self.reached_concurrency = False

    @staticmethod
    def metrics():
        return {"counters": {"store_read_operations": 0, "store_read_failures": 0}}

    def request(self, *_arguments):
        with self.condition:
            self.started += 1
            self.condition.notify_all()
            reached = self.condition.wait_for(
                lambda: self.started == self.client_concurrency, timeout=2
            )
            self.reached_concurrency = self.reached_concurrency or reached
        return 1.0, 200, "HIT", Counter()


class CacheComparisonTest(unittest.TestCase):
    def setUp(self):
        cache_info = patch.object(comparison, "cache_info", return_value=None)
        cache_info.start()
        self.addCleanup(cache_info.stop)

    def test_client_concurrency_is_independent_of_http_workers(self):
        for concurrency in (1, 4):
            with self.subTest(concurrency=concurrency):
                service = CoordinatedService(concurrency)
                jobs = [("GET", "/products/1", None)] * concurrency
                result = comparison.run_workload(service, "test", jobs)
                self.assertTrue(service.reached_concurrency)
                self.assertEqual(service.started, concurrency)
                self.assertEqual(result["client_concurrency"], concurrency)
                self.assertEqual(result["http_status_counts"], {"200": concurrency})

    def test_arguments_preserve_client_concurrency_and_validate_http_workers(self):
        with patch.dict(os.environ, {"SPHINX_TEST_MYSQL_DATABASE": "comparison_test"}):
            for concurrency, workers in ((1, 1), (128, 64)):
                with patch.object(sys, "argv", [str(SCRIPT), "--concurrency", str(concurrency)]):
                    args = comparison.parse_args()
                    self.assertEqual(args.concurrency, concurrency)
                    self.assertEqual(args.http_workers, workers)
            with patch.object(sys, "argv", [str(SCRIPT), "--concurrency", "1", "--http-workers", "4"]):
                args = comparison.parse_args()
                self.assertEqual(args.http_workers, 4)

    def test_resource_delta_requires_the_same_process_lifetime(self):
        before = {"pid": 7, "start_time_ticks": 100, "cpu_seconds": 1.0, "rss_bytes": 1024}
        after = dict(before, cpu_seconds=1.25, rss_bytes=2048)
        result = comparison.process_resource_interval(before, after)
        self.assertEqual(result["cpu_seconds_delta"], 0.25)
        self.assertEqual(result["before"]["rss_bytes"], 1024)
        self.assertEqual(result["after"]["rss_bytes"], 2048)
        for changed in (
            dict(after, pid=8),
            dict(after, start_time_ticks=200),
            dict(after, cpu_seconds=0.5),
            None,
        ):
            with self.subTest(after=changed):
                self.assertIsNone(
                    comparison.process_resource_interval(before, changed)["cpu_seconds_delta"]
                )

    def test_each_workload_records_its_own_process_interval(self):
        service = CoordinatedService(1)
        snapshots = [
            {"pid": 1, "start_time_ticks": 10, "cpu_seconds": 1.0, "rss_bytes": 100},
            None,
            {"pid": 1, "start_time_ticks": 10, "cpu_seconds": 1.5, "rss_bytes": 200},
            None,
        ]
        with patch.object(comparison, "process_snapshot", side_effect=snapshots):
            with patch.object(comparison, "cache_info", side_effect=[{"get_hits": 1}, {"get_hits": 2}]):
                result = comparison.run_workload(service, "test", [("GET", "/products/1")])
        self.assertEqual(result["process_resources"]["service"]["cpu_seconds_delta"], 0.5)
        self.assertIsNone(result["process_resources"]["cache"]["cpu_seconds_delta"])
        self.assertEqual(result["cache_info_before"], {"get_hits": 1})
        self.assertEqual(result["cache_info_after"], {"get_hits": 2})

    def test_batch_warmup_rejects_per_item_errors_even_with_http_200(self):
        service = SimpleNamespace(request=lambda *_args: (1.0, 200, "MISS", {"read_busy": 1}))
        with self.assertRaises(RuntimeError):
            comparison.warm_cache(service, [1])


if __name__ == "__main__":
    unittest.main()
