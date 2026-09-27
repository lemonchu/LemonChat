"""Process and independent TLS peer helpers shared by integration/terminal tests."""
import hashlib
import json
import os
from pathlib import Path
import queue
import socket
import ssl
import struct
import subprocess
import threading
import time
from urllib.parse import urlparse, parse_qs


class Process:
    def __init__(self, binary, *arguments):
        self.process = subprocess.Popen([str(binary), *map(str, arguments)], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                        encoding="utf-8", bufsize=1)
        self.queue = queue.Queue()
        self.pending = []
        self.history = []
        self.errors = []
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.error_reader = threading.Thread(target=self._errors, daemon=True)
        self.reader.start()
        self.error_reader.start()

    def _read(self):
        for line in self.process.stdout:
            try:
                event = json.loads(line)
            except ValueError:
                event = {"type": "invalid_stdout", "line": line}
            self.queue.put(event)
        self.queue.put({"type": "process_exited"})

    def _errors(self):
        self.errors.extend(self.process.stderr.readlines())

    def send(self, kind, **fields):
        self.process.stdin.write(json.dumps({"type": kind, **fields}, ensure_ascii=False) + "\n")
        self.process.stdin.flush()

    def wait(self, kind, timeout=8, **fields):
        def matches(event):
            return event.get("type") == kind and all(event.get(k) == v for k, v in fields.items())
        for index, event in enumerate(self.pending):
            if matches(event):
                return self.pending.pop(index)
        deadline = time.monotonic() + timeout
        while True:
            try:
                event = self.queue.get(timeout=max(0, deadline - time.monotonic()))
            except queue.Empty:
                raise AssertionError(f"Timed out waiting for {kind} {fields}; pending={self.pending[-8:]}; stderr={self.errors}") from None
            self.history.append(event)
            if matches(event):
                return event
            self.pending.append(event)
            if event["type"] == "process_exited":
                raise AssertionError(f"Process exited before {kind}: {self.errors}; {self.pending[-8:]}")

    def absent(self, kind, timeout=0.15, **fields):
        try:
            self.wait(kind, timeout, **fields)
        except AssertionError as error:
            if str(error).startswith("Timed out"):
                return
            raise
        raise AssertionError(f"Unexpected {kind} {fields}")

    def close(self, server=False):
        if self.process.poll() is None:
            if server:
                self.process.terminate()
            else:
                try:
                    self.send("quit")
                except (BrokenPipeError, OSError):
                    pass
            try:
                self.process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=3)
                raise AssertionError("Process failed to shut down")
        self.reader.join(timeout=2)
        self.error_reader.join(timeout=2)
        for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
            stream.close()
        if not server and self.process.returncode != 0:
            raise AssertionError(f"Client exit {self.process.returncode}: {self.errors}")


class Peer:
    """Independent implementation: verifies the real C++ TLS and wire protocol."""
    def __init__(self, invitation, name="raw", authenticate=True, tls_version=ssl.TLSVersion.TLSv1_3):
        link = urlparse(invitation)
        query = parse_qs(link.query)
        self.secret = query["key"][0]
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE
        context.minimum_version = tls_version
        context.maximum_version = tls_version
        sock = socket.create_connection((link.hostname, link.port), timeout=5)
        try:
            self.socket = context.wrap_socket(sock, server_hostname=link.hostname)
        except Exception:
            sock.close()
            raise
        assert hashlib.sha256(self.socket.getpeercert(binary_form=True)).hexdigest() == query["pin"][0]
        self.pending = []
        if authenticate:
            self.send("hello", version=2, name=name, secret=self.secret)
            self.id = self.wait("welcome")["self"]

    def raw(self, payload, fragment=False):
        data = struct.pack("!I", len(payload)) + payload
        if fragment:
            for byte in data:
                self.socket.sendall(bytes([byte]))
        else:
            self.socket.sendall(data)

    def send(self, kind, **fields):
        self.raw(b"\x01" + json.dumps({"type": kind, **fields}, ensure_ascii=False).encode())

    def chunk(self, transfer, offset, data):
        self.raw(b"\x02" + transfer.encode() + struct.pack("!Q", offset) + data)

    def exact(self, size):
        data = b""
        while len(data) < size:
            part = self.socket.recv(size - len(data))
            if not part:
                raise EOFError("Peer closed")
            data += part
        return data

    def receive(self):
        length = struct.unpack("!I", self.exact(4))[0]
        if length > 128 * 1024:
            raise AssertionError("Unbounded server frame")
        data = self.exact(length)
        if data[0] == 1:
            return json.loads(data[1:])
        assert data[0] == 2
        return {"type": "chunk", "transfer": data[1:33].decode(), "offset": struct.unpack("!Q", data[33:41])[0], "data": data[41:]}

    def wait(self, kind, timeout=5, **fields):
        deadline = time.monotonic() + timeout
        while True:
            for index, event in enumerate(self.pending):
                if event["type"] == kind and all(event.get(k) == v for k, v in fields.items()):
                    return self.pending.pop(index)
            self.socket.settimeout(max(0.001, deadline - time.monotonic()))
            event = self.receive()
            if event["type"] == "ping":
                self.send("pong")
            else:
                self.pending.append(event)

    def close(self):
        self.socket.close()


def no_partials(directory):
    return not list(Path(directory).rglob(".lemonchat-*.part"))
