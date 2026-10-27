#include "../src/rtl_stream_proto.h"
#include "fixtures/wire.h"

int main(void)
{
	unsigned char expected[128], actual[128];
	struct rtlsdr_stream_req req = {RTLSTREAM_MAGIC_REQ, GOLD_FREQ, 240000, 192000, RTLSTREAM_MODE_IQ};
	struct rtlsdr_stream_req decoded;
	struct rtlsdr_stream_iq_hdr iq = {RTLSTREAM_MAGIC_IQ, GOLD_FREQ, 2400000, GOLD_SEQ, 6};
	struct rtlsdr_stream_fft_hdr fft = {RTLSTREAM_MAGIC_FFT, GOLD_FREQ, 2400000, GOLD_SEQ, 4};
	const int16_t samples[] = {-32768, -1, 0, 1, 256, 32767};
	const float power[] = {0, 1, -2.5f, .5f};
	size_t n = fixture("request_iq.hex", expected);
	assert(rtlsdr_stream_encode_req(actual, &req) == 29);
	assert(memcmp(actual, expected, n) == 0);
	assert(rtlsdr_stream_decode_req(&decoded, actual) == 0);
	assert(decoded.freq == GOLD_FREQ && decoded.rate == 240000 && decoded.bandwidth == 192000);
	actual[2] = 0x53;
	assert(rtlsdr_stream_decode_req(&decoded, actual) == -1);
	req.freq = req.rate = req.bandwidth = 0;
	req.mode = RTLSTREAM_MODE_FFT;
	n = fixture("request_fft.hex", expected);
	assert(rtlsdr_stream_encode_req(actual, &req) == 29);
	assert(memcmp(actual, expected, n) == 0);
	req.freq = 1;
	assert(rtlsdr_stream_encode_req(actual, &req) == -1);
	actual[11] = 1;
	assert(rtlsdr_stream_decode_req(&decoded, actual) == -1);
	req.mode = 2;
	assert(rtlsdr_stream_encode_req(actual, &req) == -1);

	n = fixture("iq.hex", expected);
	assert(rtlsdr_stream_encode_iq_hdr(actual, &iq) == 32);
	assert(rtlsdr_stream_encode_iq_payload(actual + 32, 12, samples, 6) == 0);
	assert(memcmp(actual, expected, n) == 0);
	assert(rtlsdr_stream_encode_iq_payload(actual, 11, samples, 6) == -1);
	assert(rtlsdr_stream_encode_iq_payload(actual, sizeof(actual), samples, 3) == -1);
	assert(rtlsdr_stream_encode_iq_payload(actual, SIZE_MAX, samples, SIZE_MAX) == -1);
	assert(rtlsdr_stream_encode_iq_payload(NULL, 0, NULL, 0) == 0);
	iq.nsamples = 3;
	assert(rtlsdr_stream_encode_iq_hdr(actual, &iq) == -1);
	iq.nsamples = RTLSTREAM_MAX_IQ_COMPONENTS + 2;
	assert(rtlsdr_stream_encode_iq_hdr(actual, &iq) == -1);

	n = fixture("fft.hex", expected);
	assert(rtlsdr_stream_encode_fft_hdr(actual, &fft) == 32);
	assert(rtlsdr_stream_encode_fft_payload(actual + 32, 16, power, 4) == 0);
	assert(memcmp(actual, expected, n) == 0);
	assert(rtlsdr_stream_encode_fft_payload(actual, 15, power, 4) == -1);
	assert(rtlsdr_stream_encode_fft_payload(actual, sizeof(actual), power, 1) == -1);
	assert(rtlsdr_stream_encode_fft_payload(actual, SIZE_MAX, power, SIZE_MAX) == -1);
	fft.bins = 1;
	assert(rtlsdr_stream_encode_fft_hdr(actual, &fft) == -1);
	fft.bins = RTLSTREAM_MAX_FFT_BINS + 1;
	assert(rtlsdr_stream_encode_fft_hdr(actual, &fft) == -1);

	n = fixture("frequency.hex", expected);
	assert(rtlsdr_stream_encode_evt(actual, RTLSTREAM_EVT_FREQ_CHANGE, GOLD_FREQ) == 16);
	assert(memcmp(actual, expected, n) == 0);
	n = fixture("end.hex", expected);
	assert(rtlsdr_stream_encode_evt(actual, RTLSTREAM_EVT_STREAM_END, 0) == 8);
	assert(memcmp(actual, expected, n) == 0);
	assert(rtlsdr_stream_encode_evt(actual, 3, 0) == -1);
	puts("protocol fixtures passed");
	return 0;
}
