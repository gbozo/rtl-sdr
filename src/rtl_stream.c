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
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>

#include "rtl-sdr.h"
#include "rtl_ring.h"
#include "rtl_stream_server.h"
#include "fft/fft_backend.h"

#define RING_SLOT_COUNT  512
#define RING_SLOT_SIZE   (256 * 1024)

static volatile sig_atomic_t do_exit;
static volatile sig_atomic_t signal_write_fd = -1;
static atomic_int capture_done;
static atomic_int server_done;
static int capture_result;
static int server_result;
static rtlsdr_dev_t *g_dev = NULL;
static struct rtlsdr_server g_server;

static void wake_main(void)
{
	uint8_t byte = 1;
	int saved = errno;
	if (signal_write_fd >= 0)
		(void)write(signal_write_fd, &byte, 1);
	errno = saved;
}

static void sighandler(int signum)
{
	(void)signum;
	do_exit = 1;
	wake_main();
}

static void rtlsdr_callback(unsigned char *buf, uint32_t len, void *ctx)
{
	struct rtlsdr_server *srv = (struct rtlsdr_server *)ctx;
	if (!atomic_load(&srv->stopping))
		rtlsdr_server_publish(srv, buf, len);
}

static void *async_reader_thread(void *arg)
{
	rtlsdr_dev_t *dev = (rtlsdr_dev_t *)arg;
	capture_result = rtlsdr_read_async(dev, rtlsdr_callback, &g_server, 0, RING_SLOT_SIZE);
	atomic_store(&capture_done, 1);
	wake_main();
	return NULL;
}

static void *server_thread(void *arg)
{
	(void)arg;
	server_result = rtlsdr_server_run(&g_server);
	atomic_store(&server_done, 1);
	wake_main();
	return NULL;
}

static double parse_freq(const char *arg)
{
	char *end;
	double val = strtod(arg, &end);
	if (end == arg)
		return NAN;
	if (*end) {
		switch (*end) {
		case 'k': case 'K': val *= 1e3;  break;
		case 'm': case 'M': val *= 1e6;  break;
		case 'g': case 'G': val *= 1e9;  break;
		default: return NAN;
		}
		end++;
	}
	return *end ? NAN : val;
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
	pthread_t listener_thread;
	int signal_pipe[2] = {-1, -1};
	int initialized = 0, capture_started = 0, server_started = 0;
	int result = 1, retuned = 0;
	uint32_t actual_rate, actual_freq;
	struct sigaction action;
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

	if (!isfinite(freq) || freq <= 0 || freq > UINT32_MAX ||
	    !isfinite(sample_rate) || sample_rate <= 0 || sample_rate > UINT32_MAX ||
	    iq_port < 1 || iq_port > 65535 || fft_port < 0 || fft_port > 65535 || dev_index < 0)
		usage();

	if (pipe(signal_pipe) < 0 ||
	    fcntl(signal_pipe[0], F_SETFL, O_NONBLOCK) < 0 ||
	    fcntl(signal_pipe[1], F_SETFL, O_NONBLOCK) < 0)
		goto cleanup;
	signal_write_fd = signal_pipe[1];
	memset(&action, 0, sizeof(action));
	action.sa_handler = sighandler;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGINT, &action, NULL) < 0 || sigaction(SIGTERM, &action, NULL) < 0)
		goto cleanup;

	ring = rtlsdr_ring_create(RING_SLOT_COUNT, RING_SLOT_SIZE);
	if (!ring) {
		fprintf(stderr, "Failed to create ring buffer\n");
		goto cleanup;
	}

	if (rtlsdr_open(&g_dev, (uint32_t)dev_index) < 0) {
		fprintf(stderr, "Failed to open device %d\n", dev_index);
		goto cleanup;
	}

	if (rtlsdr_set_tuner_gain_mode(g_dev, 1) < 0 ||
	    rtlsdr_set_center_freq(g_dev, (uint32_t)freq) < 0 ||
	    rtlsdr_set_sample_rate(g_dev, (uint32_t)sample_rate) < 0 ||
	    rtlsdr_reset_buffer(g_dev) < 0) {
		fprintf(stderr, "Failed to configure device\n");
		goto cleanup;
	}
	actual_rate = rtlsdr_get_sample_rate(g_dev);
	actual_freq = rtlsdr_get_center_freq(g_dev);
	if (!actual_rate || !actual_freq) {
		fprintf(stderr, "Failed to read device configuration\n");
		goto cleanup;
	}
	if (rtlsdr_server_init(&g_server, iq_port, fft_port, ring, actual_freq, actual_rate) < 0) {
		fprintf(stderr, "Failed to initialize server\n");
		goto cleanup;
	}
	initialized = 1;

	fprintf(stderr, "rtl_stream: freq=%.0f rate=%.0f iq_port=%d fft_port=%d dev=%d\n",
		(double)actual_freq, (double)actual_rate, iq_port, fft_port, dev_index);
	fprintf(stderr, "rtl_stream: FFT backend: %s\n", fft_backend_name());

	atomic_init(&capture_done, 0);
	atomic_init(&server_done, 0);
	if (do_exit || pthread_create(&listener_thread, NULL, server_thread, NULL))
		goto cleanup;
	server_started = 1;
	if (pthread_create(&async_thread, NULL, async_reader_thread, g_dev))
		goto cleanup;
	capture_started = 1;
	result = 0;
	while (!do_exit && !atomic_load(&capture_done) && !atomic_load(&server_done)) {
		struct pollfd pfd = {signal_pipe[0], POLLIN, 0};
		int n = poll(&pfd, 1, 1000);
		if (n < 0 && errno != EINTR) {
			result = 1;
			break;
		}
		if (n > 0) {
			uint8_t buf[64];
			while (read(signal_pipe[0], buf, sizeof(buf)) > 0) {}
		}
		if (!retuned && !do_exit) {
			uint32_t current = rtlsdr_get_center_freq(g_dev);
			if (current && current != actual_freq) {
				fprintf(stderr, "rtl_stream: frequency changed; session terminated\n");
				rtlsdr_server_notify_frequency_change(&g_server, current);
				retuned = 1;
			}
		}
	}

cleanup:
	if (initialized)
		rtlsdr_server_request_stop(&g_server);
	if (capture_started) {
		/* Retry cancellation if shutdown raced entry into read_async(). */
		while (!atomic_load(&capture_done)) {
			rtlsdr_cancel_async(g_dev);
			poll(NULL, 0, 20);
		}
		pthread_join(async_thread, NULL);
		if (capture_result < 0)
			result = 1;
	}
	if (server_started) {
		pthread_join(listener_thread, NULL);
		if (server_result < 0)
			result = 1;
	}
	if (initialized)
		rtlsdr_server_shutdown(&g_server);
	if (g_dev)
		rtlsdr_close(g_dev);
	rtlsdr_ring_destroy(ring);
	signal_write_fd = -1;
	if (signal_pipe[0] >= 0)
		close(signal_pipe[0]);
	if (signal_pipe[1] >= 0)
		close(signal_pipe[1]);
	return result;
}
