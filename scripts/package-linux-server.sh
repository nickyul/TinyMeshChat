#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="${1:-$root/build/linux-server-release}"
if [[ "$(uname -s)" != Linux || "$(uname -m)" != x86_64 ]]; then
  echo 'Package on Ubuntu 24.04 x86_64 (the deployment baseline).' >&2
  exit 1
fi
if [[ -z "${QT_ROOT:-}" || ! -d "$QT_ROOT/plugins/tls" ]]; then
  echo 'Set QT_ROOT to the Qt kit used to build the server.' >&2
  exit 1
fi
command -v patchelf >/dev/null || { echo 'patchelf is required.' >&2; exit 1; }

cmake "-DTMC_SOURCE_DIR=$root" "-DTMC_BUILD_DIR=$build" "-DTMC_QT_ROOT=$QT_ROOT" \
  -P "$root/cmake/package-linux-server.cmake"
archive="$root/dist/TinyMeshSignalingServer-Linux-x64.tar.gz"
tar -C "$root/dist" -czf "$archive" TinyMeshSignalingServer-Linux-x64
tar -tzf "$archive" >/dev/null
echo "Created $archive"
