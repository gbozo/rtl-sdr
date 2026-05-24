#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#include "client.h"

struct rtlsdr_stream_ctx {
	int fd;
};

static int recv_all(int fd, void *buf, size_t len)
{
	unsigned char *p = (unsigned char *)buf;
	size_t left = len;
	while (left > 0) {
		ssize_t n = recv(fd, p, left, 0);
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
		ssize_t n = send(fd, p, left, 0);
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
	struct hostent *he;
	struct sockaddr_in addr;

	he = gethostbyname(host);
	if (!he)
		return NULL;

	ctx = (rtlsdr_stream_ctx *)calloc(1, sizeof(*ctx));
	if (!ctx)
		return NULL;

	ctx->fd = socket(AF_INET, SOCK_STREAM, 0);
	if (ctx->fd < 0) {
		free(ctx);
		return NULL;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);
	memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

	if (connect(ctx->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(ctx->fd);
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

	put32(buf, RTLSTREAM_MAGIC_REQ);
	put64(buf + 4,  freq);
	put64(buf + 12, rate);
	put64(buf + 20, bandwidth);
	buf[28] = mode;

	return send_all(ctx->fd, buf, 29) == 0 ? 0 : -1;
}

int rtlsdr_stream_read_iq(rtlsdr_stream_ctx *ctx,
	struct rtlsdr_stream_iq_hdr *hdr, int16_t *samples, int max_samples)
{
	unsigned char hbuf[32];
	int n;

	if (recv_all(ctx->fd, hbuf, 32) < 0)
		return -1;

	hdr->magic    = get32(hbuf);
	hdr->freq     = get64(hbuf + 4);
	hdr->rate     = get64(hbuf + 12);
	hdr->seq      = get64(hbuf + 20);
	hdr->nsamples = get32(hbuf + 28);

	if (hdr->magic != RTLSTREAM_MAGIC_IQ)
		return -1;

	n = (int)hdr->nsamples * (int)sizeof(int16_t);
	if (hdr->nsamples > (uint32_t)(max_samples * 2))
		n = max_samples * 2 * (int)sizeof(int16_t);
	if (recv_all(ctx->fd, samples, (size_t)n) < 0)
		return -1;

	return 0;
}

int rtlsdr_stream_read_fft(rtlsdr_stream_ctx *ctx,
	struct rtlsdr_stream_fft_hdr *hdr, float *power, int max_bins)
{
	unsigned char hbuf[32];
	int n;

	if (recv_all(ctx->fd, hbuf, 32) < 0)
		return -1;

	hdr->magic = get32(hbuf);
	hdr->freq  = get64(hbuf + 4);
	hdr->rate  = get64(hbuf + 12);
	hdr->seq   = get64(hbuf + 20);
	hdr->bins  = get32(hbuf + 28);

	if (hdr->magic != RTLSTREAM_MAGIC_FFT)
		return -1;

	n = (int)(hdr->bins <= (uint32_t)max_bins ? hdr->bins : (uint32_t)max_bins) * (int)sizeof(float);
	if (recv_all(ctx->fd, power, (size_t)n) < 0)
		return -1;

	return 0;
}
