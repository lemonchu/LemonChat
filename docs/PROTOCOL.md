# Protocol and core design

Protocol version 2 is incompatible with LemonChat's original UDP protocol. The
same `lemonchat_core` C++ library serves the TUI and JSON-lines automation mode.

## Connection and authentication

Connections are asynchronous TCP streams using TLS 1.3 or newer. A client pins the
SHA-256 fingerprint of the server's leaf certificate, checks its validity dates,
and then authenticates with the invitation secret. There is no plaintext fallback
or option to disable verification. TLS early data and renegotiation are disabled.

The first application frame must be:

```json
{"type":"hello","version":2,"name":"alice","secret":"64 lowercase hex characters"}
```

The server returns `welcome` with `self` (an unpredictable session ID), `name`,
`version`, and `max_file`. A `state` event lists online `peers`, public `rooms`, and
only the recipient's joined `conversations`. Session IDs change on reconnect;
room membership and private conversations are not durable identities.

Every five seconds the server sends `ping`; clients reply `pong`. TLS handshakes
have a ten-second deadline, client DNS/connect has a fifteen-second deadline, and
an established connection is closed after 45 seconds without a complete frame.
Unauthenticated sessions expire after ten seconds (checked every five seconds).

## Framing

All multi-byte integers are unsigned and big-endian. A frame is a four-byte
payload length, then that many bytes, with a maximum payload of 128 KiB.

| Payload tag | Remaining payload |
| --- | --- |
| `0x01` | UTF-8 JSON object, maximum nesting depth 16 |
| `0x02` | 32 ASCII hex transfer-ID bytes, eight-byte offset, 1–65,536 raw file bytes |

File data is binary, without base64 overhead. Frame reads and writes handle
fragmentation through `asio::async_read` / `asio::async_write`. Each connection
owns a bounded output queue; broadcast and forwarding share immutable packet
buffers. All operations on a TLS stream occur on one event-loop thread.

## Conversations

A conversation has an ID, a member set, and a membership policy. `lobby` is the
initial public room. Every message and file offer targets a conversation ID.

| Command | Required fields | Result |
| --- | --- | --- |
| `room_create` | `name` | New room, creator joined |
| `room_join` | `name` | Join existing public room |
| `room_leave` | `conversation` | Leave public room and revoke related file access |
| `direct` | `peer` | Find/create a two-member private conversation |
| `message` | `conversation`, `text`, `request` | Authorized members receive `message` |
| `state` | none | Refresh current state |

Membership changes emit updated `state`; opening/joining a conversation also
returns `conversation_opened` to its requester. Private conversations never
appear in the public room directory and cannot be joined by name. An online peer
may initiate a private conversation without a separate contact-approval workflow.

Message `sender`, `name`, `id`, and `time` come from the server. Caller-supplied
identity fields are ignored. A session retains the last 256 message request IDs
for duplicate suppression. Failed commands produce `error` with a `message` and
relevant `request`, `offer`, or `transfer` identifiers. There are no read receipts.

## File lifecycle

1. Sender sends `file_offer` with `offer` (32 lowercase hex characters),
   `conversation`, portable `filename`, and unsigned `size`.
2. Current conversation members other than the sender receive `file_offer`.
   The sender receives `file_offered` with the recipient count. Recipients joining
   later do not inherit the offer.
3. Each offered recipient chooses `file_accept` or `file_decline`, with `offer`.
   Acceptance creates a separate transfer ID for that recipient. Both parties
   receive `file_begin`, including `transfer`, `offer`, `sender`, `receiver`,
   `receiver_name`, `conversation`, `filename`, and `size`.
4. Sender streams raw binary chunks at exact successive offsets. Receiver writes
   to its exclusive temporary file, hashes the bytes, and replies `file_ack` with
   `transfer` and the next expected `offset`. The server forwards acknowledgements.
   A transfer can have at most 512 KiB unacknowledged; the official client limits
   all its outgoing transfers together to 1 MiB and rotates the scheduling order.
5. After every byte is acknowledged, sender sends `file_end` with `transfer` and
   lowercase hex `sha256`. Receiver verifies size and hash, syncs and publishes
   the file without replacing an existing destination, then sends `file_done`
   with the matching hash.
6. Only then does the server send `file_complete` to both parties. Empty files use
   the same lifecycle with no data chunks. The server stores metadata, not files.

`file_cancel` cancels a transfer; `offer_cancel` cancels an outgoing offer and all
its active transfers. Disconnection, leaving the room, invalid flow control, or
120 seconds without transfer activity cancels the affected transfer. Offers with
undecided recipients expire after ten minutes. Limits are enforced before
allocating transfer state. The server verifies membership and participant roles
for every control operation; arbitrary clients cannot accept or cancel someone
else's transfer.

There is no transfer resumption or snapshot of a changing source file. Each
recipient gets an independent stream/hash of the source. The official client
checks source size before and after streaming, including the underlying file size
to avoid buffered-read differences across platforms. Files must remain unchanged
while being sent; same-size modifications may produce a mixed-version stream.

## Headless client commands/events

`join --headless` and `host --headless` expose the same application core with one
JSON object per stdin/stdout line. Do not send wire frames to this interface.
The core fills message request IDs and authenticated identity fields.

```json
{"type":"room_create","name":"design"}
{"type":"message","conversation":"lobby","text":"Hello, world"}
{"type":"file_offer","conversation":"lobby","path":"/path/to/source.bin"}
{"type":"file_accept","offer":"32-character ID","path":"/new/destination.bin"}
{"type":"reconnect"}
{"type":"quit"}
```

Omit the accept `path` to use the configured downloads directory. Do not send
another command until `welcome`; connecting and rejected commands are reported as
events. `host --headless` additionally emits `ready` with the invitation.

Client-local events include `connecting`, `disconnected`, `message_pending`,
`file_queued`, `file_accepting`, `file_progress` (`done`, `size`), and `file_saved`
(`path`, `sha256`). The TUI model combines these with server events to show pending
messages, errors, progress, and final acknowledgements. Local storage failures
are surfaced without claiming delivery. The host's identity and secret are the
trust boundary; this protocol does not protect against a malicious host.
