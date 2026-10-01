#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""直接调用 ClusterClient 的三节点分片与故障恢复测试。"""

from __future__ import annotations

import socket
import subprocess
import sys
import time


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def start_server(executable: str, port: int) -> subprocess.Popen[bytes]:
    process = subprocess.Popen(
        [executable, "-l", "127.0.0.1", "-p", str(port), "-t", "1", "-m", "8", "-s", "1"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if process.poll() is not None:
            error = process.stderr.read().decode(errors="replace")
            raise AssertionError(f"server {port} exited during startup: {error}")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.05):
                return process
        except OSError:
            time.sleep(0.01)
    process.terminate()
    process.wait(timeout=2)
    raise AssertionError(f"server {port} did not become ready")


def stop_server(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is None:
        process.terminate()
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=2)
    if process.returncode not in (0, -15):
        error = process.stderr.read().decode(errors="replace")
        raise AssertionError(f"server failed with {process.returncode}: {error}")


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: cluster_network_test.py /path/to/sphinxd /path/to/cluster-client-integration-driver"
        )
    server_executable, driver_executable = sys.argv[1:]
    ports = [free_port() for _ in range(3)]
    nodes = ",".join(f"127.0.0.1:{port}" for port in ports)
    processes: list[subprocess.Popen[bytes]] = []

    def run_driver() -> subprocess.CompletedProcess[bytes]:
        return subprocess.run(
            [driver_executable, nodes], capture_output=True, timeout=10, check=False
        )

    def require_success() -> None:
        result = run_driver()
        if result.returncode != 0 or result.stdout != b"PASS\n" or result.stderr:
            raise AssertionError(f"cluster driver failed: {result.stdout!r} {result.stderr!r}")

    try:
        for port in ports:
            processes.append(start_server(server_executable, port))
        require_success()
        stop_server(processes[0])
        result = run_driver()
        owner = f"127.0.0.1:{ports[0]}".encode()
        if result.returncode == 0 or owner not in result.stderr:
            raise AssertionError(f"failed node was not reported: {result.stdout!r} {result.stderr!r}")
        processes[0] = start_server(server_executable, ports[0])
        require_success()
    finally:
        for process in processes:
            stop_server(process)


if __name__ == "__main__":
    try:
        main()
    except Exception as error:  # pylint: disable=broad-except
        print(f"cluster network test failed: {error}", file=sys.stderr)
        raise
