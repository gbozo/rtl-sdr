#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <errno.h>
#include <float.h>

#include "client.h"

struct rtlsdr_stream_ctx {
	int fd;
	int requested;
	struct rtlsdr_stream_event event;
};

static int recv_all(int fd, void *buf, size_t len)
{
	unsigned char *p = (unsigned char *)buf;
	size_t left = len;
	while (left > 0) {
		ssize_t n = recv(fd, p, left, 0);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		p += n;
		left -= (size_t)n;
	}
	return 0;
}

static int send_all(int fd, const void *buf, size_t len)
{
	const unsigned char *p = (const unsigned char *)buf;
	size_t left = len;
	while (left > 0) {
		ssize_t n;
#ifdef MSG_NOSIGNAL
		n = send(fd, p, left, MSG_NOSIGNAL);
#else
		n = send(fd, p, left, 0);
#endif
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		p += n;
		left -= (size_t)n;
	}
	return 0;
}

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

rtlsdr_stream_ctx *rtlsdr_stream_connect(const char *host, int port)
{
	rtlsdr_stream_ctx *ctx;
	struct addrinfo hints, *addresses, *address;
	char service[6];

	if (!host || port < 1 || port > 65535)
		return NULL;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	snprintf(service, sizeof(service), "%d", port);
	if (getaddrinfo(host, service, &hints, &addresses) != 0)
		return NULL;

	ctx = (rtlsdr_stream_ctx *)calloc(1, sizeof(*ctx));
	if (!ctx) {
		freeaddrinfo(addresses);
		return NULL;
	}
	ctx->fd = -1;
	for (address = addresses; address; address = address->ai_next) {
		ctx->fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
		if (ctx->fd < 0)
			continue;
#ifdef SO_NOSIGPIPE
		{
			int enabled = 1;
			setsockopt(ctx->fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
		}
#endif
		if (connect(ctx->fd, address->ai_addr, address->ai_addrlen) == 0)
			break;
		close(ctx->fd);
		ctx->fd = -1;
	}
	freeaddrinfo(addresses);
	if (ctx->fd < 0) {
		free(ctx);
		return NULL;
	}

	return ctx;
}

void rtlsdr_stream_close(rtlsdr_stream_ctx *ctx)
{
	if (!ctx)
		return;
	if (ctx->fd >= 0)
		close(ctx->fd);
	free(ctx);
}

int rtlsdr_stream_request(rtlsdr_stream_ctx *ctx,
	uint64_t freq, uint64_t rate, uint64_t bandwidth, uint8_t mode)
{
	unsigned char buf[29];
	if (!ctx || ctx->fd < 0 || ctx->requested ||
	    (mode != RTLSTREAM_MODE_IQ && mode != RTLSTREAM_MODE_FFT) ||
	    (mode == RTLSTREAM_MODE_FFT && (freq || rate || bandwidth)))
		return -1;
	ctx->requested = 1;

	put32(buf, RTLSTREAM_MAGIC_REQ);
	put64(buf + 4,  freq);
	put64(buf + 12, rate);
	put64(buf + 20, bandwidth);
	buf[28] = mode;

	if (send_all(ctx->fd, buf, 29) == 0)
		return 0;
	close(ctx->fd);
	ctx->fd = -1;
	return -1;
}

static int fail(rtlsdr_stream_ctx *ctx)
{
	if (ctx->fd >= 0)
		close(ctx->fd);
	ctx->fd = -1;
	return RTLSTREAM_ERROR;
}

const struct rtlsdr_stream_event *rtlsdr_stream_last_event(const rtlsdr_stream_ctx *ctx)
{
	return ctx ? &ctx->event : NULL;
}

static int read_header(rtlsdr_stream_ctx *ctx, unsigned char *buf, uint32_t magic)
{
	uint32_t type;
	if (recv_all(ctx->fd, buf, 4) < 0)
		return fail(ctx);
	if (get32(buf) == RTLSTREAM_MAGIC_EVT) {
		if (recv_all(ctx->fd, buf + 4, 4) < 0)
			return fail(ctx);
		type = get32(buf + 4);
		if (type != RTLSTREAM_EVT_FREQ_CHANGE && type != RTLSTREAM_EVT_STREAM_END)
			return fail(ctx);
		ctx->event.type = type;
		ctx->event.freq = 0;
		if (type == RTLSTREAM_EVT_FREQ_CHANGE) {
			if (recv_all(ctx->fd, buf + 8, 8) < 0)
				return fail(ctx);
			ctx->event.freq = get64(buf + 8);
		} else {
			fail(ctx);
		}
		return RTLSTREAM_READ_EVENT;
	}
	if (get32(buf) != magic || recv_all(ctx->fd, buf + 4, 28) < 0)
		return fail(ctx);
	return RTLSTREAM_READ_OK;
}

static int drain(rtlsdr_stream_ctx *ctx, size_t bytes)
{
	unsigned char buf[1024];
	while (bytes) {
		size_t n = bytes < sizeof(buf) ? bytes : sizeof(buf);
		if (recv_all(ctx->fd, buf, n) < 0)
			return fail(ctx);
		bytes -= n;
	}
	return RTLSTREAM_ERROR_CAPACITY;
}

int rtlsdr_stream_read_iq(rtlsdr_stream_ctx *ctx,
	struct rtlsdr_stream_iq_hdr *hdr, int16_t *samples, int max_components)
{
	unsigned char buf[32];
	uint32_t i;
	int result;
	if (!ctx || !hdr || max_components < 0 || (!samples && max_components))
		return RTLSTREAM_ERROR;
	result = read_header(ctx, buf, RTLSTREAM_MAGIC_IQ);
	if (result != RTLSTREAM_READ_OK)
		return result;
	hdr->magic = get32(buf);
	hdr->freq = get64(buf + 4);
	hdr->rate = get64(buf + 12);
	hdr->seq = get64(buf + 20);
	hdr->nsamples = get32(buf + 28);
	if (hdr->nsamples > RTLSTREAM_MAX_IQ_COMPONENTS || (hdr->nsamples & 1))
		return fail(ctx);
	if (hdr->nsamples > (uint32_t)max_components)
		return drain(ctx, (size_t)hdr->nsamples * 2);
	if (recv_all(ctx->fd, samples, (size_t)hdr->nsamples * 2) < 0)
		return fail(ctx);
	for (i = 0; i < hdr->nsamples; i++) {
		unsigned char *p = (unsigned char *)(samples + i);
		uint32_t value = ((uint32_t)p[0] << 8) | p[1];
		samples[i] = (int16_t)(value < 32768 ? (int32_t)value : (int32_t)value - 65536);
	}
	return RTLSTREAM_READ_OK;
}

int rtlsdr_stream_read_fft(rtlsdr_stream_ctx *ctx,
	struct rtlsdr_stream_fft_hdr *hdr, float *power, int max_bins)
{
	unsigned char buf[32];
	uint32_t i;
	int result;
	if (!ctx || !hdr || max_bins < 0 || (!power && max_bins) ||
	    sizeof(float) != 4 || FLT_RADIX != 2 || FLT_MANT_DIG != 24 || FLT_MAX_EXP != 128)
		return RTLSTREAM_ERROR;
	result = read_header(ctx, buf, RTLSTREAM_MAGIC_FFT);
	if (result != RTLSTREAM_READ_OK)
		return result;
	hdr->magic = get32(buf);
	hdr->freq = get64(buf + 4);
	hdr->rate = get64(buf + 12);
	hdr->seq = get64(buf + 20);
	hdr->bins = get32(buf + 28);
	if (hdr->bins < 2 || hdr->bins > RTLSTREAM_MAX_FFT_BINS)
		return fail(ctx);
	if (hdr->bins > (uint32_t)max_bins)
		return drain(ctx, (size_t)hdr->bins * 4);
	if (recv_all(ctx->fd, power, (size_t)hdr->bins * 4) < 0)
		return fail(ctx);
	for (i = 0; i < hdr->bins; i++) {
		uint32_t bits = get32((unsigned char *)(power + i));
		memcpy(power + i, &bits, sizeof(bits));
	}
	return RTLSTREAM_READ_OK;
}
