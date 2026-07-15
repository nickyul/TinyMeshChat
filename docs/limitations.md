# Limitations

- No TURN/relay: direct connection is not guaranteed under strict/symmetric NAT, some CG-NAT, blocked UDP, or corporate policy.
- UUID identity is persistent but unauthenticated; compare identity out of band. Key pairs/signatures are future work.
- Dynamic room membership and full-mesh signaling are connected end-to-end. Any member may invite another member. With no signaling server, restoring a room with no surviving channel requires one fresh manual offer/answer; the remaining links are automatic.
- Full mesh is intentionally bounded (8 members by default, configurable up to 16) because connection count grows quadratically.
- The last room and local history are restored after restart. WebRTC connections are ephemeral, so participants must exchange fresh invitations; after reconnection, up to 500 recent message IDs are reconciled and pending local messages are resent.
- Text only; no files, voice, video, mobile/web client, server, VPN, or account recovery.
