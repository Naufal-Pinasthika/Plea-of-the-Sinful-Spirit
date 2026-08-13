#ifndef CPUWATCH_UI_H
#define CPUWATCH_UI_H

#include "common.h"

struct cpuwatch_cpu_rate {
	unsigned long long syscall_delta;
	unsigned long long context_switch_delta;
	unsigned long long page_fault_delta;
	unsigned long long rate_limited_delta;
	unsigned long long ringbuf_drop_delta;
	double syscalls;
	double context_switches;
	double page_faults;
	double rate_limited;
	double ringbuf_drops;
};

void cpuwatch_ui_render(const char *kernel_release,
			const struct cpuwatch_cpu_rate *rates, int nr_cpus,
			unsigned int interval_ms, double elapsed_seconds);

#endif
