#ifndef RTLSTREAM_CLIENT_H
#define RTLSTREAM_CLIENT_H

#include <stdint.h>

#define RTLSTREAM_MAGIC_REQ   0x52543252U
#define RTLSTREAM_MAGIC_IQ    0x52543249U
#define RTLSTREAM_MAGIC_FFT   0x52543246U
#define RTLSTREAM_MAGIC_EVT   0x52543245U

#define RTLSTREAM_MODE_IQ     0
#define RTLSTREAM_MODE_FFT    1

#define RTLSTREAM_REQ_SIZE      29
#define RTLSTREAM_IQ_HDR_SIZE   32
#define RTLSTREAM_FFT_HDR_SIZE  32
#define RTLSTREAM_EVT_HDR_SIZE  8
#define RTLSTREAM_EVT_FREQ_SIZE 16
#define RTLSTREAM_EVT_FREQ_CHANGE 1
#define RTLSTREAM_EVT_STREAM_END 2
#define RTLSTREAM_MAX_IQ_COMPONENTS 262144U
#define RTLSTREAM_MAX_FFT_BINS 65536U

#define RTLSTREAM_READ_OK 0
#define RTLSTREAM_READ_EVENT 1
#define RTLSTREAM_ERROR -1
#define RTLSTREAM_ERROR_CAPACITY -2

struct rtlsdr_stream_event {
	uint32_t type;
	uint64_t freq;
};

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
const struct rtlsdr_stream_event *rtlsdr_stream_last_event(const rtlsdr_stream_ctx *ctx);

int rtlsdr_stream_request(rtlsdr_stream_ctx *ctx,
	uint64_t freq, uint64_t rate, uint64_t bandwidth, uint8_t mode);

/* Capacity is scalar components (I and Q separately), not complex samples.
 * A legal oversized frame is fully drained and returns ERROR_CAPACITY.
 * READ_EVENT leaves hdr/payload untouched; inspect last_event instead.
 * Protocol/I/O errors close the underlying connection. Not thread-safe. */
int rtlsdr_stream_read_iq(rtlsdr_stream_ctx *ctx,
	struct rtlsdr_stream_iq_hdr *hdr, int16_t *samples, int max_components);

int rtlsdr_stream_read_fft(rtlsdr_stream_ctx *ctx,
	struct rtlsdr_stream_fft_hdr *hdr, float *power, int max_bins);

#endif
