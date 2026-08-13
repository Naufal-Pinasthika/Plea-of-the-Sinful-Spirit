#ifndef CPUWATCH_EVENTS_H
#define CPUWATCH_EVENTS_H

#include "common.h"

struct cpuwatch_event_header {
	__u64 timestamp_ns;
	__u32 tgid;
	__u32 pid;
	__u32 cpu;
	__u32 type;
	char comm[CPUWATCH_TASK_COMM_LEN];
};

struct cpuwatch_syscall_enter_payload {
	__s64 syscall_nr;
	__u64 args[6];
};

struct cpuwatch_syscall_exit_payload {
	__s64 syscall_nr;
	__s64 return_value;
	__u64 duration_ns;
	__u32 args_valid;
	__u32 reserved;
	__u64 args[6];
};

struct cpuwatch_sched_switch_payload {
	__u32 prev_pid;
	__u32 next_pid;
	char prev_comm[CPUWATCH_TASK_COMM_LEN];
	char next_comm[CPUWATCH_TASK_COMM_LEN];
};

struct cpuwatch_page_fault_payload {
	__u64 address;
};

struct cpuwatch_open_payload {
	__s32 dfd;
	__u32 source;
	__u64 flags;
	__u64 mode;
	__s64 return_value;
	__u64 duration_ns;
	char filename[CPUWATCH_PATH_LEN];
};

struct cpuwatch_rate_limit_payload {
	__u32 target_syscall;
	__u32 current_count;
	__u32 threshold;
	__s32 error_code;
};

struct cpuwatch_event {
	struct cpuwatch_event_header header;
	union {
		struct cpuwatch_syscall_enter_payload syscall_enter;
		struct cpuwatch_syscall_exit_payload syscall_exit;
		struct cpuwatch_sched_switch_payload sched_switch;
		struct cpuwatch_page_fault_payload page_fault;
		struct cpuwatch_open_payload open;
		struct cpuwatch_rate_limit_payload rate_limit;
	} data;
};

#endif
