# Plea of the Sinful SPirit

Merupakan program eBPF yang bertujuan untuk mencatat & melihat aktivitas CPU dan syscall. Secara garis besar, program mencatat:

- syscall masuk dan keluar
- context switch pada setiap CPU
- page fault
- informasi proses, PID, CPU, timestamp, nomor syscall, dan argumen syscall
- dan sebagainya.

## Build

Program dibangun dan dijalankan di VM Linux dengan kernel yang sebaiknya memiliki memiliki kompabilitas BTF. Make sure tools berikut sudah terpasang
```bash
sudo apt update
sudo apt install clang llvm bpftool libbpf-dev libelf-dev zlib1g-dev \
  build-essential pkg-config
```

Build project:

```bash
make clean
make
make verifier
```

Hasil build berada di folder `build/`.

## Menjalankan Program

Program dapat langsung dijalankan dengan
```bash
sudo ./build/cpuwatch
```
Namun biasanya kurang bagus jika tidak ditabahkan suatu opsi sebagai konfigurasi. Maka dari itu, biasanya porgram dijalankan dengan mempass beberapa flag. Contoh berikut merupakan cara menjalankan program dengan melihat evet selama 10 detik:

```bash
sudo ./build/cpuwatch \
  --events \
  --interval 1000 \
  --duration 10
```

Dapat juga melakukan filter berdasarkan proses dan syscall:

```bash
sudo ./build/cpuwatch \
  --events \
  --pid PID \
  --syscall getpid \
  --interval 500 \
  --duration 10
```

Opsi tambahan atau opsi tambahan lainnya

```text
--pagefault       mengaktifkan tracing page fault
--fentry          mengaktifkan fentry/fexit untuk do_sys_openat2
--kprobe-open     mengaktifkan kprobe/kretprobe untuk do_sys_openat2
--rate-limit-pid  PID proses yang akan dibatasi
--rate-limit      jumlah openat yang diizinkan per CPU per detik
--no-events       hanya menampilkan statistik tanpa log event
```

N.B.: Khusus untuk rate limiter dibutuhkan konfigutasi membutuhkan BPF LSM. Cek dengan:

```bash
cat /sys/kernel/security/lsm
```

Output harus memuat `bpf`.

## Test

Pengujian dilakukan di VM dan hasilnya disimpan di folder `evidence/`.

Test hook wajib:

```bash
sudo ./scripts/test_mandatory.sh
```

Test bonus page fault:

```bash
sudo ./scripts/test_bonus_pagefault.sh
```

Test bonus rate limiting:

```bash
sudo ./scripts/test_bonus_rate_limit.sh
```

Test bonus fentry/fexit dan kprobe:

```bash
sudo ./scripts/test_bonus_fentry-fexit.sh both
```

Test race pada map per-CPU:

```bash
sudo ./scripts/test_bonus_race-data.sh 1000000
```

Test verifier:

```bash
sudo ./scripts/verifier_test.sh
```

Benchmark:

```bash
sudo env CALLS=100000 ITERATIONS=3 ./scripts/benchmark.sh
```

## Video demo

Link video tertera pada di bawah:
https://drive.google.com/file/d/1oVAGW17-dJNUNBT9Z60aMdMymNEUinIy/view?usp=sharing

## Laporan

Dokumen laporan dan panduan pengujian tersedia di folder `docs/`:
