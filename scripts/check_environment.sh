#!/usr/bin/env bash
set -uo pipefail

repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
evidence_dir="$repo_dir/evidence"
environment_file="$evidence_dir/environment.txt"
feature_file="$evidence_dir/feature-probe.txt"

mkdir -p "$evidence_dir"
{
    echo "CPUWatch environment report"
    date --iso-8601=seconds
    uname -a
    echo "kernel=$(uname -r)"
    echo "architecture=$(uname -m)"
    echo "online_cpus=$(getconf _NPROCESSORS_ONLN)"
    if [[ -r /sys/kernel/btf/vmlinux ]]; then
        echo "btf=/sys/kernel/btf/vmlinux (readable)"
    else
        echo "btf=missing"
    fi
    command -v clang || true
    clang --version 2>&1 | head -n 2 || true
    command -v bpftool || true
    bpftool version 2>&1 || true
    pkg-config --modversion libbpf 2>&1 || true
    if grep -qw handle_mm_fault /proc/kallsyms 2>/dev/null; then
        echo "handle_mm_fault=present"
    else
        echo "handle_mm_fault=unavailable"
    fi
    if grep -qw do_sys_openat2 /proc/kallsyms 2>/dev/null; then
        echo "do_sys_openat2=present"
    else
        echo "do_sys_openat2=unavailable"
    fi
    if [[ -r /sys/kernel/security/lsm ]]; then
        echo "lsm=$(< /sys/kernel/security/lsm)"
    else
        echo "lsm=status file unavailable"
    fi
} | tee "$environment_file"

if command -v bpftool >/dev/null 2>&1; then
    bpftool feature probe kernel 2>&1 | tee "$feature_file"
else
    echo "bpftool is not installed" | tee "$feature_file"
    exit 1
fi
