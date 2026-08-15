#!/usr/bin/env bash

set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/test_common.sh"

require_root
require_binary "$repo_dir/build/cpuwatch" "$repo_dir/build/pagefault_test"

duration=${DURATION:-6}
pages=${PAGES:-4096}
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
    save_evidence "$temporary_dir/workload.log" pagefault-workload.log
    save_evidence "$temporary_dir/cpuwatch.log" pagefault-cpuwatch.log
    rm -rf "$temporary_dir"
    exit "$status"
}
trap cleanup EXIT INT TERM

mkfifo "$input_fifo"
exec 3<>"$input_fifo"
"$repo_dir/build/pagefault_test" --pages "$pages" <"$input_fifo" \
    >"$temporary_dir/workload.log" 2>&1 &
workload_pid=$!

wait_for_line "$temporary_dir/workload.log" '^PAGEFAULT_READY ' "$workload_pid" "page-fault workload"
ready_line=$(grep '^PAGEFAULT_READY ' "$temporary_dir/workload.log")
target_pid=$(extract_pid "$ready_line")

"$repo_dir/build/cpuwatch" \
    --events \
    --pagefault \
    --pid "$target_pid" \
    --interval 500 \
    --duration "$duration" \
    >"$temporary_dir/cpuwatch.log" 2>&1 &
monitor_pid=$!

wait_for_line "$temporary_dir/cpuwatch.log" '^CPUWATCH_READY ' "$monitor_pid" "page-fault CPUWatch"
printf '\n' >&3
wait "$workload_pid"
workload_pid=""
wait "$monitor_pid"
monitor_pid=""

echo "Page-fault test completed; evidence saved under evidence/."
