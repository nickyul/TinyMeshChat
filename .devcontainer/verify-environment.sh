#!/usr/bin/env bash
set -euo pipefail

check_command() {
  local command_name="$1"
  command -v "$command_name" >/dev/null
  printf '%-8s %s\n' "$command_name" "$(command -v "$command_name")"
}

echo "TinyMeshChat toolchain"
check_command cmake
check_command ninja
check_command g++
check_command gdb
check_command qtpaths6
