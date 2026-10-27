#ifndef RTL_RING_H
#define RTL_RING_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

enum rtlsdr_ring_result {
	RTL_RING_ERROR = -1,
	RTL_RING_EMPTY = 0,
	RTL_RING_DATA = 1,
	RTL_RING_GAP = 2
};

struct rtlsdr_ring_cursor {
	uint64_t next_seq;
	uint64_t generation;
};

struct rtlsdr_ring {
	uint8_t *data;
	size_t *slot_lens;
	size_t slot_count;
	size_t slot_size;
	uint64_t next_seq;
	uint64_t generation;
	uint64_t retained;
	pthread_mutex_t mutex;
};

struct rtlsdr_ring *rtlsdr_ring_create(size_t slot_count, size_t slot_size);
void rtlsdr_ring_destroy(struct rtlsdr_ring *r);
int rtlsdr_ring_write_copy(struct rtlsdr_ring *r, const uint8_t *src, size_t len);
int rtlsdr_ring_cursor_now(struct rtlsdr_ring *r, struct rtlsdr_ring_cursor *cursor);
int rtlsdr_ring_read_copy(struct rtlsdr_ring *r, struct rtlsdr_ring_cursor *cursor,
	uint8_t *dst, size_t capacity, size_t *len);
void rtlsdr_ring_mark_gap(struct rtlsdr_ring *r);

#endif
