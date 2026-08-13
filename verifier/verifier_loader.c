#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <bpf/libbpf.h>

static int libbpf_log(enum libbpf_print_level level, const char *format, va_list args)
{
	(void)level;
	return vfprintf(stderr, format, args);
}

int main(int argc, char **argv)
{
	struct bpf_object *object;
	long error;

	if (argc != 2) {
		fprintf(stderr, "usage: %s BPF_OBJECT\n", argv[0]);
		return EXIT_FAILURE;
	}
	libbpf_set_print(libbpf_log);
	object = bpf_object__open_file(argv[1], NULL);
	if (!object) {
		fprintf(stderr, "open failed: %s\n", strerror(errno ? errno : EINVAL));
		return EXIT_FAILURE;
	}
	error = libbpf_get_error(object);
	if (error) {
		fprintf(stderr, "open failed: %s\n", strerror((int)-error));
		return EXIT_FAILURE;
	}
	error = bpf_object__load(object);
	if (error) {
		fprintf(stderr, "load rejected: %s\n", strerror((int)-error));
		bpf_object__close(object);
		return EXIT_FAILURE;
	}
	puts("load accepted");
	bpf_object__close(object);
	return EXIT_SUCCESS;
}
