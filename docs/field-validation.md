# Field validation checklist

Use at least three computers A, B, and C with synchronized clocks. Keep `Инструменты → Диагностика сети` available on every computer. Do not publish signaling documents because SDP contains local addressing information.

## Basic mesh

1. A creates one room and invites B.
2. B, not A, invites C to verify that every member can create invitations.
3. Wait until every computer shows `Прямые связи: 2/2`.
4. Send a unique message from each computer and confirm it appears once on both other computers.
5. Confirm local messages reach `доставлено 2/2`.

## Ephemeral state and reconnect

1. Close B, send messages between A and C, and start B again.
2. Confirm B starts without a room, peers, messages, or delivery state.
3. C creates a fresh invitation for B; B imports it and returns the new answer.
4. Confirm B does not receive messages sent while it was offline.
5. Send a new message after reconnection and confirm it appears once on every connected peer.

## Network changes

Repeat the basic mesh with the computers distributed across home broadband and two unrelated mobile operators. Then repeat while one peer changes from Wi-Fi to a mobile hotspot. If at least one process still has the room in memory, reconnect through its fresh invitation. If all processes restarted, create a new room.

Record for every failure: local time, peer names/IDs, the visible connection state, operator/network type, and console diagnostics captured during that run. Expected direct-connect failures under strict/symmetric NAT or blocked UDP are a product limitation until TURN support is added.
