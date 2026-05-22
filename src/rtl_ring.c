#include <stdlib.h>
#include <string.h>

#include "rtl_ring.h"

struct rtlsdr_ring *rtlsdr_ring_create(size_t slot_count, size_t slot_size)
{
	struct rtlsdr_ring *r;

	if (slot_count == 0 || (slot_count & (slot_count - 1)) != 0)
		return NULL;

	r = (struct rtlsdr_ring *)calloc(1, sizeof(*r));
	if (!r)
		return NULL;

	r->data = (unsigned char *)malloc(slot_count * slot_size);
	if (!r->data) {
		free(r);
		return NULL;
	}

	r->slot_lens = (uint32_t *)calloc(slot_count, sizeof(uint32_t));
	if (!r->slot_lens) {
		free(r->data);
		free(r);
		return NULL;
	}

	r->slot_count = slot_count;
	r->slot_size = slot_size;
	r->mask = (uint32_t)(slot_count - 1);
	atomic_init(&r->write_pos, 0);

	return r;
}

void rtlsdr_ring_destroy(struct rtlsdr_ring *r)
{
	if (r) {
		free(r->data);
		free(r->slot_lens);
		free(r);
	}
}

void *rtlsdr_ring_write_ptr(struct rtlsdr_ring *r)
{
	uint32_t wp = atomic_load_explicit(&r->write_pos, memory_order_relaxed);
	return r->data + (size_t)(wp & r->mask) * r->slot_size;
}

void rtlsdr_ring_publish(struct rtlsdr_ring *r, uint32_t len)
{
	uint32_t wp = atomic_load_explicit(&r->write_pos, memory_order_relaxed);
	r->slot_lens[wp & r->mask] = len;
	atomic_store_explicit(&r->write_pos, wp + 1, memory_order_release);
}

const void *rtlsdr_ring_read_ptr(struct rtlsdr_ring *r, uint32_t *len, uint32_t read_pos)
{
	*len = r->slot_lens[read_pos & r->mask];
	return r->data + (size_t)(read_pos & r->mask) * r->slot_size;
}

uint32_t rtlsdr_ring_write_pos(struct rtlsdr_ring *r)
{
	return atomic_load_explicit(&r->write_pos, memory_order_acquire);
}
