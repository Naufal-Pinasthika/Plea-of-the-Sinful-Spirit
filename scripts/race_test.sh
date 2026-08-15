#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
calls=${1:-1000000}
temporary_dir=$(mktemp -d)
race_pid=""
monitor_pid=""

cleanup() {
    if [[ -n "$race_pid" ]]; then kill "$race_pid" 2>/dev/null || true; fi
    if [[ -n "$monitor_pid" ]]; then kill -INT "$monitor_pid" 2>/dev/null || true; fi
    rm -rf "$temporary_dir"
}
trap cleanup EXIT INT TERM

if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
    echo "run this script as root inside the test VM" >&2
    exit 1
fi
if [[ ! -x "$repo_dir/build/race_test" || ! -x "$repo_dir/build/cpuwatch" ]]; then
    echo "missing binaries; run make first" >&2
    exit 1
fi

"$repo_dir/build/race_test" --calls "$calls" >"$temporary_dir/race.log" 2>&1 &
race_pid=$!
for _ in $(seq 1 100); do
    grep -q '^RACE_READY ' "$temporary_dir/race.log" 2>/dev/null && break
    sleep 0.1
done
ready_line=$(grep '^RACE_READY ' "$temporary_dir/race.log" || true)
if [[ -z "$ready_line" ]]; then
    echo "race workload did not become ready" >&2
    exit 1
fi
target_pid=$(sed -n 's/.*pid=\([0-9][0-9]*\).*/\1/p' <<<"$ready_line")

"$repo_dir/build/cpuwatch" --pid "$target_pid" --syscall getpid --json --no-events --interval 500 \
    >"$temporary_dir/stats.jsonl" 2>"$temporary_dir/cpuwatch.log" &
monitor_pid=$!
for _ in $(seq 1 200); do
    grep -q '^CPUWATCH_READY ' "$temporary_dir/cpuwatch.log" 2>/dev/null && break
    if ! kill -0 "$monitor_pid" 2>/dev/null; then
        cat "$temporary_dir/cpuwatch.log" >&2
        exit 1
    fi
    sleep 0.1
done
grep -q '^CPUWATCH_READY ' "$temporary_dir/cpuwatch.log"
kill -USR1 "$race_pid"
wait "$race_pid"
race_pid=""
# SIGINT triggers cpuwatch's terminal per-CPU snapshot, so no timing sleep is needed.
kill -INT "$monitor_pid"
wait "$monitor_pid"
monitor_pid=""

python3 - "$temporary_dir/race.log" "$temporary_dir/stats.jsonl" <<'PY'
import json
import re
import sys

expected = {}
expected_total = None
for line in open(sys.argv[1], encoding="utf-8"):
    match = re.match(r"RACE_CPU cpu=(\d+) expected=(\d+)", line)
    if match:
        expected[int(match.group(1))] = int(match.group(2))
    match = re.match(r"RACE_DONE expected_total=(\d+)", line)
    if match:
        expected_total = int(match.group(1))

observed = {}
for line in open(sys.argv[2], encoding="utf-8"):
    sample = json.loads(line)
    if sample.get("record") != "stats":
        continue
    for cpu in sample["cpus"]:
        observed[cpu["cpu"]] = observed.get(cpu["cpu"], 0) + cpu["syscalls_delta"]

failed = False
for cpu, wanted in sorted(expected.items()):
    got = observed.get(cpu, 0)
    print(f"CPU{cpu} expected={wanted} observed={got} difference={got-wanted}")
    failed |= got != wanted
total = sum(observed.get(cpu, 0) for cpu in expected)
print(f"TOTAL expected={expected_total} observed={total} difference={total-expected_total}")
failed |= total != expected_total
raise SystemExit(1 if failed else 0)
PY

mkdir -p "$repo_dir/evidence"
cp "$temporary_dir/race.log" "$repo_dir/evidence/race-workload.txt"
cp "$temporary_dir/stats.jsonl" "$repo_dir/evidence/race-stats.jsonl"
echo "Race validation passed; evidence saved under evidence/."
