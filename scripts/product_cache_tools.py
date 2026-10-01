# SPDX-License-Identifier: Apache-2.0
"""Shared disposable cache processes and protocol helpers for acceptance and comparison."""

import os
import shutil
import signal
import socket
import subprocess
import tempfile
import time
from pathlib import Path


class RedisCommandError(RuntimeError):
    """A complete Redis error reply, distinct from a broken connection."""


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
        raise RedisCommandError(payload.decode(errors="replace"))
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
        result = subprocess.run(
            command,
            env=self.environment,
            input=Path(input_file).read_text(encoding="utf-8") if input_file else "",
            capture_output=True,
            text=True,
            check=False,
        )
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
        escaped_user = self.user.replace("'", "''")
        query = (
            "SELECT COALESCE(SUM(statement.COUNT_EXECUTE),0) "
            "FROM performance_schema.prepared_statements_instances AS statement "
            "JOIN performance_schema.threads AS thread "
            "ON thread.THREAD_ID=statement.OWNER_THREAD_ID "
            f"WHERE thread.PROCESSLIST_USER='{escaped_user}' "
            f"AND thread.PROCESSLIST_DB='{escaped_database}' "
            "AND statement.SQL_TEXT LIKE 'SELECT%products%' "
            "AND statement.SQL_TEXT NOT LIKE '%FOR UPDATE%'"
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

    def __enter__(self):
        try:
            self.start()
        except Exception:
            self.close()
            raise
        return self

    def __exit__(self, *_exception):
        self.close()
