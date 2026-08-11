#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="$root/build/macos-release"
dist_root="$root/dist"
dist="$dist_root/TinyMeshChat-macOS"
archive="$dist_root/TinyMeshChat-macOS.zip"
updater_enabled="${TMC_ENABLE_UPDATER:-0}"
build_velopack="${TMC_BUILD_VELOPACK:-0}"

if [[ "$build_velopack" == "1" && "$updater_enabled" != "1" ]]; then
  echo "TMC_BUILD_VELOPACK=1 requires TMC_ENABLE_UPDATER=1" >&2
  exit 1
fi

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

configure_args=(--preset macos-release -DTMC_ENABLE_UPDATER=OFF)
velopack_root="${TMC_VELOPACK_ROOT:-}"
if [[ "$updater_enabled" == "1" ]]; then
  if [[ -z "$velopack_root" ]]; then
    velopack_root="$root/build/tools/velopack"
    bash "$root/scripts/bootstrap-velopack.sh" "$velopack_root"
  fi
  configure_args[2]=-DTMC_ENABLE_UPDATER=ON
  configure_args+=("-DTMC_VELOPACK_ROOT=$velopack_root")
fi
cmake "${configure_args[@]}"
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

if [[ "$updater_enabled" == "1" ]]; then
  velopack_name="velopack_libc_osx.dylib"
  velopack_source="$velopack_root/lib/$velopack_name"
  velopack_destination="$dist/TinyMeshChat.app/Contents/Frameworks/$velopack_name"
  [[ -f "$velopack_source" ]] || { echo "Velopack runtime is missing: $velopack_source" >&2; exit 1; }
  mkdir -p "$(dirname "$velopack_destination")"
  ditto "$velopack_source" "$velopack_destination"
  original_id="$(otool -D "$velopack_source" | tail -n 1 | xargs)"
  install_name_tool -id "@rpath/$velopack_name" "$velopack_destination"
  install_name_tool -change "$original_id" "@rpath/$velopack_name" \
    "$dist/TinyMeshChat.app/Contents/MacOS/TinyMeshChat"
fi

codesign --force --deep --sign - "$dist/TinyMeshChat.app"
codesign --verify --deep --strict "$dist/TinyMeshChat.app"

run_with_timeout() {
  local timeout_seconds="$1"
  shift
  "$@" &
  local process_id=$!
  (
    sleep "$timeout_seconds"
    if kill -0 "$process_id" 2>/dev/null; then
      kill -TERM "$process_id" 2>/dev/null || true
      sleep 2
      kill -KILL "$process_id" 2>/dev/null || true
    fi
  ) &
  local watchdog_id=$!
  local status=0
  wait "$process_id" || status=$?
  kill "$watchdog_id" 2>/dev/null || true
  wait "$watchdog_id" 2>/dev/null || true
  if [[ "$status" -ne 0 ]]; then
    echo "Smoke command failed or timed out with status $status: $*" >&2
    return "$status"
  fi
}

smoke_data="$(mktemp -d)"
cleanup_smoke_data() {
  rm -rf "$smoke_data"
}
trap cleanup_smoke_data EXIT

run_with_timeout 30 env TMC_DATA_DIR="$smoke_data" \
  "$dist/TinyMeshChat.app/Contents/MacOS/TinyMeshChat" --console --display-name "CI Smoke" \
  <<< "/quit"
run_with_timeout 30 env TMC_DATA_DIR="$smoke_data" QT_QPA_PLATFORM=offscreen \
  "$dist/TinyMeshChat.app/Contents/MacOS/TinyMeshChat" --qml-smoke --display-name "CI Smoke"
cleanup_smoke_data
trap - EXIT

while IFS= read -r candidate; do
  if file "$candidate" | grep -q 'Mach-O'; then
    architectures="$(lipo -archs "$candidate")"
    if [[ " $architectures " != *" arm64 "* ]]; then
      echo "Packaged Mach-O file is not arm64: $candidate ($architectures)" >&2
      exit 1
    fi
    if otool -L "$candidate" | grep -Fq "$build/vcpkg_installed"; then
      echo "Packaged file still references the vcpkg build tree: $candidate" >&2
      exit 1
    fi
    if [[ "$updater_enabled" == "1" ]] &&
       otool -l "$candidate" | grep -Fq "$velopack_root"; then
      echo "Packaged file still contains the Velopack SDK build path: $candidate" >&2
      exit 1
    fi
  fi
done < <(find "$dist/TinyMeshChat.app" -type f)

rm -f "$archive"
ditto -c -k --sequesterRsrc --keepParent "$dist/TinyMeshChat.app" "$archive"
unzip -t "$archive"
echo "Created $archive"

if [[ "$build_velopack" == "1" ]]; then
  command -v dotnet >/dev/null || { echo "dotnet is required for Velopack packaging." >&2; exit 1; }
  dotnet tool restore
  version_file="$build/tmc-app-version.txt"
  [[ -f "$version_file" ]] || { echo "CMake did not produce the application version file: $version_file" >&2; exit 1; }
  version="$(tr -d '\r\n' < "$version_file")"
  velopack_output="$dist_root/velopack-osx-arm64"
  mkdir -p "$velopack_output"
  dotnet tool run vpk pack \
    --packId TinyMeshChat.Mac \
    --packTitle "TinyMesh Chat" \
    --packVersion "$version" \
    --packDir "$dist/TinyMeshChat.app" \
    --mainExe TinyMeshChat \
    --runtime osx-arm64 \
    --channel osx-arm64 \
    --noPortable true \
    --outputDir "$velopack_output"
  find "$velopack_output" -maxdepth 1 -type f -name 'releases.*.json' -print -quit | grep -q . || {
    echo "Velopack release feed was not produced" >&2
    exit 1
  }
fi
