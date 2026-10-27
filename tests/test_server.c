#include "rtl_stream_server.h"
#include "rtl_stream_proto.h"
#include "rtl_dsp.h"
#include "check.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

static int fail_fft_input, fail_thread_create, input_allocations;
static struct rtlsdr_server *allocating_server;
static int allocation_fds[4];

static void *fault_malloc(size_t size)
{
	input_allocations++;
	if (fail_fft_input) {
		allocation_fds[0] = allocating_server->listen_fd_iq;
		allocation_fds[1] = allocating_server->listen_fd_fft;
		allocation_fds[2] = allocating_server->control[0];
		allocation_fds[3] = allocating_server->control[1];
		return NULL;
	}
	return malloc(size);
}

static int fault_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
	void *(*start)(void *), void *arg)
{
	if (fail_thread_create)
		return EAGAIN;
	return pthread_create(thread, attr, start, arg);
}

/* An aliased white-box copy permits portable fault injection without adding
 * production hooks or replacing the normally linked server used below. */
void fault_server_request_stop(struct rtlsdr_server *srv);
void fault_server_shutdown(struct rtlsdr_server *srv);
#define rtlsdr_server_init fault_server_init
#define rtlsdr_server_run fault_server_run
#define rtlsdr_server_shutdown fault_server_shutdown
#define rtlsdr_server_request_stop fault_server_request_stop
#define rtlsdr_server_notify_frequency_change fault_server_notify_frequency_change
#define rtlsdr_server_process_fft fault_server_process_fft
#define rtlsdr_server_publish fault_server_publish
#define rtlsdr_stream_sendv fault_stream_sendv
#define malloc fault_malloc
#define pthread_create fault_pthread_create
#include "../src/rtl_stream_server.c"
#undef pthread_create
#undef malloc
#undef rtlsdr_stream_sendv
#undef rtlsdr_server_publish
#undef rtlsdr_server_process_fft
#undef rtlsdr_server_notify_frequency_change
#undef rtlsdr_server_request_stop
#undef rtlsdr_server_shutdown
#undef rtlsdr_server_run
#undef rtlsdr_server_init

static int64_t now_ms(void)
{
	struct timespec ts;
	CHECK(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void *run_server(void *arg)
{
	CHECK(rtlsdr_server_run(arg) == 0);
	return NULL;
}

static int port_of(int fd)
{
	struct sockaddr_in addr;
	socklen_t len = sizeof(addr);
	CHECK(getsockname(fd, (struct sockaddr *)&addr, &len) == 0);
	return ntohs(addr.sin_port);
}

static int unused_port(void)
{
	struct sockaddr_in addr;
	int fd = socket(AF_INET, SOCK_STREAM, 0), port;
	CHECK(fd >= 0);
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	CHECK(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
	port = port_of(fd);
	close(fd);
	return port;
}

static int connect_client(int listener)
{
	struct sockaddr_in addr;
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	CHECK(fd >= 0);
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(port_of(listener));
	CHECK(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
	return fd;
}

static void request(int fd, uint8_t mode, uint64_t freq, uint64_t rate, uint64_t bw)
{
	uint8_t buf[RTLSTREAM_REQ_SIZE];
	struct rtlsdr_stream_req req = {RTLSTREAM_MAGIC_REQ, freq, rate, bw, mode};
	CHECK(rtlsdr_stream_encode_req(buf, &req) == RTLSTREAM_REQ_SIZE);
	CHECK(send(fd, buf, sizeof(buf), 0) == sizeof(buf));
}

static void receive_all(int fd, void *dst, size_t bytes)
{
	uint8_t *out = dst;
	int64_t end = now_ms() + 3000;
	while (bytes) {
		struct pollfd pfd = {fd, POLLIN, 0};
		ssize_t n;
		CHECK(now_ms() < end);
		CHECK(poll(&pfd, 1, (int)(end - now_ms())) > 0);
		n = recv(fd, out, bytes, 0);
		CHECK(n > 0);
		out += n;
		bytes -= (size_t)n;
	}
}

static void expect_closed(int fd, int timeout)
{
	struct pollfd pfd = {fd, POLLIN, 0};
	uint8_t byte;
	CHECK(poll(&pfd, 1, timeout) > 0);
	CHECK(recv(fd, &byte, 1, 0) <= 0);
}

static uint64_t be(const uint8_t *p, size_t bytes)
{
	uint64_t result = 0;
	while (bytes--)
		result = result * 256 + *p++;
	return result;
}

static void wait_clients(struct rtlsdr_server *srv, int count, int running)
{
	int64_t end = now_ms() + 3000;
	for (;;) {
		int found = 0;
		pthread_mutex_lock(&srv->client_lock);
		if (!running)
			found = srv->client_count;
		else
			for (int i = 0; i < RTLSTREAM_MAX_CLIENTS; i++)
				found += srv->clients[i].state == RTL_CLIENT_RUNNING;
		pthread_mutex_unlock(&srv->client_lock);
		if (found == count)
			return;
		CHECK(now_ms() < end);
		poll(NULL, 0, 2);
	}
}

static void finish(struct rtlsdr_server *srv, pthread_t thread)
{
	int64_t start = now_ms();
	rtlsdr_server_request_stop(srv);
	CHECK(pthread_join(thread, NULL) == 0);
	rtlsdr_server_shutdown(srv);
	CHECK(now_ms() - start < 2000);
}

static void test_publish(void)
{
	struct rtlsdr_server srv;
	struct rtlsdr_ring *ring = rtlsdr_ring_create(4, 16);
	struct rtlsdr_ring_cursor cursor;
	uint8_t input[42], output[16];
	size_t len;
	CHECK(ring);
	CHECK(rtlsdr_server_init(&srv, 0, 0, ring, 100000000, 48000) == 0);
	CHECK(rtlsdr_ring_cursor_now(ring, &cursor) == 0);
	for (size_t j = 0; j < sizeof(input); j++)
		input[j] = (uint8_t)j;
	CHECK(rtlsdr_server_publish(&srv, input, sizeof(input)) == 0);
	for (size_t pos = 0; pos < sizeof(input); pos += len) {
		CHECK(rtlsdr_ring_read_copy(ring, &cursor, output, sizeof(output), &len) == RTL_RING_DATA);
		CHECK(len == (pos < 32 ? 16 : 10));
		CHECK(memcmp(output, input + pos, len) == 0);
	}
	CHECK(rtlsdr_server_publish(&srv, NULL, 0) == 0);
	CHECK(rtlsdr_ring_read_copy(ring, &cursor, output, sizeof(output), &len) == RTL_RING_EMPTY);
	CHECK(rtlsdr_server_publish(&srv, input, 3) < 0);
	CHECK(rtlsdr_ring_read_copy(ring, &cursor, output, sizeof(output), &len) == RTL_RING_GAP);
	rtlsdr_server_shutdown(&srv);
	rtlsdr_ring_destroy(ring);
}

static void test_iq_and_reuse(void)
{
	struct rtlsdr_server srv;
	struct rtlsdr_ring *ring = rtlsdr_ring_create(16, 256);
	pthread_t thread;
	uint8_t input[256], hdr[32], wire[512];
	int16_t expected[256];
	CHECK(ring);
	CHECK(rtlsdr_server_init(&srv, 0, 0, ring, 100000000, 48000) == 0);
	CHECK(srv.listen_fd_fft == -1);
	CHECK(pthread_create(&thread, NULL, run_server, &srv) == 0);
	for (size_t j = 0; j < sizeof(input); j++)
		input[j] = (uint8_t)(j * 37);
	for (int iteration = 0; iteration < 40; iteration++) {
		uint64_t freq = iteration & 1 ? 100001000 : 99999000;
		uint64_t rate = 12000, bw = 9600;
		size_t components;
		int fd = connect_client(srv.listen_fd_iq);
		struct rtlsdr_dsp *dsp = rtlsdr_dsp_create(48000, rate, bw,
			(int64_t)freq - 100000000);
		CHECK(dsp);
		request(fd, RTLSTREAM_MODE_IQ, freq, rate, bw);
		wait_clients(&srv, 1, 1);
		CHECK(rtlsdr_server_publish(&srv, input, sizeof(input)) == 0);
		CHECK(rtlsdr_dsp_process(dsp, input, sizeof(input), expected, 256, &components) == 0);
		receive_all(fd, hdr, sizeof(hdr));
		CHECK(memcmp(hdr, "RT2I", 4) == 0);
		CHECK(be(hdr + 4, 8) == freq && be(hdr + 12, 8) == rate);
		CHECK(be(hdr + 20, 8) == 0 && be(hdr + 28, 4) == components);
		receive_all(fd, wire, components * 2);
		for (size_t j = 0; j < components; j++)
			CHECK(be(wire + j * 2, 2) == (uint16_t)expected[j]);
		rtlsdr_dsp_destroy(dsp);
		close(fd);
		wait_clients(&srv, 0, 0);
	}
	/* Listener mode, exact rate, bounded frequency and passband checks. */
	{
		const struct rtlsdr_stream_req bad[] = {
			{RTLSTREAM_MAGIC_REQ, 0, 0, 0, RTLSTREAM_MODE_FFT},
			{RTLSTREAM_MAGIC_REQ, 100000000, 11000, 8000, RTLSTREAM_MODE_IQ},
			{RTLSTREAM_MAGIC_REQ, 100000000, 12000, 12000, RTLSTREAM_MODE_IQ},
			{RTLSTREAM_MAGIC_REQ, UINT64_MAX, 12000, 8000, RTLSTREAM_MODE_IQ},
			{RTLSTREAM_MAGIC_REQ, 100030000, 12000, 8000, RTLSTREAM_MODE_IQ},
			{RTLSTREAM_MAGIC_REQ, 100000000, 0, 0, RTLSTREAM_MODE_IQ}
		};
		for (size_t j = 0; j < sizeof(bad) / sizeof(*bad); j++) {
			int fd = connect_client(srv.listen_fd_iq);
			request(fd, bad[j].mode, bad[j].freq, bad[j].rate, bad[j].bandwidth);
			expect_closed(fd, 3000);
			close(fd);
			wait_clients(&srv, 0, 0);
		}
	}
	/* A gap is terminal, never an implicit cursor jump. */
	{
		int fd = connect_client(srv.listen_fd_iq);
		request(fd, RTLSTREAM_MODE_IQ, 100000000, 48000, 48000);
		wait_clients(&srv, 1, 1);
		CHECK(rtlsdr_server_publish(&srv, input, 1) < 0);
		expect_closed(fd, 3000);
		close(fd);
		wait_clients(&srv, 0, 0);
	}
	finish(&srv, thread);
	rtlsdr_ring_destroy(ring);
}

static void test_retune(void)
{
	struct rtlsdr_server srv;
	struct rtlsdr_ring *ring = rtlsdr_ring_create(8, 256);
	pthread_t thread;
	int fds[3];
	uint8_t events[24];
	CHECK(ring);
	CHECK(rtlsdr_server_init(&srv, 0, unused_port(), ring, 100000000, 48000) == 0);
	CHECK(pthread_create(&thread, NULL, run_server, &srv) == 0);
	for (int j = 0; j < 3; j++) {
		fds[j] = connect_client(j == 2 ? srv.listen_fd_fft : srv.listen_fd_iq);
		request(fds[j], j == 2 ? RTLSTREAM_MODE_FFT : RTLSTREAM_MODE_IQ,
			j == 2 ? 0 : 100000000, j == 2 ? 0 : 48000, j == 2 ? 0 : 48000);
	}
	wait_clients(&srv, 3, 1);
	rtlsdr_server_notify_frequency_change(&srv, 101000000);
	for (int j = 0; j < 3; j++) {
		receive_all(fds[j], events, sizeof(events));
		CHECK(memcmp(events, "RT2E", 4) == 0 && be(events + 4, 4) == 1);
		CHECK(be(events + 8, 8) == 101000000);
		CHECK(memcmp(events + 16, "RT2E", 4) == 0 && be(events + 20, 4) == 2);
		expect_closed(fds[j], 3000);
		close(fds[j]);
	}
	CHECK(srv.capture_freq == 100000000);
	wait_clients(&srv, 0, 0);
	finish(&srv, thread);
	rtlsdr_ring_destroy(ring);
}

static void test_fft_gap(void)
{
	struct rtlsdr_server srv;
	struct rtlsdr_ring *ring = rtlsdr_ring_create(8, 65536);
	pthread_t thread;
	uint8_t input[40960], hdr[32], wire[4096];
	int fd;
	size_t frame = FFT_DEFAULT_STEP * FFT_DEFAULT_ACCUM * 2;
	uint64_t generation;
	CHECK(ring);
	memset(input, 140, sizeof(input));
	CHECK(rtlsdr_server_init(&srv, 0, unused_port(), ring, 100000000, 48000) == 0);
	CHECK(pthread_create(&thread, NULL, run_server, &srv) == 0);
	fd = connect_client(srv.listen_fd_fft);
	request(fd, RTLSTREAM_MODE_FFT, 0, 0, 0);
	wait_clients(&srv, 1, 1);
	{
		struct pollfd pfd = {fd, POLLIN, 0};
		CHECK(poll(&pfd, 1, 150) == 0);
	}
	/* One complete average plus one accumulated hop in the same ring slot. */
	CHECK(rtlsdr_server_publish(&srv, input, sizeof(input)) == 0);
	receive_all(fd, hdr, sizeof(hdr));
	CHECK(memcmp(hdr, "RT2F", 4) == 0 && be(hdr + 12, 8) == 48000);
	CHECK(be(hdr + 28, 4) == 1024);
	receive_all(fd, wire, sizeof(wire));
	pthread_mutex_lock(&srv.fft_mutex);
	generation = srv.fft_generation;
	for (int j = 0; j < 1024; j++) {
		uint32_t bits;
		memcpy(&bits, &srv.fft_power[j], sizeof(bits));
		CHECK(be(wire + j * 4, 4) == bits);
	}
	pthread_mutex_unlock(&srv.fft_mutex);
	CHECK(rtlsdr_server_publish(&srv, input, 1) < 0);
	expect_closed(fd, 3000);
	close(fd);
	wait_clients(&srv, 0, 0);
	pthread_mutex_lock(&srv.fft_mutex);
	CHECK(srv.fft_generation == generation + 1 && !srv.fft_ready);
	pthread_mutex_unlock(&srv.fft_mutex);
	fd = connect_client(srv.listen_fd_fft);
	request(fd, RTLSTREAM_MODE_FFT, 0, 0, 0);
	wait_clients(&srv, 1, 1);
	CHECK(rtlsdr_server_publish(&srv, input, frame - 2) == 0);
	{
		struct pollfd pfd = {fd, POLLIN, 0};
		CHECK(poll(&pfd, 1, 200) == 0);
	}
	CHECK(rtlsdr_server_publish(&srv, input, 2) == 0);
	receive_all(fd, hdr, sizeof(hdr));
	receive_all(fd, wire, sizeof(wire));
	close(fd);
	finish(&srv, thread);
	rtlsdr_ring_destroy(ring);
}

static void test_shutdown_and_partial_init(void)
{
	struct rtlsdr_server srv, failed;
	struct rtlsdr_ring *ring = rtlsdr_ring_create(8, 262144);
	pthread_t thread;
	int fd, before = fcntl(STDIN_FILENO, F_GETFD);
	CHECK(ring);
	CHECK(rtlsdr_server_init(&srv, 0, 0, ring, 100000000, 48000) == 0);
	/* Fail only after locks, control pipe and the I/Q listener were initialized. */
	CHECK(rtlsdr_server_init(&failed, 0, port_of(srv.listen_fd_iq), ring, 100000000, 48000) < 0);
	rtlsdr_server_shutdown(&failed);
	CHECK(fcntl(STDIN_FILENO, F_GETFD) == before);
	CHECK(pthread_create(&thread, NULL, run_server, &srv) == 0);
	fd = connect_client(srv.listen_fd_iq);
	wait_clients(&srv, 1, 0);
	{
		int64_t start = now_ms();
		rtlsdr_server_shutdown(&srv);
		CHECK(pthread_join(thread, NULL) == 0);
		CHECK(now_ms() - start < 2000);
	}
	expect_closed(fd, 1000);
	close(fd);
	CHECK(fcntl(STDIN_FILENO, F_GETFD) == before);
	/* A sender blocked behind a non-reading peer must be interruptible. */
	CHECK(rtlsdr_server_init(&srv, 0, 0, ring, 100000000, 48000) == 0);
	CHECK(pthread_create(&thread, NULL, run_server, &srv) == 0);
	fd = connect_client(srv.listen_fd_iq);
	{
		int small = 4096;
		uint8_t *input = calloc(262144, 1);
		CHECK(input);
		CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small)) == 0);
		request(fd, RTLSTREAM_MODE_IQ, 100000000, 48000, 48000);
		wait_clients(&srv, 1, 1);
		pthread_mutex_lock(&srv.client_lock);
		for (int j = 0; j < RTLSTREAM_MAX_CLIENTS; j++)
			if (srv.clients[j].state == RTL_CLIENT_RUNNING)
				CHECK(setsockopt(srv.clients[j].fd, SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)) == 0);
		pthread_mutex_unlock(&srv.client_lock);
		CHECK(rtlsdr_server_publish(&srv, input, 262144) == 0);
		{
			struct pollfd pfd = {fd, POLLIN, 0};
			CHECK(poll(&pfd, 1, 3000) > 0);
		}
		finish(&srv, thread);
		free(input);
	}
	close(fd);
	rtlsdr_ring_destroy(ring);
}

static void test_request_deadline(void)
{
	struct rtlsdr_server srv;
	struct rtlsdr_ring *ring = rtlsdr_ring_create(4, 256);
	pthread_t thread;
	int fd;
	int64_t start;
	CHECK(ring);
	CHECK(rtlsdr_server_init(&srv, 0, 0, ring, 100000000, 48000) == 0);
	CHECK(pthread_create(&thread, NULL, run_server, &srv) == 0);
	fd = connect_client(srv.listen_fd_iq);
	start = now_ms();
	CHECK(send(fd, "R", 1, 0) == 1);
	expect_closed(fd, 6500);
	CHECK(now_ms() - start >= 4500 && now_ms() - start < 6500);
	close(fd);
	finish(&srv, thread);
	rtlsdr_ring_destroy(ring);
}

static void check_clean_server(struct rtlsdr_server *srv)
{
	CHECK(!srv->initialized && !srv->run_active && !srv->fft_thread_started);
	CHECK(!srv->fft_plan && !srv->fft_input && !srv->fft_in && !srv->fft_power && !srv->fft_acc);
	CHECK(srv->listen_fd_iq == -1 && srv->listen_fd_fft == -1);
	CHECK(srv->control[0] == -1 && srv->control[1] == -1);
	CHECK(srv->client_count == 0);
	for (int j = 0; j < RTLSTREAM_MAX_CLIENTS; j++)
		CHECK(srv->clients[j].state == RTL_CLIENT_FREE && srv->clients[j].fd == -1);
}

static void test_fft_start_failures(void)
{
	struct rtlsdr_server srv;
	struct rtlsdr_ring *ring = rtlsdr_ring_create(8, 256);
	int iq_port = unused_port(), fft_port;
	pthread_t thread;
	int fd;
	CHECK(ring);
	do { fft_port = unused_port(); } while (fft_port == iq_port);
	allocating_server = &srv;
	input_allocations = 0;
	fail_fft_input = 1;
	CHECK(fault_server_init(&srv, iq_port, fft_port, ring, 100000000, 48000) < 0);
	CHECK(input_allocations == 1);
	CHECK(fault_server_run(&srv) < 0);
	check_clean_server(&srv);
	for (int j = 0; j < 4; j++) {
		CHECK(allocation_fds[j] >= 0);
		CHECK(fcntl(allocation_fds[j], F_GETFD) == -1 && errno == EBADF);
	}
	fail_fft_input = 0;
	CHECK(fault_server_init(&srv, iq_port, fft_port, ring, 100000000, 48000) == 0);
	CHECK(srv.fft_input && input_allocations == 2);
	fail_thread_create = 1;
	CHECK(fault_server_run(&srv) < 0);
	CHECK(!srv.run_active && !srv.fft_thread_started && atomic_load(&srv.stopping));
	fail_thread_create = 0;
	fault_server_shutdown(&srv);
	check_clean_server(&srv);
	allocating_server = NULL;
	CHECK(rtlsdr_server_init(&srv, iq_port, fft_port, ring, 100000000, 48000) == 0);
	CHECK(pthread_create(&thread, NULL, run_server, &srv) == 0);
	fd = connect_client(srv.listen_fd_fft);
	request(fd, RTLSTREAM_MODE_FFT, 0, 0, 0);
	wait_clients(&srv, 1, 1);
	finish(&srv, thread);
	check_clean_server(&srv);
	close(fd);
	rtlsdr_ring_destroy(ring);
}

static void test_client_saturation(void)
{
	struct rtlsdr_server srv;
	struct rtlsdr_ring *ring = rtlsdr_ring_create(4, 256);
	pthread_t thread;
	int fds[RTLSTREAM_MAX_CLIENTS], rejected;
	CHECK(ring);
	CHECK(rtlsdr_server_init(&srv, 0, 0, ring, 100000000, 48000) == 0);
	CHECK(pthread_create(&thread, NULL, run_server, &srv) == 0);
	for (int j = 0; j < RTLSTREAM_MAX_CLIENTS; j++) {
		fds[j] = connect_client(srv.listen_fd_iq);
		request(fds[j], RTLSTREAM_MODE_IQ, 100000000, 48000, 48000);
		wait_clients(&srv, j + 1, 1);
	}
	rejected = connect_client(srv.listen_fd_iq);
	expect_closed(rejected, 3000);
	close(rejected);
	wait_clients(&srv, RTLSTREAM_MAX_CLIENTS, 1);
	finish(&srv, thread);
	check_clean_server(&srv);
	for (int j = 0; j < RTLSTREAM_MAX_CLIENTS; j++) {
		expect_closed(fds[j], 1000);
		close(fds[j]);
	}
	rtlsdr_ring_destroy(ring);
}

int main(void)
{
	test_publish();
	test_iq_and_reuse();
	test_retune();
	test_fft_gap();
	test_shutdown_and_partial_init();
	test_request_deadline();
	test_fft_start_failures();
	test_client_saturation();
	return 0;
}
