# Troubleshooting

If configuration fails, validate UTF-8 JSON and ensure every ICE URI begins with `stun:`. If SQLite fails, confirm the portable package contains `sqldrivers/qsqlite.dll` and that the application data directory is writable.

For ICE failures, verify both clocks, recreate offers after a network change, allow UDP in Windows Firewall, try a different configured STUN server, then use a less restrictive network. `host` candidates are local, `srflx` candidates show STUN-discovered mappings, and `relay` is never used. A `srflx` candidate does not guarantee the peer's NAT filtering permits a direct path. Never publish full SDP: it contains local addressing and ephemeral ICE credentials.

Logs are in the Qt application data directory and rotate at 2 MiB. They intentionally omit message text, full SDP, and secrets.

On macOS, build with the Qt `macos` kit rather than an iOS kit. `QT_ROOT/bin/macdeployqt` must exist, and `VCPKG_ROOT` must point to a vcpkg checkout. The packaging script creates an ad-hoc signed app; distributing without the Finder warning requires a Developer ID signature and Apple notarization.
