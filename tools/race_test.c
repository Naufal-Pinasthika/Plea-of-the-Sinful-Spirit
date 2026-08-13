#define _GNU_SOURCE

#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

struct worker_args {
	int cpu;
	unsigned long long calls;
	int error;
};

static void *run_worker(void *opaque)
{
	struct worker_args *args = opaque;
	cpu_set_t set;
	volatile long sink = 0;
	int error;

	CPU_ZERO(&set);
	CPU_SET(args->cpu, &set);
	error = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
	if (error) {
		args->error = error;
		return NULL;
	}
	for (unsigned long long i = 0; i < args->calls; i++)
		sink ^= syscall(SYS_getpid);
	if (sink == -1)
		args->error = EIO;
	return NULL;
}

static unsigned long long parse_calls(const char *text)
{
	char *end = NULL;
	unsigned long long value;

	errno = 0;
	value = strtoull(text, &end, 10);
	if (errno || !text[0] || !end || *end || !value) {
		fprintf(stderr, "invalid call count: %s\n", text);
		exit(EXIT_FAILURE);
	}
	return value;
}

int main(int argc, char **argv)
{
	static const struct option options[] = {
		{ "calls", required_argument, NULL, 'c' },
		{ "no-wait", no_argument, NULL, 'n' },
		{0},
	};
	unsigned long long calls = 1000000ULL;
	struct worker_args *args = NULL;
	pthread_t *threads = NULL;
	cpu_set_t allowed;
	sigset_t wait_set;
	int *cpu_ids = NULL;
	int wait_for_signal = 1;
	int cpu_count = 0, created = 0, option, signal_number, result = EXIT_FAILURE;

	while ((option = getopt_long(argc, argv, "c:n", options, NULL)) != -1) {
		if (option == 'c')
			calls = parse_calls(optarg);
		else if (option == 'n')
			wait_for_signal = 0;
		else
			return EXIT_FAILURE;
	}
	if (sched_getaffinity(0, sizeof(allowed), &allowed)) {
		perror("sched_getaffinity");
		return EXIT_FAILURE;
	}
	for (int cpu = 0; cpu < CPU_SETSIZE; cpu++)
		if (CPU_ISSET(cpu, &allowed))
			cpu_count++;
	if (!cpu_count) {
		fprintf(stderr, "no CPUs available in affinity mask\n");
		return EXIT_FAILURE;
	}
	cpu_ids = calloc((size_t)cpu_count, sizeof(*cpu_ids));
	args = calloc((size_t)cpu_count, sizeof(*args));
	threads = calloc((size_t)cpu_count, sizeof(*threads));
	if (!cpu_ids || !args || !threads) {
		fprintf(stderr, "out of memory\n");
		goto cleanup;
	}
	for (int cpu = 0, index = 0; cpu < CPU_SETSIZE; cpu++)
		if (CPU_ISSET(cpu, &allowed))
			cpu_ids[index++] = cpu;

	sigemptyset(&wait_set);
	sigaddset(&wait_set, SIGUSR1);
	if (pthread_sigmask(SIG_BLOCK, &wait_set, NULL)) {
		fprintf(stderr, "unable to block SIGUSR1\n");
		goto cleanup;
	}
	printf("RACE_READY pid=%ld workers=%d calls_per_worker=%llu expected=%llu\n",
	       (long)getpid(), cpu_count, calls, calls * (unsigned long long)cpu_count);
	fflush(stdout);
	if (wait_for_signal) {
		if (sigwait(&wait_set, &signal_number)) {
			fprintf(stderr, "sigwait failed\n");
			goto cleanup;
		}
	}

	for (int i = 0; i < cpu_count; i++) {
		args[i].cpu = cpu_ids[i];
		args[i].calls = calls;
		int error = pthread_create(&threads[i], NULL, run_worker, &args[i]);
		if (error) {
			fprintf(stderr, "pthread_create for CPU%d: %s\n", cpu_ids[i], strerror(error));
			goto join_threads;
		}
		created++;
	}

join_threads:
	for (int i = 0; i < created; i++)
		pthread_join(threads[i], NULL);
	if (created != cpu_count)
		goto cleanup;
	for (int i = 0; i < cpu_count; i++) {
		if (args[i].error) {
			fprintf(stderr, "worker CPU%d: %s\n", args[i].cpu, strerror(args[i].error));
			goto cleanup;
		}
		printf("RACE_CPU cpu=%d expected=%llu\n", args[i].cpu, calls);
	}
	printf("RACE_DONE expected_total=%llu\n",
	       calls * (unsigned long long)cpu_count);
	result = EXIT_SUCCESS;

cleanup:
	free(threads);
	free(args);
	free(cpu_ids);
	return result;
}
