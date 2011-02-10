/*
 * Interface to bashc runtime-support code.
 */

#ifndef LIBBASHC_H
#define LIBBASHC_H

/* Magic number for "close this fd" */
#define IO_CLOSE_FD (-1)

/* run-time I/O context */
struct rtioctx {
	int numfds;
	int fds[][2];
};

void exec_argv(char* const argv[], struct rtioctx* ioc) __attribute__((noreturn));
int forkexec_argv(char* const argv[], struct rtioctx* ioc);

#endif
