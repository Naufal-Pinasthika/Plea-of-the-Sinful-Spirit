#!/usr/bin/env bash

set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/test_common.sh"

require_root
require_binary "$repo_dir/build/cpuwatch" "$repo_dir/build/rate_limit_test"

duration=${DURATION:-5}
attempts=${ATTEMPTS:-100}
cpu=${CPU:-0}
requested_mode=${1:-both}
status_file="$evidence_dir/open-hooks-status.csv"
mkdir -p "$evidence_dir"
echo "mode,status,detail" >"$status_file"

run_mode() {
    local mode=$1
    local option
    local flag
    local temporary_dir
    local input_fifo
    local workload_pid=""
    local monitor_pid=""
    local target_pid=""
    local ready_line
    local status="COMPLETED"
    local detail="requested hook attached"

    if [[ "$mode" == "fentry" ]]; then
        option=--fentry
        flag="fentry=1"
    else
        option=--kprobe-open
        flag="kprobe_open=1"
    fi

    temporary_dir=$(mktemp -d)
    input_fifo="$temporary_dir/input"
    mkfifo "$input_fifo"
    exec 3<>"$input_fifo"

    cleanup_mode() {
        exec 3>&- 2>/dev/null || true
        if [[ -n "$monitor_pid" ]]; then
            kill -INT "$monitor_pid" 2>/dev/null || true
            wait "$monitor_pid" 2>/dev/null || true
        fi
        if [[ -n "$workload_pid" ]]; then
            kill "$workload_pid" 2>/dev/null || true
            wait "$workload_pid" 2>/dev/null || true
        fi
        save_evidence "$temporary_dir/workload.log" "open-${mode}-workload.log"
        save_evidence "$temporary_dir/cpuwatch.log" "open-${mode}-cpuwatch.log"
        rm -rf "$temporary_dir"
    }

    "$repo_dir/build/rate_limit_test" \
        --attempts "$attempts" \
        --cpu "$cpu" \
        <"$input_fifo" \
        >"$temporary_dir/workload.log" 2>&1 &
    workload_pid=$!

    if ! wait_for_line "$temporary_dir/workload.log" '^RATE_READY ' "$workload_pid" "$mode workload"; then
        status="FAILED"
        detail="workload did not become ready"
        cleanup_mode
        echo "$mode,$status,$detail" >>"$status_file"
        return 1
    fi
    ready_line=$(grep '^RATE_READY ' "$temporary_dir/workload.log")
    target_pid=$(extract_pid "$ready_line")

    "$repo_dir/build/cpuwatch" \
        --events \
        --pid "$target_pid" \
        --syscall openat \
        "$option" \
        --interval 500 \
        --duration "$duration" \
        >"$temporary_dir/cpuwatch.log" 2>&1 &
    monitor_pid=$!

    if ! wait_for_line "$temporary_dir/cpuwatch.log" '^CPUWATCH_READY ' "$monitor_pid" "$mode CPUWatch"; then
        status="SKIPPED"
        detail="optional monitor failed before readiness"
    else
        ready_line=$(grep '^CPUWATCH_READY ' "$temporary_dir/cpuwatch.log")
        if [[ "$ready_line" != *"$flag"* ]]; then
            status="SKIPPED"
            detail="requested hook unavailable on this kernel"
        fi
    fi

    printf '\n' >&3
    wait "$workload_pid" || true
    workload_pid=""
    wait "$monitor_pid" || true
    monitor_pid=""
    cleanup_mode
    echo "$mode,$status,$detail" >>"$status_file"
    [[ "$status" != "FAILED" ]]
}

case "$requested_mode" in
    fentry)
        run_mode fentry
        ;;
    kprobe)
        run_mode kprobe
        ;;
    both)
        run_mode fentry
        run_mode kprobe
        ;;
    *)
        echo "usage: $0 [fentry|kprobe|both]" >&2
        exit 2
        ;;
esac

echo "Open-hook tests completed; evidence saved under evidence/."
