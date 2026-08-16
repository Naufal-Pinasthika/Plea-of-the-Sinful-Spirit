#!/usr/bin/env bash

set -euo pipefail

repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
evidence_dir="$repo_dir/evidence"

require_root() {
    [[ ${EUID:-$(id -u)} -eq 0 ]] || { echo "run this script as root inside the test VM" >&2; exit 1; }
}

require_binary() {
    local binary
    for binary in "$@"; do
        [[ -x "$binary" ]] || { echo "missing $binary; run make first" >&2; exit 1; }
    done
}

wait_for_line() {
    local file=$1 pattern=$2 pid=$3 description=$4
    for _ in $(seq 1 200); do
        grep -qE "$pattern" "$file" 2>/dev/null && return 0
        [[ -z "$pid" ]] || kill -0 "$pid" 2>/dev/null || { echo "$description exited before becoming ready" >&2; return 1; }
        sleep 0.1
    done
    echo "$description did not become ready" >&2
    return 1
}

extract_pid() { sed -n 's/.*pid=\([0-9][0-9]*\).*/\1/p' <<<"$1"; }

save_evidence() {
    mkdir -p "$evidence_dir"
    [[ -f "$1" ]] && cp "$1" "$evidence_dir/$2"
}

require_root
require_binary "$repo_dir/build/cpuwatch" "$repo_dir/build/syscall_bench"

duration=${DURATION:-5}
calls=${CALLS:-100000}
temporary_dir=$(mktemp -d)
workload_pid=""
monitor_pid=""

cleanup() {
    local status=$?

    trap - EXIT INT TERM
    if [[ -n "$monitor_pid" ]]; then
        kill -INT "$monitor_pid" 2>/dev/null || true
        wait "$monitor_pid" 2>/dev/null || true
    fi
    if [[ -n "$workload_pid" ]]; then
        kill "$workload_pid" 2>/dev/null || true
        wait "$workload_pid" 2>/dev/null || true
    fi
    save_evidence "$temporary_dir/workload.csv" mandatory-workload.csv
    save_evidence "$temporary_dir/workload.log" mandatory-workload.log
    save_evidence "$temporary_dir/cpuwatch.log" mandatory-cpuwatch.log
    rm -rf "$temporary_dir"
    exit "$status"
}
trap cleanup EXIT INT TERM

"$repo_dir/build/syscall_bench" \
    --mode mandatory \
    --syscall getpid \
    --count "$calls" \
    --iterations 1 \
    --wait-signal \
    >"$temporary_dir/workload.csv" \
    2>"$temporary_dir/workload.log" &
workload_pid=$!

wait_for_line "$temporary_dir/workload.log" '^BENCH_READY ' "$workload_pid" "mandatory workload"
ready_line=$(grep '^BENCH_READY ' "$temporary_dir/workload.log")
target_pid=$(extract_pid "$ready_line")

"$repo_dir/build/cpuwatch" \
    --events \
    --pid "$target_pid" \
    --syscall getpid \
    --interval 500 \
    --duration "$duration" \
    >"$temporary_dir/cpuwatch.log" 2>&1 &
monitor_pid=$!

wait_for_line "$temporary_dir/cpuwatch.log" '^CPUWATCH_READY ' "$monitor_pid" "mandatory CPUWatch"
kill -USR1 "$workload_pid"
wait "$workload_pid"
workload_pid=""
wait "$monitor_pid"
monitor_pid=""

echo "Mandatory test completed; evidence saved under evidence/."
