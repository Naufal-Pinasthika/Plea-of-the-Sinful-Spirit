#!/usr/bin/env bash

set -euo pipefail

repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
evidence_dir="$repo_dir/evidence"

require_root() {
    if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
        echo "run this script as root inside the test VM" >&2
        exit 1
    fi
}

require_binary() {
    local binary

    for binary in "$@"; do
        if [[ ! -x "$binary" ]]; then
            echo "missing $binary; run make first" >&2
            exit 1
        fi
    done
}

wait_for_line() {
    local file=$1
    local pattern=$2
    local pid=$3
    local description=$4

    for _ in $(seq 1 200); do
        if grep -qE "$pattern" "$file" 2>/dev/null; then
            return 0
        fi
        if [[ -n "$pid" ]] && ! kill -0 "$pid" 2>/dev/null; then
            echo "$description exited before becoming ready" >&2
            return 1
        fi
        sleep 0.1
    done
    echo "$description did not become ready" >&2
    return 1
}

extract_pid() {
    sed -n 's/.*pid=\([0-9][0-9]*\).*/\1/p' <<<"$1"
}

save_evidence() {
    local source=$1
    local destination=$2

    mkdir -p "$evidence_dir"
    if [[ -f "$source" ]]; then
        cp "$source" "$evidence_dir/$destination"
    fi
}
