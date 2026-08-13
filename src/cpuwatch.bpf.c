#include "vmlinux.h"

#include <bpf/bpf_core_read.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

#include "common.h"
#include "events.h"

#define CPUWATCH_EAGAIN 11

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, struct cpuwatch_cpu_stats);
} stats SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, struct cpuwatch_config);
} config SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_RINGBUF);
	__uint(max_entries, 1 << 24);
} events SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_LRU_HASH);
	__uint(max_entries, 16384);
	__type(key, __u64);
	__type(value, struct cpuwatch_inflight_syscall);
} inflight SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_LRU_HASH);
	__uint(max_entries, 4096);
	__type(key, __u64);
	__type(value, __s64);
} target_inflight SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_HASH);
	__uint(max_entries, 1024);
	__type(key, struct cpuwatch_rate_key);
	__type(value, struct cpuwatch_rate_state);
} rate_states SEC(".maps");

static __always_inline struct cpuwatch_config *get_config(void)
{
	__u32 key = CPUWATCH_CONFIG_KEY;

	return bpf_map_lookup_elem(&config, &key);
}

static __always_inline struct cpuwatch_cpu_stats *get_stats(void)
{
	__u32 key = CPUWATCH_STATS_KEY;

	return bpf_map_lookup_elem(&stats, &key);
}

static __always_inline bool syscall_selected(const struct cpuwatch_config *cfg,
					      __u32 tgid, __s64 syscall_nr)
{
	if (!cfg)
		return true;
	if ((cfg->filter_flags & CPUWATCH_FILTER_TGID) &&
	    cfg->monitor_tgid != tgid)
		return false;
	if ((cfg->filter_flags & CPUWATCH_FILTER_SYSCALL) &&
	    cfg->monitor_syscall != syscall_nr)
		return false;
	return true;
}

static __always_inline bool process_selected(const struct cpuwatch_config *cfg,
					      __u32 tgid)
{
	if (!cfg)
		return true;
	return !(cfg->filter_flags & CPUWATCH_FILTER_TGID) ||
	       cfg->monitor_tgid == tgid;
}

static __always_inline void fill_header(struct cpuwatch_event *event, __u32 type)
{
	__u64 pid_tgid = bpf_get_current_pid_tgid();

	event->header.timestamp_ns = bpf_ktime_get_ns();
	event->header.tgid = pid_tgid >> 32;
	event->header.pid = (__u32)pid_tgid;
	event->header.cpu = bpf_get_smp_processor_id();
	event->header.type = type;
	bpf_get_current_comm(event->header.comm, sizeof(event->header.comm));
}

static __always_inline struct cpuwatch_event *reserve_event(
		const struct cpuwatch_config *cfg, struct cpuwatch_cpu_stats *cpu_stats,
		__u32 type)
{
	struct cpuwatch_event *event;

	if (!cfg || !cfg->emit_events)
		return 0;

	event = bpf_ringbuf_reserve(&events, sizeof(*event), 0);
	if (!event) {
		if (cpu_stats)
			cpu_stats->ringbuf_drops++;
		return 0;
	}

	__builtin_memset(event, 0, sizeof(*event));
	fill_header(event, type);
	return event;
}

SEC("tracepoint/raw_syscalls/sys_enter")
int handle_sys_enter(struct trace_event_raw_sys_enter *ctx)
{
	struct cpuwatch_inflight_syscall state = {0};
	struct cpuwatch_cpu_stats *cpu_stats;
	struct cpuwatch_config *cfg;
	struct cpuwatch_event *event;
	__u64 pid_tgid = bpf_get_current_pid_tgid();
	__u32 tgid = pid_tgid >> 32;
	__s64 syscall_nr = ctx->id;
	int i;

	cfg = get_config();
	if (cfg && cfg->rate_limit_enabled && cfg->rate_limit_tgid == tgid &&
	    cfg->rate_limit_syscall == syscall_nr)
		bpf_map_update_elem(&target_inflight, &pid_tgid,
				    &syscall_nr, BPF_ANY);

	if (!syscall_selected(cfg, tgid, syscall_nr))
		return 0;

	cpu_stats = get_stats();
	if (cpu_stats)
		cpu_stats->syscall_enter++;

	state.timestamp_ns = bpf_ktime_get_ns();
	state.syscall_nr = syscall_nr;
#pragma unroll
	for (i = 0; i < 6; i++)
		state.args[i] = ctx->args[i];
	bpf_map_update_elem(&inflight, &pid_tgid, &state, BPF_ANY);

	event = reserve_event(cfg, cpu_stats, CPUWATCH_EVENT_SYSCALL_ENTER);
	if (!event)
		return 0;
	event->data.syscall_enter.syscall_nr = syscall_nr;
#pragma unroll
	for (i = 0; i < 6; i++)
		event->data.syscall_enter.args[i] = state.args[i];
	bpf_ringbuf_submit(event, 0);
	return 0;
}

SEC("tracepoint/raw_syscalls/sys_exit")
int handle_sys_exit(struct trace_event_raw_sys_exit *ctx)
{
	struct cpuwatch_inflight_syscall *state;
	struct cpuwatch_cpu_stats *cpu_stats;
	struct cpuwatch_config *cfg;
	struct cpuwatch_event *event;
	__u64 pid_tgid = bpf_get_current_pid_tgid();
	__u32 tgid = pid_tgid >> 32;
	__u64 now = bpf_ktime_get_ns();
	__u64 duration = 0;

	bpf_map_delete_elem(&target_inflight, &pid_tgid);
	cfg = get_config();
	if (!syscall_selected(cfg, tgid, ctx->id)) {
		bpf_map_delete_elem(&inflight, &pid_tgid);
		return 0;
	}

	cpu_stats = get_stats();
	if (cpu_stats)
		cpu_stats->syscall_exit++;
	state = bpf_map_lookup_elem(&inflight, &pid_tgid);
	if (state && now >= state->timestamp_ns)
		duration = now - state->timestamp_ns;

	event = reserve_event(cfg, cpu_stats, CPUWATCH_EVENT_SYSCALL_EXIT);
	if (event) {
		event->data.syscall_exit.syscall_nr = ctx->id;
		event->data.syscall_exit.return_value = ctx->ret;
		event->data.syscall_exit.duration_ns = duration;
		if (state) {
			event->data.syscall_exit.args_valid = 1;
#pragma unroll
			for (int i = 0; i < 6; i++)
				event->data.syscall_exit.args[i] = state->args[i];
		}
		bpf_ringbuf_submit(event, 0);
	}
	bpf_map_delete_elem(&inflight, &pid_tgid);
	return 0;
}

SEC("tracepoint/sched/sched_switch")
int handle_sched_switch(struct trace_event_raw_sched_switch *ctx)
{
	struct cpuwatch_cpu_stats *cpu_stats = get_stats();
	struct cpuwatch_config *cfg = get_config();
	struct cpuwatch_event *event;

	if (cpu_stats)
		cpu_stats->context_switches++;
	event = reserve_event(cfg, cpu_stats, CPUWATCH_EVENT_SCHED_SWITCH);
	if (!event)
		return 0;
	event->data.sched_switch.prev_pid = ctx->prev_pid;
	event->data.sched_switch.next_pid = ctx->next_pid;
	bpf_probe_read_kernel_str(event->data.sched_switch.prev_comm,
				  sizeof(event->data.sched_switch.prev_comm),
				  ctx->prev_comm);
	bpf_probe_read_kernel_str(event->data.sched_switch.next_comm,
				  sizeof(event->data.sched_switch.next_comm),
				  ctx->next_comm);
	bpf_ringbuf_submit(event, 0);
	return 0;
}

SEC("kprobe/handle_mm_fault")
int BPF_KPROBE(handle_page_fault, struct vm_area_struct *vma,
	       unsigned long address, unsigned int flags)
{
	struct cpuwatch_cpu_stats *cpu_stats;
	struct cpuwatch_config *cfg = get_config();
	struct cpuwatch_event *event;
	__u32 tgid = bpf_get_current_pid_tgid() >> 32;

	if (!cfg || !cfg->pagefault_enabled || !process_selected(cfg, tgid))
		return 0;
	cpu_stats = get_stats();
	if (cpu_stats)
		cpu_stats->page_faults++;
	event = reserve_event(cfg, cpu_stats, CPUWATCH_EVENT_PAGE_FAULT);
	if (!event)
		return 0;
	event->data.page_fault.address = address;
	bpf_ringbuf_submit(event, 0);
	return 0;
}

static __always_inline int emit_open_entry(int dfd, const char *filename,
					    struct open_how *how, __u32 type)
{
	struct cpuwatch_config *cfg = get_config();
	struct cpuwatch_cpu_stats *cpu_stats;
	struct cpuwatch_inflight_syscall *state;
	struct cpuwatch_event *event;
	__u64 pid_tgid = bpf_get_current_pid_tgid();
	__u32 tgid = pid_tgid >> 32;

	if (!cfg || !process_selected(cfg, tgid))
		return 0;
	state = bpf_map_lookup_elem(&inflight, &pid_tgid);
	if ((cfg->filter_flags & CPUWATCH_FILTER_SYSCALL) &&
	    (!state || !syscall_selected(cfg, tgid, state->syscall_nr)))
		return 0;
	cpu_stats = get_stats();
	event = reserve_event(cfg, cpu_stats, type);
	if (!event)
		return 0;
	event->data.open.dfd = dfd;
	event->data.open.source = type;
	if (how) {
		event->data.open.flags = BPF_CORE_READ(how, flags);
		event->data.open.mode = BPF_CORE_READ(how, mode);
	}
	if (filename)
		bpf_probe_read_user_str(event->data.open.filename,
					sizeof(event->data.open.filename), filename);
	bpf_ringbuf_submit(event, 0);
	return 0;
}

static __always_inline int emit_open_exit(long ret, __u32 type)
{
	struct cpuwatch_inflight_syscall *state;
	struct cpuwatch_config *cfg = get_config();
	struct cpuwatch_cpu_stats *cpu_stats;
	struct cpuwatch_event *event;
	__u64 pid_tgid = bpf_get_current_pid_tgid();
	__u32 tgid = pid_tgid >> 32;
	__u64 now = bpf_ktime_get_ns();

	if (!cfg || !process_selected(cfg, tgid))
		return 0;
	state = bpf_map_lookup_elem(&inflight, &pid_tgid);
	if ((cfg->filter_flags & CPUWATCH_FILTER_SYSCALL) &&
	    (!state || !syscall_selected(cfg, tgid, state->syscall_nr)))
		return 0;
	cpu_stats = get_stats();
	event = reserve_event(cfg, cpu_stats, type);
	if (!event)
		return 0;
	event->data.open.source = type;
	event->data.open.return_value = ret;
	if (state && now >= state->timestamp_ns)
		event->data.open.duration_ns = now - state->timestamp_ns;
	bpf_ringbuf_submit(event, 0);
	return 0;
}

SEC("fentry/do_sys_openat2")
int BPF_PROG(fentry_do_sys_openat2, int dfd, const char *filename,
	     struct open_how *how)
{
	struct cpuwatch_config *cfg = get_config();

	if (!cfg || !cfg->fentry_enabled)
		return 0;
	return emit_open_entry(dfd, filename, how, CPUWATCH_EVENT_FENTRY_OPEN);
}

SEC("fexit/do_sys_openat2")
int BPF_PROG(fexit_do_sys_openat2, int dfd, const char *filename,
	     struct open_how *how, long ret)
{
	struct cpuwatch_config *cfg = get_config();

	if (!cfg || !cfg->fentry_enabled)
		return 0;
	return emit_open_exit(ret, CPUWATCH_EVENT_FEXIT_OPEN);
}

SEC("kprobe/do_sys_openat2")
int BPF_KPROBE(kprobe_do_sys_openat2, int dfd, const char *filename,
	       struct open_how *how)
{
	struct cpuwatch_config *cfg = get_config();

	if (!cfg || !cfg->kprobe_open_enabled)
		return 0;
	return emit_open_entry(dfd, filename, how, CPUWATCH_EVENT_KPROBE_OPEN);
}

SEC("kretprobe/do_sys_openat2")
int BPF_KRETPROBE(kretprobe_do_sys_openat2, long ret)
{
	struct cpuwatch_config *cfg = get_config();

	if (!cfg || !cfg->kprobe_open_enabled)
		return 0;
	return emit_open_exit(ret, CPUWATCH_EVENT_KRETPROBE_OPEN);
}

SEC("lsm/file_open")
int BPF_PROG(enforce_file_open, struct file *file, int ret)
{
	struct cpuwatch_rate_state initial = {0};
	struct cpuwatch_rate_state *state;
	struct cpuwatch_rate_key key;
	struct cpuwatch_cpu_stats *cpu_stats;
	struct cpuwatch_config *cfg = get_config();
	struct cpuwatch_event *event;
	__u64 pid_tgid = bpf_get_current_pid_tgid();
	__s64 *syscall_nr;
	__u64 now;

	if (ret)
		return ret;
	if (!cfg || !cfg->rate_limit_enabled || !cfg->rate_limit_threshold)
		return 0;

	syscall_nr = bpf_map_lookup_elem(&target_inflight, &pid_tgid);
	if (!syscall_nr || *syscall_nr != cfg->rate_limit_syscall)
		return 0;
	bpf_map_delete_elem(&target_inflight, &pid_tgid);

	key.tgid = pid_tgid >> 32;
	if (key.tgid != cfg->rate_limit_tgid)
		return 0;
	now = bpf_ktime_get_ns();
	state = bpf_map_lookup_elem(&rate_states, &key);
	if (!state) {
		initial.window_start_ns = now;
		initial.count = 1;
		bpf_map_update_elem(&rate_states, &key, &initial, BPF_NOEXIST);
		state = bpf_map_lookup_elem(&rate_states, &key);
		if (!state)
			return 0;
	} else if (now - state->window_start_ns >= CPUWATCH_NSEC_PER_SEC) {
		state->window_start_ns = now;
		state->count = 1;
		state->denied = 0;
	} else {
		state->count++;
	}

	if (state->count <= cfg->rate_limit_threshold)
		return 0;
	state->denied++;
	cpu_stats = get_stats();
	if (cpu_stats)
		cpu_stats->rate_limited++;
	event = reserve_event(cfg, cpu_stats, CPUWATCH_EVENT_RATE_LIMIT);
	if (event) {
		event->data.rate_limit.target_syscall = cfg->rate_limit_syscall;
		event->data.rate_limit.current_count = state->count;
		event->data.rate_limit.threshold = cfg->rate_limit_threshold;
		event->data.rate_limit.error_code = -CPUWATCH_EAGAIN;
		bpf_ringbuf_submit(event, 0);
	}
	return -CPUWATCH_EAGAIN;
}

char LICENSE[] SEC("license") = "GPL";
