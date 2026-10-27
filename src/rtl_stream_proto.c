#include <string.h>
#include <float.h>

#include "rtl_stream_proto.h"

static void put32(unsigned char *buf, uint32_t val)
{
	buf[0] = (unsigned char)(val >> 24);
	buf[1] = (unsigned char)(val >> 16);
	buf[2] = (unsigned char)(val >> 8);
	buf[3] = (unsigned char)(val);
}

static void put64(unsigned char *buf, uint64_t val)
{
	put32(buf,     (uint32_t)(val >> 32));
	put32(buf + 4, (uint32_t)(val));
}

static uint32_t get32(const unsigned char *buf)
{
	return ((uint32_t)buf[0] << 24) |
	       ((uint32_t)buf[1] << 16) |
	       ((uint32_t)buf[2] << 8)  |
	       ((uint32_t)buf[3]);
}

static uint64_t get64(const unsigned char *buf)
{
	return ((uint64_t)get32(buf) << 32) | get32(buf + 4);
}

static int valid_req(const struct rtlsdr_stream_req *req)
{
	return req->magic == RTLSTREAM_MAGIC_REQ &&
		(req->mode == RTLSTREAM_MODE_IQ ||
		 (req->mode == RTLSTREAM_MODE_FFT && !req->freq &&
		  !req->rate && !req->bandwidth));
}

int rtlsdr_stream_encode_req(unsigned char *buf, const struct rtlsdr_stream_req *req)
{
	if (!buf || !req || !valid_req(req))
		return -1;
	put32(buf,      req->magic);
	put64(buf + 4,  req->freq);
	put64(buf + 12, req->rate);
	put64(buf + 20, req->bandwidth);
	buf[28] = req->mode;
	return RTLSTREAM_REQ_SIZE;
}

int rtlsdr_stream_decode_req(struct rtlsdr_stream_req *req, const unsigned char *buf)
{
	if (!req || !buf)
		return -1;
	req->magic     = get32(buf);
	req->freq      = get64(buf + 4);
	req->rate      = get64(buf + 12);
	req->bandwidth = get64(buf + 20);
	req->mode      = buf[28];
	return valid_req(req) ? 0 : -1;
}

int rtlsdr_stream_encode_iq_hdr(unsigned char *buf, const struct rtlsdr_stream_iq_hdr *hdr)
{
	if (!buf || !hdr || hdr->magic != RTLSTREAM_MAGIC_IQ ||
	    hdr->nsamples > RTLSTREAM_MAX_IQ_COMPONENTS || (hdr->nsamples & 1))
		return -1;
	put32(buf,      hdr->magic);
	put64(buf + 4,  hdr->freq);
	put64(buf + 12, hdr->rate);
	put64(buf + 20, hdr->seq);
	put32(buf + 28, hdr->nsamples);
	return RTLSTREAM_IQ_HDR_SIZE;
}

int rtlsdr_stream_encode_fft_hdr(unsigned char *buf, const struct rtlsdr_stream_fft_hdr *hdr)
{
	if (!buf || !hdr || hdr->magic != RTLSTREAM_MAGIC_FFT ||
	    hdr->bins < 2 || hdr->bins > RTLSTREAM_MAX_FFT_BINS)
		return -1;
	put32(buf,      hdr->magic);
	put64(buf + 4,  hdr->freq);
	put64(buf + 12, hdr->rate);
	put64(buf + 20, hdr->seq);
	put32(buf + 28, hdr->bins);
	return RTLSTREAM_FFT_HDR_SIZE;
}

int rtlsdr_stream_encode_evt(unsigned char *buf, uint32_t type, uint64_t freq)
{
	if (!buf || (type != RTLSTREAM_EVT_FREQ_CHANGE && type != RTLSTREAM_EVT_STREAM_END))
		return -1;
	put32(buf, RTLSTREAM_MAGIC_EVT);
	put32(buf + 4, type);
	if (type == RTLSTREAM_EVT_FREQ_CHANGE) {
		put64(buf + 8, freq);
		return RTLSTREAM_EVT_FREQ_SIZE;
	}
	return RTLSTREAM_EVT_HDR_SIZE;
}

int rtlsdr_stream_encode_iq_payload(uint8_t *dst, size_t capacity_bytes,
	const int16_t *samples, size_t component_count)
{
	size_t i;
	if (component_count > RTLSTREAM_MAX_IQ_COMPONENTS || (component_count & 1) ||
	    capacity_bytes < component_count * 2 || (!dst && component_count) ||
	    (!samples && component_count))
		return -1;
	for (i = 0; i < component_count; i++) {
		uint16_t value = (uint16_t)samples[i];
		dst[2 * i] = (uint8_t)(value >> 8);
		dst[2 * i + 1] = (uint8_t)value;
	}
	return 0;
}

int rtlsdr_stream_encode_fft_payload(uint8_t *dst, size_t capacity_bytes,
	const float *power, size_t bin_count)
{
	size_t i;
	if (!dst || !power || bin_count < 2 || bin_count > RTLSTREAM_MAX_FFT_BINS ||
	    capacity_bytes < bin_count * 4 || sizeof(float) != 4 ||
	    FLT_RADIX != 2 || FLT_MANT_DIG != 24 || FLT_MAX_EXP != 128)
		return -1;
	for (i = 0; i < bin_count; i++) {
		uint32_t bits;
		memcpy(&bits, power + i, sizeof(bits));
		put32(dst + 4 * i, bits);
	}
	return 0;
}
