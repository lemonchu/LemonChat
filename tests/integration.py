import hashlib
import json
import os
from pathlib import Path
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from urllib.parse import urlparse
from support import Process, Peer, no_partials

BINARY = Path(sys.argv.pop(1)).resolve()


class Chat(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="lemonchat-integration-")
        self.root = Path(self.temp.name)
        self.clients = []
        self.peers = []
        self.server = Process(BINARY, "serve", "--port", "0", "--bind", "127.0.0.1", "--data-dir", self.root / "server")
        self.invite = self.server.wait("ready")["invitation"]

    def tearDown(self):
        errors = []
        for peer in self.peers:
            peer.close()
        for client in self.clients:
            try:
                client.close()
            except AssertionError as error:
                errors.append(str(error))
        self.server.close(server=True)
        self.temp.cleanup()
        self.assertFalse(errors, errors)

    def client(self, name, invite=None, welcome=True, *extra):
        client = Process(BINARY, "join", "--headless", "--name", name, "--invite", invite or self.invite,
                         "--data-dir", self.root / name, *extra)
        self.clients.append(client)
        if welcome:
            client.id = client.wait("welcome")["self"]
            client.wait("state")
        return client

    def peer(self, name="raw", **kwargs):
        peer = Peer(self.invite, name, **kwargs)
        self.peers.append(peer)
        return peer

    def offer(self, sender, recipients, data, filename="payload.bin", conversation="lobby"):
        path = self.root / filename
        path.write_bytes(data)
        sender.send("file_offer", conversation=conversation, path=str(path))
        offer = sender.wait("file_queued")["offer"]
        sender.wait("file_offered", offer=offer)
        for recipient in recipients:
            recipient.wait("file_offer", offer=offer)
        return offer

    def raw_offer(self, sender, recipient, size, filename="raw.bin"):
        offer = os.urandom(16).hex()
        sender.send("file_offer", conversation="lobby", offer=offer, filename=filename, size=size)
        recipient.wait("file_offer", offer=offer)
        recipient.send("file_accept", offer=offer)
        tid = sender.wait("file_begin", offer=offer)["transfer"]
        recipient.wait("file_begin", transfer=tid)
        return tid

    def assert_clean(self, name):
        self.assertTrue(no_partials(self.root / name), list((self.root / name).rglob("*")))

    def test_three_client_chat_unicode_long_and_sender_identity(self):
        a, b, c = [self.client(name) for name in ("alice", "bob", "中文")]
        for content in ("你好，世界 🍋\nsecond line", "x" * 16384):
            a.send("message", conversation="lobby", text=content, sender=c.id, name="imposter")
            for client in (a, b, c):
                message = client.wait("message", text=content)
                self.assertEqual(message["sender"], a.id)
                self.assertEqual(message["name"], "alice")
        a.send("message", conversation="lobby", text="x" * 16385)
        a.wait("error")

    def test_rooms_membership_isolation_and_private_conversations(self):
        a, b, c = [self.client(name) for name in ("alice", "bob", "eve")]
        a.send("room_create", name="design")
        room = a.wait("conversation_opened")["conversation"]
        b.send("room_join", name="design")
        self.assertEqual(b.wait("conversation_opened")["conversation"], room)
        a.send("message", conversation=room, text="members only")
        b.wait("message", text="members only")
        c.absent("message", text="members only")
        c.send("message", conversation=room, text="intrusion")
        c.wait("error")
        b.send("room_leave", conversation=room)
        b.send("message", conversation=room, text="after leaving")
        b.wait("error")
        a.send("direct", peer=b.id)
        direct = a.wait("conversation_opened")["conversation"]
        a.send("message", conversation=direct, text="private")
        b.wait("message", text="private")
        c.absent("message", text="private")
        b.send("direct", peer=a.id)
        self.assertEqual(b.wait("conversation_opened")["conversation"], direct)
        c.send("room_join", name="Direct")
        c.wait("error")

    def test_raw_fragmented_protocol_and_deduplication(self):
        client = self.client("alice")
        peer = self.peer()
        command = {"type": "message", "conversation": "lobby", "text": "fragmented", "request": "unique", "sender": client.id}
        peer.raw(b"\x01" + json.dumps(command).encode(), fragment=True)
        event = client.wait("message", text="fragmented")
        self.assertEqual(event["sender"], peer.id)
        peer.raw(b"\x01" + json.dumps(command).encode())
        client.absent("message", text="fragmented")
        self.assertEqual(peer.socket.version(), "TLSv1.3")

    def test_wrong_pin_secret_and_duplicate_nickname(self):
        link = urlparse(self.invite)
        pin = link.query.split("pin=")[1].split("&")[0]
        bad = self.client("badpin", self.invite.replace(pin, "0" * 64), False)
        bad.wait("disconnected")
        bad.absent("welcome")
        bad = self.client("badsecret", self.invite[:-64] + "0" * 64, False)
        bad.wait("error")
        bad.wait("disconnected")
        self.client("alice")
        duplicate = self.client("alice", welcome=False)
        duplicate.wait("error")
        duplicate.wait("disconnected")

    def test_tls12_and_plaintext_rejected(self):
        with self.assertRaises((ssl.SSLError, ConnectionError)):
            Peer(self.invite, tls_version=ssl.TLSVersion.TLSv1_2)
        link = urlparse(self.invite)
        with socket.create_connection((link.hostname, link.port), timeout=3) as sock:
            sock.sendall(b'GET / HTTP/1.1\r\n\r\n')
            try:
                self.assertEqual(sock.recv(1024), b"")
            except ConnectionResetError:
                pass
        self.client("healthy")

    def test_malformed_frames_do_not_crash_server(self):
        for payload in (b"\x01{bad json", b"\x01[]", b"\x07ab", b"\x02short", b"\x01" + b'{"a":' * 20 + b"0" + b"}" * 20):
            with self.subTest(payload=payload[:20]):
                peer = self.peer("bad", authenticate=False)
                peer.raw(payload)
                with self.assertRaises((EOFError, ConnectionError, ssl.SSLError)):
                    peer.receive()
                peer.close()
        peer = self.peer("oversized", authenticate=False)
        peer.socket.sendall(struct.pack("!I", 128 * 1024 + 1))
        with self.assertRaises((EOFError, ConnectionError, ssl.SSLError)):
            peer.receive()
        self.client("healthy")

    def test_files_round_trip_boundaries_and_group_recipients(self):
        a, b, c = [self.client(name) for name in ("alice", "bob", "carol")]
        for size in (0, 1, 65535, 65536, 65537, 524289, 4 * 1024 * 1024):
            with self.subTest(size=size):
                data = os.urandom(size)
                filename = f"中文 文件 {size}.bin"
                offer = self.offer(a, (b, c), data, filename)
                b.send("file_accept", offer=offer)
                c.send("file_accept", offer=offer)
                for receiver, name in ((b, "bob"), (c, "carol")):
                    saved = receiver.wait("file_saved", offer=offer)
                    self.assertEqual(Path(saved["path"]).read_bytes(), data)
                    self.assertEqual(saved["sha256"], hashlib.sha256(data).hexdigest())
                    receiver.wait("file_complete", offer=offer)
                    a.wait("file_complete", offer=offer)
                    self.assert_clean(name)
                a.wait("file_offer_closed", offer=offer)

    def test_no_accept_no_file_and_decline(self):
        a, b = self.client("alice"), self.client("bob")
        offer = self.offer(a, (b,), b"secret")
        b.absent("file_begin")
        self.assertFalse((self.root / "bob" / "downloads").exists())
        b.send("file_decline", offer=offer)
        a.wait("file_declined", offer=offer)
        a.wait("file_offer_closed", offer=offer)

    def test_existing_destination_and_late_destination_preserved(self):
        a, b = self.client("alice"), self.client("bob")
        path = self.root / "existing.bin"
        path.write_bytes(b"original")
        offer = self.offer(a, (b,), b"new")
        b.send("file_accept", offer=offer, path=str(path))
        b.wait("error")
        self.assertEqual(path.read_bytes(), b"original")
        self.assert_clean("bob")
        peer = self.peer()
        offer = os.urandom(16).hex()
        peer.send("file_offer", offer=offer, conversation="lobby", filename="late.bin", size=3)
        b.wait("file_offer", offer=offer)
        late = self.root / "late.bin"
        b.send("file_accept", offer=offer, path=str(late))
        tid = peer.wait("file_begin", offer=offer)["transfer"]
        late.write_bytes(b"late original")
        peer.chunk(tid, 0, b"new")
        peer.wait("file_ack", transfer=tid, offset=3)
        peer.send("file_end", transfer=tid, sha256=hashlib.sha256(b"new").hexdigest())
        b.wait("file_cancelled", transfer=tid)
        self.assertEqual(late.read_bytes(), b"late original")

    def test_checksum_mismatch_and_disconnect_cleanup(self):
        b = self.client("bob")
        peer = self.peer()
        tid = self.raw_offer(peer, b, 4)
        peer.chunk(tid, 0, b"data")
        peer.wait("file_ack", transfer=tid, offset=4)
        peer.send("file_end", transfer=tid, sha256="0" * 64)
        b.wait("file_cancelled", transfer=tid)
        self.assertFalse((self.root / "bob" / "downloads" / "raw.bin").exists())
        self.assert_clean("bob")
        tid = self.raw_offer(peer, b, 999, "interrupted.bin")
        peer.chunk(tid, 0, b"partial")
        peer.wait("file_ack", transfer=tid, offset=7)
        peer.close()
        b.wait("file_cancelled", transfer=tid)
        self.assert_clean("bob")

    def test_invalid_chunk_offset_and_truncated_payload(self):
        b = self.client("bob")
        peer = self.peer()
        tid = self.raw_offer(peer, b, 10)
        peer.chunk(tid, 1, b"bad")
        b.wait("file_cancelled", transfer=tid)
        self.assert_clean("bob")
        tid = self.raw_offer(peer, b, 10)
        peer.chunk(tid, 0, b"part")
        peer.wait("file_ack", transfer=tid, offset=4)
        peer.send("file_end", transfer=tid, sha256=hashlib.sha256(b"part").hexdigest())
        peer.wait("error", transfer=tid)
        peer.send("file_cancel", transfer=tid)
        b.wait("file_cancelled", transfer=tid)
        self.assert_clean("bob")

    def test_source_changed_before_acceptance(self):
        a, b = self.client("alice"), self.client("bob")
        for content in (b"short", b"much longer changed file"):
            offer = self.offer(a, (b,), b"original", "changing.bin")
            (self.root / "changing.bin").write_bytes(content)
            b.send("file_accept", offer=offer)
            a.wait("file_cancelled")
            b.wait("file_cancelled")
            a.wait("file_offer_closed", offer=offer)
            self.assert_clean("bob")

    def test_sender_requires_final_recipient_acknowledgement(self):
        a = self.client("alice")
        peer = self.peer()
        data = b"confirm delivery"
        path = self.root / "source.bin"
        path.write_bytes(data)
        a.send("file_offer", conversation="lobby", path=str(path))
        offer = peer.wait("file_offer")["offer"]
        peer.send("file_accept", offer=offer)
        tid = peer.wait("file_begin")["transfer"]
        part = peer.wait("chunk", transfer=tid)
        self.assertEqual(part["data"], data)
        peer.send("file_ack", transfer=tid, offset=len(data))
        end = peer.wait("file_end", transfer=tid)
        self.assertEqual(end["sha256"], hashlib.sha256(data).hexdigest())
        a.absent("file_complete", offer=offer)
        peer.send("file_done", transfer=tid, sha256=end["sha256"])
        a.wait("file_complete", transfer=tid)

    def test_cancel_and_leaving_room_revoke_file_access(self):
        a, b = self.client("alice"), self.client("bob")
        offer = self.offer(a, (b,), b"offer")
        a.send("offer_cancel", offer=offer)
        b.wait("file_offer_closed", offer=offer)
        b.send("file_accept", offer=offer)
        b.wait("error")
        peer = self.peer()
        tid = self.raw_offer(peer, b, 10000)
        peer.chunk(tid, 0, b"some bytes")
        peer.wait("file_ack", transfer=tid)
        b.send("room_leave", conversation="lobby")
        b.wait("file_cancelled", transfer=tid)
        peer.wait("file_cancelled", transfer=tid)
        self.assert_clean("bob")
        b.send("room_join", name="lobby")
        b.wait("conversation_opened")
        offer = self.offer(a, (b,), b"pending", "pending.bin")
        b.send("room_leave", conversation="lobby")
        b.wait("file_offer_closed", offer=offer)

    def test_reconnect_and_server_shutdown(self):
        a, b = self.client("alice"), self.client("bob")
        a.send("reconnect")
        a.wait("welcome")
        a.send("message", conversation="lobby", text="reconnected")
        b.wait("message", text="reconnected")
        self.server.close(server=True)
        a.wait("disconnected")
        b.wait("disconnected")

    def test_size_limit_and_unsafe_names(self):
        b = self.client("bob", None, True, "--max-file", "3")
        peer = self.peer()
        for filename, size in (("../escape", 1), ("unsafe:name", 1), ("large", 1073741825)):
            peer.send("file_offer", conversation="lobby", offer=os.urandom(16).hex(), filename=filename, size=size)
            peer.wait("error")
        offer = os.urandom(16).hex()
        peer.send("file_offer", conversation="lobby", offer=offer, filename="big.bin", size=4)
        b.wait("file_offer", offer=offer)
        b.send("file_accept", offer=offer)
        b.wait("error")
        self.assert_clean("bob")

    def test_many_recipients_share_bounded_sender_window(self):
        sender = self.client("sender")
        receivers = [self.client(f"recipient{i}") for i in range(8)]
        data = os.urandom(2 * 1024 * 1024 + 3)
        offer = self.offer(sender, receivers, data)
        for recipient in receivers:
            recipient.send("file_accept", offer=offer)
        for recipient in receivers:
            saved = recipient.wait("file_saved", offer=offer)
            self.assertEqual(Path(saved["path"]).read_bytes(), data)
            sender.wait("file_complete", offer=offer)

    def test_source_changes_during_stream_abort_before_checksum(self):
        sender = self.client("sender")
        receiver = self.peer("receiver")
        path = self.root / "changing.bin"
        for changed in (b"short", b"longer" * 100000):
            path.write_bytes(b"x" * (512 * 1024 + 5))
            sender.send("file_offer", conversation="lobby", path=str(path))
            offer = receiver.wait("file_offer")["offer"]
            receiver.send("file_accept", offer=offer)
            tid = receiver.wait("file_begin")["transfer"]
            first = receiver.wait("chunk", transfer=tid)
            path.write_bytes(changed)
            receiver.send("file_ack", transfer=tid, offset=len(first["data"]))
            while True:
                event = receiver.receive()
                if event["type"] == "chunk":
                    receiver.send("file_ack", transfer=tid, offset=event["offset"] + len(event["data"]))
                elif event["type"] == "ping":
                    receiver.send("pong")
                elif event["type"] == "file_cancelled" and event["transfer"] == tid:
                    break
                elif event["type"] == "file_end":
                    self.fail("A changed source must not send its final checksum")
            sender.wait("file_offer_closed", offer=offer)

    def test_explicit_receiver_cancel_and_symlink_protection(self):
        receiver = self.client("receiver")
        sender = self.peer("sender")
        tid = self.raw_offer(sender, receiver, 100000)
        sender.chunk(tid, 0, b"partial")
        sender.wait("file_ack", transfer=tid)
        receiver.send("file_cancel", transfer=tid)
        sender.wait("file_cancelled", transfer=tid)
        receiver.wait("file_cancelled", transfer=tid)
        self.assert_clean("receiver")
        target = self.root / "original"
        target.write_bytes(b"original")
        link = self.root / "symlink"
        try:
            link.symlink_to(target)
        except (OSError, NotImplementedError):
            return  # Windows may require developer mode for symbolic links.
        offer = os.urandom(16).hex()
        sender.send("file_offer", offer=offer, conversation="lobby", filename="link.bin", size=1)
        receiver.wait("file_offer", offer=offer)
        receiver.send("file_accept", offer=offer, path=str(link))
        receiver.wait("error")
        self.assertEqual(target.read_bytes(), b"original")

    def test_flow_control_window_enforced(self):
        sender, receiver = self.peer("sender"), self.peer("receiver")
        offer = os.urandom(16).hex()
        sender.send("file_offer", offer=offer, conversation="lobby", filename="flood.bin", size=1024 * 1024)
        receiver.wait("file_offer", offer=offer)
        receiver.send("file_accept", offer=offer)
        tid = sender.wait("file_begin")["transfer"]
        for offset in range(0, 9 * 65536, 65536):
            sender.chunk(tid, offset, b"x" * 65536)
        sender.wait("file_cancelled", transfer=tid)
        receiver.wait("file_cancelled", transfer=tid)

    def test_large_stream_and_chat_while_transferring(self):
        sender, receiver = self.client("sender"), self.client("receiver")
        data = bytes(range(256)) * (64 * 1024 * 1024 // 256)
        offer = self.offer(sender, (receiver,), data, "large.bin")
        start = time.monotonic()
        receiver.send("file_accept", offer=offer)
        sender.send("message", conversation="lobby", text="chat stays available")
        receiver.wait("message", text="chat stays available")
        saved = receiver.wait("file_saved", timeout=30, offer=offer)
        sender.wait("file_complete", offer=offer)
        self.assertEqual(hashlib.sha256(Path(saved["path"]).read_bytes()).digest(), hashlib.sha256(data).digest())
        print(f"\n64 MiB transfer completed in {time.monotonic() - start:.2f}s", flush=True)

    def test_private_offer_cannot_be_accepted_by_another_peer(self):
        alice, bob = self.client("alice"), self.client("bob")
        eve = self.peer("eve")
        alice.send("direct", peer=bob.id)
        conversation = alice.wait("conversation_opened")["conversation"]
        offer = self.offer(alice, (bob,), b"private file", conversation=conversation)
        eve.send("file_accept", offer=offer)
        eve.wait("error", offer=offer)
        bob.send("file_accept", offer=offer)
        bob.wait("file_saved", offer=offer)
        alice.wait("file_complete", offer=offer)

    def test_ipv6_loopback(self):
        try:
            with socket.socket(socket.AF_INET6, socket.SOCK_STREAM) as probe:
                probe.bind(("::1", 0))
        except OSError:
            self.skipTest("IPv6 loopback unavailable on this runner")
        server = Process(BINARY, "serve", "--port", "0", "--bind", "::1", "--advertise", "::1", "--data-dir", self.root / "ipv6")
        try:
            invitation = server.wait("ready")["invitation"]
            self.assertIn("[::1]", invitation)
            client = self.client("ipv6client", invitation)
            client.send("message", conversation="lobby", text="IPv6 works")
            client.wait("message", text="IPv6 works")
        finally:
            server.close(server=True)

    def test_host_also_participates_as_client(self):
        host = Process(BINARY, "host", "--headless", "--name", "host", "--port", "0", "--bind", "127.0.0.1", "--data-dir", self.root / "host")
        self.clients.append(host)
        invitation = host.wait("ready")["invitation"]
        host.wait("welcome")
        guest = self.client("guest", invitation)
        guest.send("message", conversation="lobby", text="hello host")
        host.wait("message", text="hello host")

    def test_cli_errors_and_help(self):
        for args in (("--help",), ("--version",)):
            result = subprocess.run([str(BINARY), *args], capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(b"LemonChat", result.stdout)
        for args in (("join",), ("host", "--port", "65536"), ("--headless",), ("host", "--port", "-1"),
                     ("join", "--invite", "bad"), ("--unknown",), ("host", "--port")):
            result = subprocess.run([str(BINARY), *args], capture_output=True, timeout=5)
            self.assertNotEqual(result.returncode, 0, args)


if __name__ == "__main__":
    unittest.main(verbosity=2)
