#ifndef TAPING_TEST_H
#define TAPING_TEST_H

#include <stdio.h>
#include <stdlib.h>

#define check(_cond) do {						\
	if (!(_cond)) {							\
		fprintf(stderr, "%s:%d: check failed: %s\n",		\
			__FILE__, __LINE__, #_cond);			\
		exit(1);						\
	}								\
} while (0)

#endif
