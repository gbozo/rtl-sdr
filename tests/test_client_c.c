#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <time.h>
#include "../stream_client/c/client.h"
#include "fixtures/peer.h"

static rtlsdr_stream_ctx *open_peer(struct peer peer, int fft)
{
	rtlsdr_stream_ctx *ctx = rtlsdr_stream_connect("127.0.0.1", peer.port);
	assert(ctx);
	assert(rtlsdr_stream_request(ctx, fft ? 0 : GOLD_FREQ,
		fft ? 0 : 240000, fft ? 0 : 192000, (uint8_t)fft) == 0);
	assert(rtlsdr_stream_request(ctx, 0, 0, 0, 1) == -1);
	return ctx;
}

static void valid_frames(int fft)
{
	unsigned char data[512];
	size_t n = fixture(fft ? "fft.hex" : "iq.hex", data);
	struct peer peer;
	rtlsdr_stream_ctx *ctx;
	struct rtlsdr_stream_iq_hdr iq;
	struct rtlsdr_stream_fft_hdr fh;
	int16_t samples[6];
	float power[4];
	n += fixture("frequency.hex", data + n);
	n += fixture(fft ? "fft.hex" : "iq.hex", data + n);
	n += fixture(fft ? "fft.hex" : "iq.hex", data + n);
	n += fixture("end.hex", data + n);
	peer = start_peer(data, n, fft ? "request_fft.hex" : "request_iq.hex", 1);
	ctx = open_peer(peer, fft);
#define READ(cap) (fft ? rtlsdr_stream_read_fft(ctx, &fh, power, cap) : rtlsdr_stream_read_iq(ctx, &iq, samples, cap))
	assert(READ(2) == RTLSTREAM_ERROR_CAPACITY);
	assert(READ(2) == RTLSTREAM_READ_EVENT);
	assert(rtlsdr_stream_last_event(ctx)->type == RTLSTREAM_EVT_FREQ_CHANGE);
	assert(rtlsdr_stream_last_event(ctx)->freq == GOLD_FREQ);
	assert(READ(fft ? 4 : 6) == 0);
	assert(READ(fft ? 4 : 6) == 0);
	if (fft) {
		assert(fh.freq == GOLD_FREQ && fh.rate == 2400000 && fh.seq == GOLD_SEQ && fh.bins == 4);
		assert(power[0] == 0 && power[1] == 1 && power[2] == -2.5f && power[3] == .5f);
	} else {
		const int16_t expected[] = {-32768, -1, 0, 1, 256, 32767};
		assert(iq.freq == GOLD_FREQ && iq.rate == 2400000 && iq.seq == GOLD_SEQ && iq.nsamples == 6);
		assert(memcmp(samples, expected, sizeof(expected)) == 0);
	}
	assert(READ(2) == RTLSTREAM_READ_EVENT);
	assert(rtlsdr_stream_last_event(ctx)->type == RTLSTREAM_EVT_STREAM_END);
	assert(READ(2) == RTLSTREAM_ERROR);
#undef READ
	rtlsdr_stream_close(ctx);
	finish_peer(peer);
}

static void invalid_frame(int fft, int kind)
{
	unsigned char data[64];
	size_t n = fixture(fft ? "fft.hex" : "iq.hex", data);
	struct peer peer;
	rtlsdr_stream_ctx *ctx;
	struct rtlsdr_stream_iq_hdr iq;
	struct rtlsdr_stream_fft_hdr fh;
	int16_t samples[6];
	float power[4];
	if (kind == 0) data[2] = 0x53;
	if (kind == 1) memset(data + 28, 0xff, 4);
	if (kind == 2) { memset(data + 28, 0, 4); data[31] = 1; }
	if (kind == 3) n = 17;
	if (kind == 4) n--;
	if (kind == 5) { n = fixture("frequency.hex", data); data[7] = 99; }
	if (kind == 6) { n = fixture("frequency.hex", data) - 1; }
	peer = start_peer(data, n, fft ? "request_fft.hex" : "request_iq.hex", 0);
	ctx = open_peer(peer, fft);
	assert((fft ? rtlsdr_stream_read_fft(ctx, &fh, power, 4) :
		rtlsdr_stream_read_iq(ctx, &iq, samples, 6)) == RTLSTREAM_ERROR);
	assert(rtlsdr_stream_read_iq(ctx, &iq, samples, 6) == RTLSTREAM_ERROR);
	rtlsdr_stream_close(ctx);
	finish_peer(peer);
}

int main(void)
{
	int fft, kind;
	alarm(15);
	for (fft = 0; fft < 2; fft++) {
		valid_frames(fft);
		for (kind = 0; kind < 7; kind++) invalid_frame(fft, kind);
	}
	puts("C client fixtures passed");
	return 0;
}
