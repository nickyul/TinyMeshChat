# Architecture

The dependency direction is UI/CLI → application orchestration → messaging/signaling → protocol/network. `PeerConnection` is the only class allowed to use `rtc::PeerConnection`. A room is a bounded dynamic full mesh: each member has up to `N-1` direct DataChannels and the room has `N×(N-1)/2` links. There is no permanent owner, hub, or message server.

Qt Widgets, controllers, and all ephemeral room state run on the main Qt thread. libdatachannel owns callback threads. Callbacks capture `QPointer` plus shared/weak transport state and enqueue Qt work with `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`. Destruction first detaches local state and closes DataChannel/PeerConnection through RAII. Locks are not held while user callbacks execute.

Identity is currently a persistent random peer/device UUID. An `IdentityManager` boundary permits replacing it with Ed25519 public-key hashes and signed signaling without redesigning packet routing. DTLS is transport encryption, not user authentication.

Membership is learned through bounded `peer.list` packets. The lexicographically smaller peer ID initiates each missing pair. Mesh signaling is flooded through available channels with a UUID route ID, hop limit, and deduplication; chat packets are never relayed.

Rooms, membership, message IDs, Lamport clocks, and delivery acknowledgements exist only in process memory. Identity and user configuration are the only persistent application data. No connection or message activity is written to disk.
