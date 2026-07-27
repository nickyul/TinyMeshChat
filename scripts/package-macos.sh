#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="$root/build/macos-release"
dist_root="$root/dist"
dist="$dist_root/TinyMeshChat-macOS"
archive="$dist_root/TinyMeshChat-macOS.zip"

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "This packaging script must run on macOS." >&2
  exit 1
fi
if [[ -z "${VCPKG_ROOT:-}" || ! -f "$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" ]]; then
  echo "Set VCPKG_ROOT to a working vcpkg checkout." >&2
  exit 1
fi
if [[ -z "${QT_ROOT:-}" || ! -x "$QT_ROOT/bin/macdeployqt" ]]; then
  echo "Set QT_ROOT to the Qt macOS kit, for example $HOME/Qt/6.9.3/macos." >&2
  exit 1
fi
command -v cmake >/dev/null || { echo "cmake is required." >&2; exit 1; }
command -v ninja >/dev/null || { echo "ninja is required." >&2; exit 1; }

cmake --preset macos-release
cmake --build --preset macos-release

app="$build/TinyMeshChat.app"
if [[ ! -x "$app/Contents/MacOS/TinyMeshChat" ]]; then
  echo "TinyMeshChat.app was not produced at $app" >&2
  exit 1
fi

mkdir -p "$dist_root"
rm -rf "$dist"
mkdir -p "$dist"
ditto "$app" "$dist/TinyMeshChat.app"

deploy_args=(-always-overwrite -verbose=1)
while IFS= read -r libdir; do
  deploy_args+=("-libpath=$libdir")
done < <(find "$build/vcpkg_installed" -type d -path '*/lib' ! -path '*/debug/*' 2>/dev/null)
"$QT_ROOT/bin/macdeployqt" "$dist/TinyMeshChat.app" "-qmldir=$root/qml" "${deploy_args[@]}"

codesign --force --deep --sign - "$dist/TinyMeshChat.app"
codesign --verify --deep --strict "$dist/TinyMeshChat.app"

smoke_data="$(mktemp -d)"
cleanup_smoke_data() {
  rm -rf "$smoke_data"
}
trap cleanup_smoke_data EXIT

TMC_DATA_DIR="$smoke_data" "$dist/TinyMeshChat.app/Contents/MacOS/TinyMeshChat" \
  --console --display-name "CI Smoke" \
  <<< "/quit"
cleanup_smoke_data
trap - EXIT

rm -f "$archive"
ditto -c -k --sequesterRsrc --keepParent "$dist/TinyMeshChat.app" "$archive"
echo "Created $archive"
