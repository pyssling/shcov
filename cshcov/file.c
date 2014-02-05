#include <fcntl.h>
#include <openssl/md5.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/queue.h>
#include <sys/stat.h>
#include <unistd.h>

struct file {
	char md5sum[MD5_DIGEST_LENGTH];
	int line[0];
};

struct file_container {
	char *name;
	struct file *file;
	LIST_ENTRY(file_container) list;
};
	

LIST_HEAD(file_list, file_container);

static struct file_list _file_list = LIST_HEAD_INITIALIZER(file_list);
static struct file_list *file_list = &_file_list;

struct file_container *find_file_container(char *source_file) {
	struct file_container *file_c;

	LIST_FOREACH(file_c, file_list, list) {
		if (!strcmp(source_file, file_c->name))
			return file_c;
	}
}

static void profile_file(char *source_file, long *line_count, char *md5) {
	int fd;
	char *mapped_file, *pos;
	struct stat stat;

	fd = open(source_file, O_RDONLY);
	if (fd < 0) {
		perror("Failed to open source file");
		exit(EXIT_FAILURE);
	}

	if (fstat(fd, &stat) < 0) {
		perror("Failed to stat source file");
		exit(EXIT_FAILURE);
	}

	mapped_file = mmap(NULL, stat.st_size, PROT_READ, MAP_SHARED, fd, 0);
	if (mapped_file == MAP_FAILED) {
		perror("Failed to mmap source file");
		exit(EXIT_FAILURE);
	}

	*line_count = 0;
	for (*line_count=0, pos = mapped_file; pos; pos = strchr(pos, '\n')) {
		pos++;
		(*line_count)++;
	}

	munmap(mapped_file, stat.st_size);
	close(fd);
}

struct file *new_file(char *source_file, long line_count, char *md5sum)
{
	
}

struct file_container *new_file_container(char *source_file) {
	struct file_container *file_c;
	long line_count;
	char md5sum[MD5_DIGEST_LENGTH];

	file_c = malloc(sizeof(*file_c));
	if (!file_c) {
		perror("Failed to allocate file_container");
		exit(EXIT_FAILURE);
	}

	file_c->name = strdup(source_file);
	if (!file_c->name) {
		perror("Failed to allocate file container name");
		exit(EXIT_FAILURE);
	}

	profile_file(source_file, &line_count, md5sum);

	printf("%s lines: %ld\n", source_file, line_count);
}


void file_tabulate_line(char *source_file, long lineno)
{
	struct file_container *file_c;

	file_c = find_file_container(source_file);
	if (!file_c) {
		file_c = new_file_container(source_file);
	}

	
	printf("Tabulating %s:%ld\n", source_file, lineno);
}
