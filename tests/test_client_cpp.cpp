#include <ctime>
#include "../stream_client/cpp/client.hpp"
#include "fixtures/peer.h"

static void request(rtlsdr::StreamClient &client, struct peer peer, bool fft)
{
	client.connect("127.0.0.1", peer.port);
	client.request(fft ? 0 : GOLD_FREQ, fft ? 0 : 240000, fft ? 0 : 192000, fft ? 1 : 0);
	bool rejected = false;
	try { client.request(0, 0, 0, 1); } catch (const std::invalid_argument &) { rejected = true; }
	assert(rejected);
}

static void valid_frames(bool fft)
{
	unsigned char data[512];
	size_t n = fixture(fft ? "fft.hex" : "iq.hex", data);
	n += fixture("frequency.hex", data + n);
	n += fixture(fft ? "fft.hex" : "iq.hex", data + n);
	n += fixture("end.hex", data + n);
	struct peer peer = start_peer(data, n, fft ? "request_fft.hex" : "request_iq.hex", 1);
	rtlsdr::StreamClient client;
	request(client, peer, fft);
	std::vector<int16_t> samples;
	std::vector<float> power;
	for (int i = 0; i < 4; i++) {
		bool event = false;
		try {
			if (fft) {
				auto hdr = client.readFFT(power);
				assert(hdr.freq == GOLD_FREQ && hdr.seq == GOLD_SEQ && hdr.rate == 2400000 && hdr.bins == 4);
				assert((power == std::vector<float>{0, 1, -2.5f, .5f}));
			} else {
				auto hdr = client.readIQ(samples);
				assert(hdr.freq == GOLD_FREQ && hdr.seq == GOLD_SEQ && hdr.rate == 2400000 && hdr.nsamples == 6);
				assert((samples == std::vector<int16_t>{-32768, -1, 0, 1, 256, 32767}));
			}
		} catch (const rtlsdr::StreamEvent &e) {
			event = true;
			assert(e.type == (i == 1 ? rtlsdr::EventType::FrequencyChange : rtlsdr::EventType::StreamEnd));
			assert(e.freq == (i == 1 ? GOLD_FREQ : 0));
		}
		assert(event == (i == 1 || i == 3));
	}
	finish_peer(peer);
}

static void invalid_frame(bool fft, int kind)
{
	unsigned char data[64];
	size_t n = fixture(fft ? "fft.hex" : "iq.hex", data);
	if (kind == 0) data[2] = 0x53;
	if (kind == 1) memset(data + 28, 0xff, 4);
	if (kind == 2) { memset(data + 28, 0, 4); data[31] = 1; }
	if (kind == 3) n = 17;
	if (kind == 4) n--;
	if (kind == 5) { n = fixture("frequency.hex", data); data[7] = 99; }
	if (kind == 6) n = fixture("frequency.hex", data) - 1;
	struct peer peer = start_peer(data, n, fft ? "request_fft.hex" : "request_iq.hex", 0);
	rtlsdr::StreamClient client;
	request(client, peer, fft);
	std::vector<int16_t> samples;
	std::vector<float> power;
	for (int i = 0; i < 2; i++) {
		bool failed = false;
		try { if (fft) client.readFFT(power); else client.readIQ(samples); }
		catch (const std::runtime_error &) { failed = true; }
		assert(failed);
	}
	finish_peer(peer);
}

int main()
{
	alarm(15);
	for (int fft = 0; fft < 2; fft++) {
		valid_frames(fft != 0);
		for (int kind = 0; kind < 7; kind++) invalid_frame(fft != 0, kind);
	}
	puts("C++ client fixtures passed");
}
