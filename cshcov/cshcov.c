#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>

#include "file.h"

#define PROG_NAME "cshcov"
#define DEFAULT_SHELL "/bin/bash -x"
#define SEPARATOR ":::"
#define SHCOV_MAGIC "SHCOV:::"
#define PS4 SHCOV_MAGIC "${BASH_SOURCE}:::${LINENO}::: SHCOV:"

char *split_on_separator(char **line) {
	char *start, *tmp;
	int i;

	start = *line;
	*line = strstr(*line, SEPARATOR);
	if (!*line)
		return NULL;

	**line = '\0';
	*line = (char *)((long)(*line) + strlen(SEPARATOR));

	return start;
}

void handle_stderr_line(char *line, ssize_t linelen, int *tick_count)
{
	char *tmp, *lineno_str, *bash_source_str;
	char source_file[PATH_MAX];
	long lineno;

	if (!strstr(line, SHCOV_MAGIC)) {
		/* 
		 * No tick implies single line of output -> output,
		 * one tick on the previous line implies continued 
		 * shcov line -> no output,
		 * more than one tick means stderr output with ticks -> output
		 */
		if (*tick_count == 0 || *tick_count > 1) {
			fwrite(line, linelen, 1, stderr);
		}
		return;
	}


	for (*tick_count = 0, tmp = line; tmp; tmp = strchr(tmp, '\'')) {
		tmp++;
		(*tick_count)++;
	}

	/* Strip prefix */
	split_on_separator(&line);

	/* Extract source name */
	bash_source_str = split_on_separator(&line);
	if (!bash_source_str) {
		fprintf(stderr, "Garbled line, exiting\n");
		exit(EXIT_FAILURE);
	}

	/* Fetch the realpath */
	if (!realpath(bash_source_str, source_file)) {
		fprintf(stderr, "Failed to resolve real path for %s\n",
			bash_source_str);
		exit(EXIT_FAILURE);
	}

	/* Extract line number */
	lineno_str = strsep(&line, SEPARATOR);
	if (!lineno_str) {
		fprintf(stderr, "Garbled line, exiting\n");
		exit(EXIT_FAILURE);
	}

	errno = 0;
	lineno = strtol(lineno_str, NULL, 10);
	if (errno) {
		fprintf(stderr, "Failed to resolve lineno from %s\n",
			lineno_str);
		exit(EXIT_FAILURE);
	}

	/* Do the tabulation */
	file_tabulate_line(source_file, lineno);
	
}

void parse_stderr_until_close(int stderr_pipe)
{
	FILE *fstderr_pipe;
	char *line = NULL;
	size_t linecap = 0;
	ssize_t linelen;
	int tick_count = 0;

	fstderr_pipe = fdopen(stderr_pipe, "r");
	if (!fstderr_pipe) {
		perror("Failed to open file on stderr pipe");
		exit(EXIT_FAILURE);
	}

	while ((linelen = getline(&line, &linecap, fstderr_pipe)) > 0) {
		handle_stderr_line(line, linelen, &tick_count);
	}
}

int spawn_script(const char *script_path, const char *output_path, 
		 char * const new_argv[])
{
	int stderr_pipe[2];
	pid_t pid;

	if (pipe(stderr_pipe) < 0) {
		perror("Failed to open pipe");
		exit(EXIT_FAILURE);
	}

	pid = fork();
	if (pid < 0) {
		perror("Failed to fork for script");
		exit(EXIT_FAILURE);
	} else if (pid > 0) {
		int stat_loc;

		/* Close the parents write end of the pipe */
		close(stderr_pipe[1]);

		parse_stderr_until_close(stderr_pipe[0]);
		wait(&stat_loc);
		return stat_loc;
	} else {
		
		/* Close the childs read end of the pipe */
		close(stderr_pipe[0]);

		if (dup2(stderr_pipe[1], 2) < 0) {
			perror("Failed to exchange stderr for pipe");
			exit(EXIT_FAILURE);
		}

		/* Close the childs old write end of the pipe */
		close(stderr_pipe[1]);

		/* Set up PS4 for tracing */
		if (setenv("PS4", PS4, 1)) {
			perror("Failed to set PS4 environment");
			exit(EXIT_FAILURE);
		}

		/* Execute the script */
		if (execv(new_argv[0], new_argv)) {
			perror("Failed to execute script");
			exit(EXIT_FAILURE);
		}
	}

	return 0;
}


/* 
 * Creates a new argv with first the shell to be called, followed
 * by whatever arguments were provided for the script.
 */
char **create_argv(char *shell, char *script_path, int argc, char *argv[])
{
	char **new_argv, **tmp_argv, **tmp;
	char *new_shell;
	int i = 0, j = 0;

	/* Create a copy for modification */
	new_shell = strdup(shell);
	if (!new_shell) {
		perror("Failed to allocate argv");
		exit(EXIT_FAILURE);
	}

	/* Allocate an initial argv of length 1 */
	new_argv = malloc(sizeof(char *));
	if (!new_argv) {
		perror("Failed to allocate argv");
		exit(EXIT_FAILURE);
	}

	/* Create an array from the shell */
	while ((new_argv[i] = strsep(&new_shell, " "))) {
		i++;
		new_argv = realloc(new_argv, (i + 1) * sizeof(char *));
		if (!new_argv) {
			perror("Failed to allocate argv");
			exit(EXIT_FAILURE);
		}
	}

	/*
	 * Allocate the final argv, with space for the script_path, the
	 * passed argv and a NULL terminator.
	 */
	new_argv = realloc(new_argv, (i + argc + 3) * sizeof(char *));
	if (!new_argv) {
		perror("Failed to allocate argv");
		exit(EXIT_FAILURE);
	}

	new_argv[i++] = script_path;
	
	/* Copy any remaining arguments from argv */
	for (j = 0; j < argc; j++) {
		new_argv[i++] = argv[j];
	}

	argv[i] = NULL;
	
/* 	for (tmp = new_argv; *tmp; tmp++) { */
/* 		printf("%s\n", *tmp); */
/* 	} */

	return new_argv;
}

static void print_help_and_exit(void)
{
	fprintf(stderr,
		"Usage: " PROG_NAME " [-h] [--output=where] [--shell=what] script...\n"
		"Produce coverage data for 'script'. Options are\n"
		"--output=where  write data to 'where' instead of /tmp/shcov\n"
		"--shell=what    ues 'what' (including arguments) as shell instead of 'bash -x'\n");
	exit(EXIT_FAILURE);
}

void set_posixly_correct(char **posixly_correct)
{
	char *tmp;

	tmp = getenv("POSIXLY_CORRECT");

	if (tmp) {
		*posixly_correct = strdup(tmp);
	}

	if (setenv("POSIXLY_CORRECT", "1", 1)) {
		perror("Failed to set \"POSIXLY_CORRECT\"");
		exit(EXIT_FAILURE);
	}
}

void restore_old_posixly_correct(char *posixly_correct)
{
	if (posixly_correct) {
		if (setenv("POSIXLY_CORRECT", posixly_correct, 1)) {
			perror("Failed to restore old \"POSIXLY_CORRECT\"");
			exit(EXIT_FAILURE);
		}
	} else {
		if (unsetenv("POSIXLY_CORRECT")) {
			perror("Failed to unset \"POSIXLY_CORRECT\"");
			exit(EXIT_FAILURE);
		}
	}
}

int main(int argc, char *argv[])
{
	char *script_path = NULL;
	char *output_path = NULL;
	char *shell = DEFAULT_SHELL;
	char *posixly_correct = NULL;
	char **new_argv;
	int opt;

	struct option opts[] = {
		{"output", required_argument, NULL, 'o'},
		{"shell", required_argument, NULL, 's'},
		{"help", no_argument, NULL, 'h'},
		{ 0 }
	};

	/* 
	 * Use posixly correct getopts parsing in order to pass additional
	 * arguments to the script.
	 */
	set_posixly_correct(&posixly_correct);

	while ((opt = getopt_long(argc, argv, "ho:s:", opts, NULL)) != -1) {
		switch(opt) {
		case 'h':
			print_help_and_exit();
		case 'o':
			output_path = optarg;
			break;
		case 's':
			shell = optarg;
			break;
		default:
			print_help_and_exit();
		}
	}

	restore_old_posixly_correct(posixly_correct);

	if (argc - optind < 1) {
		fprintf(stderr, "No script to execute provided\n");
		exit(EXIT_FAILURE);
	}

	script_path = argv[optind];
	optind++;

	new_argv = create_argv(shell,
			       script_path,
			       argc - optind, 
			       argv + optind);

	spawn_script(script_path, output_path, new_argv);

	return 0;
}
