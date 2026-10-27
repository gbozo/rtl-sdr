#ifndef RTL_STREAM_PROTO_H
#define RTL_STREAM_PROTO_H

#include <stdint.h>
#include <stddef.h>

#define RTLSTREAM_MAGIC_REQ   0x52543252U  /* "RT2R" */
#define RTLSTREAM_MAGIC_IQ    0x52543249U  /* "RT2I" */
#define RTLSTREAM_MAGIC_FFT   0x52543246U  /* "RT2F" */
#define RTLSTREAM_MAGIC_EVT   0x52543245U  /* "RT2E" */

#define RTLSTREAM_MODE_IQ  0
#define RTLSTREAM_MODE_FFT 1

#define RTLSTREAM_REQ_SIZE       29
#define RTLSTREAM_IQ_HDR_SIZE    32
#define RTLSTREAM_FFT_HDR_SIZE   32
#define RTLSTREAM_EVT_HDR_SIZE    8
#define RTLSTREAM_EVT_FREQ_SIZE  16

#define RTLSTREAM_EVT_FREQ_CHANGE 1
#define RTLSTREAM_EVT_STREAM_END  2

#define RTLSTREAM_MAX_IQ_COMPONENTS 262144U
#define RTLSTREAM_MAX_FFT_BINS       65536U

struct rtlsdr_stream_req {
	uint32_t magic;
	uint64_t freq;
	uint64_t rate;
	uint64_t bandwidth;
	uint8_t  mode;
};

/* One request per connection; FFT requires freq, rate and bandwidth all zero.
 * Version 1 and unknown modes are rejected by the request decoder. */

struct rtlsdr_stream_iq_hdr {
	uint32_t magic;
	uint64_t freq;
	uint64_t rate;
	uint64_t seq;
	uint32_t nsamples; /* int16_t components (I and Q counted separately) */
};

struct rtlsdr_stream_fft_hdr {
	uint32_t magic;
	uint64_t freq;
	uint64_t rate;
	uint64_t seq;
	uint32_t bins;
};

int rtlsdr_stream_encode_req(unsigned char *buf, const struct rtlsdr_stream_req *req);
int rtlsdr_stream_decode_req(struct rtlsdr_stream_req *req, const unsigned char *buf);

int rtlsdr_stream_encode_iq_hdr(unsigned char *buf, const struct rtlsdr_stream_iq_hdr *hdr);
int rtlsdr_stream_encode_fft_hdr(unsigned char *buf, const struct rtlsdr_stream_fft_hdr *hdr);

int rtlsdr_stream_encode_evt(unsigned char *buf, uint32_t type, uint64_t freq);

/* RTLSTREAM/2: all fields and payloads are big-endian. Data count is at
 * offset 28, payload at 32. I/Q counts scalar components and must be even.
 * FFT rate is the capture rate; payload is IEEE-754 binary32.
 * Payload buffers must not overlap. Encoders return 0 on success, -1 on
 * invalid count, insufficient byte capacity, or invalid pointers. */
int rtlsdr_stream_encode_iq_payload(uint8_t *dst, size_t capacity_bytes,
	const int16_t *samples, size_t component_count);
int rtlsdr_stream_encode_fft_payload(uint8_t *dst, size_t capacity_bytes,
	const float *power, size_t bin_count);

#endif
