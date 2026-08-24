#!/usr/bin/env python3
"""Deterministic black-box checks for the single-node TCP service."""

import concurrent.futures
import os
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

    def get_multi_response(self):
        """Read one multi-key response, preserving VALUE block order."""
        values = []
        while True:
            line = self.read_line()
            if line == b"END\r\n":
                return values
            parts = line[:-2].split(b" ")
            if len(parts) != 4 or parts[0] != b"VALUE":
                raise AssertionError(f"unexpected multi-get header: {line!r}")
            key = parts[1].decode("ascii")
            flags = int(parts[2])
            size = int(parts[3])
            value = self.read_exact(size)
            if self.read_exact(2) != b"\r\n":
                raise AssertionError("multi-get value did not end with CRLF")
            values.append((key, flags, value))

    def stats_response(self):
        """Read stats in wire order and return both order and parsed values."""
        lines = []
        values = {}
        while True:
            line = self.read_line()
            if line == b"END\r\n":
                return lines, values
            parts = line[:-2].split(b" ", 2)
            if len(parts) != 3 or parts[0] != b"STAT":
                raise AssertionError(f"unexpected stats line: {line!r}")
            name = parts[1].decode("ascii")
            value = parts[2].decode("ascii")
            lines.append(name)
            values[name] = value


def free_port():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def start_server(executable, threads, extra_env=None):
    port = free_port()
    environment = os.environ.copy()
    if extra_env:
        environment.update(extra_env)
    process = subprocess.Popen(
        [executable, "-l", "127.0.0.1", "-p", str(port), "-t", str(threads), "-m", "8", "-s", "1"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=environment,
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


STATS_ORDER = [
    "version",
    "threads",
    "limit_maxbytes",
    "cmd_get",
    "get_hits",
    "get_misses",
    "cmd_set",
    "cmd_add",
    "cmd_replace",
    "cmd_delete",
    "cmd_incr",
    "cmd_decr",
]


def assert_stats(client, threads, expected):
    order, values = client.stats_response()
    if order != STATS_ORDER:
        raise AssertionError(f"stats fields were not in fixed order: {order!r}")
    if values.get("threads") != str(threads):
        raise AssertionError(f"stats reported wrong thread count: {values!r}")
    if values.get("limit_maxbytes") != str(8 * 1024 * 1024):
        raise AssertionError(f"stats reported wrong memory limit: {values!r}")
    for name, value in expected.items():
        if values.get(name) != str(value):
            raise AssertionError(f"stats {name} expected {value}, got {values.get(name)}")
    return values


def run_protocol_extensions(address, threads):
    """Exercise W2 commands and prove their process-wide counters."""
    client = Client(address)
    try:
        client.send(b"stats\r\n")
        order, initial = client.stats_response()
        if order != STATS_ORDER:
            raise AssertionError("initial stats response had the wrong field order")
        if not initial.get("version"):
            raise AssertionError("stats did not expose a version")
        if initial.get("threads") != str(threads):
            raise AssertionError("stats did not expose the configured thread count")
        if initial.get("limit_maxbytes") != str(8 * 1024 * 1024):
            raise AssertionError("stats did not expose the configured byte limit")
        for name in STATS_ORDER[3:]:
            if initial.get(name) != "0":
                raise AssertionError(f"counter {name} was not zero at process start")

        client.send(b"set ext-hit 7 0 2\r\n10\r\n")
        if client.storage_response() != "STORED\r\n":
            raise AssertionError("set for arithmetic test failed")
        client.send(b"add ext-add 2 0 2\r\n20\r\n")
        if client.storage_response() != "STORED\r\n":
            raise AssertionError("add for multi-get test failed")
        client.send(b"replace ext-hit 8 0 2\r\n10\r\n")
        if client.storage_response() != "STORED\r\n":
            raise AssertionError("replace for stats test failed")
        client.send(b"set ext-text 0 0 3\r\nabc\r\n")
        if client.storage_response() != "STORED\r\n":
            raise AssertionError("set for non-numeric test failed")

        client.send(b"get ext-hit missing ext-hit ext-add\r\n")
        expected = [
            ("ext-hit", 8, b"10"),
            ("ext-hit", 8, b"10"),
            ("ext-add", 2, b"20"),
        ]
        if client.get_multi_response() != expected:
            raise AssertionError("multi-get did not preserve hit order or duplicate keys")

        client.send(b"delete ext-add\r\n")
        if client.storage_response() != "DELETED\r\n":
            raise AssertionError("delete did not remove an existing key")
        client.send(b"add ext-add 2 0 2\r\n20\r\n")
        if client.storage_response() != "STORED\r\n":
            raise AssertionError("add could not recreate a deleted key")
        client.send(b"get ext-hit ext-add\r\n")
        if client.get_multi_response() != [
            ("ext-hit", 8, b"10"),
            ("ext-add", 2, b"20"),
        ]:
            raise AssertionError("multi-get after delete/add was incorrect")

        client.send(b"delete absent-delete\r\n")
        if client.storage_response() != "NOT_FOUND\r\n":
            raise AssertionError("delete reported success for a missing key")
        client.send(b"incr ext-hit 2\r\n")
        if client.storage_response() != "12\r\n":
            raise AssertionError("incr returned the wrong value")
        client.send(b"decr ext-hit 99\r\n")
        if client.storage_response() != "0\r\n":
            raise AssertionError("decr did not clamp at zero")
        client.send(b"incr ext-text 1\r\n")
        if client.storage_response() != (
            "CLIENT_ERROR cannot increment or decrement non-numeric value\r\n"
        ):
            raise AssertionError("incr accepted a non-numeric value")
        client.send(b"incr absent-counter 1\r\n")
        if client.storage_response() != "NOT_FOUND\r\n":
            raise AssertionError("incr created a missing key")
        client.send(b"decr absent-counter 1\r\n")
        if client.storage_response() != "NOT_FOUND\r\n":
            raise AssertionError("decr created a missing key")
        client.send(b"incr ext-hit 18446744073709551616\r\n")
        if client.storage_response() != "CLIENT_ERROR invalid numeric argument\r\n":
            raise AssertionError("overflowing delta did not return CLIENT_ERROR")
        client.send(b"version\r\n")
        if client.storage_response() != "VERSION 1.5.16\r\n":
            raise AssertionError("overflowing delta broke the following request")

        client.send(b"stats\r\n")
        expected_counts = {
            "cmd_get": 2,
            "get_hits": 5,
            "get_misses": 1,
            "cmd_set": 2,
            "cmd_add": 2,
            "cmd_replace": 1,
            "cmd_delete": 2,
            "cmd_incr": 3,
            "cmd_decr": 2,
        }
        assert_stats(client, threads, expected_counts)
        return expected_counts
    finally:
        client.close()


def run_concurrent_increments(address, threads, baseline):
    """Concurrent clients must serialize one counter on its owning worker."""
    client = Client(address)
    try:
        client.send(b"set concurrent-counter 0 0 1\r\n0\r\n")
        if client.storage_response() != "STORED\r\n":
            raise AssertionError("could not initialize concurrent counter")
    finally:
        client.close()

    clients = 8
    increments_per_client = 32

    def increment_worker(_worker):
        worker_client = Client(address)
        try:
            for _ in range(increments_per_client):
                worker_client.send(b"incr concurrent-counter 1\r\n")
                if worker_client.storage_response() is None:
                    raise AssertionError("counter client received no response")
        finally:
            worker_client.close()

    with concurrent.futures.ThreadPoolExecutor(max_workers=clients) as pool:
        list(pool.map(increment_worker, range(clients)))

    client = Client(address)
    try:
        client.send(b"get concurrent-counter\r\n")
        expected_value = str(clients * increments_per_client).encode()
        if client.get_response() != ("concurrent-counter", 0, expected_value):
            raise AssertionError("concurrent increments lost an update")
        client.send(b"stats\r\n")
        expected = dict(baseline)
        expected["cmd_set"] += 1
        expected["cmd_incr"] += clients * increments_per_client
        expected["cmd_get"] += 1
        expected["get_hits"] += 1
        assert_stats(client, threads, expected)
    finally:
        client.close()


def run_multi_get_edges(address):
    """Exercise mixed misses, a large value, duplicate keys, and early close."""
    client = Client(address)
    try:
        large = b"x" * (256 * 1024)
        client.send(b"set mget-large 4 0 " + str(len(large)).encode() + b"\r\n" + large + b"\r\n")
        if client.storage_response() != "STORED\r\n":
            raise AssertionError("large value setup failed")
        client.send(b"set mget-small 5 0 5\r\nsmall\r\n")
        if client.storage_response() != "STORED\r\n":
            raise AssertionError("small value setup failed")
        expired = str(int(time.time()) - 1).encode()
        client.send(b"set mget-expired 6 " + expired + b" 7\r\nexpired\r\n")
        if client.storage_response() != "STORED\r\n":
            raise AssertionError("expired multi-get setup failed")
        client.send(b"get mget-small mget-missing mget-expired mget-large mget-small\r\n")
        if client.get_multi_response() != [
            ("mget-small", 5, b"small"),
            ("mget-large", 4, large),
            ("mget-small", 5, b"small"),
        ]:
            raise AssertionError("multi-get failed mixed/large/duplicate ordering")
        client.send(b"get mget-missing-a mget-missing-b\r\n")
        if client.get_multi_response() != []:
            raise AssertionError("multi-get returned a VALUE block for all-miss input")
    finally:
        client.close()

    client = socket.create_connection(address, timeout=2)
    try:
        client.sendall(b"get mget-large mget-missing mget-small\r\n")
    finally:
        client.close()


def run_bare_lf(address):
    """A bare LF is retained and must not produce an ERROR response."""
    sock = socket.create_connection(address, timeout=2)
    try:
        sock.sendall(b"get bare-lf\n")
        sock.settimeout(0.2)
        try:
            data = sock.recv(4096)
        except socket.timeout:
            data = b""
        if data:
            raise AssertionError(f"bare LF was treated as a command: {data!r}")
    finally:
        sock.close()

    probe = Client(address)
    try:
        probe.send(b"version\r\n")
        if probe.storage_response() != "VERSION 1.5.16\r\n":
            raise AssertionError("server did not survive a bare-LF connection")
    finally:
        probe.close()


def run_mixed_pipeline(address):
    """One pipeline must keep multi-get boundaries around other commands."""
    client = Client(address)
    try:
        client.send(
            b"set mixed-key 1 0 1\r\nx\r\n"
            b"get mixed-key mixed-missing\r\n"
            b"delete mixed-key\r\n"
            b"add mixed-key 2 0 1\r\ny\r\n"
            b"set mixed-counter 0 0 1\r\n1\r\n"
            b"incr mixed-counter 2\r\n"
            b"get mixed-key mixed-counter\r\n"
        )
        if client.storage_response() != "STORED\r\n":
            raise AssertionError("pipelined set failed")
        if client.get_multi_response() != [("mixed-key", 1, b"x")]:
            raise AssertionError("pipelined multi-get response boundary was wrong")
        if client.storage_response() != "DELETED\r\n":
            raise AssertionError("pipelined delete response was out of order")
        if client.storage_response() != "STORED\r\n":
            raise AssertionError("pipelined add response was out of order")
        if client.storage_response() != "STORED\r\n":
            raise AssertionError("pipelined counter setup failed")
        if client.storage_response() != "3\r\n":
            raise AssertionError("pipelined incr response was out of order")
        if client.get_multi_response() != [
            ("mixed-key", 2, b"y"),
            ("mixed-counter", 0, b"3"),
        ]:
            raise AssertionError("pipelined multi-get did not preserve request order")
    finally:
        client.close()


def run_concurrent_multi_get(address):
    """Several connections may aggregate multi-key reads independently."""
    setup = Client(address)
    keys = [f"concurrent-mget-{index}" for index in range(12)]
    try:
        commands = []
        for index, key in enumerate(keys):
            value = f"value-{index}".encode()
            commands.append(
                b"set "
                + key.encode()
                + b" 0 0 "
                + str(len(value)).encode()
                + b"\r\n"
                + value
                + b"\r\n"
            )
        setup.send(b"".join(commands))
        for _ in keys:
            if setup.storage_response() != "STORED\r\n":
                raise AssertionError("concurrent multi-get setup failed")
    finally:
        setup.close()

    request = b"get " + b" ".join(key.encode() for key in keys) + b"\r\n"
    expected = [
        (key, 0, f"value-{index}".encode()) for index, key in enumerate(keys)
    ]

    def read_worker(_worker):
        client = Client(address)
        try:
            client.send(request)
            if client.get_multi_response() != expected:
                raise AssertionError("concurrent multi-get returned the wrong aggregate")
        finally:
            client.close()

    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        list(pool.map(read_worker, range(8)))


def run_mget_queue_failure(executable):
    """The injected one-shot queue failure must yield one complete error."""
    process, address = start_server(
        executable,
        4,
        {"SPHINXD_TEST_FAIL_MGET_QUEUE_ONCE": "1"},
    )
    try:
        client = Client(address)
        try:
            keys = [f"queue-failure-{index}".encode() for index in range(64)]
            client.send(b"get " + b" ".join(keys) + b"\r\n")
            response = client.storage_response()
            if response != "SERVER_ERROR request queue is full\r\n":
                raise AssertionError(
                    f"multi-get queue failure did not return one SERVER_ERROR: {response!r}"
                )
            client.sock.settimeout(0.2)
            try:
                extra = client.sock.recv(4096)
            except socket.timeout:
                extra = b""
            if extra:
                raise AssertionError(f"queue failure returned partial data: {extra!r}")
            client.sock.settimeout(2)
            client.send(b"version\r\n")
            if client.storage_response() != "VERSION 1.5.16\r\n":
                raise AssertionError("queue failure broke the following request")
        finally:
            client.close()
    finally:
        stop_server(process)


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
    client.sendall(b"get half missing-half\r\n")
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
        baseline = run_protocol_extensions(address, threads)
        run_concurrent_increments(address, threads, baseline)
        run_multi_get_edges(address)
        run_mixed_pipeline(address)
        run_concurrent_multi_get(address)
        run_bare_lf(address)
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
    run_mget_queue_failure(sys.argv[1])


if __name__ == "__main__":
    try:
        main()
    except Exception as error:  # pylint: disable=broad-except
        print(f"network test failed: {error}", file=sys.stderr)
        raise
