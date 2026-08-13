#!/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
output=$(mktemp)
trap 'rm -f "$output"' EXIT

printf q | PATH="$project_dir/tests/fake-bin:$PATH" \
    script -qec "stty rows 30 cols 120; TERM=xterm $project_dir/wifi-tui" "$output" >/dev/null

grep -q 'Lab WiFi' "$output"
grep -q '192.0.2.20' "$output"
grep -q '255.255.255.0' "$output"

echo 'smoke test passed'
