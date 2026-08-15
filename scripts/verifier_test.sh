#!/usr/bin/env bash
set -u

repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
evidence_dir="$repo_dir/evidence"
bad_object="$repo_dir/build/verifier_bad.bpf.o"
loader="$repo_dir/build/verifier_loader"
cpuwatch="$repo_dir/build/cpuwatch"

if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
    echo "run this script as root inside the test VM" >&2
    exit 1
fi
for required in "$bad_object" "$loader" "$cpuwatch"; do
    if [[ ! -e "$required" ]]; then
        echo "missing $required; run make and make verifier first" >&2
        exit 1
    fi
done
mkdir -p "$evidence_dir"

set +e
"$loader" "$bad_object" >"$evidence_dir/verifier-rejected.txt" 2>&1
bad_status=$?
set -e
if [[ $bad_status -eq 0 ]]; then
    echo "invalid BPF object was unexpectedly accepted" >&2
    exit 1
fi
echo "Verifier rejected the intentionally invalid object as expected."

set +e
"$cpuwatch" --json --duration 1 --no-events \
    >"$evidence_dir/verifier-accepted.txt" 2>&1
good_status=$?
set -e
if [[ $good_status -ne 0 ]]; then
    echo "production BPF object did not load; inspect evidence/verifier-accepted.txt" >&2
    exit 1
fi
echo "Production object loaded successfully. Evidence saved under evidence/."

