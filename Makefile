SHELL := /bin/bash

BUILD_DIR := build
SRC_DIR := src
TOOLS_DIR := tools
VERIFIER_DIR := verifier

CC ?= cc
BPF_CLANG ?= clang
BPFTOOL ?= bpftool
PKG_CONFIG ?= pkg-config
PANDOC ?= pandoc

HOST_ARCH := $(shell uname -m)
TARGET_ARCH_x86_64 := x86
TARGET_ARCH_aarch64 := arm64
TARGET_ARCH_arm64 := arm64
TARGET_ARCH_riscv64 := riscv
TARGET_ARCH_s390x := s390
TARGET_ARCH := $(or $(TARGET_ARCH_$(HOST_ARCH)),$(HOST_ARCH))
MULTIARCH := $(shell $(CC) -print-multiarch 2>/dev/null || gcc -print-multiarch 2>/dev/null)

CPPFLAGS := -I$(SRC_DIR) -I$(BUILD_DIR)
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra -Wpedantic
BPF_CFLAGS := -g -O2 -target bpf -D__TARGET_ARCH_$(TARGET_ARCH) \
	-I$(BUILD_DIR) -I$(SRC_DIR) -I/usr/include
ifneq ($(MULTIARCH),)
BPF_CFLAGS += -I/usr/include/$(MULTIARCH)
endif

LIBBPF_CFLAGS := $(shell $(PKG_CONFIG) --cflags libbpf 2>/dev/null)
LIBBPF_LIBS := $(shell $(PKG_CONFIG) --libs libbpf 2>/dev/null)
ifeq ($(strip $(LIBBPF_LIBS)),)
LIBBPF_LIBS := -lbpf -lelf -lz
endif

TOOLS := syscall_bench race_test pagefault_test rate_limit_test
TOOL_BINARIES := $(addprefix $(BUILD_DIR)/,$(TOOLS))

.PHONY: all clean check tools verifier test benchmark test-mandatory test-pagefault test-rate-limit test-race test-open-hooks report help

all: $(BUILD_DIR)/cpuwatch tools

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/vmlinux.h: /sys/kernel/btf/vmlinux | $(BUILD_DIR)
	@test -r $< || { echo "BTF file $< is not readable" >&2; exit 1; }
	$(BPFTOOL) btf dump file $< format c > $@.tmp
	mv $@.tmp $@

$(BUILD_DIR)/cpuwatch.bpf.o: $(SRC_DIR)/cpuwatch.bpf.c $(SRC_DIR)/common.h \
		$(SRC_DIR)/events.h $(BUILD_DIR)/vmlinux.h | $(BUILD_DIR)
	$(BPF_CLANG) $(BPF_CFLAGS) -c $< -o $@

$(BUILD_DIR)/cpuwatch.skel.h: $(BUILD_DIR)/cpuwatch.bpf.o
	$(BPFTOOL) gen skeleton $< > $@.tmp
	mv $@.tmp $@

$(BUILD_DIR)/cpuwatch: $(SRC_DIR)/cpuwatch.c $(SRC_DIR)/ui.c $(SRC_DIR)/ui.h \
		$(SRC_DIR)/common.h $(SRC_DIR)/events.h $(BUILD_DIR)/cpuwatch.skel.h
	$(CC) $(CPPFLAGS) $(LIBBPF_CFLAGS) $(CFLAGS) \
		$(SRC_DIR)/cpuwatch.c $(SRC_DIR)/ui.c -o $@ $(LIBBPF_LIBS)

tools: $(TOOL_BINARIES)

$(BUILD_DIR)/syscall_bench: $(TOOLS_DIR)/syscall_bench.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD_DIR)/race_test: $(TOOLS_DIR)/race_test.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< -o $@ -pthread

$(BUILD_DIR)/pagefault_test: $(TOOLS_DIR)/pagefault_test.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD_DIR)/rate_limit_test: $(TOOLS_DIR)/rate_limit_test.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD_DIR)/verifier_bad.bpf.o: $(VERIFIER_DIR)/verifier_bad.bpf.c \
		$(BUILD_DIR)/vmlinux.h | $(BUILD_DIR)
	$(BPF_CLANG) $(BPF_CFLAGS) -c $< -o $@

$(BUILD_DIR)/verifier_loader: $(VERIFIER_DIR)/verifier_loader.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(LIBBPF_CFLAGS) $(CFLAGS) $< -o $@ $(LIBBPF_LIBS)

verifier: $(BUILD_DIR)/verifier_bad.bpf.o $(BUILD_DIR)/verifier_loader
	@echo "Verifier artifacts built. Run sudo scripts/verifier_test.sh in the VM to load them."

check:
	./scripts/check_environment.sh

test: all verifier
	$(BUILD_DIR)/cpuwatch --help >/dev/null
	@for script in scripts/*.sh; do bash -n "$$script"; done
	@echo "Static/safe checks passed. Privileged integration tests remain explicit scripts."

benchmark: all
	@echo "This target runs privileged workloads and must only be used in the test VM."
	./scripts/benchmark.sh

test-mandatory: all
	@echo "This target runs a one-shot mandatory-hook evidence test in the VM."
	./scripts/test_mandatory.sh

test-pagefault: all
	@echo "This target runs the page-fault bonus evidence test in the VM."
	./scripts/test_pagefault.sh

test-rate-limit: all
	@echo "This target runs the BPF LSM rate-limit evidence test in the VM."
	./scripts/test_rate_limit.sh

test-race: all
	@echo "This target runs the exact per-CPU race validation in the VM."
	./scripts/race_test.sh

test-open-hooks: all
	@echo "This target runs fentry and kprobe open-hook evidence tests in the VM."
	./scripts/test_open_hooks.sh both

report: docs/REPORT.md | $(BUILD_DIR)
	@command -v $(PANDOC) >/dev/null 2>&1 || { \
		echo "pandoc is required to build the PDF report" >&2; exit 1; }
	$(PANDOC) docs/REPORT.md --from gfm --pdf-engine=xelatex \
		--toc --number-sections -o $(BUILD_DIR)/CPUWatch-Report.pdf

clean:
	rm -rf $(BUILD_DIR)

help:
	@echo "make            build cpuwatch and workload tools"
	@echo "make check      collect VM environment evidence"
	@echo "make verifier   build the intentionally invalid verifier example"
	@echo "make test       compile and run non-privileged static/safe checks"
	@echo "make benchmark  run the VM-only benchmark workflow"
	@echo "make test-mandatory   capture one-shot mandatory-hook evidence"
	@echo "make test-pagefault   capture page-fault bonus evidence"
	@echo "make test-rate-limit  capture BPF LSM rate-limit evidence"
	@echo "make test-race        validate exact per-CPU syscall counts"
	@echo "make test-open-hooks  capture fentry and kprobe evidence"
	@echo "make report     render docs/REPORT.md as PDF"
	@echo "make clean      remove generated build artifacts"
