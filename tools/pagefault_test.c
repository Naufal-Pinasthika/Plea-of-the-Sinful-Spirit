#define _GNU_SOURCE

#include <errno.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	static const struct option options[] = {
		{ "pages", required_argument, NULL, 'p' },
		{ "no-wait", no_argument, NULL, 'n' },
		{0},
	};
	size_t pages = 4096;
	int wait_for_input = 1, option;
	long page_size = sysconf(_SC_PAGESIZE);
	volatile unsigned long checksum = 0;

	while ((option = getopt_long(argc, argv, "p:n", options, NULL)) != -1) {
		if (option == 'p') {
			char *end = NULL;
			unsigned long parsed;

			errno = 0;
			parsed = strtoul(optarg, &end, 10);
			if (errno || !optarg[0] || !end || *end || !parsed)
				return fprintf(stderr, "invalid page count: %s\n", optarg), EXIT_FAILURE;
			pages = parsed;
		} else if (option == 'n') {
			wait_for_input = 0;
		} else {
			return EXIT_FAILURE;
		}
	}
	if (page_size <= 0 || pages > SIZE_MAX / (size_t)page_size) {
		fprintf(stderr, "invalid allocation size\n");
		return EXIT_FAILURE;
	}
	size_t length = pages * (size_t)page_size;
	unsigned char *memory = mmap(NULL, length, PROT_READ | PROT_WRITE,
				     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (memory == MAP_FAILED) {
		perror("mmap");
		return EXIT_FAILURE;
	}
	printf("PAGEFAULT_READY pid=%ld pages=%zu bytes=%zu\n", (long)getpid(), pages, length);
	if (wait_for_input) {
		printf("Start cpuwatch with --events --pagefault --pid %ld, then press ENTER...\n",
		       (long)getpid());
		fflush(stdout);
		(void)getchar();
	}
	for (size_t page = 0; page < pages; page++) {
		memory[page * (size_t)page_size] = (unsigned char)page;
		checksum += memory[page * (size_t)page_size];
	}
	printf("PAGEFAULT_DONE touched=%zu checksum=%lu\n", pages, checksum);
	munmap(memory, length);
	return EXIT_SUCCESS;
}
