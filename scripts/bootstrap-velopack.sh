#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 DESTINATION" >&2
  exit 2
fi

version="1.2.0"
expected_sha256="547262ed7a1ab1ff62f580aa53851ede2f1a451ac61b8974eb7bc01117488835"
destination="$1"
header="$destination/include/Velopack.hpp"
if [[ -f "$header" ]]; then
  echo "Velopack SDK is already available at $destination"
  exit 0
fi

archive="$(mktemp -t velopack_libc.XXXXXX.zip)"
cleanup() {
  rm -f "$archive"
}
trap cleanup EXIT

curl --fail --location --retry 3 \
  "https://github.com/velopack/velopack/releases/download/$version/velopack_libc_$version.zip" \
  --output "$archive"
actual_sha256="$(shasum -a 256 "$archive" | awk '{print $1}')"
if [[ "$actual_sha256" != "$expected_sha256" ]]; then
  echo "Velopack SDK checksum mismatch: $actual_sha256" >&2
  exit 1
fi

mkdir -p "$destination"
unzip -q "$archive" -d "$destination"
[[ -f "$header" ]] || { echo "Velopack.hpp was not found after extracting the SDK" >&2; exit 1; }
echo "Installed Velopack SDK $version to $destination"
