# LemonChat 2 rewrite

## Product scope

A terminal chat application for Windows, macOS, and Linux. The default experience
is a full-screen TUI with a conversation list, message timeline, member list,
composer, search, and file-transfer view. Qt and the original UDP implementation
are replaced. A headless mode uses the same core for automation and integration
testing; it is not a substitute for the interactive interface.

One participant hosts a server; everyone, including the host's own UI, connects
as a client. Multiple named rooms and direct conversations are supported. Room
membership determines who receives room messages. Messages identify their actual
authenticated session sender, not a caller-supplied sender field. Server receipt
is distinguished from a recipient reading a message.

Direct and group chat use one conversation model: an ID, a member set, and a
membership policy. All message/file operations take a conversation ID. Direct
conversations have two fixed members; public rooms support join/leave. A hosted
server and a separately deployed server use the same implementation. Direct P2P
transport is not required for this version. Implementation stays in C++.

All connections require TLS 1.3, an explicitly pinned SHA-256 certificate
fingerprint, and an invitation secret. No plaintext or insecure verification mode
is provided. Certificates/keys are generated with OpenSSL. The host is trusted:
it can read the messages and files it forwards. This is transport encryption,
not end-to-end encryption against the host. Invitations must be shared through
a trusted channel. NAT traversal and public discovery are outside this scope.

Files can be offered to a person or a room. Each recipient explicitly accepts or
declines. Transfers are bounded-memory streams with per-chunk backpressure,
SHA-256 verification, progress, cancellation, and final recipient acknowledgement.
Incoming files use exclusive temporary files and must not overwrite existing
files. Unaccepted files are never written. The server forwards chunks without
storing files. Broken connections clean up incomplete transfers.

## Structure

- `crypto`: TLS identity, pinning, random identifiers, SHA-256.
- `storage`: exclusive temporary reception and checked publication.
- `transport`: bounded length-prefixed control JSON and raw binary file chunks
  over asynchronous TLS streams; server forwarding shares immutable buffers.
- `server`: membership, routing, authenticated sessions, transfer coordination.
- `client`: connection lifecycle, messages, sending/receiving files, UI events.
- `model`: conversations, unread state, bounded history, search, terminal-safe text.
- `tui`: launch form, room/member navigation, composer, transfer actions, help.
- `main`: argument parsing, interactive and automation entry points.

Networking runs on a joined background event-loop thread. Each TLS stream is
used only on its owning event loop. UI callbacks enqueue events and request a
redraw; they do not mutate FTXUI components from network threads. Connections have
handshake deadlines, bounded frames and output queues, and heartbeat expiry.

## Completion evidence required

- Build and execute with Qt removed, including Linux.
- Real three-client room chat, membership changes, room isolation, and private
  messages; Unicode and long messages; no sender spoofing.
- Encrypted connections, rejection of plaintext, wrong pins and wrong secrets.
- Real binary/empty/large/Unicode-named file transfer, room offers with multiple
  recipients, SHA-256 verification, explicit acceptance, cancellation, truncated
  input, disconnect cleanup, and existing-file protection.
- TUI launch/join, conversation switching, composition, scrolling/search, and
  transfer controls using pseudo-terminal tests plus rendered-screen inspection.
- Malformed frame/JSON handling, size/queue limits, orderly shutdown, and useful
  error messages.
- Cross-platform builds/tests, documentation of trust model, commands, and limits.

This checklist records the requested end state. Passing a smaller test subset
does not establish completion.
