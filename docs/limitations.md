# Limitations

- No TURN/relay: direct connection is not guaranteed under strict/symmetric NAT, some CG-NAT, blocked UDP, or corporate policy.
- UUID identity is persistent but unauthenticated; compare identity out of band. Key pairs/signatures are future work.
- Dynamic room membership and full-mesh signaling are connected end-to-end. Any member may invite another member while its process still has the room in memory.
- Full mesh is intentionally bounded (8 members by default, configurable up to 16) because connection count grows quadratically.
- Rooms, membership, messages, and delivery state are ephemeral. Restarting the application requires creating a room or importing a fresh invitation. Late or reconnecting peers do not receive earlier messages, and pending messages are not resent.
- Text only; no files, voice, video, mobile/web client, server, VPN, or account recovery.
