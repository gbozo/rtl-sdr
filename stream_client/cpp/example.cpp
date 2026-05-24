#include <iostream>
#include <csignal>
#include <cstdlib>
#include "client.hpp"

static volatile bool running = true;

extern "C" void handle_sigint(int)
{
	running = false;
}

int main(int argc, char **argv)
{
	if (argc < 4) {
		std::cerr << "Usage: " << argv[0]
		          << " <host> <port> <freq_hz>\n";
		return 1;
	}

	std::signal(SIGINT, handle_sigint);

	try {
		rtlsdr::StreamClient client;
		client.connect(argv[1], std::atoi(argv[2]));

		uint64_t freq = std::atoll(argv[3]);
		client.request(freq, 2400000ULL, 200000ULL,
		               rtlsdr::RTLSTREAM_MODE_IQ);

		std::cout << "Connected to rtl_stream at " << argv[1]
		          << ":" << argv[2]
		          << ", freq=" << freq << " Hz\n";
		std::cout << "Receiving I/Q frames (Ctrl+C to stop)...\n";

		std::vector<int16_t> samples(4096);

		while (running) {
			auto hdr = client.readIQ(samples);
			std::cout << "I/Q frame: freq=" << hdr.freq
			          << " rate=" << hdr.rate
			          << " seq=" << hdr.seq
			          << " sample[0]=("
			          << samples[0] << ","
			          << samples[1] << ")\n";
		}
	} catch (const std::exception &e) {
		std::cerr << "Error: " << e.what() << "\n";
		return 1;
	}

	std::cout << "\nDisconnected.\n";
	return 0;
}
