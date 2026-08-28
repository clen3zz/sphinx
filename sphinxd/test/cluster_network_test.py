#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""静态客户端集群分片的三节点冒烟测试。"""

from __future__ import annotations

import socket
import subprocess
import sys
import time
from pathlib import Path


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


def run_cli(executable: str, nodes: str, *arguments: str) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(
        [executable, "--nodes", nodes, *arguments],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=5,
        check=False,
    )


def run_single_node_driver(executable: str, node: str) -> None:
    result = subprocess.run(
        [executable, node],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=10,
        check=False,
    )
    if result.returncode != 0 or result.stdout != b"PASS\n" or result.stderr:
        raise AssertionError(f"same-client integration driver failed: {result.stdout!r} {result.stderr!r}")


def route(cli: str, nodes: str, key: str) -> str:
    result = run_cli(cli, nodes, "route", key)
    if result.returncode != 0:
        raise AssertionError(f"route failed: {result.stderr.decode(errors='replace')}")
    if result.stderr or result.stdout.count(b"\n") != 1 or not result.stdout.endswith(b"\n"):
        raise AssertionError(f"route output was not one line: {result.stdout!r} {result.stderr!r}")
    owner = result.stdout[:-1].decode("ascii")
    if owner.count(":") != 1 or not owner.rsplit(":", 1)[1].isdigit():
        raise AssertionError(f"route output was not host:port: {result.stdout!r}")
    return owner


def read_line(sock: socket.socket, pending: bytearray) -> bytes:
    while b"\r\n" not in pending:
        chunk = sock.recv(4096)
        if not chunk:
            raise AssertionError("node closed before response line")
        pending.extend(chunk)
    end = pending.index(b"\r\n") + 2
    result = bytes(pending[:end])
    del pending[:end]
    return result


def read_exact(sock: socket.socket, size: int, pending: bytearray) -> bytes:
    while len(pending) < size:
        chunk = sock.recv(max(4096, size - len(pending)))
        if not chunk:
            raise AssertionError("node closed before response body")
        pending.extend(chunk)
    result = bytes(pending[:size])
    del pending[:size]
    return result


def direct_get(address: tuple[str, int], key: str) -> bytes | None:
    with socket.create_connection(address, timeout=2) as sock:
        sock.settimeout(2)
        sock.sendall(f"get {key}\r\n".encode())
        pending = bytearray()
        header = read_line(sock, pending)
        if header == b"END\r\n":
            return None
        fields = header[:-2].split(b" ")
        if len(fields) != 4 or fields[0] != b"VALUE":
            raise AssertionError(f"invalid direct get response: {header!r}")
        value = read_exact(sock, int(fields[3]), pending)
        if read_exact(sock, 2, pending) != b"\r\n" or read_line(sock, pending) != b"END\r\n":
            raise AssertionError("invalid direct get terminator")
        return value


def main() -> None:
    if len(sys.argv) not in (2, 3, 4):
        raise SystemExit(
            "usage: cluster_network_test.py /path/to/sphinxd [/path/to/sphinx-cluster] "
            "[/path/to/cluster-client-integration-driver]"
        )
    server_executable = sys.argv[1]
    cli_executable = sys.argv[2] if len(sys.argv) >= 3 else str(Path(server_executable).with_name("sphinx-cluster"))
    driver_executable = sys.argv[3] if len(sys.argv) == 4 else None

    ports = [free_port() for _ in range(3)]
    addresses = [("127.0.0.1", port) for port in ports]
    nodes = ",".join(f"127.0.0.1:{port}" for port in ports)
    reversed_nodes = ",".join(f"127.0.0.1:{port}" for port in reversed(ports))
    processes: list[subprocess.Popen[bytes]] = []
    try:
        help_result = subprocess.run(
            [cli_executable, "--help"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5, check=False
        )
        if help_result.returncode != 0 or b"Usage:" not in help_result.stdout:
            raise AssertionError(f"help failed: {help_result.stdout!r} {help_result.stderr!r}")
        invalid_result = run_cli(cli_executable, "not-a-node", "route", "key")
        if invalid_result.returncode == 0 or not invalid_result.stderr:
            raise AssertionError("invalid node configuration was accepted")
        unknown_result = run_cli(cli_executable, nodes, "unknown", "key")
        if unknown_result.returncode == 0 or not unknown_result.stderr:
            raise AssertionError("unknown command was accepted")

        for port in ports:
            processes.append(start_server(server_executable, port))
        if driver_executable is not None:
            run_single_node_driver(driver_executable, f"127.0.0.1:{ports[0]}")

        keys_by_node: dict[str, str] = {}
        for index in range(10000):
            key = f"cluster-key-{index}"
            owner = route(cli_executable, nodes, key)
            if owner not in keys_by_node:
                keys_by_node[owner] = key
            if len(keys_by_node) == len(ports):
                break
        if len(keys_by_node) != len(ports):
            raise AssertionError(f"not every node received a key: {keys_by_node}")

        for owner, key in keys_by_node.items():
            value = f"value-for-{key}"
            result = run_cli(cli_executable, nodes, "set", key, value)
            if result.returncode != 0 or result.stdout != b"STORED\n":
                raise AssertionError(f"cluster set failed: {result.stdout!r} {result.stderr!r}")
            result = run_cli(cli_executable, nodes, "get", key)
            if result.returncode != 0 or result.stdout != (value + "\n").encode():
                raise AssertionError(f"cluster get failed: {result.stdout!r} {result.stderr!r}")

            for address in addresses:
                expected = value.encode() if f"{address[0]}:{address[1]}" == owner else None
                if direct_get(address, key) != expected:
                    raise AssertionError(f"key {key} was not isolated to {owner}")

            result = run_cli(cli_executable, nodes, "delete", key)
            if result.returncode != 0 or result.stdout != b"DELETED\n" or result.stderr:
                raise AssertionError(f"cluster delete failed: {result.stdout!r} {result.stderr!r}")
            if direct_get(addresses[ports.index(int(owner.rsplit(':', 1)[1]))], key) is not None:
                raise AssertionError("deleted key remained on owner")
            result = run_cli(cli_executable, nodes, "get", key)
            if result.returncode != 0 or result.stdout != b"NOT_FOUND\n" or result.stderr:
                raise AssertionError(f"cluster get miss was not exact: {result.stdout!r} {result.stderr!r}")
            result = run_cli(cli_executable, nodes, "delete", key)
            if result.returncode != 0 or result.stdout != b"NOT_FOUND\n" or result.stderr:
                raise AssertionError(f"cluster delete miss was not exact: {result.stdout!r} {result.stderr!r}")

        for key in keys_by_node.values():
            if route(cli_executable, nodes, key) != route(cli_executable, reversed_nodes, key):
                raise AssertionError("node input order changed routing")

        failure_key = next(iter(keys_by_node.values()))
        owner = route(cli_executable, nodes, failure_key)
        owner_index = ports.index(int(owner.rsplit(":", 1)[1]))
        stop_server(processes[owner_index])
        result = run_cli(cli_executable, nodes, "get", failure_key)
        if result.returncode == 0 or owner.encode() not in result.stderr:
            raise AssertionError(f"failed node was not reported: {result.stdout!r} {result.stderr!r}")
        processes[owner_index] = start_server(server_executable, ports[owner_index])
        recovered_value = "reconnected-value"
        result = run_cli(cli_executable, nodes, "set", failure_key, recovered_value)
        if result.returncode != 0 or result.stdout != b"STORED\n":
            raise AssertionError(f"client did not reconnect: {result.stdout!r} {result.stderr!r}")
        result = run_cli(cli_executable, nodes, "get", failure_key)
        if result.returncode != 0 or result.stdout != (recovered_value + "\n").encode():
            raise AssertionError(f"reconnected node did not serve data: {result.stdout!r} {result.stderr!r}")
    finally:
        for process in processes:
            if process.args:
                stop_server(process)


if __name__ == "__main__":
    try:
        main()
    except Exception as error:  # pylint: disable=broad-except
        print(f"cluster network test failed: {error}", file=sys.stderr)
        raise
