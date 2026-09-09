#!/usr/bin/env bash
set -euo pipefail

project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build_dir="$project_root/build"
demo_dir=${DEMO_DIR:-}

if [[ -z "$demo_dir" ]]; then
  demo_dir=$(mktemp -d "${TMPDIR:-/tmp}/swarmsync-demo.XXXXXX")
else
  mkdir -p "$demo_dir"
fi

shared_token=${SWARMSYNC_TOKEN:-demo-local-token}
tracker_port=${TRACKER_PORT:-0}
seed_port=${SEED_PORT:-0}
download_a_port=${DOWNLOAD_A_PORT:-0}
download_b_port=${DOWNLOAD_B_PORT:-0}

tracker_pid=""
seed_pid=""
download_a_pid=""
download_b_pid=""

cleanup() {
  status=$?
  trap - EXIT INT TERM
  for pid in "$download_a_pid" "$download_b_pid" "$seed_pid" "$tracker_pid"; do
    if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
      kill "$pid" 2>/dev/null || true
    fi
  done
  for pid in "$download_a_pid" "$download_b_pid" "$seed_pid" "$tracker_pid"; do
    if [[ -n "$pid" ]]; then
      wait "$pid" 2>/dev/null || true
    fi
  done
  if [[ $status -ne 0 ]]; then
    for log in "$demo_dir/logs/tracker.log" "$demo_dir/logs/seeder.log" \
      "$demo_dir/logs/downloader-a.log" "$demo_dir/logs/downloader-b.log"; do
      if [[ -f "$log" ]]; then
        printf '\n%s\n' "--- $log ---" >&2
        cat "$log" >&2
      fi
    done
  fi
  exit "$status"
}

trap cleanup EXIT
trap 'exit 130' INT TERM

hash_file() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  else
    shasum -a 256 "$1" | awk '{print $1}'
  fi
}

wait_for_log() {
  local pid=$1
  local marker=$2
  local log=$3
  for _ in $(seq 1 100); do
    if grep -q "$marker" "$log"; then
      return 0
    fi
    if ! kill -0 "$pid" 2>/dev/null; then
      printf '%s\n' "Service exited before readiness: $log" >&2
      return 1
    fi
    sleep 0.05
  done
  printf '%s\n' "Service readiness timed out: $log" >&2
  return 1
}

make -C "$project_root" all >/dev/null
mkdir -p "$demo_dir/logs" "$demo_dir/download-a" "$demo_dir/download-b"

source_file="$demo_dir/demo-source.bin"
manifest_file="$demo_dir/demo-source.swarmsync"
dd if=/dev/zero of="$source_file" bs=1024 count=1152 status=none

"$build_dir/swarmsync" manifest create --file "$source_file" --out "$manifest_file" >/dev/null

SWARMSYNC_TOKEN="$shared_token" "$build_dir/swarmsync-tracker" \
  --port "$tracker_port" --bind 127.0.0.1 --ttl-seconds 10 >"$demo_dir/logs/tracker.log" 2>&1 &
tracker_pid=$!
wait_for_log "$tracker_pid" "SwarmSync tracker listening on" "$demo_dir/logs/tracker.log"
tracker_port=$(awk '/SwarmSync tracker listening on / { split($5, endpoint, ":"); print endpoint[2]; exit }' \
  "$demo_dir/logs/tracker.log")
if [[ -z "$tracker_port" ]]; then
  printf '%s\n' "Unable to read the tracker port from its readiness log" >&2
  exit 1
fi

SWARMSYNC_TOKEN="$shared_token" "$build_dir/swarmsync" seed \
  --manifest "$manifest_file" --file "$source_file" --tracker-host 127.0.0.1 --tracker-port "$tracker_port" \
  --listen-port "$seed_port" --peer-id seed-a --heartbeat-ms 200 >"$demo_dir/logs/seeder.log" 2>&1 &
seed_pid=$!
wait_for_log "$seed_pid" "Seeder peer seed-a is serving swarm" "$demo_dir/logs/seeder.log"

SWARMSYNC_TOKEN="$shared_token" "$build_dir/swarmsync" download \
  --manifest "$manifest_file" --out-dir "$demo_dir/download-a" --tracker-host 127.0.0.1 --tracker-port "$tracker_port" \
  --listen-port "$download_a_port" --peer-id downloader-a --workers 3 --heartbeat-ms 150 \
  >"$demo_dir/logs/downloader-a.log" 2>&1 &
download_a_pid=$!

SWARMSYNC_TOKEN="$shared_token" "$build_dir/swarmsync" download \
  --manifest "$manifest_file" --out-dir "$demo_dir/download-b" --tracker-host 127.0.0.1 --tracker-port "$tracker_port" \
  --listen-port "$download_b_port" --peer-id downloader-b --workers 3 --heartbeat-ms 150 \
  >"$demo_dir/logs/downloader-b.log" 2>&1 &
download_b_pid=$!

if ! wait "$download_a_pid"; then
  exit 1
fi
if ! wait "$download_b_pid"; then
  exit 1
fi

source_hash=$(hash_file "$source_file")
download_a_hash=$(hash_file "$demo_dir/download-a/demo-source.bin")
download_b_hash=$(hash_file "$demo_dir/download-b/demo-source.bin")

if [[ "$source_hash" != "$download_a_hash" || "$source_hash" != "$download_b_hash" ]]; then
  printf '%s\n' "Demo hash verification failed. Logs: $demo_dir/logs" >&2
  exit 1
fi

printf '%s\n' "Demo verified: source, downloader A, and downloader B have the same SHA-256."
printf '%s\n' "Artifacts retained at: $demo_dir"
