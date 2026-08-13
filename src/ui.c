#include <stdio.h>

#include "ui.h"

void cpuwatch_ui_render(const char *kernel_release,
			const struct cpuwatch_cpu_rate *rates, int nr_cpus,
			unsigned int interval_ms, double elapsed_seconds)
{
	struct cpuwatch_cpu_rate total = {0};
	int cpu;

	printf("\nSTATS elapsed=%.3fs kernel=%s interval=%ums\n",
	       elapsed_seconds, kernel_release, interval_ms);
	printf("+-----+-------------+-------------+-------------+-------------+-------------+-------------+\n");
	printf("| CPU |  Syscalls/s | Context Δ   | ContextSw/s | PageFault/s |   Limited/s |     Drops/s |\n");
	printf("+-----+-------------+-------------+-------------+-------------+-------------+-------------+\n");
	for (cpu = 0; cpu < nr_cpus; cpu++) {
		printf("| %3d | %11.0f | %11llu | %11.0f | %11.0f | %11.0f | %11.0f |\n",
		       cpu, rates[cpu].syscalls, rates[cpu].context_switch_delta,
		       rates[cpu].context_switches,
		       rates[cpu].page_faults, rates[cpu].rate_limited,
		       rates[cpu].ringbuf_drops);
		total.context_switch_delta += rates[cpu].context_switch_delta;
		total.syscalls += rates[cpu].syscalls;
		total.context_switches += rates[cpu].context_switches;
		total.page_faults += rates[cpu].page_faults;
		total.rate_limited += rates[cpu].rate_limited;
		total.ringbuf_drops += rates[cpu].ringbuf_drops;
	}
	printf("+-----+-------------+-------------+-------------+-------------+-------------+-------------+\n");
	printf("| ALL | %11.0f | %11llu | %11.0f | %11.0f | %11.0f | %11.0f |\n",
	       total.syscalls, total.context_switch_delta, total.context_switches, total.page_faults,
	       total.rate_limited, total.ringbuf_drops);
	printf("+-----+-------------+-------------+-------------+-------------+-------------+-------------+\n");
	fflush(stdout);
}
