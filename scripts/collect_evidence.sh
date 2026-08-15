#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
evidence_dir="$repo_dir/evidence"

if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
    echo "run this script as root while cpuwatch is loaded" >&2
    exit 1
fi
command -v bpftool >/dev/null 2>&1 || { echo "bpftool is required" >&2; exit 1; }
mkdir -p "$evidence_dir"
bpftool prog list >"$evidence_dir/bpftool-prog-list.txt"
bpftool map list >"$evidence_dir/bpftool-map-list.txt"
if ! bpftool map dump name stats >"$evidence_dir/bpftool-stats-dump.txt" 2>&1; then
    echo "stats map is not currently loaded; map list was still captured" >&2
fi
echo "bpftool evidence saved under evidence/."

