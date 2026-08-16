#!/usr/bin/env bash

set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/test_common.sh"

require_root
require_binary "$repo_dir/build/cpuwatch" "$repo_dir/build/rate_limit_test"

if ! grep -qw bpf /sys/kernel/security/lsm 2>/dev/null; then
    echo "BPF LSM is not active; check /sys/kernel/security/lsm and GRUB lsm=..." >&2
    exit 2
fi

duration=${DURATION:-6}
attempts=${ATTEMPTS:-200}
threshold=${THRESHOLD:-50}
cpu=${CPU:-0}
temporary_dir=$(mktemp -d)
input_fifo="$temporary_dir/input"
workload_pid=""
monitor_pid=""

cleanup() {
    local status=$?

    trap - EXIT INT TERM
    exec 3>&- 2>/dev/null || true
    if [[ -n "$monitor_pid" ]]; then
        kill -INT "$monitor_pid" 2>/dev/null || true
        wait "$monitor_pid" 2>/dev/null || true
    fi
    if [[ -n "$workload_pid" ]]; then
        kill "$workload_pid" 2>/dev/null || true
        wait "$workload_pid" 2>/dev/null || true
    fi
    save_evidence "$temporary_dir/workload.log" rate-limit-workload.log
    save_evidence "$temporary_dir/cpuwatch.log" rate-limit-cpuwatch.log
    rm -rf "$temporary_dir"
    exit "$status"
}
trap cleanup EXIT INT TERM

mkfifo "$input_fifo"
exec 3<>"$input_fifo"
"$repo_dir/build/rate_limit_test" \
    --attempts "$attempts" \
    --cpu "$cpu" \
    <"$input_fifo" \
    >"$temporary_dir/workload.log" 2>&1 &
workload_pid=$!

wait_for_line "$temporary_dir/workload.log" '^RATE_READY ' "$workload_pid" "rate-limit workload"
ready_line=$(grep '^RATE_READY ' "$temporary_dir/workload.log")
target_pid=$(extract_pid "$ready_line")

"$repo_dir/build/cpuwatch" \
    --events \
    --pid "$target_pid" \
    --syscall openat \
    --rate-limit-pid "$target_pid" \
    --rate-limit "$threshold" \
    --interval 500 \
    --duration "$duration" \
    >"$temporary_dir/cpuwatch.log" 2>&1 &
monitor_pid=$!

wait_for_line "$temporary_dir/cpuwatch.log" '^CPUWATCH_READY ' "$monitor_pid" "rate-limit CPUWatch"
printf '\n' >&3
wait "$workload_pid"
workload_pid=""
wait "$monitor_pid"
monitor_pid=""

echo "Rate-limit test completed; evidence saved under evidence/."
