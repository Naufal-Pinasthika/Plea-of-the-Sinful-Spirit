#ifndef CPUWATCH_COMMON_H
#define CPUWATCH_COMMON_H

#ifndef __VMLINUX_H__
#include <linux/types.h>
#endif

#define CPUWATCH_TASK_COMM_LEN 16
#define CPUWATCH_PATH_LEN 128
#define CPUWATCH_NSEC_PER_SEC 1000000000ULL
#define CPUWATCH_CONFIG_KEY 0U
#define CPUWATCH_STATS_KEY 0U

enum cpuwatch_event_type {
	CPUWATCH_EVENT_SYSCALL_ENTER = 1,
	CPUWATCH_EVENT_SYSCALL_EXIT,
	CPUWATCH_EVENT_SCHED_SWITCH,
	CPUWATCH_EVENT_PAGE_FAULT,
	CPUWATCH_EVENT_FENTRY_OPEN,
	CPUWATCH_EVENT_FEXIT_OPEN,
	CPUWATCH_EVENT_KPROBE_OPEN,
	CPUWATCH_EVENT_KRETPROBE_OPEN,
	CPUWATCH_EVENT_RATE_LIMIT,
};

enum cpuwatch_filter_flags {
	CPUWATCH_FILTER_TGID = 1U << 0,
	CPUWATCH_FILTER_SYSCALL = 1U << 1,
};

struct cpuwatch_config {
	__u32 emit_events;
	__u32 filter_flags;
	__u32 monitor_tgid;
	__s32 monitor_syscall;
	__u32 pagefault_enabled;
	__u32 fentry_enabled;
	__u32 kprobe_open_enabled;
	__u32 rate_limit_enabled;
	__u32 rate_limit_tgid;
	__u32 rate_limit_threshold;
	__s32 rate_limit_syscall;
	__u32 reserved;
};

struct cpuwatch_cpu_stats {
	__u64 syscall_enter;
	__u64 syscall_exit;
	__u64 context_switches;
	__u64 page_faults;
	__u64 rate_limited;
	__u64 ringbuf_drops;
};

struct cpuwatch_inflight_syscall {
	__u64 timestamp_ns;
	__s64 syscall_nr;
	__u64 args[6];
};

struct cpuwatch_rate_key {
	__u32 tgid;
};

struct cpuwatch_rate_state {
	__u64 window_start_ns;
	__u32 count;
	__u32 denied;
};

#endif

