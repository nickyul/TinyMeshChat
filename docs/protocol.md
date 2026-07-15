# Protocol v1

Every packet is compact UTF-8 JSON with `protocol_version`, `packet_type`, UUID `packet_id`, UUID `room_id`, known `sender_id`, UTC `created_at`, and object `payload`. Input is capped at 64 KiB. Unknown versions/types/senders, wrong rooms, invalid timestamps/UUIDs, and malformed payloads are rejected.

- `peer.hello`: device/display identity and protocol capabilities; `peer.hello_ack`: acceptance.
- `peer.list`: up to 16 validated identities; the application limit defaults to 8.
- `chat.message`: UUID `message_id`, string `text` (≤4096 Unicode code points), positive `logical_clock`; `chat.ack`: `message_id`.
- `sync.summary`: total count and up to 500 recent IDs; `sync.request`: up to 200 missing IDs; `sync.messages`: a size-bounded message array.
- `mesh.offer`, `mesh.answer`: route UUID, bounded hop count, connection UUID, origin identity, target peer, and complete non-trickle SDP. Intermediate peers relay only these service packets.
- `ping`, `pong`: UUID nonce and UTC send time used for keepalive.

All packet payloads are validated by type before dispatch. Arrays are bounded, UUIDs and timestamps are checked, display names and chat text have length limits, and SDP must still fit the global packet limit.

Receiving clock `r` updates local Lamport clock as `max(local,r)+1`; sending increments it. Display order is `(logical_clock, sender_id, message_id)`. UUID uniqueness plus SQLite primary keys makes replay idempotent.
