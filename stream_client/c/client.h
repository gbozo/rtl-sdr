#ifndef RTLSTREAM_CLIENT_H
#define RTLSTREAM_CLIENT_H

#include <stdint.h>

#define RTLSTREAM_MAGIC_REQ   0x52545352U
#define RTLSTREAM_MAGIC_IQ    0x52545349U
#define RTLSTREAM_MAGIC_FFT   0x52545346U
#define RTLSTREAM_MAGIC_EVT   0x52545345U

#define RTLSTREAM_MODE_IQ     0
#define RTLSTREAM_MODE_FFT    1

#define RTLSTREAM_REQ_SIZE      29
#define RTLSTREAM_IQ_HDR_SIZE   32
#define RTLSTREAM_FFT_HDR_SIZE  32

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

struct rtlsdr_stream_ctx;
typedef struct rtlsdr_stream_ctx rtlsdr_stream_ctx;

rtlsdr_stream_ctx *rtlsdr_stream_connect(const char *host, int port);
void rtlsdr_stream_close(rtlsdr_stream_ctx *ctx);

int rtlsdr_stream_request(rtlsdr_stream_ctx *ctx,
	uint64_t freq, uint64_t rate, uint64_t bandwidth, uint8_t mode);

int rtlsdr_stream_read_iq(rtlsdr_stream_ctx *ctx,
	struct rtlsdr_stream_iq_hdr *hdr, int16_t *samples, int max_samples);

int rtlsdr_stream_read_fft(rtlsdr_stream_ctx *ctx,
	struct rtlsdr_stream_fft_hdr *hdr, float *power, int max_bins);

#endif
