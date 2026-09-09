#!/usr/bin/env bash
set -euo pipefail

workspace_dir=$(mktemp -d)
trap 'rm -rf "$workspace_dir"' EXIT

if ./build/swarmsync manifest create --file "$workspace_dir/missing.bin" --out "$workspace_dir/out.swarmsync"; then
  printf '%s\n' "manifest creation unexpectedly accepted a missing file" >&2
  exit 1
fi

./build/swarmsync --help | grep -q "download"
./build/swarmsync-tracker --help | grep -q -- "--max-swarms"
