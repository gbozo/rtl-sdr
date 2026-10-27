#include "rtl_stream_server.h"
#include "check.h"
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static void near(float got, double expected)
{
	CHECK(fabs(got - expected) < 0.002 + fabs(expected) * 0.00002);
}

static void dft(const float *input, double *power, int bins)
{
	for (int k = 0; k < bins; k++) {
		double re = 0, im = 0;
		for (int n = 0; n < bins; n++) {
			double angle = -2 * M_PI * n * k / bins;
			re += input[n*2] * cos(angle) - input[n*2+1] * sin(angle);
			im += input[n*2] * sin(angle) + input[n*2+1] * cos(angle);
		}
		power[k] = re*re + im*im;
	}
}

static void test_fft_alias(void)
{
	struct fft_plan *plan = fft_plan_create(16);
	float input[32], saved[32], output[32], inplace[32];
	double power[16];
	CHECK(plan);
	for (int i = 0; i < 32; i++)
		input[i] = (float)(i * 7 % 19) - 9.5f;
	memcpy(saved, input, sizeof(input));
	memcpy(inplace, input, sizeof(input));
	dft(input, power, 16);
	fft_execute(plan, input, output);
	fft_execute(plan, inplace, inplace);
	CHECK(memcmp(input, saved, sizeof(input)) == 0);
	CHECK(memcmp(inplace, output, sizeof(output)) == 0);
	for (int i = 0; i < 16; i++)
		near(output[2*i]*output[2*i] + output[2*i+1]*output[2*i+1], power[i]);
	fft_plan_destroy(plan);
}

static void fft_init(struct rtlsdr_server *srv, int step)
{
	memset(srv, 0, sizeof(*srv));
	srv->fft_bins = 16;
	srv->fft_step = step;
	srv->fft_accum = 3;
	srv->fft_plan = fft_plan_create(srv->fft_bins);
	CHECK(srv->fft_plan);
	srv->fft_window = fft_get_window(srv->fft_plan);
	srv->fft_in = calloc(2 * srv->fft_bins, sizeof(float));
	srv->fft_power = calloc(srv->fft_bins, sizeof(float));
	srv->fft_acc = calloc(srv->fft_bins, sizeof(float));
	CHECK(srv->fft_in && srv->fft_power && srv->fft_acc);
	CHECK(pthread_mutex_init(&srv->fft_mutex, NULL) == 0);
	CHECK(pthread_cond_init(&srv->fft_cond, NULL) == 0);
}

static void fft_destroy(struct rtlsdr_server *srv)
{
	fft_plan_destroy(srv->fft_plan);
	free(srv->fft_in);
	free(srv->fft_power);
	free(srv->fft_acc);
	pthread_mutex_destroy(&srv->fft_mutex);
	pthread_cond_destroy(&srv->fft_cond);
}

static void test_fft_stream(int step)
{
	struct rtlsdr_server a, b;
	size_t bytes = (size_t)(step * 11 - 1) * 2;
	uint8_t *input = malloc(bytes);
	const size_t chunks[] = {2, 8, 30, 6, 100, 4, 18};
	size_t pos = 0, chunk_idx = 0;
	double average[16] = {0};
	fft_init(&a, step);
	fft_init(&b, step);
	CHECK(input);
	for (size_t j = 0; j < bytes; j++)
		input[j] = (uint8_t)(j * 29 + j / 13);
	input[0] = 0;
	input[1] = 255;
	CHECK(rtlsdr_server_process_fft(&a, input, 2) == 0);
	CHECK(a.fft_nsamples == 1 && a.fft_in[0] == -127.5f && a.fft_in[1] == 127.5f);
	CHECK(rtlsdr_server_process_fft(&a, input + 2, 1) < 0);
	CHECK(a.fft_nsamples == 1);
	a.fft_step = 15;
	CHECK(rtlsdr_server_process_fft(&a, input + 2, 2) < 0);
	CHECK(a.fft_nsamples == 1);
	a.fft_step = step;
	CHECK(rtlsdr_server_process_fft(&a, input + 2, 30) == 0);
	CHECK(a.fft_seq == 0);
	CHECK(a.fft_accum_count == (step == 16 ? 1 : 0));
	CHECK(a.fft_nsamples == (step == 16 ? 0 : 16));
	CHECK(rtlsdr_server_process_fft(&a, input + 32, bytes - 32) == 0);
	while (pos < bytes) {
		size_t chunk = chunks[chunk_idx++ % (sizeof(chunks) / sizeof(*chunks))];
		if (chunk > bytes - pos)
			chunk = bytes - pos;
		CHECK(rtlsdr_server_process_fft(&b, input + pos, chunk) == 0);
		pos += chunk;
	}
	CHECK(a.fft_seq == 3 && b.fft_seq == 3);
	CHECK(a.fft_nsamples == step - 1 && b.fft_nsamples == step - 1);
	CHECK(a.fft_accum_count == 1 && b.fft_accum_count == 1);
	CHECK(memcmp(a.fft_power, b.fft_power, 16 * sizeof(float)) == 0);
	CHECK(memcmp(a.fft_acc, b.fft_acc, 16 * sizeof(float)) == 0);
	CHECK(memcmp(a.fft_in, b.fft_in, 32 * sizeof(float)) == 0);
	/* Latest publication averages hops 6..8; hop 9 remains accumulated. */
	for (int hop = 6; hop <= 9; hop++) {
		float windowed[32];
		double power[16];
		for (int j = 0; j < 16; j++) {
			windowed[2*j] = (input[2*(hop*step+j)] - 127.5f) * a.fft_window[j];
			windowed[2*j+1] = (input[2*(hop*step+j)+1] - 127.5f) * a.fft_window[j];
		}
		dft(windowed, power, 16);
		for (int j = 0; j < 16; j++) {
			if (hop < 9)
				average[j] += power[j] / 3;
			else
				near(a.fft_acc[j], power[j]);
		}
	}
	for (int j = 0; j < 16; j++)
		near(a.fft_power[j], average[j]);
	/* Complete the partial hop, including its skipped tail. */
	CHECK(rtlsdr_server_process_fft(&a, input, 2) == 0);
	CHECK(a.fft_nsamples == 0 && a.fft_accum_count == 2 && a.fft_seq == 3);
	fft_destroy(&a);
	fft_destroy(&b);
	free(input);
}

struct reader {
	int fd;
	pthread_t sender;
	const uint8_t *expected;
	size_t len;
};

static volatile sig_atomic_t interrupts;

static void interrupt_handler(int sig)
{
	(void)sig;
	interrupts++;
}

static void *read_socket(void *arg)
{
	struct reader *reader = arg;
	uint8_t buf[997];
	size_t pos = 0, calls = 0;
	usleep(20000);
	CHECK(pthread_kill(reader->sender, SIGUSR1) == 0);
	while (pos < reader->len) {
		ssize_t len = read(reader->fd, buf, sizeof(buf));
		if (len < 0 && errno == EINTR)
			continue;
		CHECK(len > 0 && (size_t)len <= reader->len - pos);
		CHECK(memcmp(buf, reader->expected + pos, (size_t)len) == 0);
		pos += (size_t)len;
		if (++calls % 256 == 0)
			CHECK(pthread_kill(reader->sender, SIGUSR1) == 0);
	}
	return NULL;
}

static void test_socket(void)
{
	int fd[2], small = 4096;
	size_t bytes = 2 * 1024 * 1024;
	uint8_t *payload = malloc(bytes);
	struct iovec iov[2];
	struct reader reader;
	pthread_t thread;
	struct sigaction action, old;
	CHECK(payload);
	for (size_t j = 0; j < bytes; j++)
		payload[j] = (uint8_t)(j * 19 + j / 7);
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
	CHECK(setsockopt(fd[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)) == 0);
	memset(&action, 0, sizeof(action));
	action.sa_handler = interrupt_handler;
	sigemptyset(&action.sa_mask);
	CHECK(sigaction(SIGUSR1, &action, &old) == 0);
	reader.fd = fd[1];
	reader.sender = pthread_self();
	reader.expected = payload;
	reader.len = bytes;
	iov[0].iov_base = payload;
	iov[0].iov_len = 17;
	iov[1].iov_base = payload + 17;
	iov[1].iov_len = bytes - 17;
	{
		struct iovec empty[2] = {{payload, 0}, {payload, 0}};
		CHECK(rtlsdr_stream_sendv(fd[0], empty, 2) == 0);
		CHECK(rtlsdr_stream_sendv(fd[0], empty, 0) < 0 && errno == EINVAL);
	}
	CHECK(pthread_create(&thread, NULL, read_socket, &reader) == 0);
	CHECK(rtlsdr_stream_sendv(fd[0], iov, 2) == 0);
	CHECK(pthread_join(thread, NULL) == 0);
	CHECK(interrupts > 0);
	CHECK(iov[0].iov_base == payload && iov[0].iov_len == 17);
	CHECK(iov[1].iov_base == payload + 17 && iov[1].iov_len == bytes - 17);
	CHECK(sigaction(SIGUSR1, &old, NULL) == 0);
	close(fd[1]);
	/* Default SIGPIPE disposition must not terminate this process. */
	signal(SIGPIPE, SIG_DFL);
	CHECK(rtlsdr_stream_sendv(fd[0], iov, 2) < 0);
	close(fd[0]);
	free(payload);
}

static void test_send_deadline(void)
{
	int fd[2], small = 4096;
	uint8_t *payload = calloc(2 * 1024 * 1024, 1);
	struct iovec iov = {payload, 2 * 1024 * 1024};
	struct timespec start, end;
	double elapsed;
	CHECK(payload);
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
	CHECK(setsockopt(fd[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)) == 0);
	CHECK(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
	CHECK(rtlsdr_stream_sendv(fd[0], &iov, 1) < 0 && errno == ETIMEDOUT);
	CHECK(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
	elapsed = end.tv_sec - start.tv_sec + (end.tv_nsec - start.tv_nsec) / 1e9;
	CHECK(elapsed >= 4.5 && elapsed < 6.5);
	close(fd[0]);
	close(fd[1]);
	free(payload);
}

int main(void)
{
	test_fft_alias();
	test_fft_stream(16);
	test_fft_stream(23);
	test_socket();
	test_send_deadline();
	return 0;
}
