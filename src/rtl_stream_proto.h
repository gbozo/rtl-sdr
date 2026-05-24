#ifndef RTL_STREAM_PROTO_H
#define RTL_STREAM_PROTO_H

#include <stdint.h>

#define RTLSTREAM_MAGIC_REQ   0x52545352U  /* "RTSR" */
#define RTLSTREAM_MAGIC_IQ    0x52545349U  /* "RTSI" */
#define RTLSTREAM_MAGIC_FFT   0x52545346U  /* "RTSF" */
#define RTLSTREAM_MAGIC_EVT   0x52545345U  /* "RTSE" */

#define RTLSTREAM_MODE_IQ  0
#define RTLSTREAM_MODE_FFT 1

#define RTLSTREAM_REQ_SIZE       29
#define RTLSTREAM_IQ_HDR_SIZE    32
#define RTLSTREAM_FFT_HDR_SIZE   32
#define RTLSTREAM_EVT_HDR_SIZE    8
#define RTLSTREAM_EVT_FREQ_SIZE  16

#define RTLSTREAM_EVT_FREQ_CHANGE 1
#define RTLSTREAM_EVT_STREAM_END  2

struct rtlsdr_stream_req {
	uint32_t magic;
	uint64_t freq;
	uint64_t rate;
	uint64_t bandwidth;
	uint8_t  mode;
};

struct rtlsdr_stream_iq_hdr {
	uint32_t magic;
	uint64_t freq;
	uint64_t rate;
	uint64_t seq;
	uint32_t nsamples;
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

#endif
