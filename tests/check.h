#ifndef TEST_CHECK_H
#define TEST_CHECK_H

#include <stdio.h>
#include <stdlib.h>

#define CHECK(expr) do { \
	if (!(expr)) { \
		fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); \
		exit(1); \
	} \
} while (0)

#endif
