# Field validation checklist

Use at least three Windows computers A, B, and C with synchronized clocks. Keep `Инструменты → Диагностика сети` and the application log available on every computer. Do not publish signaling documents or logs publicly because SDP contains local addressing information.

## Basic mesh

1. A creates one room and invites B.
2. B, not A, invites C to verify that every member can create invitations.
3. Wait until every computer shows `Прямые связи: 2/2`.
4. Send a unique message from each computer and confirm it appears once on both other computers.
5. Confirm local messages reach `доставлено 2/2`.

## Restart and history

1. Close B, send messages between A and C, and start B again.
2. Confirm B restores the same local room and old history.
3. C creates a fresh invitation for B; B imports it and returns the new answer.
4. Confirm missing messages appear on B in logical order without duplicates.
5. Send a message from B before reconnecting and confirm it is delivered after the new channel opens.

## Network changes

Repeat the basic mesh with the computers distributed across home broadband and two unrelated mobile operators. Then repeat while one peer changes from Wi-Fi to a mobile hotspot. A room with no surviving path is restored through any fresh member-to-member invitation; it must not be recreated.

Record for every failure: local time, peer names/IDs, the visible connection state, operator/network type, and the `tiny-mesh.log` files. Expected direct-connect failures under strict/symmetric NAT or blocked UDP are a product limitation until TURN support is added.
