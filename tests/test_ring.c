#include "rtl_ring.h"
#include "check.h"
#include <stdatomic.h>
#include <string.h>
#include <sched.h>

static void test_retention(void)
{
	struct rtlsdr_ring *r = rtlsdr_ring_create(3, 16);
	struct rtlsdr_ring_cursor cursor, saved;
	uint8_t input[16], output[16];
	size_t len = 99;
	CHECK(r);
	CHECK(!rtlsdr_ring_create(0, 16));
	CHECK(!rtlsdr_ring_create(1, 0));
	CHECK(!rtlsdr_ring_create(SIZE_MAX, 16));
	CHECK(rtlsdr_ring_cursor_now(r, &cursor) == 0);
	saved = cursor;
	CHECK(rtlsdr_ring_read_copy(r, &cursor, output, sizeof(output), &len) == RTL_RING_EMPTY);
	CHECK(len == 0 && memcmp(&cursor, &saved, sizeof(cursor)) == 0);
	memset(input, 7, sizeof(input));
	CHECK(rtlsdr_ring_write_copy(r, input, 17) < 0);
	CHECK(rtlsdr_ring_write_copy(r, NULL, 0) == 0);
	for (int j = 0; j < 3; j++) {
		memset(input, j, sizeof(input));
		CHECK(rtlsdr_ring_write_copy(r, input, sizeof(input)) == 0);
	}
	CHECK(rtlsdr_ring_read_copy(r, &cursor, output, 15, &len) == RTL_RING_ERROR);
	CHECK(len == 0 && cursor.next_seq == saved.next_seq);
	for (int j = 0; j < 3; j++) {
		CHECK(rtlsdr_ring_read_copy(r, &cursor, output, sizeof(output), &len) == RTL_RING_DATA);
		CHECK(len == 16);
		for (size_t k = 0; k < len; k++)
			CHECK(output[k] == j);
	}
	for (int j = 0; j < 100; j++)
		CHECK(rtlsdr_ring_write_copy(r, input, sizeof(input)) == 0);
	saved = cursor;
	for (int j = 0; j < 2; j++) {
		CHECK(rtlsdr_ring_read_copy(r, &cursor, output, sizeof(output), &len) == RTL_RING_GAP);
		CHECK(len == 0 && memcmp(&cursor, &saved, sizeof(cursor)) == 0);
	}
	CHECK(rtlsdr_ring_cursor_now(r, &cursor) == 0);
	saved = cursor;
	rtlsdr_ring_mark_gap(r);
	CHECK(rtlsdr_ring_read_copy(r, &cursor, output, sizeof(output), &len) == RTL_RING_GAP);
	CHECK(memcmp(&cursor, &saved, sizeof(cursor)) == 0);
	CHECK(rtlsdr_ring_cursor_now(r, &cursor) == 0);
	CHECK(cursor.generation == saved.generation + 1);
	CHECK(rtlsdr_ring_read_copy(r, &cursor, output, sizeof(output), &len) == RTL_RING_EMPTY);
	CHECK(rtlsdr_ring_write_copy(r, input, sizeof(input)) == 0);
	CHECK(rtlsdr_ring_read_copy(r, &cursor, output, sizeof(output), &len) == RTL_RING_DATA);
	/* Exercise sequence rollover without billions of writes. */
	pthread_mutex_lock(&r->mutex);
	r->next_seq = UINT64_MAX;
	r->retained = 0;
	pthread_mutex_unlock(&r->mutex);
	CHECK(rtlsdr_ring_cursor_now(r, &cursor) == 0);
	CHECK(rtlsdr_ring_write_copy(r, input, sizeof(input)) == 0);
	CHECK(rtlsdr_ring_read_copy(r, &cursor, output, sizeof(output), &len) == RTL_RING_GAP);
	rtlsdr_ring_destroy(r);
}

struct concurrent {
	struct rtlsdr_ring *ring;
	atomic_int done;
};

static void *writer(void *arg)
{
	struct concurrent *ctx = arg;
	uint64_t data[512];
	for (uint64_t seq = 1; seq <= 30000; seq++) {
		for (size_t j = 0; j < 512; j++)
			data[j] = seq;
		CHECK(rtlsdr_ring_write_copy(ctx->ring, (uint8_t *)data, sizeof(data)) == 0);
		if (seq % 101 == 0)
			rtlsdr_ring_mark_gap(ctx->ring);
		if (seq % 8 == 0)
			sched_yield();
	}
	atomic_store(&ctx->done, 1);
	return NULL;
}

static void *reader(void *arg)
{
	struct concurrent *ctx = arg;
	struct rtlsdr_ring_cursor cursor;
	uint64_t data[512], previous = 0;
	size_t len;
	CHECK(rtlsdr_ring_cursor_now(ctx->ring, &cursor) == 0);
	do {
		int result = rtlsdr_ring_read_copy(ctx->ring, &cursor, (uint8_t *)data, sizeof(data), &len);
		CHECK(result != RTL_RING_ERROR);
		if (result == RTL_RING_DATA) {
			CHECK(len == sizeof(data) && data[0] > previous);
			previous = data[0];
			for (size_t j = 1; j < 512; j++)
				CHECK(data[j] == data[0]);
		} else if (result == RTL_RING_GAP) {
			CHECK(rtlsdr_ring_cursor_now(ctx->ring, &cursor) == 0);
		} else {
			sched_yield();
		}
	} while (!atomic_load(&ctx->done));
	return NULL;
}

int main(void)
{
	struct concurrent ctx;
	pthread_t producer, readers[3];
	test_retention();
	ctx.ring = rtlsdr_ring_create(4, 4096);
	CHECK(ctx.ring);
	atomic_init(&ctx.done, 0);
	for (int j = 0; j < 3; j++)
		CHECK(pthread_create(&readers[j], NULL, reader, &ctx) == 0);
	CHECK(pthread_create(&producer, NULL, writer, &ctx) == 0);
	CHECK(pthread_join(producer, NULL) == 0);
	for (int j = 0; j < 3; j++)
		CHECK(pthread_join(readers[j], NULL) == 0);
	rtlsdr_ring_destroy(ctx.ring);
	return 0;
}
