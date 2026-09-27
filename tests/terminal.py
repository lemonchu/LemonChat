"""Exercise the actual full-screen interface through a pseudo-terminal."""
import fcntl
import json
import os
from pathlib import Path
import pty
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time
import unittest
import pyte
from support import Process

BINARY = Path(sys.argv.pop(1)).resolve()


class Terminal:
    def __init__(self, *arguments, width=120, height=36):
        self.master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", height, width, 0, 0))
        self.screen = pyte.Screen(width, height)
        self.stream = pyte.ByteStream(self.screen)
        self.process = subprocess.Popen([str(BINARY), *map(str, arguments)], stdin=slave, stdout=slave,
                                        stderr=slave, start_new_session=True,
                                        env={**os.environ, "TERM": "xterm-256color", "LANG": "C.UTF-8"})
        os.close(slave)
        self.raw = bytearray()

    def read(self, duration=0.08):
        deadline = time.monotonic() + duration
        while time.monotonic() < deadline:
            if not select.select([self.master], [], [], max(0, deadline - time.monotonic()))[0]:
                break
            try:
                data = os.read(self.master, 65536)
            except OSError:
                break
            if not data:
                break
            self.raw.extend(data)
            self.stream.feed(data)
            if b"\x1b[6n" in data:
                os.write(self.master, b"\x1b[1;1R")
        return "\n".join(self.screen.display)

    def send(self, text):
        os.write(self.master, text.encode() if isinstance(text, str) else text)
        self.read()

    def expect(self, text, timeout=6):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            screen = self.read()
            if text in screen:
                return screen
        raise AssertionError(f"TUI did not display {text!r}:\n{screen}")

    def command(self, text):
        self.send("\x0c")  # Ctrl+L focuses the composer.
        self.send(text)
        self.send("\r")

    def resize(self, width, height):
        fcntl.ioctl(self.master, termios.TIOCSWINSZ, struct.pack("HHHH", height, width, 0, 0))
        self.screen.resize(height, width)
        os.kill(self.process.pid, signal.SIGWINCH)
        self.read(0.2)

    def close(self):
        if self.process.poll() is None:
            self.send("\x11")
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
                raise AssertionError("TUI did not exit with Ctrl+Q")
        self.read()
        os.close(self.master)
        if self.process.returncode != 0:
            raise AssertionError(f"TUI exit {self.process.returncode}: {self.raw[-1000:]}")


class UI(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="lemonchat-terminal-")
        self.root = Path(self.temp.name)
        self.processes = []
        self.terminals = []
        self.server = Process(BINARY, "serve", "--port", "0", "--bind", "127.0.0.1", "--data-dir", self.root / "server")
        self.invite = self.server.wait("ready")["invitation"]

    def tearDown(self):
        for terminal in self.terminals:
            terminal.close()
        for process in self.processes:
            process.close()
        self.server.close(server=True)
        self.temp.cleanup()

    def client(self, name):
        process = Process(BINARY, "join", "--headless", "--name", name, "--invite", self.invite, "--data-dir", self.root / name)
        self.processes.append(process)
        process.id = process.wait("welcome")["self"]
        return process

    def tui(self, *args):
        terminal = Terminal(*args)
        self.terminals.append(terminal)
        return terminal

    def test_composition_rooms_private_search_and_resize(self):
        alice = self.client("alice")
        tui = self.tui("join", "--name", "lemon", "--invite", self.invite, "--data-dir", self.root / "lemon")
        tui.expect("connected")
        tui.expect("CONVERSATIONS")
        tui.command("你好 from the TUI")
        alice.wait("message", text="你好 from the TUI")
        tui.expect("sent")
        tui.send("\x0cfirst line\x0esecond line\r")
        alice.wait("message", text="first line\nsecond line")
        tui.command("/room design")
        tui.expect("#design")
        tui.command("/dm alice")
        tui.expect("@alice")
        tui.command("a private message")
        message = alice.wait("message", text="a private message")
        self.assertNotEqual(message["conversation"], "lobby")
        tui.command("/search private")
        tui.expect("a private message")
        tui.send("\x1b[5~")  # PageUp
        tui.send("\x1b[6~")  # PageDown
        tui.command("/rooms")
        tui.expect("Join selected")
        tui.command("/help")
        tui.expect("QUICK REFERENCE")
        tui.resize(80, 24)
        tui.expect("LEMONCHAT")
        tui.send("\x0c")
        tui.expect("Message or /help")

    def test_files_offer_accept_and_export_from_tui(self):
        alice = self.client("alice")
        tui = self.tui("join", "--name", "lemon", "--invite", self.invite, "--data-dir", self.root / "lemon")
        tui.expect("connected")
        source = self.root / "来自终端 文件.bin"
        source.write_bytes(bytes(range(256)) * 300)
        tui.send("\x0c\t\t\r")  # Composer -> Send -> Send file.
        tui.expect("Offer a file")
        tui.send(str(source))
        tui.send("\r")
        offer = alice.wait("file_offer")["offer"]
        alice.send("file_accept", offer=offer)
        saved = alice.wait("file_saved", offer=offer)
        self.assertEqual(Path(saved["path"]).read_bytes(), source.read_bytes())
        tui.expect("complete")
        source2 = self.root / "incoming.bin"
        source2.write_bytes(b"accepted from actual TUI")
        alice.send("file_offer", conversation="lobby", path=str(source2))
        offer = alice.wait("file_queued")["offer"]
        tui.expect("incoming.bin")
        tui.send("\x1bOR")  # F3 focuses the transfer list.
        tui.send("\x1b[H\t\r")  # Select newest offer, then the Accept button.
        alice.wait("file_complete", offer=offer)
        self.assertEqual((self.root / "lemon" / "downloads" / "incoming.bin").read_bytes(), source2.read_bytes())
        tui.command("local transcript entry")
        alice.wait("message", text="local transcript entry")
        export = self.root / "history.json"
        tui.command(f'/save "{export}"')
        tui.expect("Saved plaintext transcript")
        self.assertIn("local transcript entry", export.read_text())

    def test_launch_form_join_and_host(self):
        tui = self.tui("--name", "formuser", "--invite", self.invite, "--data-dir", self.root / "formuser")
        tui.expect("Create a server")
        # Nickname -> mode -> select Join -> invitation -> Start.
        tui.send("\t\x1b[B\t\t\r")
        tui.expect("connected")
        tui.command("from launch form")
        tui.expect("sent")
        host = self.tui("host", "--name", "hostuser", "--port", "0", "--data-dir", self.root / "hostuser")
        host.expect("connected")
        host.command("/help")
        host.expect("YOUR SERVER INVITATION")

    def test_launch_form_creates_server_and_cancel_dialog(self):
        import socket
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        tui = self.tui("--name", "newhost", "--port", str(port), "--bind", "127.0.0.1", "--data-dir", self.root / "newhost")
        tui.expect("Create a server")
        tui.send("\t\t\t\t\t\r")
        tui.expect("connected")
        tui.send("\x0c\t\t\r")
        tui.expect("Offer a file")
        tui.send("\x1b")
        tui.expect("Message or /help")


if __name__ == "__main__":
    unittest.main(verbosity=2)
