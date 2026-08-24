#!/usr/bin/env python3
"""Deterministic black-box checks for the single-node TCP service."""

import concurrent.futures
import socket
import subprocess
import sys
import time


class Client:
    def __init__(self, address):
        self.sock = socket.create_connection(address, timeout=2)
        self.sock.settimeout(2)
        self.buffer = bytearray()

    def close(self):
        self.sock.close()

    def send(self, data):
        self.sock.sendall(data)

    def read_exact(self, size):
        while len(self.buffer) < size:
            chunk = self.sock.recv(max(4096, size - len(self.buffer)))
            if not chunk:
                raise AssertionError("server closed before response completed")
            self.buffer.extend(chunk)
        result = bytes(self.buffer[:size])
        del self.buffer[:size]
        return result

    def read_line(self):
        while b"\r\n" not in self.buffer:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise AssertionError("server closed before response line")
            self.buffer.extend(chunk)
        end = self.buffer.index(b"\r\n") + 2
        result = bytes(self.buffer[:end])
        del self.buffer[:end]
        return result

    def storage_response(self):
        return self.read_line().decode("ascii")

    def get_response(self):
        line = self.read_line()
        if line == b"END\r\n":
            return None
        parts = line[:-2].split(b" ")
        if len(parts) != 4 or parts[0] != b"VALUE":
            raise AssertionError(f"unexpected get header: {line!r}")
        key = parts[1].decode("ascii")
        flags = int(parts[2])
        size = int(parts[3])
        value = self.read_exact(size)
        if self.read_exact(2) != b"\r\n":
            raise AssertionError("value did not end with CRLF")
        if self.read_line() != b"END\r\n":
            raise AssertionError("get response did not end with END")
        return key, flags, value


def free_port():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def start_server(executable, threads):
    port = free_port()
    process = subprocess.Popen(
        [executable, "-l", "127.0.0.1", "-p", str(port), "-t", str(threads), "-m", "8", "-s", "1"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if process.poll() is not None:
            error = process.stderr.read().decode(errors="replace")
            raise AssertionError(f"server exited during startup ({process.returncode}): {error}")
        try:
            probe = socket.create_connection(("127.0.0.1", port), timeout=0.05)
            probe.close()
            return process, ("127.0.0.1", port)
        except OSError:
            time.sleep(0.005)
    process.terminate()
    process.wait(timeout=2)
    raise AssertionError("server did not become ready")


def stop_server(process):
    process.terminate()
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=2)
    if process.returncode not in (-15, 0):
        error = process.stderr.read().decode(errors="replace")
        raise AssertionError(f"server failed ({process.returncode}): {error}")


def run_basic(address):
    client = Client(address)
    try:
        client.send(b"version\r\n")
        if client.storage_response() != "VERSION 1.5.16\r\n":
            raise AssertionError("wrong version response")

        client.send(b"set foo 42 0 3\r\nbar\r\n")
        if client.storage_response() != "STORED\r\n":
            raise AssertionError("set failed")
        client.send(b"get foo\r\n")
        if client.get_response() != ("foo", 42, b"bar"):
            raise AssertionError("get did not preserve flags/value")

        client.send(b"add foo 0 0 3\r\nbaz\r\n")
        if client.storage_response() != "NOT_STORED\r\n":
            raise AssertionError("add overwrote an existing key")
        client.send(b"replace foo 9 0 3\r\nbaz\r\n")
        if client.storage_response() != "STORED\r\n":
            raise AssertionError("replace failed for an existing key")
        client.send(b"get foo\r\n")
        if client.get_response() != ("foo", 9, b"baz"):
            raise AssertionError("replace did not preserve new flags/value")

        # An exptime above 30 days is an absolute Unix timestamp.
        expired = str(int(time.time()) - 1).encode()
        client.send(b"set old 7 " + expired + b" 3\r\nold\r\n")
        if client.storage_response() != "STORED\r\n":
            raise AssertionError("absolute-expiration set failed")
        client.send(b"get old\r\n")
        if client.get_response() is not None:
            raise AssertionError("already-expired value was returned")

        # A relative one-second expiry is visible first, then disappears.
        client.send(b"set ttl 11 1 3\r\nttl\r\n")
        if client.storage_response() != "STORED\r\n":
            raise AssertionError("relative-expiration set failed")
        client.send(b"get ttl\r\n")
        if client.get_response() != ("ttl", 11, b"ttl"):
            raise AssertionError("relative-expiration value was not initially visible")
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            client.send(b"get ttl\r\n")
            if client.get_response() is None:
                break
            time.sleep(0.05)
        else:
            raise AssertionError("relative-expiration value did not expire")
    finally:
        client.close()


def run_pipeline(address, count=256):
    client = Client(address)
    try:
        commands = []
        for i in range(count):
            key = f"pipe-{i}".encode()
            value = f"value-{i}".encode()
            commands.append(b"set " + key + b" " + str(i % 1000).encode() + b" 0 " + str(len(value)).encode() + b"\r\n" + value + b"\r\n")
        client.send(b"".join(commands))
        for _ in range(count):
            if client.storage_response() != "STORED\r\n":
                raise AssertionError("pipelined set did not complete")

        gets = b"".join(b"get pipe-" + str(i).encode() + b"\r\n" for i in range(count))
        client.send(gets)
        for i in range(count):
            expected = (f"pipe-{i}", i % 1000, f"value-{i}".encode())
            if client.get_response() != expected:
                raise AssertionError(f"out-of-order or incorrect pipelined response at {i}")
    finally:
        client.close()


def run_early_close(address):
    client = socket.create_connection(address, timeout=2)
    client.sendall(b"set half 0 0 5\r\nxy")
    client.close()
    client = socket.create_connection(address, timeout=2)
    client.sendall(b"set complete-close 0 0 1\r\nx\r\n")
    client.close()
    probe = Client(address)
    try:
        probe.send(b"version\r\n")
        if probe.storage_response() != "VERSION 1.5.16\r\n":
            raise AssertionError("server did not survive an early client close")
    finally:
        probe.close()


def run_concurrent(address, worker):
    client = Client(address)
    try:
        count = 96
        commands = []
        for i in range(count):
            key = f"worker-{worker}-{i}".encode()
            value = f"v-{worker}-{i}".encode()
            commands.append(b"set " + key + b" 3 0 " + str(len(value)).encode() + b"\r\n" + value + b"\r\n")
        client.send(b"".join(commands))
        for _ in range(count):
            if client.storage_response() != "STORED\r\n":
                raise AssertionError("concurrent set failed")
        client.send(b"".join(b"get worker-" + str(worker).encode() + b"-" + str(i).encode() + b"\r\n" for i in range(count)))
        for i in range(count):
            if client.get_response() != (f"worker-{worker}-{i}", 3, f"v-{worker}-{i}".encode()):
                raise AssertionError("concurrent get failed")
    finally:
        client.close()


def run_suite(executable, threads):
    process, address = start_server(executable, threads)
    try:
        run_basic(address)
        run_pipeline(address)
        run_early_close(address)
        with concurrent.futures.ThreadPoolExecutor(max_workers=min(8, threads + 2)) as pool:
            list(pool.map(lambda worker: run_concurrent(address, worker), range(8)))
        if threads >= 8:
            # More requests than one inter-core queue can hold, with responses
            # consumed continuously.  This exercises backpressure without
            # allowing a full queue to abort or strand a request.
            run_pipeline(address, 12000)
    finally:
        stop_server(process)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: network_test.py /path/to/sphinxd")
    for threads in (1, 4, 8):
        run_suite(sys.argv[1], threads)


if __name__ == "__main__":
    try:
        main()
    except Exception as error:  # pylint: disable=broad-except
        print(f"network test failed: {error}", file=sys.stderr)
        raise
