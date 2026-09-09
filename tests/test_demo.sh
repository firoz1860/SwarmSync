#!/usr/bin/env bash
set -euo pipefail

demo_dir=$(mktemp -d)
output_file=$(mktemp)
trap 'rm -rf "$demo_dir" "$output_file"' EXIT

DEMO_DIR="$demo_dir" TRACKER_PORT=0 SEED_PORT=0 DOWNLOAD_A_PORT=0 DOWNLOAD_B_PORT=0 \
  SWARMSYNC_TOKEN="test-demo-token" bash scripts/local_demo.sh >"$output_file"
grep -q "Demo verified" "$output_file"
test -f "$demo_dir/download-a/demo-source.bin"
test -f "$demo_dir/download-b/demo-source.bin"
