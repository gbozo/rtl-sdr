#include <stdlib.h>
#include <string.h>

#include "rtl_ring.h"

struct rtlsdr_ring *rtlsdr_ring_create(size_t slot_count, size_t slot_size)
{
	struct rtlsdr_ring *r;
	if (!slot_count || !slot_size || slot_count > SIZE_MAX / slot_size ||
	    slot_count > SIZE_MAX / sizeof(size_t))
		return NULL;
	r = calloc(1, sizeof(*r));
	if (!r)
		return NULL;
	r->data = malloc(slot_count * slot_size);
	r->slot_lens = calloc(slot_count, sizeof(*r->slot_lens));
	if (!r->data || !r->slot_lens || pthread_mutex_init(&r->mutex, NULL)) {
		free(r->data);
		free(r->slot_lens);
		free(r);
		return NULL;
	}
	r->slot_count = slot_count;
	r->slot_size = slot_size;
	r->generation = 1;
	return r;
}

void rtlsdr_ring_destroy(struct rtlsdr_ring *r)
{
	if (!r)
		return;
	pthread_mutex_destroy(&r->mutex);
	free(r->data);
	free(r->slot_lens);
	free(r);
}

int rtlsdr_ring_write_copy(struct rtlsdr_ring *r, const uint8_t *src, size_t len)
{
	size_t slot;
	if (!r || (!src && len) || len > r->slot_size)
		return -1;
	if (!len)
		return 0;
	pthread_mutex_lock(&r->mutex);
	if (r->next_seq == UINT64_MAX) {
		r->generation++;
		r->next_seq = r->retained = 0;
	}
	slot = r->next_seq % r->slot_count;
	memcpy(r->data + slot * r->slot_size, src, len);
	r->slot_lens[slot] = len;
	r->next_seq++;
	if (r->retained < r->slot_count)
		r->retained++;
	pthread_mutex_unlock(&r->mutex);
	return 0;
}

int rtlsdr_ring_cursor_now(struct rtlsdr_ring *r, struct rtlsdr_ring_cursor *cursor)
{
	if (!r || !cursor)
		return -1;
	pthread_mutex_lock(&r->mutex);
	cursor->next_seq = r->next_seq;
	cursor->generation = r->generation;
	pthread_mutex_unlock(&r->mutex);
	return 0;
}

int rtlsdr_ring_read_copy(struct rtlsdr_ring *r, struct rtlsdr_ring_cursor *cursor,
	uint8_t *dst, size_t capacity, size_t *len)
{
	int result = RTL_RING_ERROR;
	size_t slot, bytes;
	if (len)
		*len = 0;
	if (!r || !cursor || !dst || !len)
		return result;
	pthread_mutex_lock(&r->mutex);
	if (cursor->generation != r->generation ||
	    cursor->next_seq < r->next_seq - r->retained) {
		result = RTL_RING_GAP;
	} else if (cursor->next_seq == r->next_seq) {
		result = RTL_RING_EMPTY;
	} else if (cursor->next_seq < r->next_seq) {
		slot = cursor->next_seq % r->slot_count;
		bytes = r->slot_lens[slot];
		if (bytes <= capacity) {
			memcpy(dst, r->data + slot * r->slot_size, bytes);
			*len = bytes;
			cursor->next_seq++;
			result = RTL_RING_DATA;
		}
	}
	pthread_mutex_unlock(&r->mutex);
	return result;
}

void rtlsdr_ring_mark_gap(struct rtlsdr_ring *r)
{
	if (!r)
		return;
	pthread_mutex_lock(&r->mutex);
	r->generation++;
	r->retained = 0;
	pthread_mutex_unlock(&r->mutex);
}
