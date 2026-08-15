#define _GNU_SOURCE

#include <errno.h>
#include <getopt.h>
#include <linux/btf.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include <bpf/bpf.h>
#include <bpf/btf.h>
#include <bpf/libbpf.h>

#include "common.h"
#include "cpuwatch.skel.h"
#include "events.h"
#include "ui.h"

struct app_options {
	unsigned int interval_ms;
	double duration_sec;
	bool events;
	bool json;
	bool pagefault;
	bool fentry;
	bool kprobe_open;
	bool have_pid;
	bool have_syscall;
	bool have_rate_pid;
	bool have_rate_limit;
	__u32 pid;
	__s32 syscall_nr;
	__u32 rate_pid;
	__u32 rate_limit;
};

struct app_context {
	bool json;
};

static volatile sig_atomic_t exiting;

static void handle_signal(int signo)
{
	(void)signo;
	exiting = 1;
}

static void usage(FILE *stream, const char *program)
{
	fprintf(stream,
		"Usage: %s [OPTIONS]\n"
		"\n"
		"  --interval MS              refresh interval (default: 1000)\n"
		"  --events                  log every ring-buffer event (default)\n"
		"  --no-events               disable logs for benchmark/diagnostics only\n"
		"  --pid TGID                filter syscall/page-fault activity\n"
		"  --syscall NR|NAME         filter syscall (getpid/openat/openat2)\n"
		"  --pagefault               request handle_mm_fault tracing\n"
		"  --fentry                  inspect do_sys_openat2 with fentry/fexit\n"
		"  --kprobe-open             inspect do_sys_openat2 with kprobe/kretprobe\n"
		"  --rate-limit-pid TGID     process to enforce\n"
		"  --rate-limit COUNT        openat quota per CPU per second\n"
		"  --json                    emit event and statistics JSON Lines\n"
		"  --duration SEC            exit after the requested duration\n"
		"  --help                    show this help\n",
		program);
}

static int parse_u32(const char *text, __u32 *value)
{
	char *end = NULL;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(text, &end, 10);
	if (errno || !text[0] || !end || *end || parsed > UINT32_MAX)
		return -EINVAL;
	*value = (__u32)parsed;
	return 0;
}

static int parse_syscall(const char *text, __s32 *nr)
{
	__u32 numeric;

	if (!strcmp(text, "getpid"))
		*nr = SYS_getpid;
	else if (!strcmp(text, "openat"))
		*nr = SYS_openat;
#ifdef SYS_openat2
	else if (!strcmp(text, "openat2"))
		*nr = SYS_openat2;
#endif
	else if (!parse_u32(text, &numeric) && numeric <= INT32_MAX)
		*nr = (__s32)numeric;
	else
		return -EINVAL;
	return 0;
}

static int parse_options(int argc, char **argv, struct app_options *opts)
{
	enum {
		OPT_NO_EVENTS = 1000,
		OPT_PAGEFAULT,
		OPT_FENTRY,
		OPT_KPROBE_OPEN,
		OPT_RATE_PID,
		OPT_RATE_LIMIT,
		OPT_JSON,
		OPT_DURATION,
	};
	static const struct option long_options[] = {
		{ "interval", required_argument, NULL, 'i' },
		{ "events", no_argument, NULL, 'e' },
		{ "no-events", no_argument, NULL, OPT_NO_EVENTS },
		{ "pid", required_argument, NULL, 'p' },
		{ "syscall", required_argument, NULL, 's' },
		{ "pagefault", no_argument, NULL, OPT_PAGEFAULT },
		{ "fentry", no_argument, NULL, OPT_FENTRY },
		{ "kprobe-open", no_argument, NULL, OPT_KPROBE_OPEN },
		{ "rate-limit-pid", required_argument, NULL, OPT_RATE_PID },
		{ "rate-limit", required_argument, NULL, OPT_RATE_LIMIT },
		{ "json", no_argument, NULL, OPT_JSON },
		{ "duration", required_argument, NULL, OPT_DURATION },
		{ "help", no_argument, NULL, 'h' },
		{0},
	};
	int option;

	memset(opts, 0, sizeof(*opts));
	opts->interval_ms = 1000;
	opts->events = true;
	opts->syscall_nr = -1;
	while ((option = getopt_long(argc, argv, "i:ep:s:h", long_options, NULL)) != -1) {
		switch (option) {
		case 'i':
			if (parse_u32(optarg, &opts->interval_ms) ||
			    opts->interval_ms < 50 || opts->interval_ms > 60000) {
				fprintf(stderr, "invalid interval: %s (expected 50..60000 ms)\n", optarg);
				return -EINVAL;
			}
			break;
		case 'e':
			opts->events = true;
			break;
		case OPT_NO_EVENTS:
			opts->events = false;
			break;
		case 'p':
			if (parse_u32(optarg, &opts->pid) || !opts->pid)
				return fprintf(stderr, "invalid TGID: %s\n", optarg), -EINVAL;
			opts->have_pid = true;
			break;
		case 's':
			if (parse_syscall(optarg, &opts->syscall_nr))
				return fprintf(stderr, "invalid syscall: %s\n", optarg), -EINVAL;
			opts->have_syscall = true;
			break;
		case OPT_PAGEFAULT:
			opts->pagefault = true;
			break;
		case OPT_FENTRY:
			opts->fentry = true;
			break;
		case OPT_KPROBE_OPEN:
			opts->kprobe_open = true;
			break;
		case OPT_RATE_PID:
			if (parse_u32(optarg, &opts->rate_pid) || !opts->rate_pid)
				return fprintf(stderr, "invalid rate-limit TGID: %s\n", optarg), -EINVAL;
			opts->have_rate_pid = true;
			break;
		case OPT_RATE_LIMIT:
			if (parse_u32(optarg, &opts->rate_limit) || !opts->rate_limit)
				return fprintf(stderr, "invalid rate limit: %s\n", optarg), -EINVAL;
			opts->have_rate_limit = true;
			break;
		case OPT_JSON:
			opts->json = true;
			break;
		case OPT_DURATION: {
			char *end = NULL;

			errno = 0;
			opts->duration_sec = strtod(optarg, &end);
			if (errno || !end || *end || opts->duration_sec <= 0.0)
				return fprintf(stderr, "invalid duration: %s\n", optarg), -EINVAL;
			break;
		}
		case 'h':
			usage(stdout, argv[0]);
			exit(EXIT_SUCCESS);
		default:
			usage(stderr, argv[0]);
			return -EINVAL;
		}
	}
	if (optind != argc)
		return fprintf(stderr, "unexpected argument: %s\n", argv[optind]), -EINVAL;
	if (opts->fentry && opts->kprobe_open)
		return fprintf(stderr, "--fentry and --kprobe-open are mutually exclusive\n"), -EINVAL;
	if (opts->have_rate_pid != opts->have_rate_limit)
		return fprintf(stderr, "--rate-limit-pid and --rate-limit must be used together\n"), -EINVAL;
	return 0;
}

static bool kernel_symbol_exists(const char *wanted)
{
	FILE *file;
	char line[512];
	char address[32], type, name[256];
	bool found = false;

	file = fopen("/proc/kallsyms", "re");
	if (!file)
		return false;
	while (fgets(line, sizeof(line), file)) {
		if (sscanf(line, "%31s %c %255s", address, &type, name) != 3)
			continue;
		if (!strcmp(name, wanted)) {
			found = true;
			break;
		}
	}
	fclose(file);
	return found;
}

static bool btf_function_exists(const char *name)
{
	struct btf *btf;
	long error;
	int id;

	btf = btf__load_vmlinux_btf();
	if (!btf)
		return false;
	error = libbpf_get_error(btf);
	if (error)
		return false;
	id = btf__find_by_name_kind(btf, name, BTF_KIND_FUNC);
	btf__free(btf);
	return id >= 0;
}

static bool bpf_lsm_enabled(void)
{
	FILE *file;
	char buffer[512] = {0};

	file = fopen("/sys/kernel/security/lsm", "re");
	if (!file)
		return false;
	if (!fgets(buffer, sizeof(buffer), file)) {
		fclose(file);
		return false;
	}
	fclose(file);
	for (char *token = strtok(buffer, ",\n"); token; token = strtok(NULL, ",\n"))
		if (!strcmp(token, "bpf"))
			return true;
	return false;
}

static int attach_one(struct bpf_program *program, struct bpf_link **link, const char *name, bool required)
{
	long error;

	*link = bpf_program__attach(program);
	if (!*link) {
		error = errno ? -errno : -EINVAL;
	} else {
		error = libbpf_get_error(*link);
	}
	if (!error)
		return 0;
	*link = NULL;
	fprintf(stderr, "%s unable to attach %s: %s\n",
		required ? "[ERROR]" : "[WARN]", name, strerror((int)-error));
	return (int)error;
}

static int 	configure_autoload(struct cpuwatch_bpf *skel, struct app_options *opts)
{
	bool have_fault = kernel_symbol_exists("handle_mm_fault");
	bool have_open_symbol = kernel_symbol_exists("do_sys_openat2");
	bool have_open_btf = btf_function_exists("do_sys_openat2");
	bool have_lsm = bpf_lsm_enabled();

	if (opts->pagefault && !have_fault) {
		fprintf(stderr, "[WARN] handle_mm_fault unavailable; page-fault tracing disabled\n");
		opts->pagefault = false;
	}
	if (opts->fentry && !have_open_btf) {
		fprintf(stderr, "[WARN] do_sys_openat2 absent from BTF; fentry/fexit disabled\n");
		opts->fentry = false;
	}
	if (opts->kprobe_open && !have_open_symbol) {
		fprintf(stderr, "[WARN] do_sys_openat2 symbol unavailable; kprobe pair disabled\n");
		opts->kprobe_open = false;
	}
	if (opts->have_rate_limit && !have_lsm) {
		fprintf(stderr, "[ERROR] BPF is not active in /sys/kernel/security/lsm; refusing false enforcement\n");
		return -EOPNOTSUPP;
	}

	bpf_program__set_autoload(skel->progs.handle_page_fault, opts->pagefault);
	bpf_program__set_autoload(skel->progs.fentry_do_sys_openat2, opts->fentry);
	bpf_program__set_autoload(skel->progs.fexit_do_sys_openat2, opts->fentry);
	bpf_program__set_autoload(skel->progs.kprobe_do_sys_openat2, opts->kprobe_open);
	bpf_program__set_autoload(skel->progs.kretprobe_do_sys_openat2, opts->kprobe_open);
	bpf_program__set_autoload(skel->progs.enforce_file_open, opts->have_rate_limit);
	return 0;
}

static int attach_programs(struct cpuwatch_bpf *skel, struct app_options *opts)
{
	int error;

	error = attach_one(skel->progs.handle_sys_enter, &skel->links.handle_sys_enter,
			   "raw_syscalls/sys_enter", true);
	if (error)
		return error;
	error = attach_one(skel->progs.handle_sys_exit, &skel->links.handle_sys_exit,
			   "raw_syscalls/sys_exit", true);
	if (error)
		return error;
	error = attach_one(skel->progs.handle_sched_switch, &skel->links.handle_sched_switch,
			   "sched/sched_switch", true);
	if (error)
		return error;

	if (opts->pagefault && attach_one(skel->progs.handle_page_fault,
		&skel->links.handle_page_fault, "handle_mm_fault", false))
		opts->pagefault = false;
	if (opts->fentry) {
		error = attach_one(skel->progs.fentry_do_sys_openat2,
				   &skel->links.fentry_do_sys_openat2, "fentry/do_sys_openat2", false);
		error |= attach_one(skel->progs.fexit_do_sys_openat2,
				    &skel->links.fexit_do_sys_openat2, "fexit/do_sys_openat2", false);
		if (error) {
			bpf_link__destroy(skel->links.fentry_do_sys_openat2);
			bpf_link__destroy(skel->links.fexit_do_sys_openat2);
			skel->links.fentry_do_sys_openat2 = NULL;
			skel->links.fexit_do_sys_openat2 = NULL;
			opts->fentry = false;
		}
	}
	if (opts->kprobe_open) {
		error = attach_one(skel->progs.kprobe_do_sys_openat2,
				   &skel->links.kprobe_do_sys_openat2, "kprobe/do_sys_openat2", false);
		error |= attach_one(skel->progs.kretprobe_do_sys_openat2,
				    &skel->links.kretprobe_do_sys_openat2, "kretprobe/do_sys_openat2", false);
		if (error) {
			bpf_link__destroy(skel->links.kprobe_do_sys_openat2);
			bpf_link__destroy(skel->links.kretprobe_do_sys_openat2);
			skel->links.kprobe_do_sys_openat2 = NULL;
			skel->links.kretprobe_do_sys_openat2 = NULL;
			opts->kprobe_open = false;
		}
	}
	if (opts->have_rate_limit) {
		error = attach_one(skel->progs.enforce_file_open,
				   &skel->links.enforce_file_open, "lsm/file_open", true);
		if (error)
			return error;
	}
	return 0;
}

static int update_config(struct cpuwatch_bpf *skel, const struct app_options *opts)
{
	struct cpuwatch_config cfg = {
		.emit_events = opts->events,
		.filter_flags = (opts->have_pid ? CPUWATCH_FILTER_TGID : 0) | (opts->have_syscall ? CPUWATCH_FILTER_SYSCALL : 0),
		.monitor_tgid = opts->pid,
		.monitor_syscall = opts->syscall_nr,
		.pagefault_enabled = opts->pagefault,
		.fentry_enabled = opts->fentry,
		.kprobe_open_enabled = opts->kprobe_open,
		.rate_limit_enabled = opts->have_rate_limit,
		.rate_limit_tgid = opts->rate_pid,
		.rate_limit_threshold = opts->rate_limit,
		.rate_limit_syscall = SYS_openat,
	};
	__u32 key = CPUWATCH_CONFIG_KEY;
	int fd = bpf_map__fd(skel->maps.runtime_cfg);

	if (bpf_map_update_elem(fd, &key, &cfg, BPF_ANY)) {
		fprintf(stderr, "[ERROR] unable to configure BPF maps: %s\n", strerror(errno));
		return -errno;
	}
	return 0;
}

static const char *event_name(__u32 type)
{
	switch (type) {
	case CPUWATCH_EVENT_SYSCALL_ENTER: return "sys_enter";
	case CPUWATCH_EVENT_SYSCALL_EXIT: return "sys_exit";
	case CPUWATCH_EVENT_SCHED_SWITCH: return "sched_switch";
	case CPUWATCH_EVENT_PAGE_FAULT: return "page_fault";
	case CPUWATCH_EVENT_FENTRY_OPEN: return "fentry_open";
	case CPUWATCH_EVENT_FEXIT_OPEN: return "fexit_open";
	case CPUWATCH_EVENT_KPROBE_OPEN: return "kprobe_open";
	case CPUWATCH_EVENT_KRETPROBE_OPEN: return "kretprobe_open";
	case CPUWATCH_EVENT_RATE_LIMIT: return "rate_limit";
	default: return "unknown";
	}
}

static void print_args(const __u64 args[6])
{
	printf("[0x%llx,0x%llx,0x%llx,0x%llx,0x%llx,0x%llx]",
	       (unsigned long long)args[0], (unsigned long long)args[1],
	       (unsigned long long)args[2], (unsigned long long)args[3],
	       (unsigned long long)args[4], (unsigned long long)args[5]);
}

static void print_json_string(const char *value, size_t maximum)
{
	putchar('"');
	for (size_t index = 0; index < maximum && value[index]; index++) {
		unsigned char character = (unsigned char)value[index];

		if (character == '"' || character == '\\')
			printf("\\%c", character);
		else if (character < 0x20)
			printf("\\u%04x", (unsigned int)character);
		else
			putchar(character);
	}
	putchar('"');
}

static void print_plain_string(const char *value, size_t maximum)
{
	putchar('"');
	for (size_t index = 0; index < maximum && value[index]; index++) {
		unsigned char character = (unsigned char)value[index];

		if (character == '"' || character == '\\')
			printf("\\%c", character);
		else if (character < 0x20 || character == 0x7f)
			printf("\\x%02x", (unsigned int)character);
		else
			putchar(character);
	}
	putchar('"');
}

static void emit_event_plain(const struct cpuwatch_event *event)
{
	printf("EVENT timestamp_ns=%llu type=%s pid=%u tid=%u comm=",
	       (unsigned long long)event->header.timestamp_ns,
	       event_name(event->header.type), event->header.tgid,
	       event->header.pid);
	print_plain_string(event->header.comm, CPUWATCH_TASK_COMM_LEN);
	printf(" cpu=%u", event->header.cpu);
	switch (event->header.type) {
	case CPUWATCH_EVENT_SYSCALL_ENTER:
		printf(" syscall_nr=%lld args=",
		       (long long)event->data.syscall_enter.syscall_nr);
		print_args(event->data.syscall_enter.args);
		break;
	case CPUWATCH_EVENT_SYSCALL_EXIT:
		printf(" syscall_nr=%lld args=",
		       (long long)event->data.syscall_exit.syscall_nr);
		if (event->data.syscall_exit.args_valid)
			print_args(event->data.syscall_exit.args);
		else
			printf("unavailable");
		printf(" return=%lld duration_ns=%llu",
		       (long long)event->data.syscall_exit.return_value,
		       (unsigned long long)event->data.syscall_exit.duration_ns);
		break;
	case CPUWATCH_EVENT_SCHED_SWITCH:
		printf(" prev_pid=%u prev_comm=", event->data.sched_switch.prev_pid);
		print_plain_string(event->data.sched_switch.prev_comm,
				   CPUWATCH_TASK_COMM_LEN);
		printf(" next_pid=%u next_comm=", event->data.sched_switch.next_pid);
		print_plain_string(event->data.sched_switch.next_comm,
				   CPUWATCH_TASK_COMM_LEN);
		break;
	case CPUWATCH_EVENT_PAGE_FAULT:
		printf(" address=0x%llx",
		       (unsigned long long)event->data.page_fault.address);
		break;
	case CPUWATCH_EVENT_FENTRY_OPEN:
	case CPUWATCH_EVENT_KPROBE_OPEN:
		printf(" dfd=%d flags=0x%llx mode=0%llo path=",
		       event->data.open.dfd,
		       (unsigned long long)event->data.open.flags,
		       (unsigned long long)event->data.open.mode);
		print_plain_string(event->data.open.filename, CPUWATCH_PATH_LEN);
		break;
	case CPUWATCH_EVENT_FEXIT_OPEN:
	case CPUWATCH_EVENT_KRETPROBE_OPEN:
		printf(" return=%lld duration_ns=%llu",
		       (long long)event->data.open.return_value,
		       (unsigned long long)event->data.open.duration_ns);
		break;
	case CPUWATCH_EVENT_RATE_LIMIT:
		printf(" syscall_nr=%u count=%u threshold=%u error=%d",
		       event->data.rate_limit.target_syscall,
		       event->data.rate_limit.current_count,
		       event->data.rate_limit.threshold,
		       event->data.rate_limit.error_code);
		break;
	default:
		break;
	}
	putchar('\n');
}

static void emit_event_json(const struct cpuwatch_event *event)
{
	printf("{\"record\":\"event\",\"timestamp_ns\":%llu,\"type\":",
	       (unsigned long long)event->header.timestamp_ns);
	print_json_string(event_name(event->header.type), 32);
	printf(",\"pid\":%u,\"tid\":%u,\"comm\":",
	       event->header.tgid, event->header.pid);
	print_json_string(event->header.comm, CPUWATCH_TASK_COMM_LEN);
	printf(",\"cpu\":%u", event->header.cpu);
	switch (event->header.type) {
	case CPUWATCH_EVENT_SYSCALL_ENTER:
		printf(",\"syscall_nr\":%lld,\"args\":[%llu,%llu,%llu,%llu,%llu,%llu]",
		       (long long)event->data.syscall_enter.syscall_nr,
		       (unsigned long long)event->data.syscall_enter.args[0],
		       (unsigned long long)event->data.syscall_enter.args[1],
		       (unsigned long long)event->data.syscall_enter.args[2],
		       (unsigned long long)event->data.syscall_enter.args[3],
		       (unsigned long long)event->data.syscall_enter.args[4],
		       (unsigned long long)event->data.syscall_enter.args[5]);
		break;
	case CPUWATCH_EVENT_SYSCALL_EXIT:
		printf(",\"syscall_nr\":%lld,\"args\":",
		       (long long)event->data.syscall_exit.syscall_nr);
		if (event->data.syscall_exit.args_valid)
			printf("[%llu,%llu,%llu,%llu,%llu,%llu]",
			       (unsigned long long)event->data.syscall_exit.args[0],
			       (unsigned long long)event->data.syscall_exit.args[1],
			       (unsigned long long)event->data.syscall_exit.args[2],
			       (unsigned long long)event->data.syscall_exit.args[3],
			       (unsigned long long)event->data.syscall_exit.args[4],
			       (unsigned long long)event->data.syscall_exit.args[5]);
		else
			printf("null");
		printf(",\"return\":%lld,\"duration_ns\":%llu",
		       (long long)event->data.syscall_exit.return_value,
		       (unsigned long long)event->data.syscall_exit.duration_ns);
		break;
	case CPUWATCH_EVENT_SCHED_SWITCH:
		printf(",\"prev_pid\":%u,\"prev_comm\":",
		       event->data.sched_switch.prev_pid);
		print_json_string(event->data.sched_switch.prev_comm,
				  CPUWATCH_TASK_COMM_LEN);
		printf(",\"next_pid\":%u,\"next_comm\":",
		       event->data.sched_switch.next_pid);
		print_json_string(event->data.sched_switch.next_comm,
				  CPUWATCH_TASK_COMM_LEN);
		break;
	case CPUWATCH_EVENT_PAGE_FAULT:
		printf(",\"address\":%llu",
		       (unsigned long long)event->data.page_fault.address);
		break;
	case CPUWATCH_EVENT_FENTRY_OPEN:
	case CPUWATCH_EVENT_KPROBE_OPEN:
		printf(",\"dfd\":%d,\"flags\":%llu,\"mode\":%llu,\"path\":",
		       event->data.open.dfd,
		       (unsigned long long)event->data.open.flags,
		       (unsigned long long)event->data.open.mode);
		print_json_string(event->data.open.filename, CPUWATCH_PATH_LEN);
		break;
	case CPUWATCH_EVENT_FEXIT_OPEN:
	case CPUWATCH_EVENT_KRETPROBE_OPEN:
		printf(",\"return\":%lld,\"duration_ns\":%llu",
		       (long long)event->data.open.return_value,
		       (unsigned long long)event->data.open.duration_ns);
		break;
	case CPUWATCH_EVENT_RATE_LIMIT:
		printf(",\"syscall_nr\":%u,\"count\":%u,\"threshold\":%u,\"error\":%d",
		       event->data.rate_limit.target_syscall,
		       event->data.rate_limit.current_count,
		       event->data.rate_limit.threshold,
		       event->data.rate_limit.error_code);
		break;
	default:
		break;
	}
	puts("}");
}

static int consume_event(void *context, void *data, size_t size)
{
	const struct cpuwatch_event *event = data;
	const struct app_context *app = context;

	if (size < sizeof(*event))
		return 0;
	if (app->json)
		emit_event_json(event);
	else
		emit_event_plain(event);
	return 0;
}

static double elapsed_seconds(const struct timespec *older, const struct timespec *newer)
{
	return (double)(newer->tv_sec - older->tv_sec) +
	       (double)(newer->tv_nsec - older->tv_nsec) / 1000000000.0;
}

static __u64 counter_delta(__u64 current, __u64 previous)
{
	return current >= previous ? current - previous : current;
}

static int read_rates(int stats_fd, struct cpuwatch_cpu_stats *previous, struct cpuwatch_cpu_stats *current, struct cpuwatch_cpu_rate *rates, int nr_cpus, double seconds)
{
	__u32 key = CPUWATCH_STATS_KEY;

	if (bpf_map_lookup_elem(stats_fd, &key, current)) {
		fprintf(stderr, "[ERROR] unable to read per-CPU stats: %s\n", strerror(errno));
		return -errno;
	}
	for (int cpu = 0; cpu < nr_cpus; cpu++) {
		rates[cpu].syscall_delta = counter_delta(current[cpu].syscall_enter,
						      previous[cpu].syscall_enter);
		rates[cpu].context_switch_delta = counter_delta(current[cpu].context_switches,
							     previous[cpu].context_switches);
		rates[cpu].page_fault_delta = counter_delta(current[cpu].page_faults,
							previous[cpu].page_faults);
		rates[cpu].rate_limited_delta = counter_delta(current[cpu].rate_limited,
							  previous[cpu].rate_limited);
		rates[cpu].ringbuf_drop_delta = counter_delta(current[cpu].ringbuf_drops,
							 previous[cpu].ringbuf_drops);
		rates[cpu].syscalls = rates[cpu].syscall_delta / seconds;
		rates[cpu].context_switches = rates[cpu].context_switch_delta / seconds;
		rates[cpu].page_faults = rates[cpu].page_fault_delta / seconds;
		rates[cpu].rate_limited = rates[cpu].rate_limited_delta / seconds;
		rates[cpu].ringbuf_drops = rates[cpu].ringbuf_drop_delta / seconds;
	}
	memcpy(previous, current, (size_t)nr_cpus * sizeof(*previous));
	return 0;
}

static void emit_json(const struct cpuwatch_cpu_rate *rates, int nr_cpus,
		      double elapsed, double sample_seconds)
{
	struct cpuwatch_cpu_rate total = {0};

	printf("{\"record\":\"stats\",\"elapsed_s\":%.6f,\"sample_s\":%.6f,\"cpus\":[",
	       elapsed, sample_seconds);
	for (int cpu = 0; cpu < nr_cpus; cpu++) {
		if (cpu)
			putchar(',');
		printf("{\"cpu\":%d,\"syscalls_delta\":%llu,\"syscalls_per_s\":%.3f,\"context_switches_delta\":%llu,\"context_switches_per_s\":%.3f,"
		       "\"page_faults_delta\":%llu,\"page_faults_per_s\":%.3f,"
		       "\"limited_delta\":%llu,\"limited_per_s\":%.3f,"
		       "\"drops_delta\":%llu,\"drops_per_s\":%.3f}",
		       cpu, rates[cpu].syscall_delta, rates[cpu].syscalls,
		       rates[cpu].context_switch_delta, rates[cpu].context_switches,
		       rates[cpu].page_fault_delta, rates[cpu].page_faults,
		       rates[cpu].rate_limited_delta, rates[cpu].rate_limited,
		       rates[cpu].ringbuf_drop_delta, rates[cpu].ringbuf_drops);
		total.syscalls += rates[cpu].syscalls;
		total.syscall_delta += rates[cpu].syscall_delta;
		total.context_switches += rates[cpu].context_switches;
		total.context_switch_delta += rates[cpu].context_switch_delta;
		total.page_faults += rates[cpu].page_faults;
		total.page_fault_delta += rates[cpu].page_fault_delta;
		total.rate_limited += rates[cpu].rate_limited;
		total.rate_limited_delta += rates[cpu].rate_limited_delta;
		total.ringbuf_drops += rates[cpu].ringbuf_drops;
		total.ringbuf_drop_delta += rates[cpu].ringbuf_drop_delta;
	}
	printf("],\"total\":{\"syscalls_delta\":%llu,\"syscalls_per_s\":%.3f,"
	       "\"context_switches_delta\":%llu,\"context_switches_per_s\":%.3f,"
	       "\"page_faults_delta\":%llu,\"page_faults_per_s\":%.3f,"
	       "\"limited_delta\":%llu,\"limited_per_s\":%.3f,"
	       "\"drops_delta\":%llu,\"drops_per_s\":%.3f}}\n",
	       total.syscall_delta, total.syscalls,
	       total.context_switch_delta, total.context_switches,
	       total.page_fault_delta, total.page_faults,
	       total.rate_limited_delta, total.rate_limited,
	       total.ringbuf_drop_delta, total.ringbuf_drops);
	fflush(stdout);
}

static int sample_and_emit_stats(int stats_fd,
				 struct cpuwatch_cpu_stats *previous,
				 struct cpuwatch_cpu_stats *current,
				 struct cpuwatch_cpu_rate *rates, int nr_cpus,
				 const struct timespec *started,
				 struct timespec *sampled,
				 const struct timespec *now,
				 const struct app_options *opts,
				 const char *kernel_release)
{
	double sample_seconds = elapsed_seconds(sampled, now);

	if (sample_seconds <= 0.0)
		return 0;
	if (read_rates(stats_fd, previous, current, rates, nr_cpus, sample_seconds))
		return -1;
	if (opts->json)
		emit_json(rates, nr_cpus, elapsed_seconds(started, now), sample_seconds);
	else
		cpuwatch_ui_render(kernel_release, rates, nr_cpus, opts->interval_ms,
				   elapsed_seconds(started, now));
	*sampled = *now;
	return 0;
}

int main(int argc, char **argv)
{
	struct cpuwatch_cpu_stats *previous = NULL, *current = NULL;
	struct cpuwatch_cpu_rate *rates = NULL;
	struct cpuwatch_bpf *skel = NULL;
	struct ring_buffer *ring = NULL;
	struct app_context app = {0};
	struct app_options opts;
	struct timespec started, sampled, now;
	struct utsname uts = {0};
	bool terminal_sample_done = false;
	int nr_cpus, stats_fd, error = 0;

	_Static_assert(sizeof(struct cpuwatch_cpu_stats) % 8 == 0,
		       "per-CPU values must retain eight-byte alignment");
	_Static_assert(sizeof(struct cpuwatch_config) == 48,
		       "configuration ABI changed unexpectedly");
	_Static_assert(sizeof(struct cpuwatch_event_header) == 40,
		       "event header ABI changed unexpectedly");
	_Static_assert(sizeof(struct cpuwatch_event) == 208,
		       "event record ABI changed unexpectedly");
	if (parse_options(argc, argv, &opts))
		return EXIT_FAILURE;
	if (geteuid() != 0) {
		fprintf(stderr, "[ERROR] cpuwatch must run as root inside the test VM\n");
		return EXIT_FAILURE;
	}
	if (access("/sys/kernel/btf/vmlinux", R_OK)) {
		fprintf(stderr, "[ERROR] /sys/kernel/btf/vmlinux is unavailable\n");
		return EXIT_FAILURE;
	}

	// init libbpf (strict mode), then init skeleton ebpf 
	// skeleton ebpf is kinda like boilerplate so that itll easy to use
	libbpf_set_strict_mode(LIBBPF_STRICT_ALL);
	skel = cpuwatch_bpf__open();
	if (!skel) {
		fprintf(stderr, "[ERROR] unable to open BPF skeleton\n");
		return EXIT_FAILURE;
	}

	// check if the loaded hook exist/supported 
	if (configure_autoload(skel, &opts)) {
		error = -1;
		goto cleanup;
	}
	
	// check if ebpf is successfully loaded & verified in the kernel
	if (cpuwatch_bpf__load(skel)) {
		fprintf(stderr, "[ERROR] BPF load/verifier failed (inspect libbpf output above\n)");
		error = -1;
		goto cleanup;
	}

	// send configuration to kernel, it will be send to map afterward
	if (update_config(skel, &opts)) {
		error = -1;
		goto cleanup;
	}

	// attach hook to kernel
	if (attach_programs(skel, &opts)) {
		error = -1;
		goto cleanup;
	}
	// Optional attachment can be disabled after the initial config write.
	if (update_config(skel, &opts)) {
		error = -1;
		goto cleanup;
	}

	app.json = opts.json;
	ring = ring_buffer__new(bpf_map__fd(skel->maps.events), consume_event, &app, NULL);
	if (!ring) {
		fprintf(stderr, "[ERROR] unable to create ring-buffer consumer: %s\n", strerror(errno));
		error = -1;
		goto cleanup;
	}
	nr_cpus = libbpf_num_possible_cpus();
	if (nr_cpus <= 0) {
		fprintf(stderr, "[ERROR] unable to determine possible CPU count\n");
		error = -1;
		goto cleanup;
	}
	previous = calloc((size_t)nr_cpus, sizeof(*previous));
	current = calloc((size_t)nr_cpus, sizeof(*current));
	rates = calloc((size_t)nr_cpus, sizeof(*rates));
	if (!previous || !current || !rates) {
		fprintf(stderr, "[ERROR] out of memory\n");
		error = -1;
		goto cleanup;
	}
	stats_fd = bpf_map__fd(skel->maps.stats);
	clock_gettime(CLOCK_MONOTONIC, &started);
	sampled = started;
	uname(&uts);
	setvbuf(stdout, NULL, _IOLBF, 0);
	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);
	fprintf(stderr,
		"CPUWATCH_READY pid=%ld cpus=%d pagefault=%d fentry=%d kprobe_open=%d rate_limit=%d\n",
		(long)getpid(), nr_cpus, opts.pagefault, opts.fentry,
		opts.kprobe_open, opts.have_rate_limit);

	// read the stats per CPU
	while (!exiting) {
		error = ring_buffer__poll(ring, 100);
		if (error == -EINTR) {
			error = 0;
		}
		if (error < 0) {
			fprintf(stderr, "[ERROR] ring-buffer polling failed: %s\n", strerror(-error));
			break;
		}
		clock_gettime(CLOCK_MONOTONIC, &now);
		bool interval_due = elapsed_seconds(&sampled, &now) * 1000.0 >= opts.interval_ms;
		bool duration_due = opts.duration_sec > 0.0 && elapsed_seconds(&started, &now) >= opts.duration_sec;
		bool terminal_due = exiting || duration_due;

		if ((interval_due || terminal_due) &&
		    sample_and_emit_stats(stats_fd, previous, current, rates, nr_cpus, &started, &sampled, &now, &opts, uts.release)) {
			error = -1;
			break;
		}
		if (terminal_due) {
			terminal_sample_done = true;
			break;
		}
	}
	if (!terminal_sample_done) {
		int saved_error = error;

		clock_gettime(CLOCK_MONOTONIC, &now);
		if (sample_and_emit_stats(stats_fd, previous, current, rates, nr_cpus, &started, &sampled, &now, &opts, uts.release) && saved_error >= 0)
			error = -1;
		else
			error = saved_error;
	}

cleanup:
	free(rates);
	free(current);
	free(previous);
	ring_buffer__free(ring);
	cpuwatch_bpf__destroy(skel);
	return error < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
