#!/usr/bin/env bash
# Start vfu_kvssd, drive it with the client harness, and report the result.
# Usage: tests/run_harness.sh [bin-dir]   (default: zig-out/bin)
set -euo pipefail

bin_dir="${1:-zig-out/bin}"
sock="$(mktemp -u /tmp/vfu_kvssd.XXXXXX.sock)"

"$bin_dir/vfu_kvssd" -s "$sock" &
pid=$!
trap 'kill "$pid" 2>/dev/null || true; rm -f "$sock"' EXIT

for _ in $(seq 1 100); do
	[ -S "$sock" ] && break
	sleep 0.05
done

"$bin_dir/vfu_kvssd_harness" "$sock"
