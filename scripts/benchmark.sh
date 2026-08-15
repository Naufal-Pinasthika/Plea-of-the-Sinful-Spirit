#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
calls=${CALLS:-1000000}
iterations=${ITERATIONS:-5}
evidence="$repo_dir/evidence/benchmark.csv"
status_evidence="$repo_dir/evidence/benchmark-status.csv"
monitor_pid=""
monitor_mode=""
missing_features=""
workload_pid=""
workload_mode=""
workload_target_pid=""
temporary_dir=$(mktemp -d)

cleanup() {
    if [[ -n "$workload_pid" ]]; then
        kill "$workload_pid" 2>/dev/null || true
        wait "$workload_pid" 2>/dev/null || true
    fi
    if [[ -n "$monitor_pid" ]]; then
        kill -INT "$monitor_pid" 2>/dev/null || true
        wait "$monitor_pid" 2>/dev/null || true
    fi
    rm -rf "$temporary_dir"
}
trap cleanup EXIT INT TERM

if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
    echo "run this script as root inside the test VM" >&2
    exit 1
fi
for required in "$repo_dir/build/syscall_bench" "$repo_dir/build/cpuwatch"; do
    if [[ ! -x "$required" ]]; then
        echo "missing $required; run make first" >&2
        exit 1
    fi
done
mkdir -p "$repo_dir/evidence"
echo "mode,iteration,duration_ns,calls" >"$evidence"
echo "mode,status,detail" >"$status_evidence"

copy_workload_log() {
    cp "$temporary_dir/$workload_mode-workload.log" \
        "$repo_dir/evidence/benchmark-$workload_mode-workload.log"
}

start_workload() {
    local mode=$1
    local syscall_name=$2
    local ready_line

    workload_mode=$mode
    : >"$temporary_dir/$mode.csv"
    : >"$temporary_dir/$mode-workload.log"
    "$repo_dir/build/syscall_bench" --mode "$mode" --syscall "$syscall_name" \
        --count "$calls" --iterations "$iterations" --wait-signal \
        >"$temporary_dir/$mode.csv" 2>"$temporary_dir/$mode-workload.log" &
    workload_pid=$!
    for _ in $(seq 1 100); do
        if grep -q '^BENCH_READY ' "$temporary_dir/$mode-workload.log" 2>/dev/null; then
            break
        fi
        if ! kill -0 "$workload_pid" 2>/dev/null; then
            cat "$temporary_dir/$mode-workload.log" >&2
            return 1
        fi
        sleep 0.1
    done
    ready_line=$(grep '^BENCH_READY ' "$temporary_dir/$mode-workload.log" || true)
    if [[ -z "$ready_line" ]]; then
        echo "benchmark workload $mode did not become ready" >&2
        return 1
    fi
    workload_target_pid=$(sed -n 's/.*pid=\([0-9][0-9]*\).*/\1/p' <<<"$ready_line")
    if [[ -z "$workload_target_pid" ]]; then
        echo "unable to parse benchmark workload PID for $mode" >&2
        return 1
    fi
}

start_monitor() {
    local mode=$1
    local syscall_name=$2
    local optional_requested=0
    local ready_line
    local requested_options
    local -a missing=()
    shift 2
    missing_features=""
    requested_options=" $* "
    if [[ "$requested_options" == *" --pagefault "* ||
          "$requested_options" == *" --fentry "* ||
          "$requested_options" == *" --kprobe-open "* ]]; then
        optional_requested=1
    fi
    monitor_mode=$mode
    : >"$temporary_dir/$mode-monitor.log"
    "$repo_dir/build/cpuwatch" --interval 1000 \
        --pid "$workload_target_pid" --syscall "$syscall_name" "$@" \
        >/dev/null 2>"$temporary_dir/$mode-monitor.log" &
    monitor_pid=$!
    for _ in $(seq 1 200); do
        if grep -q '^CPUWATCH_READY ' "$temporary_dir/$mode-monitor.log" 2>/dev/null; then
            break
        fi
        if ! kill -0 "$monitor_pid" 2>/dev/null; then
            if [[ $optional_requested -eq 1 ]]; then
                return 3
            fi
            return 1
        fi
        sleep 0.1
    done
    ready_line=$(grep '^CPUWATCH_READY ' "$temporary_dir/$mode-monitor.log" || true)
    if [[ -z "$ready_line" ]]; then
        echo "cpuwatch monitor $mode did not become ready" >&2
        if [[ $optional_requested -eq 1 ]]; then
            return 3
        fi
        return 1
    fi
    if [[ "$requested_options" == *" --pagefault "* && "$ready_line" != *"pagefault=1"* ]]; then
        missing+=(pagefault)
    fi
    if [[ "$requested_options" == *" --fentry "* && "$ready_line" != *"fentry=1"* ]]; then
        missing+=(fentry)
    fi
    if [[ "$requested_options" == *" --kprobe-open "* && "$ready_line" != *"kprobe_open=1"* ]]; then
        missing+=(kprobe_open)
    fi
    if ((${#missing[@]})); then
        missing_features="${missing[*]}"
        return 2
    fi
}

stop_monitor() {
    local wait_status=0

    if [[ -n "$monitor_pid" ]]; then
        kill -INT "$monitor_pid" 2>/dev/null || true
        wait "$monitor_pid" || wait_status=$?
    fi
    cp "$temporary_dir/$monitor_mode-monitor.log" \
        "$repo_dir/evidence/benchmark-$monitor_mode-monitor.log"
    monitor_pid=""
    monitor_mode=""
    return "$wait_status"
}

cancel_workload() {
    if [[ -n "$workload_pid" ]]; then
        kill "$workload_pid" 2>/dev/null || true
        wait "$workload_pid" 2>/dev/null || true
    fi
    copy_workload_log
    workload_pid=""
    workload_mode=""
    workload_target_pid=""
}

finish_workload() {
    local wait_status=0

    kill -USR1 "$workload_pid"
    wait "$workload_pid" || wait_status=$?
    copy_workload_log
    if [[ $wait_status -eq 0 ]]; then
        cat "$temporary_dir/$workload_mode.csv" >>"$evidence"
    fi
    workload_pid=""
    workload_mode=""
    workload_target_pid=""
    return "$wait_status"
}

run_baseline() {
    local mode=$1
    local syscall_name=$2

    start_workload "$mode" "$syscall_name"
    finish_workload
    echo "$mode,COMPLETED,no monitor" >>"$status_evidence"
}

run_instrumented() {
    local mode=$1
    local syscall_name=$2
    local monitor_status
    shift 2

    start_workload "$mode" "$syscall_name"
    if start_monitor "$mode" "$syscall_name" "$@"; then
        finish_workload
        stop_monitor
        echo "$mode,COMPLETED,requested hooks attached" >>"$status_evidence"
        return 0
    else
        monitor_status=$?
    fi
    if [[ $monitor_status -eq 2 ]]; then
        stop_monitor || true
        cancel_workload
        echo "$mode,SKIPPED,missing optional hook: $missing_features" >>"$status_evidence"
        echo "Skipping $mode: missing optional hook: $missing_features" >&2
        return 0
    fi
    if [[ $monitor_status -eq 3 ]]; then
        stop_monitor || true
        cancel_workload
        echo "$mode,SKIPPED,optional monitor failed before readiness" >>"$status_evidence"
        echo "Skipping $mode: optional monitor failed before readiness; inspect its saved log" >&2
        return 0
    fi
    cat "$temporary_dir/$mode-monitor.log" >&2
    stop_monitor || true
    cancel_workload
    echo "$mode,FAILED,mandatory monitor failed before readiness" >>"$status_evidence"
    return "$monitor_status"
}

run_baseline baseline getpid
run_instrumented aggregate getpid --no-events
run_instrumented ringbuf getpid --events
run_instrumented full getpid --events --pagefault --fentry

run_baseline open-baseline openat
run_instrumented kprobe openat --events --kprobe-open
run_instrumented fentry openat --events --fentry

echo "Benchmark data saved to $evidence"
echo "Benchmark mode status saved to $status_evidence"
