/*
 * rtl_stream — channelized SDR server
 *
 * Captures full-bandwidth I/Q from an RTL-SDR dongle and serves
 * narrowband channel streams to multiple TCP clients.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <pthread.h>
#include <sys/time.h>

#include "rtl-sdr.h"
#include "rtl_ring.h"
#include "rtl_stream_server.h"

#define RING_SLOT_COUNT  512
#define RING_SLOT_SIZE   (256 * 1024)

volatile int do_exit = 0;
static rtlsdr_dev_t *g_dev = NULL;
struct rtlsdr_server g_server;

static void sighandler(int signum)
{
	(void)signum;
	do_exit = 1;
	if (g_dev)
		rtlsdr_cancel_async(g_dev);
}

static void rtlsdr_callback(unsigned char *buf, uint32_t len, void *ctx)
{
	struct rtlsdr_server *srv = (struct rtlsdr_server *)ctx;
	void *dest;

	if (do_exit || len == 0)
		return;

	dest = rtlsdr_ring_write_ptr(srv->ring);
	if (!dest)
		return;

	memcpy(dest, buf, len <= RING_SLOT_SIZE ? len : RING_SLOT_SIZE);
	rtlsdr_ring_publish(srv->ring, len);

	pthread_mutex_lock(&srv->data_mutex);
	pthread_cond_broadcast(&srv->data_cond);
	pthread_mutex_unlock(&srv->data_mutex);
}

static void *async_reader_thread(void *arg)
{
	rtlsdr_dev_t *dev = (rtlsdr_dev_t *)arg;
	rtlsdr_read_async(dev, rtlsdr_callback, &g_server, 0, 32);
	return NULL;
}

static void *freq_monitor(void *arg)
{
	(void)arg;
	while (!do_exit) {
		sleep(1);
		if (g_dev) {
			uint32_t current = rtlsdr_get_center_freq(g_dev);
			if (current != 0 &&
			    current != (uint32_t)g_server.capture_freq) {
				fprintf(stderr, "rtl_stream: freq changed "
					"from %u to %u, terminating streams\n",
					(uint32_t)g_server.capture_freq,
					current);
				g_server.freq_changed = 1;
				g_server.new_freq = current;
				do_exit = 1;
				rtlsdr_cancel_async(g_dev);
				pthread_mutex_lock(&g_server.data_mutex);
				pthread_cond_broadcast(&g_server.data_cond);
				pthread_mutex_unlock(&g_server.data_mutex);
				break;
			}
		}
	}
	return NULL;
}

static double parse_freq(const char *arg)
{
	char *end;
	double val = strtod(arg, &end);

	if (*end) {
		switch (*end) {
		case 'k': case 'K': val *= 1e3;  break;
		case 'm': case 'M': val *= 1e6;  break;
		case 'g': case 'G': val *= 1e9;  break;
		default: break;
		}
	}
	return val;
}

static void usage(void)
{
	fprintf(stderr,
		"rtl_stream, channelized SDR server\n\n"
		"Usage:\trtl_stream -f freq [-s rate] [-p port] [-P fft_port] [-i dev] [--list] [-h]\n\n"
		"Options:\n"
		"\t-f freq\t\tCenter frequency (Hz), supports suffix k/M/G\n"
		"\t-s rate\t\tSample rate (default: 2.4M)\n"
		"\t-p port\t\tI/Q TCP port (default: 1234)\n"
		"\t-P port\t\tFFT TCP port (default: 0 = disabled)\n"
		"\t-i dev\t\tDevice index (default: 0)\n"
		"\t--list\t\tList connected devices and exit\n"
		"\t-h\t\tShow this help\n");
	exit(1);
}

int main(int argc, char **argv)
{
	double freq = 0;
	double sample_rate = 2.4e6;
	int iq_port = 1234;
	int fft_port = 0;
	int dev_index = 0;
	int list_only = 0;
	int opt;
	pthread_t async_thread;
	pthread_t monitor_thread;
	struct rtlsdr_ring *ring = NULL;

	static struct option long_options[] = {
		{"list", no_argument, NULL, 'l'},
		{"help", no_argument, NULL, 'h'},
		{0, 0, 0, 0}
	};

	while ((opt = getopt_long(argc, argv, "f:s:p:P:i:lh", long_options, NULL)) != -1) {
		switch (opt) {
		case 'f':
			freq = parse_freq(optarg);
			break;
		case 's':
			sample_rate = parse_freq(optarg);
			break;
		case 'p':
			iq_port = atoi(optarg);
			break;
		case 'P':
			fft_port = atoi(optarg);
			break;
		case 'i':
			dev_index = atoi(optarg);
			break;
		case 'l':
			list_only = 1;
			break;
		case 'h':
		default:
			usage();
			break;
		}
	}

	if (list_only) {
		uint32_t i, count = rtlsdr_get_device_count();
		if (count == 0) {
			printf("No supported devices found.\n");
		} else {
			printf("Found %u device(s):\n\n", count);
			for (i = 0; i < count; i++) {
				char vendor[256] = "", product[256] = "", serial[256] = "";
				rtlsdr_get_device_usb_strings(i, vendor, product, serial);
				printf("  %u:  %s", i, rtlsdr_get_device_name(i));
				if ((int)i == dev_index)
					printf(" (default)");
				printf("\n");
				if (vendor[0])
					printf("       Manufacturer: %s\n", vendor);
				if (product[0])
					printf("       Product: %s\n", product);
				if (serial[0])
					printf("       Serial: %s\n", serial);
				printf("\n");
			}
		}
		printf("Configuration:\n");
		printf("  I/Q port:  %d\n", iq_port);
		printf("  FFT port:  %d", fft_port);
		if (fft_port == 0)
			printf(" (disabled)");
		printf("\n");
		printf("  Device:    %d\n", dev_index);
		return 0;
	}

	if (freq == 0)
		usage();

	signal(SIGINT,  sighandler);
	signal(SIGTERM, sighandler);
	signal(SIGPIPE, SIG_IGN);

	ring = rtlsdr_ring_create(RING_SLOT_COUNT, RING_SLOT_SIZE);
	if (!ring) {
		fprintf(stderr, "Failed to create ring buffer\n");
		return 1;
	}

	if (rtlsdr_server_init(&g_server, iq_port, fft_port, ring,
			       (uint64_t)freq, (uint64_t)sample_rate) < 0) {
		fprintf(stderr, "Failed to initialize server\n");
		rtlsdr_ring_destroy(ring);
		return 1;
	}

	if (rtlsdr_open(&g_dev, (uint32_t)dev_index) < 0) {
		fprintf(stderr, "Failed to open device %d\n", dev_index);
		goto cleanup;
	}

	rtlsdr_set_tuner_gain_mode(g_dev, 1);
	rtlsdr_set_center_freq(g_dev, (uint32_t)freq);
	rtlsdr_set_sample_rate(g_dev, (uint32_t)sample_rate);
	rtlsdr_reset_buffer(g_dev);

	fprintf(stderr, "rtl_stream: freq=%.0f rate=%.0f iq_port=%d fft_port=%d dev=%d\n",
		freq, sample_rate, iq_port, fft_port, dev_index);

	pthread_create(&async_thread, NULL, async_reader_thread, g_dev);
	pthread_create(&monitor_thread, NULL, freq_monitor, NULL);

	rtlsdr_server_run(&g_server);

	rtlsdr_cancel_async(g_dev);
	pthread_join(async_thread, NULL);
	pthread_join(monitor_thread, NULL);

	rtlsdr_close(g_dev);
	g_dev = NULL;

cleanup:
	rtlsdr_server_shutdown(&g_server);
	rtlsdr_ring_destroy(ring);
	return 0;
}
