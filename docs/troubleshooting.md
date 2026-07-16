# Troubleshooting

If configuration fails, validate UTF-8 JSON, ensure every ICE URI begins with `stun:`, and confirm that the application data directory is writable for identity and settings.

For ICE failures, verify both clocks, recreate offers after a network change, allow UDP in Windows Firewall, try a different configured STUN server, then use a less restrictive network. `host` candidates are local, `srflx` candidates show STUN-discovered mappings, and `relay` is never used. A `srflx` candidate does not guarantee the peer's NAT filtering permits a direct path. Never publish full SDP: it contains local addressing and ephemeral ICE credentials.

Runtime diagnostics are written only to the process console and are not persisted by the application. Never publish full SDP: it contains local addressing and ephemeral ICE credentials.

On macOS, build with the Qt `macos` kit rather than an iOS kit. `QT_ROOT/bin/macdeployqt` must exist, and `VCPKG_ROOT` must point to a vcpkg checkout. The packaging script creates an ad-hoc signed app; distributing without the Finder warning requires a Developer ID signature and Apple notarization.
