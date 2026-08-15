#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
binary="$repo_dir/build/cpuwatch"

if [[ ! -x "$binary" ]]; then
    echo "missing $binary; run make first" >&2
    exit 1
fi
if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
    echo "run this script as root inside the test VM" >&2
    exit 1
fi
exec "$binary" "$@"

