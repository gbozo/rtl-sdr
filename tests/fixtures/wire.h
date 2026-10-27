#ifndef TEST_WIRE_H
#define TEST_WIRE_H

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define GOLD_FREQ UINT64_C(9007199254740993)
#define GOLD_SEQ UINT64_C(0x0102030405060708)

static size_t fixture(const char *name, unsigned char *buf)
{
	char path[4096];
	FILE *file;
	size_t n = 0;
	unsigned value;
	strcpy(path, __FILE__);
	strcpy(strrchr(path, '/') + 1, name);
	file = fopen(path, "r");
	assert(file);
	while (fscanf(file, "%2x", &value) == 1)
		buf[n++] = (unsigned char)value;
	assert(!ferror(file));
	fclose(file);
	return n;
}

#endif
