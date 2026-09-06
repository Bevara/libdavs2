/*
 *  Two symbols davs2 references that the solvers do not export.
 *
 *  sched_getaffinity is how davs2 counts the CPUs it could spread work across.
 *  There is exactly one here - side modules are single-threaded, and the
 *  decoder is opened with threads = 1 - so the stub reports a single
 *  processor rather than failing, which would make davs2 fall back to its own
 *  guess.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int sched_getaffinity(int pid, size_t cpusetsize, void *mask)
{
	(void)pid;
	if (mask && cpusetsize)
	{
		memset(mask, 0, cpusetsize);
		((unsigned char *)mask)[0] = 1; /* CPU 0 only */
	}
	return 0;
}

void __assert_fail(const char *expr, const char *file, unsigned int line, const char *func)
{
	fprintf(stderr, "[AVS2Dec] assertion failed: %s at %s:%u in %s\n",
	        expr ? expr : "?", file ? file : "?", line, func ? func : "?");
	abort();
}
