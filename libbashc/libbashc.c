#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "libbashc.h"

void exec_argv(char* const argv[], struct rtioctx* ioc)
{
	int i;

	if (ioc) {
		for (i = 0; i < ioc->numfds; i++) {
			if (ioc->fds[i][1] != -1) {
				if (dup2(ioc->fds[i][0],ioc->fds[i][1]) == -1) {
					perror("dup2");
					exit(1);
				} else if (close(ioc->fds[i][0])) {
					perror("close");
					exit(1);
				}
			} else {
				if (close(ioc->fds[i][0])) {
					perror("close");
					exit(1);
				}
			}
		}
	}

	execvp(argv[0],argv);
	perror("execvp");
	exit(1);
}

int forkexec_argv(char* const argv[], struct rtioctx* ioc, int flags)
{
	pid_t pid;
	int status;
	
	if (!(pid = fork())) {
		/* child */
		exec_argv(argv,ioc);
	} else if (pid == -1) {
		/* fork failed */
		return 1;
	} else if (!(flags & FE_BACKGROUND)) {
		/* parent */
		waitpid(pid,&status,0);
		return WEXITSTATUS(status);
	} else
		return 0;
}
