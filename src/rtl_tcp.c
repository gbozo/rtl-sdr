/*
 * rtl-sdr, turns your Realtek RTL2832 based DVB dongle into a SDR receiver
 * Copyright (C) 2012 by Steve Markgraf <steve@steve-m.de>
 * Copyright (C) 2012-2013 by Hoernchen <la@tfc-server.de>
 *
 * Multi-client support, ring buffer, and zero-copy enhancements
 * Copyright (C) 2024-2026
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>
#include <time.h>

#ifndef _WIN32
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#ifdef __linux__
#include <sys/sendfile.h>
#endif
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#include "getopt/getopt.h"
#endif

#include <pthread.h>

#include "rtl-sdr.h"
#include "convenience/convenience.h"

#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")

typedef int socklen_t;

#else
#define closesocket close
#define SOCKADDR struct sockaddr
#define SOCKET int
#define SOCKET_ERROR -1
#endif

/* ======================================================================
 * Configuration defaults
 * ====================================================================== */

#define DEFAULT_PORT_STR        "1234"
#define DEFAULT_SAMPLE_RATE_HZ  2048000
#define DEFAULT_MAX_NUM_BUFFERS 500
#define DEFAULT_MAX_CLIENTS     8

/* Ring buffer: power-of-2 slot count for fast modulo via bitmask */
#define RING_SLOT_COUNT         512   /* must be power of 2 */
#define RING_SLOT_MASK          (RING_SLOT_COUNT - 1)
#define RING_SLOT_SIZE          (256 * 1024)  /* 256 KB per slot */

/* ======================================================================
 * Ring buffer (lock-free SPSC between USB callback and send workers)
 * ====================================================================== */

struct ring_slot {
	uint32_t len;   /* actual bytes written (0..RING_SLOT_SIZE) */
};

struct ring_buffer {
	unsigned char  *data;          /* contiguous RING_SLOT_COUNT * RING_SLOT_SIZE bytes */
	struct ring_slot *slots;       /* per-slot metadata */
	atomic_uint     write_pos;     /* next slot to write (producer) */
	/* Per-client read positions are tracked in the client struct */
};

static struct ring_buffer ring;

static int ring_init(struct ring_buffer *rb)
{
	rb->data = (unsigned char *)malloc((size_t)RING_SLOT_COUNT * RING_SLOT_SIZE);
	if (!rb->data)
		return -1;
	rb->slots = (struct ring_slot *)calloc(RING_SLOT_COUNT, sizeof(struct ring_slot));
	if (!rb->slots) {
		free(rb->data);
		return -1;
	}
	atomic_store(&rb->write_pos, 0);
	return 0;
}

static void ring_destroy(struct ring_buffer *rb)
{
	free(rb->data);
	free(rb->slots);
	rb->data = NULL;
	rb->slots = NULL;
}

/* Returns pointer to slot data for writing. Caller fills data, then calls ring_publish. */
static unsigned char *ring_write_ptr(struct ring_buffer *rb)
{
	unsigned int wp = atomic_load_explicit(&rb->write_pos, memory_order_relaxed);
	return rb->data + (size_t)(wp & RING_SLOT_MASK) * RING_SLOT_SIZE;
}

static void ring_publish(struct ring_buffer *rb, uint32_t len)
{
	unsigned int wp = atomic_load_explicit(&rb->write_pos, memory_order_relaxed);
	rb->slots[wp & RING_SLOT_MASK].len = len;
	atomic_store_explicit(&rb->write_pos, wp + 1, memory_order_release);
}

/* ======================================================================
 * Multi-client management
 * ====================================================================== */

struct client_state {
	SOCKET        sock;
	unsigned int  read_pos;       /* this client's ring read cursor */
	int           active;         /* 1 = connected, 0 = slot free */
	int           overflows;      /* dropped buffers counter */
	pthread_t     send_thread;
	pthread_t     cmd_thread;
};

static struct client_state clients[DEFAULT_MAX_CLIENTS];
static int max_clients = DEFAULT_MAX_CLIENTS;
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
static int client_count = 0;

/* ======================================================================
 * Globals
 * ====================================================================== */

static rtlsdr_dev_t *dev = NULL;
static int enable_biastee = 0;
static volatile int do_exit = 0;

static pthread_cond_t data_cond;
static pthread_mutex_t data_mutex;

typedef struct { /* structure size must be multiple of 2 bytes */
	char magic[4];
	uint32_t tuner_type;
	uint32_t tuner_gain_count;
} dongle_info_t;

/* ======================================================================
 * Platform helpers
 * ====================================================================== */

#ifdef _WIN32
int gettimeofday(struct timeval *tv, void* ignored)
{
	FILETIME ft;
	unsigned __int64 tmp = 0;
	if (NULL != tv) {
		GetSystemTimeAsFileTime(&ft);
		tmp |= ft.dwHighDateTime;
		tmp <<= 32;
		tmp |= ft.dwLowDateTime;
		tmp /= 10;
#ifdef _MSC_VER
		tmp -= 11644473600000000Ui64;
#else
		tmp -= 11644473600000000ULL;
#endif
		tv->tv_sec = (long)(tmp / 1000000UL);
		tv->tv_usec = (long)(tmp % 1000000UL);
	}
	return 0;
}

BOOL WINAPI sighandler(int signum)
{
	if (CTRL_C_EVENT == signum) {
		fprintf(stderr, "Signal caught, exiting!\n");
		do_exit = 1;
		rtlsdr_cancel_async(dev);
		return TRUE;
	}
	return FALSE;
}
#else
static void sighandler(int signum)
{
	(void)signum;
	signal(SIGPIPE, SIG_IGN);
	fprintf(stderr, "Signal caught, exiting!\n");
	rtlsdr_cancel_async(dev);
	do_exit = 1;
}
#endif

/* ======================================================================
 * Send all bytes to socket (handles partial sends)
 * ====================================================================== */

static int send_all(SOCKET sock, const void *buf, int len)
{
	const char *ptr = (const char *)buf;
	int left = len;
	int sent;
	struct timeval tv = {1, 0};
	fd_set writefds;

	while (left > 0) {
		if (do_exit)
			return -1;

		FD_ZERO(&writefds);
		FD_SET(sock, &writefds);
		tv.tv_sec = 1;
		tv.tv_usec = 0;

		if (select(sock + 1, NULL, &writefds, NULL, &tv) <= 0)
			continue;

		sent = send(sock, ptr, left, 0);
		if (sent == SOCKET_ERROR || sent <= 0)
			return -1;

		ptr += sent;
		left -= sent;
	}
	return len;
}

/* ======================================================================
 * USB async callback -> ring buffer (zero-malloc hot path)
 * ====================================================================== */

static void rtlsdr_callback(unsigned char *buf, uint32_t len, void *ctx)
{
	(void)ctx;
	unsigned char *dest;
	uint32_t to_copy;

	if (do_exit || len == 0)
		return;

	/* Write directly into ring buffer slot — no malloc, no linked list */
	dest = ring_write_ptr(&ring);
	to_copy = (len <= RING_SLOT_SIZE) ? len : RING_SLOT_SIZE;
	memcpy(dest, buf, to_copy);
	ring_publish(&ring, to_copy);

	/* Wake all client send threads */
	pthread_mutex_lock(&data_mutex);
	pthread_cond_broadcast(&data_cond);
	pthread_mutex_unlock(&data_mutex);
}

/* ======================================================================
 * Per-client send worker thread
 * ====================================================================== */

static void *client_send_worker(void *arg)
{
	struct client_state *cl = (struct client_state *)arg;
	unsigned int wp;
	unsigned int rp;
	unsigned int behind;

	while (!do_exit && cl->active) {
		pthread_mutex_lock(&data_mutex);

		/* Wait for new data */
		wp = atomic_load_explicit(&ring.write_pos, memory_order_acquire);
		if (wp == cl->read_pos) {
			struct timespec ts;
			struct timeval tp;
			gettimeofday(&tp, NULL);
			ts.tv_sec = tp.tv_sec + 2;
			ts.tv_nsec = tp.tv_usec * 1000;
			pthread_cond_timedwait(&data_cond, &data_mutex, &ts);
		}
		pthread_mutex_unlock(&data_mutex);

		if (do_exit || !cl->active)
			break;

		/* Send all available slots */
		wp = atomic_load_explicit(&ring.write_pos, memory_order_acquire);
		rp = cl->read_pos;

		/* Check if we're too far behind (ring wrapped) */
		behind = wp - rp;
		if (behind > RING_SLOT_COUNT) {
			/* Client is too slow — skip to latest minus a small margin */
			cl->overflows += (int)(behind - RING_SLOT_COUNT / 4);
			rp = wp - RING_SLOT_COUNT / 4;
			cl->read_pos = rp;
			fprintf(stderr, "client overflow, skipped %u buffers\n",
				behind - RING_SLOT_COUNT / 4);
		}

		while (rp != wp && !do_exit && cl->active) {
			unsigned int idx = rp & RING_SLOT_MASK;
			uint32_t slot_len = ring.slots[idx].len;
			unsigned char *slot_data = ring.data + (size_t)idx * RING_SLOT_SIZE;

			if (send_all(cl->sock, slot_data, (int)slot_len) < 0) {
				fprintf(stderr, "client send error, disconnecting\n");
				cl->active = 0;
				break;
			}
			rp++;
		}
		cl->read_pos = rp;
	}

	return NULL;
}

/* ======================================================================
 * Per-client command reader thread
 * ====================================================================== */

#ifdef _WIN32
#define __attribute__(x)
#pragma pack(push, 1)
#endif
struct command {
	unsigned char cmd;
	unsigned int param;
}__attribute__((packed));
#ifdef _WIN32
#pragma pack(pop)
#endif

static int set_gain_by_index(rtlsdr_dev_t *_dev, unsigned int index)
{
	int res = 0;
	int *gains;
	int count = rtlsdr_get_tuner_gains(_dev, NULL);

	if (count > 0 && (unsigned int)count > index) {
		gains = malloc(sizeof(int) * count);
		count = rtlsdr_get_tuner_gains(_dev, gains);
		res = rtlsdr_set_tuner_gain(_dev, gains[index]);
		free(gains);
	}

	return res;
}

static void *client_cmd_worker(void *arg)
{
	struct client_state *cl = (struct client_state *)arg;
	int left, received;
	fd_set readfds;
	struct command cmd = {0, 0};
	struct timeval tv = {1, 0};
	int r;
	uint32_t tmp;

	while (!do_exit && cl->active) {
		left = sizeof(cmd);
		while (left > 0) {
			FD_ZERO(&readfds);
			FD_SET(cl->sock, &readfds);
			tv.tv_sec = 1;
			tv.tv_usec = 0;
			r = select(cl->sock + 1, &readfds, NULL, NULL, &tv);

			if (do_exit || !cl->active)
				goto cmd_exit;

			if (r > 0) {
				received = recv(cl->sock,
					(char *)&cmd + (sizeof(cmd) - left), left, 0);
				if (received <= 0) {
					fprintf(stderr, "client command recv error\n");
					cl->active = 0;
					goto cmd_exit;
				}
				left -= received;
			}
		}

		switch (cmd.cmd) {
		case 0x01:
			printf("set freq %u\n", ntohl(cmd.param));
			rtlsdr_set_center_freq(dev, ntohl(cmd.param));
			break;
		case 0x02:
			printf("set sample rate %u\n", ntohl(cmd.param));
			rtlsdr_set_sample_rate(dev, ntohl(cmd.param));
			break;
		case 0x03:
			printf("set gain mode %u\n", ntohl(cmd.param));
			rtlsdr_set_tuner_gain_mode(dev, ntohl(cmd.param));
			break;
		case 0x04:
			printf("set gain %u\n", ntohl(cmd.param));
			rtlsdr_set_tuner_gain(dev, ntohl(cmd.param));
			break;
		case 0x05:
			printf("set freq correction %u\n", ntohl(cmd.param));
			rtlsdr_set_freq_correction(dev, (int)ntohl(cmd.param));
			break;
		case 0x06:
			tmp = ntohl(cmd.param);
			printf("set if stage %u gain %d\n", tmp >> 16, (short)(tmp & 0xffff));
			rtlsdr_set_tuner_if_gain(dev, tmp >> 16, (short)(tmp & 0xffff));
			break;
		case 0x07:
			printf("set test mode %u\n", ntohl(cmd.param));
			rtlsdr_set_testmode(dev, ntohl(cmd.param));
			break;
		case 0x08:
			printf("set agc mode %u\n", ntohl(cmd.param));
			rtlsdr_set_agc_mode(dev, ntohl(cmd.param));
			break;
		case 0x09:
			printf("set direct sampling %u\n", ntohl(cmd.param));
			rtlsdr_set_direct_sampling(dev, ntohl(cmd.param));
			break;
		case 0x0a:
			printf("set offset tuning %u\n", ntohl(cmd.param));
			rtlsdr_set_offset_tuning(dev, ntohl(cmd.param));
			break;
		case 0x0b:
			printf("set rtl xtal %u\n", ntohl(cmd.param));
			rtlsdr_set_xtal_freq(dev, ntohl(cmd.param), 0);
			break;
		case 0x0c:
			printf("set tuner xtal %u\n", ntohl(cmd.param));
			rtlsdr_set_xtal_freq(dev, 0, ntohl(cmd.param));
			break;
		case 0x0d:
			printf("set tuner gain by index %u\n", ntohl(cmd.param));
			set_gain_by_index(dev, ntohl(cmd.param));
			break;
		case 0x0e:
			printf("set bias tee %u\n", ntohl(cmd.param));
			rtlsdr_set_bias_tee(dev, (int)ntohl(cmd.param));
			break;
		default:
			break;
		}
		cmd.cmd = 0xff;
	}

cmd_exit:
	return NULL;
}

/* ======================================================================
 * Client connection/disconnection
 * ====================================================================== */

static int add_client(SOCKET sock)
{
	int i;
	int r;
	dongle_info_t dongle_info;
	int nodelay = 1;

	/* Set TCP_NODELAY for low-latency streaming */
	setsockopt(sock, IPPROTO_TCP, TCP_NODELAY,
		(const char *)&nodelay, sizeof(nodelay));

	/* Send dongle info header */
	memset(&dongle_info, 0, sizeof(dongle_info));
	memcpy(&dongle_info.magic, "RTL0", 4);

	r = rtlsdr_get_tuner_type(dev);
	if (r >= 0)
		dongle_info.tuner_type = htonl(r);

	r = rtlsdr_get_tuner_gains(dev, NULL);
	if (r >= 0)
		dongle_info.tuner_gain_count = htonl(r);

	if (send_all(sock, &dongle_info, sizeof(dongle_info)) < 0) {
		fprintf(stderr, "failed to send dongle info to new client\n");
		closesocket(sock);
		return -1;
	}

	pthread_mutex_lock(&clients_mutex);

	if (client_count >= max_clients) {
		pthread_mutex_unlock(&clients_mutex);
		fprintf(stderr, "max clients reached (%d), rejecting connection\n", max_clients);
		closesocket(sock);
		return -1;
	}

	/* Find a free slot */
	for (i = 0; i < max_clients; i++) {
		if (!clients[i].active)
			break;
	}

	if (i >= max_clients) {
		pthread_mutex_unlock(&clients_mutex);
		closesocket(sock);
		return -1;
	}

	clients[i].sock = sock;
	clients[i].active = 1;
	clients[i].overflows = 0;
	/* Start reading from current write position (no backfill) */
	clients[i].read_pos = atomic_load_explicit(&ring.write_pos, memory_order_acquire);

	client_count++;
	pthread_mutex_unlock(&clients_mutex);

	/* Launch send and command threads for this client */
	pthread_create(&clients[i].send_thread, NULL, client_send_worker, &clients[i]);
	pthread_create(&clients[i].cmd_thread, NULL, client_cmd_worker, &clients[i]);

	printf("client %d connected (total: %d)\n", i, client_count);
	return i;
}

static void remove_client(int idx)
{
	if (idx < 0 || idx >= max_clients)
		return;

	if (!clients[idx].active)
		return;

	clients[idx].active = 0;

	/* Wake the send thread so it can exit */
	pthread_mutex_lock(&data_mutex);
	pthread_cond_broadcast(&data_cond);
	pthread_mutex_unlock(&data_mutex);

	pthread_join(clients[idx].send_thread, NULL);
	pthread_join(clients[idx].cmd_thread, NULL);

	closesocket(clients[idx].sock);
	clients[idx].sock = -1;

	pthread_mutex_lock(&clients_mutex);
	client_count--;
	pthread_mutex_unlock(&clients_mutex);

	printf("client %d disconnected (total: %d)\n", idx, client_count);
}

/* ======================================================================
 * Stale client reaper thread
 * ====================================================================== */

static void *reaper_thread(void *arg)
{
	(void)arg;
	int i;

	while (!do_exit) {
		/* Check every 2 seconds for dead clients */
#ifdef _WIN32
		Sleep(2000);
#else
		struct timespec ts = {2, 0};
		nanosleep(&ts, NULL);
#endif

		/* Clean up clients that marked themselves inactive */
		for (i = 0; i < max_clients; i++) {
			if (clients[i].sock >= 0 && !clients[i].active) {
				remove_client(i);
			}
		}
	}
	return NULL;
}

/* ======================================================================
 * Async USB reader thread (wraps blocking rtlsdr_read_async)
 * ====================================================================== */

static void *async_reader_thread(void *arg)
{
	uint32_t num = (uint32_t)(uintptr_t)arg;
	rtlsdr_read_async(dev, rtlsdr_callback, NULL, num, 0);
	return NULL;
}

/* ======================================================================
 * Usage
 * ====================================================================== */

void usage(void)
{
	printf("rtl_tcp, an I/Q spectrum server for RTL2832 based DVB-T receivers\n\n");
	printf("Usage:\t[-a listen address]\n");
	printf("\t[-p listen port (default: %s)]\n", DEFAULT_PORT_STR);
	printf("\t[-f frequency to tune to [Hz]]\n");
	printf("\t[-g gain (default: 0 for auto)]\n");
	printf("\t[-s samplerate in Hz (default: %d Hz)]\n", DEFAULT_SAMPLE_RATE_HZ);
	printf("\t[-b number of buffers (default: 15, set by library)]\n");
	printf("\t[-n max number of ring buffer slots (default: %d)]\n", RING_SLOT_COUNT);
	printf("\t[-d device index or serial (default: 0)]\n");
	printf("\t[-P ppm_error (default: 0)]\n");
	printf("\t[-T enable bias-T on GPIO PIN 0 (works for rtl-sdr.com v3 dongles)]\n");
	printf("\t[-D enable direct sampling (default: off)]\n");
	printf("\t[-c max clients (default: %d)]\n", DEFAULT_MAX_CLIENTS);
	exit(1);
}

/* ======================================================================
 * Main
 * ====================================================================== */

int main(int argc, char **argv)
{
	int r, opt, i;
	char *addr = "127.0.0.1";
	const char *port = DEFAULT_PORT_STR;
	uint32_t frequency = 100000000, samp_rate = DEFAULT_SAMPLE_RATE_HZ;
	struct sockaddr_storage local, remote;
	struct addrinfo *ai;
	struct addrinfo *aiHead;
	struct addrinfo hints = { 0 };
	char hostinfo[NI_MAXHOST];
	char portinfo[NI_MAXSERV];
	char remhostinfo[NI_MAXHOST];
	char remportinfo[NI_MAXSERV];
	int aiErr;
	uint32_t buf_num = 0;
	int dev_index = 0;
	int dev_given = 0;
	int gain = 0;
	int ppm_error = 0;
	int direct_sampling = 0;
	struct timeval tv = {1, 0};
	struct linger ling = {1, 0};
	SOCKET listensocket = 0;
	socklen_t rlen;
	fd_set readfds;
	pthread_t reaper_tid;
#ifdef _WIN32
	u_long blockmode = 1;
	WSADATA wsd;
	i = WSAStartup(MAKEWORD(2, 2), &wsd);
#else
	struct sigaction sigact, sigign;
#endif

	/* Initialize client slots */
	memset(clients, 0, sizeof(clients));
	for (i = 0; i < DEFAULT_MAX_CLIENTS; i++) {
		clients[i].sock = -1;
		clients[i].active = 0;
	}

	while ((opt = getopt(argc, argv, "a:p:f:g:s:b:n:d:P:TDc:")) != -1) {
		switch (opt) {
		case 'd':
			dev_index = verbose_device_search(optarg);
			dev_given = 1;
			break;
		case 'f':
			frequency = (uint32_t)atofs(optarg);
			break;
		case 'g':
			gain = (int)(atof(optarg) * 10); /* tenths of a dB */
			break;
		case 's':
			samp_rate = (uint32_t)atofs(optarg);
			break;
		case 'a':
			addr = strdup(optarg);
			break;
		case 'p':
			port = strdup(optarg);
			break;
		case 'b':
			buf_num = atoi(optarg);
			break;
		case 'n':
			/* Legacy compat: ignored (ring buffer is fixed-size) */
			break;
		case 'P':
			ppm_error = atoi(optarg);
			break;
		case 'T':
			enable_biastee = 1;
			break;
		case 'D':
			direct_sampling = 1;
			break;
		case 'c':
			max_clients = atoi(optarg);
			if (max_clients < 1) max_clients = 1;
			if (max_clients > DEFAULT_MAX_CLIENTS) max_clients = DEFAULT_MAX_CLIENTS;
			break;
		default:
			usage();
			break;
		}
	}

	if (argc < optind)
		usage();

	if (!dev_given) {
		dev_index = verbose_device_search("0");
	}

	if (dev_index < 0) {
		exit(1);
	}

	rtlsdr_open(&dev, (uint32_t)dev_index);
	if (NULL == dev) {
		fprintf(stderr, "Failed to open rtlsdr device #%d.\n", dev_index);
		exit(1);
	}

#ifndef _WIN32
	sigact.sa_handler = sighandler;
	sigemptyset(&sigact.sa_mask);
	sigact.sa_flags = 0;
	sigign.sa_handler = SIG_IGN;
	sigaction(SIGINT, &sigact, NULL);
	sigaction(SIGTERM, &sigact, NULL);
	sigaction(SIGQUIT, &sigact, NULL);
	sigaction(SIGPIPE, &sigign, NULL);
#else
	SetConsoleCtrlHandler((PHANDLER_ROUTINE)sighandler, TRUE);
#endif

	/* Set direct sampling */
	if (direct_sampling)
		verbose_direct_sampling(dev, 2);

	/* Set the tuner error */
	verbose_ppm_set(dev, ppm_error);

	/* Set the sample rate */
	r = rtlsdr_set_sample_rate(dev, samp_rate);
	if (r < 0)
		fprintf(stderr, "WARNING: Failed to set sample rate.\n");

	/* Set the frequency */
	r = rtlsdr_set_center_freq(dev, frequency);
	if (r < 0)
		fprintf(stderr, "WARNING: Failed to set center freq.\n");
	else
		fprintf(stderr, "Tuned to %u Hz.\n", frequency);

	if (0 == gain) {
		/* Enable automatic gain */
		r = rtlsdr_set_tuner_gain_mode(dev, 0);
		if (r < 0)
			fprintf(stderr, "WARNING: Failed to enable automatic gain.\n");
	} else {
		/* Enable manual gain */
		r = rtlsdr_set_tuner_gain_mode(dev, 1);
		if (r < 0)
			fprintf(stderr, "WARNING: Failed to enable manual gain.\n");

		/* Set the tuner gain */
		r = rtlsdr_set_tuner_gain(dev, gain);
		if (r < 0)
			fprintf(stderr, "WARNING: Failed to set tuner gain.\n");
		else
			fprintf(stderr, "Tuner gain set to %f dB.\n", gain / 10.0);
	}

	rtlsdr_set_bias_tee(dev, enable_biastee);
	if (enable_biastee)
		fprintf(stderr, "activated bias-T on GPIO PIN 0\n");

	/* Reset endpoint before we start reading from it (mandatory) */
	r = rtlsdr_reset_buffer(dev);
	if (r < 0)
		fprintf(stderr, "WARNING: Failed to reset buffers.\n");

	/* Initialize synchronization primitives (no duplicates!) */
	pthread_mutex_init(&data_mutex, NULL);
	pthread_cond_init(&data_cond, NULL);

	/* Initialize ring buffer */
	if (ring_init(&ring) < 0) {
		fprintf(stderr, "Failed to allocate ring buffer (%d x %d = %zu bytes)\n",
			RING_SLOT_COUNT, RING_SLOT_SIZE,
			(size_t)RING_SLOT_COUNT * RING_SLOT_SIZE);
		rtlsdr_close(dev);
		exit(1);
	}
	fprintf(stderr, "Ring buffer: %d slots x %d KB = %zu MB\n",
		RING_SLOT_COUNT, RING_SLOT_SIZE / 1024,
		(size_t)RING_SLOT_COUNT * RING_SLOT_SIZE / (1024 * 1024));

	/* Set up listening socket */
	hints.ai_flags = AI_PASSIVE;
	hints.ai_family = PF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	if ((aiErr = getaddrinfo(addr, port, &hints, &aiHead)) != 0) {
		fprintf(stderr, "local address %s ERROR - %s.\n",
			addr, gai_strerror(aiErr));
		ring_destroy(&ring);
		rtlsdr_close(dev);
		return -1;
	}
	memcpy(&local, aiHead->ai_addr, aiHead->ai_addrlen);

	for (ai = aiHead; ai != NULL; ai = ai->ai_next) {
		aiErr = getnameinfo((struct sockaddr *)ai->ai_addr, ai->ai_addrlen,
				    hostinfo, NI_MAXHOST,
				    portinfo, NI_MAXSERV, NI_NUMERICSERV | NI_NUMERICHOST);
		if (aiErr)
			fprintf(stderr, "getnameinfo ERROR - %s.\n", hostinfo);

		listensocket = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (listensocket < 0)
			continue;

		r = 1;
		setsockopt(listensocket, SOL_SOCKET, SO_REUSEADDR, (char *)&r, sizeof(int));
		setsockopt(listensocket, SOL_SOCKET, SO_LINGER, (char *)&ling, sizeof(ling));

		if (bind(listensocket, (struct sockaddr *)&local, aiHead->ai_addrlen))
			fprintf(stderr, "rtl_tcp bind error: %s\n", strerror(errno));
		else
			break;
	}

	freeaddrinfo(aiHead);

#ifdef _WIN32
	{
		u_long blockmode = 1;
		ioctlsocket(listensocket, FIONBIO, &blockmode);
	}
#else
	r = fcntl(listensocket, F_GETFL, 0);
	r = fcntl(listensocket, F_SETFL, r | O_NONBLOCK);
#endif

	listen(listensocket, max_clients);

	printf("rtl_tcp: listening on %s:%s (max %d clients)\n", hostinfo, portinfo, max_clients);
	printf("Use the device argument 'rtl_tcp=%s:%s' in OsmoSDR "
	       "(gr-osmosdr) source\n"
	       "to receive samples in GRC and control "
	       "rtl_tcp parameters (frequency, gain, ...).\n",
	       hostinfo, portinfo);

	/* Start the reaper thread for cleaning up dead clients */
	pthread_create(&reaper_tid, NULL, reaper_thread, NULL);

	/* Start async USB reading in a separate thread so we can accept connections */
	pthread_t async_thread;
	pthread_create(&async_thread, NULL, async_reader_thread, (void *)(uintptr_t)buf_num);

	/* Accept loop: continuously accept new clients */
	while (!do_exit) {
		SOCKET client_sock;
		FD_ZERO(&readfds);
		FD_SET(listensocket, &readfds);
		tv.tv_sec = 1;
		tv.tv_usec = 0;

		r = select(listensocket + 1, &readfds, NULL, NULL, &tv);
		if (do_exit)
			break;
		if (r <= 0)
			continue;

		rlen = sizeof(remote);
		client_sock = accept(listensocket, (struct sockaddr *)&remote, &rlen);
		if (client_sock == SOCKET_ERROR)
			continue;

		getnameinfo((struct sockaddr *)&remote, rlen,
			    remhostinfo, NI_MAXHOST,
			    remportinfo, NI_MAXSERV, NI_NUMERICSERV);
		printf("client connecting: %s:%s\n", remhostinfo, remportinfo);

		add_client(client_sock);
	}

	/* Shutdown: cancel async reading */
	rtlsdr_cancel_async(dev);
	pthread_join(async_thread, NULL);

	/* Disconnect all clients */
	for (i = 0; i < max_clients; i++) {
		if (clients[i].active)
			remove_client(i);
	}

	pthread_join(reaper_tid, NULL);

	ring_destroy(&ring);
	rtlsdr_close(dev);
	closesocket(listensocket);

#ifdef _WIN32
	WSACleanup();
#endif

	printf("bye!\n");
	return 0;
}
