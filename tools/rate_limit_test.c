#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static int pin_to_cpu(int cpu)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	if (sched_setaffinity(0, sizeof(set), &set)) {
		fprintf(stderr, "sched_setaffinity CPU%d: %s\n", cpu, strerror(errno));
		return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	static const struct option options[] = {
		{ "attempts", required_argument, NULL, 'n' },
		{ "cpu", required_argument, NULL, 'c' },
		{ "no-wait", no_argument, NULL, 'w' },
		{0},
	};
	unsigned int attempts = 200, successes = 0, rejected = 0, other_errors = 0;
	int cpu = -1, wait_for_input = 1, option;

	while ((option = getopt_long(argc, argv, "n:c:w", options, NULL)) != -1) {
		char *end = NULL;
		long value;

		switch (option) {
		case 'n':
			errno = 0;
			value = strtol(optarg, &end, 10);
			if (errno || !end || *end || value <= 0 || value > 10000000)
				return fprintf(stderr, "invalid attempts: %s\n", optarg), EXIT_FAILURE;
			attempts = (unsigned int)value;
			break;
		case 'c':
			errno = 0;
			value = strtol(optarg, &end, 10);
			if (errno || !end || *end || value < 0 || value >= CPU_SETSIZE)
				return fprintf(stderr, "invalid CPU: %s\n", optarg), EXIT_FAILURE;
			cpu = (int)value;
			break;
		case 'w': wait_for_input = 0; break;
		default: return EXIT_FAILURE;
		}
	}
	if (cpu >= 0 && pin_to_cpu(cpu))
		return EXIT_FAILURE;
	printf("RATE_READY pid=%ld attempts=%u cpu=%d\n", (long)getpid(), attempts, cpu);
	if (wait_for_input) {
		printf("Start cpuwatch with --rate-limit-pid %ld --rate-limit COUNT, then press ENTER...\n",
		       (long)getpid());
		fflush(stdout);
		(void)getchar();
	}
	for (unsigned int i = 0; i < attempts; i++) {
		long fd = syscall(SYS_openat, AT_FDCWD, "/dev/null", O_RDONLY | O_CLOEXEC, 0);

		if (fd >= 0) {
			successes++;
			close((int)fd);
		} else if (errno == EAGAIN) {
			rejected++;
		} else {
			other_errors++;
		}
	}
	printf("RATE_DONE attempts=%u success=%u rejected=%u other_errors=%u\n",
	       attempts, successes, rejected, other_errors);
	return other_errors ? EXIT_FAILURE : EXIT_SUCCESS;
}
