#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

#include "client.h"

static volatile int running = 1;

static void handle_sigint(int sig)
{
	(void)sig;
	running = 0;
}

int main(int argc, char **argv)
{
	rtlsdr_stream_ctx *ctx;
	struct rtlsdr_stream_iq_hdr hdr;
	int16_t samples[4096];

	if (argc < 4) {
		fprintf(stderr, "Usage: %s <host> <port> <freq_hz>\n", argv[0]);
		return 1;
	}

	signal(SIGINT, handle_sigint);

	ctx = rtlsdr_stream_connect(argv[1], atoi(argv[2]));
	if (!ctx) {
		fprintf(stderr, "Failed to connect\n");
		return 1;
	}

	uint64_t freq = (uint64_t)atoll(argv[3]);

	if (rtlsdr_stream_request(ctx, freq, 2400000ULL, 200000ULL, 0) < 0) {
		fprintf(stderr, "Failed to send request\n");
		rtlsdr_stream_close(ctx);
		return 1;
	}

	printf("Connected to rtl_stream at %s:%s, freq=%lu Hz\n",
	       argv[1], argv[2], (unsigned long)freq);
	printf("Receiving I/Q frames (Ctrl+C to stop)...\n");

	while (running) {
		if (rtlsdr_stream_read_iq(ctx, &hdr, samples, 2048) < 0)
			break;
		printf("I/Q frame: freq=%lu rate=%lu seq=%lu "
		       "sample[0]=(%d,%d)\n",
		       (unsigned long)hdr.freq,
		       (unsigned long)hdr.rate,
		       (unsigned long)hdr.seq,
		       (int)samples[0], (int)samples[1]);
	}

	rtlsdr_stream_close(ctx);
	printf("\nDisconnected.\n");
	return 0;
}
