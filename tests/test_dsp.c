#include "rtl_dsp.h"
#include "check.h"
#include <math.h>
#include <pthread.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int16_t scaled(float value)
{
	value *= 256.0f;
	return (int16_t)fmaxf(-32768.0f, fminf(32767.0f, value));
}

static void test_bytes(void)
{
	const uint8_t input[] = {0, 255, 127, 128, 255, 0, 19, 233,
		128, 127, 255, 255, 0, 0, 127, 127};
	{
		struct rtlsdr_dsp *dsp = rtlsdr_dsp_create(2400000, 2400000,
			2400000, 0);
		int16_t output[sizeof(input)];
		size_t len;
		double previous_i = 0, previous_q = 0, bi = 0, bq = 0;
		double alpha = exp(-2.0 * M_PI * 10.0 / 2400000.0);
		CHECK(dsp);
		CHECK(rtlsdr_dsp_process(dsp, input, sizeof(input), output,
			 sizeof(input), &len) == 0);
		CHECK(len == sizeof(input));
		CHECK(output[0] == -32640);
		CHECK(output[1] == 32640);
		for (size_t j = 0; j < len / 2; j++) {
			double ri = input[2*j] - 127.5;
			double rq = input[2*j+1] - 127.5;
			bi = ri - previous_i + alpha*bi;
			bq = rq - previous_q + alpha*bq;
			previous_i = ri;
			previous_q = rq;
			CHECK(abs(output[2*j] - scaled(bi)) <= 1);
			CHECK(abs(output[2*j+1] - scaled(bq)) <= 1);
		}
		rtlsdr_dsp_destroy(dsp);
	}
}

static void test_partition(unsigned int ratio, uint64_t bandwidth)
{
	const uint64_t input_rate = 4096000 / ratio * ratio;
	const size_t bytes = 256 * 1024 + 14;
	const size_t chunks[] = {2, 6, 10, 34, 258, 4094, 18, 1024};
	uint8_t *input = malloc(bytes);
	int16_t *whole = malloc((bytes + 2) * sizeof(*whole));
	int16_t *split = malloc((bytes + 2) * sizeof(*split));
	struct rtlsdr_dsp *a = rtlsdr_dsp_create(input_rate, input_rate / ratio,
		bandwidth, 12345);
	struct rtlsdr_dsp *b = rtlsdr_dsp_create(input_rate, input_rate / ratio,
		bandwidth, 12345);
	size_t expected, total = 0, pos = 0, part = 0, len;
	CHECK(input && whole && split && a && b);
	for (size_t i = 0; i < bytes; i++)
		input[i] = (uint8_t)(i * 73 + i / 17);
	whole[0] = split[0] = 1234;
	whole[bytes+1] = split[bytes+1] = 5678;
	CHECK(rtlsdr_dsp_process(a, input, bytes, whole + 1, bytes, &expected) == 0);
	CHECK(expected == (bytes / 2 / ratio) * 2);
	while (pos < bytes) {
		size_t chunk = chunks[part++ % (sizeof(chunks) / sizeof(*chunks))];
		if (chunk > bytes - pos)
			chunk = bytes - pos;
		/* Errors while a partial block is pending must consume nothing. */
		len = 99;
		CHECK(rtlsdr_dsp_process(b, input + pos, 1, split + 1 + total,
			bytes - total, &len) < 0 && len == 0);
		len = 99;
		CHECK(rtlsdr_dsp_process(b, input + pos, chunk, split + 1 + total,
			chunk - 1, &len) < 0 && len == 0);
		CHECK(rtlsdr_dsp_process(b, input + pos, chunk, split + 1 + total,
			bytes - total, NULL) < 0);
		CHECK(rtlsdr_dsp_process(b, NULL, chunk, split + 1 + total,
			bytes - total, &len) < 0 && len == 0);
		CHECK(rtlsdr_dsp_process(b, input + pos, chunk, NULL,
			bytes - total, &len) < 0 && len == 0);
		CHECK(rtlsdr_dsp_process(b, input + pos, chunk, split + 1 + total,
			bytes - total, &len) == 0);
		CHECK(len <= chunk);
		total += len;
		pos += chunk;
	}
	CHECK(total == expected);
	CHECK(memcmp(whole + 1, split + 1, total * sizeof(*whole)) == 0);
	CHECK(whole[0] == 1234 && split[0] == 1234);
	CHECK(whole[bytes+1] == 5678 && split[bytes+1] == 5678);
	/* Leave an incomplete first-stage block, then reset all history. */
	rtlsdr_dsp_reset(b);
	CHECK(rtlsdr_dsp_process(b, input, 2, split + 1, bytes, &len) == 0);
	rtlsdr_dsp_reset(b);
	CHECK(rtlsdr_dsp_process(b, input, bytes, split + 1, bytes, &len) == 0);
	CHECK(len == expected && memcmp(whole + 1, split + 1,
		len * sizeof(*whole)) == 0);
	rtlsdr_dsp_destroy(a);
	rtlsdr_dsp_destroy(b);
	free(input);
	free(whole);
	free(split);
}

static void test_fractional_filter(void)
{
	uint8_t input[1024];
	int16_t output[1024];
	size_t len;
	int fractional = 0;
	struct rtlsdr_dsp *dsp = rtlsdr_dsp_create(2400000, 1200000, 0, 0);
	CHECK(dsp);
	memset(input, 128, sizeof(input));
	CHECK(rtlsdr_dsp_process(dsp, input, sizeof(input), output, 1024, &len) == 0);
	CHECK(len == 512);
	for (size_t j = 0; j < len; j++)
		if (output[j] != 0 && output[j] % 256 != 0)
			fractional = 1;
	CHECK(fractional);
	rtlsdr_dsp_destroy(dsp);
}

static void test_full_slot(void)
{
	size_t bytes = 256 * 1024, len;
	uint8_t *input = malloc(bytes);
	int16_t *output = malloc(bytes * sizeof(*output));
	struct rtlsdr_dsp *dsp = rtlsdr_dsp_create(2400000, 2400000, 2400000, 0);
	CHECK(input && output && dsp);
	memset(input, 128, bytes);
	memset(output, 0x5a, bytes * sizeof(*output));
	CHECK(rtlsdr_dsp_process(dsp, input, bytes, output, bytes / 2, &len) < 0);
	CHECK(len == 0);
	for (size_t i = 0; i < bytes; i++)
		CHECK(output[i] == 0x5a5a);
	CHECK(rtlsdr_dsp_process(dsp, input, bytes, output, bytes, &len) == 0);
	CHECK(len == bytes && output[0] == 128 && output[1] == 128);
	CHECK(output[2] == 127 && output[3] == 127);
	CHECK(rtlsdr_dsp_process(dsp, input, 0, output, 0, &len) == 0 && len == 0);
	CHECK(rtlsdr_dsp_process(NULL, input, 2, output, bytes, &len) < 0 && len == 0);
	rtlsdr_dsp_destroy(dsp);
	free(input);
	free(output);
}

static void test_validation(void)
{
	const struct {
		uint64_t input, output, bandwidth;
		int64_t offset;
		int valid;
		uint64_t effective;
	} cases[] = {
		{0, 0, 0, 0, 0, 0},
		{2400000, 0, 0, 0, 0, 0},
		{1, 2, 0, 0, 0, 0},
		{1, 1, 0, 0, 0, 0},
		{1, 1, 1, 0, 1, 1},
		{7, 7, 0, 1, 1, 5},
		{7, 7, 0, -1, 1, 5},
		{7, 7, 0, 2, 0, 0},
		{UINT64_MAX, 1, 0, 0, 0, 0},
		{UINT64_C(4294967296), UINT64_C(4294967296), 0, 0, 0, 0},
		{UINT32_MAX, UINT32_MAX, 0, 0, 1, UINT64_C(3435973836)},
		{UINT32_MAX, UINT32_MAX, 1, INT64_C(2147483647), 1, 1},
		{UINT32_MAX, UINT32_MAX, 1, -INT64_C(2147483647), 1, 1},
		{UINT32_MAX, UINT32_MAX, 1, INT64_C(2147483648), 0, 0},
		{2400000, 200000, UINT64_MAX, 0, 0, 0},
		{2400000, 200000, 0, INT64_MIN, 0, 0},
		{2400000, 200000, 0, INT64_MAX, 0, 0},
		{2400000, 200000, 0, -INT64_MAX, 0, 0},
		{2400000, 200000, 0, 0, 1, 160000},
		{2400000, 200000, 150000, 1125000, 1, 150000},
		{2400000, 200000, 150000, -1125000, 1, 150000},
		{2400000, 200000, 150000, 1125001, 0, 0},
		{2400000, 200000, 150000, -1125001, 0, 0},
		{2400000, 200000, 160001, 0, 0, 0},
		{2400000, 200000, 200000, 0, 0, 0},
		{2400000, 2400000, 2400000, 0, 1, 2400000},
		{2400000, 2400000, 2400000, 1, 0, 0},
		{2400000, 2400000, 2400000, -1, 0, 0},
		{2400000, 199999, 0, 0, 0, 0},
		{4096000, 1000, 0, 0, 1, 800},
		{4097000, 1000, 0, 0, 0, 0},
		{1100000, 100000, 0, 0, 1, 80000},
		/* Prime plans cannot be approximated or clipped to the tap budget. */
		{211000, 1000, 0, 0, 0, 0},
		{211000, 1000, 1, 0, 1, 1},
		{422000, 1000, 0, 0, 1, 800},
		{4093000, 1000, 1, 0, 0, 0},
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
		uint64_t effective = UINT64_MAX;
		int result = rtlsdr_dsp_validate(cases[i].input, cases[i].output,
			cases[i].bandwidth, cases[i].offset, &effective);
		struct rtlsdr_dsp *dsp = rtlsdr_dsp_create(cases[i].input,
			cases[i].output, cases[i].bandwidth, cases[i].offset);
		CHECK(result == (cases[i].valid ? 0 : -1));
		CHECK((dsp != NULL) == cases[i].valid);
		CHECK(effective == (cases[i].valid ? cases[i].effective : UINT64_MAX));
		rtlsdr_dsp_destroy(dsp);
	}
	CHECK(rtlsdr_dsp_validate(2400000, 200000, 0, 0, NULL) == 0);
}

static pthread_mutex_t start_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t start_cond = PTHREAD_COND_INITIALIZER;
static int start_threads;

static void *create_worker(void *arg)
{
	int16_t *result = arg;
	uint8_t input[2048];
	size_t len;
	CHECK(pthread_mutex_lock(&start_mutex) == 0);
	while (!start_threads)
		CHECK(pthread_cond_wait(&start_cond, &start_mutex) == 0);
	CHECK(pthread_mutex_unlock(&start_mutex) == 0);
	for (int j = 0; j < 16; j++) {
		struct rtlsdr_dsp *dsp = rtlsdr_dsp_create(1100000, 100000, 0, -123456);
		CHECK(dsp);
		for (size_t i = 0; i < sizeof(input); i++)
			input[i] = (uint8_t)(i * 37 + i / 19);
		CHECK(rtlsdr_dsp_process(dsp, input, sizeof(input), result,
			sizeof(input), &len) == 0);
		CHECK(len == sizeof(input) / 2 / 11 * 2);
		rtlsdr_dsp_destroy(dsp);
	}
	return NULL;
}

static void test_concurrent_create(void)
{
	pthread_t threads[12];
	int16_t results[12][2048] = {{0}};
	for (int i = 0; i < 12; i++)
		CHECK(pthread_create(&threads[i], NULL, create_worker, results[i]) == 0);
	CHECK(pthread_mutex_lock(&start_mutex) == 0);
	start_threads = 1;
	CHECK(pthread_cond_broadcast(&start_cond) == 0);
	CHECK(pthread_mutex_unlock(&start_mutex) == 0);
	for (int i = 0; i < 12; i++) {
		CHECK(pthread_join(threads[i], NULL) == 0);
		CHECK(memcmp(results[0], results[i], sizeof(results[i])) == 0);
	}
}

int main(void)
{
	/* Must precede all other creates to exercise NCO initialization. */
	test_concurrent_create();
	test_validation();
	test_bytes();
	test_fractional_filter();
	test_full_slot();
	test_partition(1, 2400000);
	test_partition(5, 600000);
	test_partition(11, 0);
	test_partition(16, 100000);
	test_partition(64, 40000);
	test_partition(128, 20000);
	test_partition(512, 5000);
	test_partition(4096, 0);
	/* Exercise smaller actual stage counts independently of bandwidth. */
	test_partition(2, 0);
	test_partition(16, 0);
	test_partition(128, 0);
	return 0;
}
