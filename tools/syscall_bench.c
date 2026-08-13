#define _GNU_SOURCE

#include <errno.h>
#include <getopt.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static uint64_t monotonic_ns(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC_RAW, &now)) {
		perror("clock_gettime");
		exit(EXIT_FAILURE);
	}
	return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static unsigned long long parse_count(const char *text, const char *name)
{
	char *end = NULL;
	unsigned long long value;

	errno = 0;
	value = strtoull(text, &end, 10);
	if (errno || !text[0] || !end || *end || !value) {
		fprintf(stderr, "invalid %s: %s\n", name, text);
		exit(EXIT_FAILURE);
	}
	return value;
}

int main(int argc, char **argv)
{
	static const struct option options[] = {
		{ "count", required_argument, NULL, 'c' },
		{ "iterations", required_argument, NULL, 'n' },
		{ "mode", required_argument, NULL, 'm' },
		{ "syscall", required_argument, NULL, 's' },
		{ "header", no_argument, NULL, 'H' },
		{0},
	};
	unsigned long long calls = 10000000ULL, iterations = 5;
	const char *mode = "unspecified";
	const char *syscall_name = "getpid";
	volatile long sink = 0;
	int option;
	int header = 0;

	while ((option = getopt_long(argc, argv, "c:n:m:s:H", options, NULL)) != -1) {
		switch (option) {
		case 'c': calls = parse_count(optarg, "call count"); break;
		case 'n': iterations = parse_count(optarg, "iteration count"); break;
		case 'm': mode = optarg; break;
		case 's': syscall_name = optarg; break;
		case 'H': header = 1; break;
		default: return EXIT_FAILURE;
		}
	}
	if (header)
		puts("mode,iteration,duration_ns,calls");
	if (strcmp(syscall_name, "getpid") && strcmp(syscall_name, "openat")) {
		fprintf(stderr, "unsupported syscall: %s\n", syscall_name);
		return EXIT_FAILURE;
	}
	for (unsigned long long iteration = 1; iteration <= iterations; iteration++) {
		uint64_t start = monotonic_ns();

		for (unsigned long long call = 0; call < calls; call++) {
			if (!strcmp(syscall_name, "getpid")) {
				sink ^= syscall(SYS_getpid);
			} else {
				long fd = syscall(SYS_openat, AT_FDCWD, "/dev/null",
						  O_RDONLY | O_CLOEXEC, 0);
				if (fd < 0) {
					perror("openat");
					return EXIT_FAILURE;
				}
				sink ^= fd;
				close((int)fd);
			}
		}
		uint64_t stop = monotonic_ns();
		printf("%s,%llu,%llu,%llu\n", mode, iteration,
		       (unsigned long long)(stop - start), calls);
		fflush(stdout);
	}
	return sink == -1 ? EXIT_FAILURE : EXIT_SUCCESS;
}
