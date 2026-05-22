#ifndef RTL_RING_H
#define RTL_RING_H

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>

struct rtlsdr_ring {
	unsigned char  *data;
	uint32_t       *slot_lens;
	atomic_uint     write_pos;
	size_t          slot_count;
	size_t          slot_size;
	uint32_t        mask;
};

struct rtlsdr_ring *rtlsdr_ring_create(size_t slot_count, size_t slot_size);
void rtlsdr_ring_destroy(struct rtlsdr_ring *r);

void *rtlsdr_ring_write_ptr(struct rtlsdr_ring *r);
void rtlsdr_ring_publish(struct rtlsdr_ring *r, uint32_t len);

const void *rtlsdr_ring_read_ptr(struct rtlsdr_ring *r, uint32_t *len, uint32_t read_pos);
uint32_t rtlsdr_ring_write_pos(struct rtlsdr_ring *r);

#endif
