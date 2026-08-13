#include "vmlinux.h"

#include <bpf/bpf_helpers.h>

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, __u64);
} one_value SEC(".maps");

SEC("tracepoint/raw_syscalls/sys_enter")
int verifier_bad(struct trace_event_raw_sys_enter *ctx)
{
	__u32 key = 0;
	__u64 *value = bpf_map_lookup_elem(&one_value, &key);

	if (!value)
		return 0;
	/* Intentionally outside the eight-byte map value. The verifier must reject it. */
	return (__s32)value[1];
}

char LICENSE[] SEC("license") = "GPL";

