/* Minimal test helpers. */
#pragma once

#include <stdio.h>
#include <string.h>

extern int failures;

#define CHECK(cond, ...)                                                                   \
	do {                                                                               \
		if (!(cond)) {                                                             \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);                        \
			printf(__VA_ARGS__);                                               \
			printf("\n");                                                      \
			failures++;                                                        \
		}                                                                          \
	} while (0)

#define CHECK_MEM(a, b, n, what) CHECK(memcmp((a), (b), (n)) == 0, "%s differs", what)
