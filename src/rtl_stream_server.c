#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "rtl_stream_server.h"
#include "rtl_stream_proto.h"
#include "rtl_dsp.h"

enum { INIT_DATA_MUTEX = 1, INIT_DATA_COND = 2, INIT_CLIENT_LOCK = 4,
	INIT_FFT_MUTEX = 8, INIT_FFT_COND = 16 };

static int nonblocking(int fd);

static int64_t monotonic_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int wait_socket(int fd, short events, int64_t deadline)
{
	struct pollfd pfd = {fd, events, 0};
	for (;;) {
		int64_t left = deadline - monotonic_ms();
		int n;
		if (left <= 0) {
			errno = ETIMEDOUT;
			return -1;
		}
		n = poll(&pfd, 1, left > INT_MAX ? INT_MAX : (int)left);
		if (n > 0) {
			if (pfd.revents & events)
				return 0;
			errno = EPIPE;
			return -1;
		}
		if (n < 0 && errno != EINTR)
			return -1;
	}
}

int rtlsdr_stream_sendv(int fd, const struct iovec *iov, int count)
{
	struct iovec pending[2];
	struct msghdr msg;
	int first = 0, flags = MSG_DONTWAIT;
	int64_t deadline = monotonic_ms() + 5000;
	if (!iov || count < 1 || count > 2) {
		errno = EINVAL;
		return -1;
	}
	/* Darwin's UNIX sockets can block in sendmsg despite MSG_DONTWAIT. */
	if (nonblocking(fd) < 0)
		return -1;
#ifdef SO_NOSIGPIPE
	{
		int on = 1;
		if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)) < 0)
			return -1;
	}
#endif
#ifdef MSG_NOSIGNAL
	flags |= MSG_NOSIGNAL;
#endif
	memcpy(pending, iov, (size_t)count * sizeof(*iov));
	while (first < count) {
		ssize_t n;
		while (first < count && pending[first].iov_len == 0)
			first++;
		if (first == count)
			break;
		if (wait_socket(fd, POLLOUT, deadline) < 0)
			return -1;
		memset(&msg, 0, sizeof(msg));
		msg.msg_iov = pending + first;
		msg.msg_iovlen = count - first;
		n = sendmsg(fd, &msg, flags);
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
				continue;
			return -1;
		}
		if (n == 0) {
			errno = EPIPE;
			return -1;
		}
		while (first < count && (size_t)n >= pending[first].iov_len) {
			n -= pending[first].iov_len;
			first++;
		}
		if (first < count) {
			pending[first].iov_base = (char *)pending[first].iov_base + n;
			pending[first].iov_len -= (size_t)n;
		}
	}
	return 0;
}

static int receive_request(int fd, uint8_t *buf)
{
	size_t pos = 0;
	int64_t deadline = monotonic_ms() + 5000;
	while (pos < RTLSTREAM_REQ_SIZE) {
		ssize_t n;
		if (wait_socket(fd, POLLIN, deadline) < 0)
			return -1;
		n = recv(fd, buf + pos, RTLSTREAM_REQ_SIZE - pos, MSG_DONTWAIT);
		if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
			continue;
		if (n <= 0)
			return -1;
		pos += (size_t)n;
	}
	return 0;
}

static void wake_control(struct rtlsdr_server *srv)
{
	uint8_t byte = 1;
	if (srv->control[1] >= 0) {
		ssize_t n;
		do { n = write(srv->control[1], &byte, 1); } while (n < 0 && errno == EINTR);
	}
}

static void wait_data(struct rtlsdr_server *srv)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_nsec += 100000000;
	if (ts.tv_nsec >= 1000000000) {
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000;
	}
	pthread_mutex_lock(&srv->data_mutex);
	if (!atomic_load(&srv->stopping))
		pthread_cond_timedwait(&srv->data_cond, &srv->data_mutex, &ts);
	pthread_mutex_unlock(&srv->data_mutex);
}

static int session_current(struct rtlsdr_server *srv, uint64_t epoch, uint64_t *freq)
{
	int current;
	pthread_mutex_lock(&srv->data_mutex);
	current = srv->capture_epoch == epoch;
	if (freq)
		*freq = srv->new_freq;
	pthread_mutex_unlock(&srv->data_mutex);
	return current;
}

static int client_current(struct rtlsdr_client *cl)
{
	struct rtlsdr_server *srv = cl->ctx;
	uint64_t freq;
	uint8_t buf[RTLSTREAM_EVT_FREQ_SIZE];
	struct iovec evt = {buf, 0};
	if (atomic_load(&srv->stopping))
		return 0;
	if (session_current(srv, cl->epoch, &freq))
		return 1;
	evt.iov_len = rtlsdr_stream_encode_evt(buf, RTLSTREAM_EVT_FREQ_CHANGE, freq);
	if (rtlsdr_stream_sendv(cl->fd, &evt, 1) == 0) {
		evt.iov_len = rtlsdr_stream_encode_evt(buf, RTLSTREAM_EVT_STREAM_END, 0);
		rtlsdr_stream_sendv(cl->fd, &evt, 1);
	}
	return 0;
}

static int socket_closed(int fd)
{
	uint8_t byte;
	ssize_t n = recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
	/* Requests are one-shot; unsolicited data is also a protocol error. */
	return n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR);
}

static void iq_client_handler(struct rtlsdr_client *cl)
{
	struct rtlsdr_server *srv = cl->ctx;
	size_t capacity = srv->ring->slot_size, len, out_len;
	uint8_t hdr[RTLSTREAM_IQ_HDR_SIZE];
	uint8_t *input = malloc(capacity), *wire = malloc(capacity * 2);
	int16_t *output = malloc(capacity * sizeof(*output));
	uint64_t seq = 0;
	struct iovec iov[2] = {{hdr, sizeof(hdr)}, {wire, 0}};
	if (!input || !wire || !output)
		goto done;
	while (client_current(cl)) {
		int result = rtlsdr_ring_read_copy(srv->ring, &cl->cursor, input, capacity, &len);
		if (result == RTL_RING_EMPTY) {
			if (socket_closed(cl->fd))
				break;
			wait_data(srv);
			continue;
		}
		if (result != RTL_RING_DATA || (len & 1))
			break;
		if (rtlsdr_dsp_process(cl->dsp, input, len, output, capacity, &out_len) < 0)
			break;
		if (out_len) {
			struct rtlsdr_stream_iq_hdr iq = {RTLSTREAM_MAGIC_IQ,
				cl->freq, cl->rate, seq++, (uint32_t)out_len};
			if (!client_current(cl) ||
			    rtlsdr_stream_encode_iq_hdr(hdr, &iq) < 0 ||
			    rtlsdr_stream_encode_iq_payload(wire, capacity * 2, output, out_len) < 0)
				break;
			iov[1].iov_len = out_len * 2;
			if (rtlsdr_stream_sendv(cl->fd, iov, 2) < 0)
				break;
		}
	}
done:
	free(input);
	free(output);
	free(wire);
}

static void fft_client_handler(struct rtlsdr_client *cl)
{
	struct rtlsdr_server *srv = cl->ctx;
	size_t bytes = (size_t)srv->fft_bins * sizeof(float);
	uint8_t hdr[RTLSTREAM_FFT_HDR_SIZE], *wire = malloc(bytes);
	float *snapshot = malloc(bytes);
	uint64_t last_seq = 0;
	struct iovec iov[2] = {{hdr, sizeof(hdr)}, {wire, bytes}};
	if (!wire || !snapshot)
		goto done;
	while (client_current(cl)) {
		uint64_t seq;
		int ready, gap;
		pthread_mutex_lock(&srv->fft_mutex);
		gap = cl->fft_generation != srv->fft_generation;
		ready = srv->fft_ready && srv->fft_seq != last_seq;
		seq = srv->fft_seq;
		if (!gap && ready)
			memcpy(snapshot, srv->fft_power, bytes);
		pthread_mutex_unlock(&srv->fft_mutex);
		if (gap)
			break;
		if (!ready) {
			if (socket_closed(cl->fd))
				break;
			wait_data(srv);
			continue;
		}
		{
			struct rtlsdr_stream_fft_hdr fft = {RTLSTREAM_MAGIC_FFT,
				srv->capture_freq, srv->capture_rate, seq, (uint32_t)srv->fft_bins};
			if (!client_current(cl) || rtlsdr_stream_encode_fft_hdr(hdr, &fft) < 0 ||
			    rtlsdr_stream_encode_fft_payload(wire, bytes, snapshot, srv->fft_bins) < 0 ||
			    rtlsdr_stream_sendv(cl->fd, iov, 2) < 0)
				break;
		}
		last_seq = seq;
	}
done:
	free(snapshot);
	free(wire);
}

static void *client_handler(void *arg)
{
	struct rtlsdr_client *cl = arg;
	struct rtlsdr_server *srv = cl->ctx;
	uint8_t buf[RTLSTREAM_REQ_SIZE];
	struct rtlsdr_stream_req req;
	const char *reason = NULL;
	if (receive_request(cl->fd, buf) < 0) {
		reason = "incomplete request or request deadline";
		goto done;
	}
	if (rtlsdr_stream_decode_req(&req, buf) < 0 || req.magic != RTLSTREAM_MAGIC_REQ) {
		reason = "invalid protocol request";
		goto done;
	}
	if (req.mode != cl->mode) {
		reason = "request mode does not match listener";
		goto done;
	}
	if (!client_current(cl))
		goto done;
	if (cl->mode == RTLSTREAM_MODE_FFT) {
		if (req.freq || req.rate || req.bandwidth || !srv->fft_plan) {
			reason = "FFT request fields must be zero";
			goto done;
		}
	} else {
		int64_t offset;
		uint64_t bandwidth;
		if (req.freq > UINT32_MAX || srv->capture_freq > UINT32_MAX) {
			reason = "frequency outside hardware range";
			goto done;
		}
		offset = (int64_t)req.freq - (int64_t)srv->capture_freq;
		if (rtlsdr_dsp_validate(srv->capture_rate, req.rate, req.bandwidth,
				offset, &bandwidth) < 0) {
			reason = "invalid rate, bandwidth or channel placement";
			goto done;
		}
		cl->freq = req.freq;
		cl->rate = req.rate;
		cl->bandwidth = bandwidth;
		cl->dsp = rtlsdr_dsp_create(srv->capture_rate, req.rate, bandwidth, offset);
		if (!cl->dsp || rtlsdr_ring_cursor_now(srv->ring, &cl->cursor) < 0) {
			reason = "DSP initialization failed";
			goto done;
		}
	}
	pthread_mutex_lock(&srv->client_lock);
	cl->state = RTL_CLIENT_RUNNING;
	pthread_mutex_unlock(&srv->client_lock);
	if (cl->mode == RTLSTREAM_MODE_FFT)
		fft_client_handler(cl);
	else
		iq_client_handler(cl);
done:
	if (reason)
		fprintf(stderr, "client %d: %s\n", (int)(cl - srv->clients), reason);
	pthread_mutex_lock(&srv->client_lock);
	cl->state = RTL_CLIENT_DONE;
	pthread_mutex_unlock(&srv->client_lock);
	wake_control(srv);
	return NULL;
}

int rtlsdr_server_process_fft(struct rtlsdr_server *srv,
	const uint8_t *input, size_t input_len)
{
	size_t pos = 0;
	int i;
	if (!srv || !input || (input_len & 1) || !srv->fft_plan ||
	    srv->fft_bins < 2 || srv->fft_step < srv->fft_bins || srv->fft_accum < 1)
		return -1;
	while (pos < input_len) {
		int remaining = srv->fft_step - srv->fft_nsamples;
		size_t count = (input_len - pos) / 2;
		if (srv->fft_nsamples < srv->fft_bins)
			remaining = srv->fft_bins - srv->fft_nsamples;
		if (count > (size_t)remaining)
			count = (size_t)remaining;
		/* Collect contiguous bins, then skip to the actual complex-sample hop end. */
		if (srv->fft_nsamples < srv->fft_bins) {
			for (size_t j = 0; j < count; j++) {
				size_t off = ((size_t)srv->fft_nsamples + j) * 2;
				srv->fft_in[off] = (float)input[pos + j * 2] - 127.5f;
				srv->fft_in[off + 1] = (float)input[pos + j * 2 + 1] - 127.5f;
			}
		}
		pos += count * 2;
		srv->fft_nsamples += (int)count;
		if (srv->fft_nsamples == srv->fft_step) {
			for (i = 0; i < srv->fft_bins; i++) {
				srv->fft_in[i * 2] *= srv->fft_window[i];
				srv->fft_in[i * 2 + 1] *= srv->fft_window[i];
			}
			if (fft_execute(srv->fft_plan, srv->fft_in, srv->fft_in) < 0)
				return -1; /* Worker stops before accumulation or publication. */
			for (i = 0; i < srv->fft_bins; i++)
				srv->fft_acc[i] += srv->fft_in[i * 2] * srv->fft_in[i * 2] +
					srv->fft_in[i * 2 + 1] * srv->fft_in[i * 2 + 1];
			srv->fft_accum_count++;
			srv->fft_nsamples = 0;
			if (srv->fft_accum_count >= srv->fft_accum) {
				pthread_mutex_lock(&srv->fft_mutex);
				for (i = 0; i < srv->fft_bins; i++) {
					srv->fft_power[i] = srv->fft_acc[i] / (float)srv->fft_accum_count;
					srv->fft_acc[i] = 0.0f;
				}
				srv->fft_seq++;
				srv->fft_ready = 1;
				pthread_cond_broadcast(&srv->fft_cond);
				pthread_mutex_unlock(&srv->fft_mutex);
				srv->fft_accum_count = 0;
			}
		}
	}
	return 0;
}

static void fft_gap(struct rtlsdr_server *srv)
{
	uint64_t generation;
	srv->fft_nsamples = srv->fft_accum_count = 0;
	memset(srv->fft_in, 0, (size_t)srv->fft_bins * 2 * sizeof(float));
	memset(srv->fft_acc, 0, (size_t)srv->fft_bins * sizeof(float));
	pthread_mutex_lock(&srv->fft_mutex);
	generation = ++srv->fft_generation;
	srv->fft_ready = 0;
	memset(srv->fft_power, 0, (size_t)srv->fft_bins * sizeof(float));
	pthread_cond_broadcast(&srv->fft_cond);
	pthread_mutex_unlock(&srv->fft_mutex);
	pthread_mutex_lock(&srv->client_lock);
	for (int i = 0; i < RTLSTREAM_MAX_CLIENTS; i++) {
		struct rtlsdr_client *cl = &srv->clients[i];
		if ((cl->state == RTL_CLIENT_STARTING || cl->state == RTL_CLIENT_RUNNING) &&
		    cl->mode == RTLSTREAM_MODE_FFT && cl->fft_generation != generation)
			shutdown(cl->fd, SHUT_RDWR);
	}
	pthread_mutex_unlock(&srv->client_lock);
}

static void *fft_compute_thread(void *arg)
{
	struct rtlsdr_server *srv = arg;
	uint8_t *input = srv->fft_input;
	while (!atomic_load(&srv->stopping) && session_current(srv, 1, NULL)) {
		size_t len;
		int result = rtlsdr_ring_read_copy(srv->ring, &srv->fft_cursor,
			input, srv->ring->slot_size, &len);
		if (result == RTL_RING_GAP) {
			fft_gap(srv);
			rtlsdr_ring_cursor_now(srv->ring, &srv->fft_cursor);
		} else if (result == RTL_RING_EMPTY) {
			wait_data(srv);
		} else if (result != RTL_RING_DATA || rtlsdr_server_process_fft(srv, input, len) < 0) {
			rtlsdr_server_request_stop(srv);
			break;
		}
	}
	return NULL;
}

int rtlsdr_server_publish(struct rtlsdr_server *srv, const uint8_t *input, size_t len)
{
	int result = 0;
	if (!srv || !srv->ring || (!input && len) || atomic_load(&srv->stopping))
		return -1;
	if (!len)
		return 0;
	if (len & 1) {
		rtlsdr_ring_mark_gap(srv->ring);
		result = -1;
	} else {
		while (len) {
			size_t count = len < srv->ring->slot_size ? len : srv->ring->slot_size;
			if (rtlsdr_ring_write_copy(srv->ring, input, count) < 0) {
				rtlsdr_ring_mark_gap(srv->ring);
				result = -1;
				break;
			}
			input += count;
			len -= count;
		}
	}
	pthread_mutex_lock(&srv->data_mutex);
	pthread_cond_broadcast(&srv->data_cond);
	pthread_mutex_unlock(&srv->data_mutex);
	return result;
}

static int nonblocking(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);
	return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int setup_listen_fd(int port)
{
	struct sockaddr_in addr;
	int fd = socket(AF_INET, SOCK_STREAM, 0), opt = 1;
	if (fd < 0)
		return -1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons((uint16_t)port);
	if (nonblocking(fd) < 0 || bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
	    listen(fd, 8) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

int rtlsdr_server_init(struct rtlsdr_server *srv, int iq_port, int fft_port,
	struct rtlsdr_ring *ring, uint64_t capture_freq, uint64_t capture_rate)
{
	if (!srv)
		return -1;
	memset(srv, 0, sizeof(*srv));
	srv->listen_fd_iq = srv->listen_fd_fft = -1;
	srv->control[0] = srv->control[1] = -1;
	for (int i = 0; i < RTLSTREAM_MAX_CLIENTS; i++)
		srv->clients[i].fd = -1;
	atomic_init(&srv->stopping, 0);
	if (!ring || (ring->slot_size & 1) ||
	    ring->slot_size > RTLSTREAM_MAX_IQ_COMPONENTS ||
	    iq_port < 0 || iq_port > 65535 || fft_port < 0 || fft_port > 65535 ||
	    capture_freq > UINT32_MAX || !capture_rate || capture_rate > UINT32_MAX)
		goto fail;
	srv->iq_port = iq_port;
	srv->fft_port = fft_port;
	srv->ring = ring;
	srv->capture_freq = capture_freq;
	srv->capture_rate = capture_rate;
	srv->capture_epoch = srv->fft_generation = 1;
	if (pthread_mutex_init(&srv->data_mutex, NULL))
		goto fail;
	srv->initialized |= INIT_DATA_MUTEX;
	if (pthread_cond_init(&srv->data_cond, NULL))
		goto fail;
	srv->initialized |= INIT_DATA_COND;
	if (pthread_mutex_init(&srv->client_lock, NULL))
		goto fail;
	srv->initialized |= INIT_CLIENT_LOCK;
	if (pthread_mutex_init(&srv->fft_mutex, NULL))
		goto fail;
	srv->initialized |= INIT_FFT_MUTEX;
	if (pthread_cond_init(&srv->fft_cond, NULL))
		goto fail;
	srv->initialized |= INIT_FFT_COND;
	if (pipe(srv->control) < 0 || nonblocking(srv->control[0]) < 0 ||
	    nonblocking(srv->control[1]) < 0)
		goto fail;
	srv->listen_fd_iq = setup_listen_fd(iq_port);
	if (srv->listen_fd_iq < 0)
		goto fail;
	if (fft_port > 0) {
		srv->listen_fd_fft = setup_listen_fd(fft_port);
		if (srv->listen_fd_fft < 0)
			goto fail;
		srv->fft_bins = FFT_DEFAULT_BINS;
		srv->fft_step = FFT_DEFAULT_STEP;
		srv->fft_accum = FFT_DEFAULT_ACCUM;
		srv->fft_plan = fft_plan_create(srv->fft_bins);
		if (!srv->fft_plan)
			goto fail;
		srv->fft_window = fft_get_window(srv->fft_plan);
		srv->fft_in = calloc((size_t)srv->fft_bins * 2, sizeof(float));
		srv->fft_power = calloc(srv->fft_bins, sizeof(float));
		srv->fft_acc = calloc(srv->fft_bins, sizeof(float));
		if (!srv->fft_in || !srv->fft_power || !srv->fft_acc)
			goto fail;
		srv->fft_input = malloc(ring->slot_size);
		if (!srv->fft_input)
			goto fail;
	}
	return 0;
fail:
	rtlsdr_server_shutdown(srv);
	return -1;
}

void rtlsdr_server_request_stop(struct rtlsdr_server *srv)
{
	if (!srv)
		return;
	atomic_store(&srv->stopping, 1);
	if (srv->initialized & INIT_CLIENT_LOCK) {
		pthread_mutex_lock(&srv->client_lock);
		for (int i = 0; i < RTLSTREAM_MAX_CLIENTS; i++) {
			struct rtlsdr_client *cl = &srv->clients[i];
			if (cl->state == RTL_CLIENT_STARTING || cl->state == RTL_CLIENT_RUNNING)
				shutdown(cl->fd, SHUT_RDWR);
		}
		pthread_mutex_unlock(&srv->client_lock);
	}
	if (srv->initialized & INIT_DATA_MUTEX) {
		pthread_mutex_lock(&srv->data_mutex);
		if (srv->initialized & INIT_DATA_COND)
			pthread_cond_broadcast(&srv->data_cond);
		pthread_mutex_unlock(&srv->data_mutex);
	}
	if (srv->initialized & INIT_FFT_MUTEX) {
		pthread_mutex_lock(&srv->fft_mutex);
		if (srv->initialized & INIT_FFT_COND)
			pthread_cond_broadcast(&srv->fft_cond);
		pthread_mutex_unlock(&srv->fft_mutex);
	}
	wake_control(srv);
}

void rtlsdr_server_notify_frequency_change(struct rtlsdr_server *srv, uint64_t freq)
{
	if (!srv || !(srv->initialized & INIT_DATA_COND))
		return;
	pthread_mutex_lock(&srv->data_mutex);
	srv->new_freq = freq;
	srv->capture_epoch++;
	pthread_cond_broadcast(&srv->data_cond);
	pthread_mutex_unlock(&srv->data_mutex);
	pthread_mutex_lock(&srv->client_lock);
	for (int i = 0; i < RTLSTREAM_MAX_CLIENTS; i++)
		if (srv->clients[i].state == RTL_CLIENT_STARTING)
			shutdown(srv->clients[i].fd, SHUT_RD);
	pthread_mutex_unlock(&srv->client_lock);
	wake_control(srv);
}

static void reap_clients(struct rtlsdr_server *srv, int all)
{
	for (int i = 0; i < RTLSTREAM_MAX_CLIENTS; i++) {
		struct rtlsdr_client *cl = &srv->clients[i];
		pthread_mutex_lock(&srv->client_lock);
		if (cl->state == RTL_CLIENT_FREE || cl->state == RTL_CLIENT_JOINING ||
		    (!all && cl->state != RTL_CLIENT_DONE)) {
			pthread_mutex_unlock(&srv->client_lock);
			continue;
		}
		cl->state = RTL_CLIENT_JOINING;
		pthread_mutex_unlock(&srv->client_lock);
		pthread_join(cl->thread, NULL);
		/* The worker no longer owns any fields. Only this reaper closes fds. */
		pthread_mutex_lock(&srv->client_lock);
		close(cl->fd);
		cl->fd = -1;
		rtlsdr_dsp_destroy(cl->dsp);
		cl->dsp = NULL;
		cl->state = RTL_CLIENT_FREE;
		srv->client_count--;
		pthread_mutex_unlock(&srv->client_lock);
	}
}

void rtlsdr_server_shutdown(struct rtlsdr_server *srv)
{
	if (!srv)
		return;
	rtlsdr_server_request_stop(srv);
	if (srv->initialized & INIT_DATA_COND) {
		pthread_mutex_lock(&srv->data_mutex);
		while (srv->run_active)
			pthread_cond_wait(&srv->data_cond, &srv->data_mutex);
		pthread_mutex_unlock(&srv->data_mutex);
	}
	if (srv->initialized & INIT_CLIENT_LOCK)
		reap_clients(srv, 1);
	if (srv->fft_plan)
		fft_plan_destroy(srv->fft_plan);
	srv->fft_plan = NULL;
	free(srv->fft_input);
	srv->fft_input = NULL;
	free(srv->fft_in);
	free(srv->fft_power);
	free(srv->fft_acc);
	srv->fft_in = srv->fft_power = srv->fft_acc = NULL;
	if (srv->listen_fd_iq >= 0)
		close(srv->listen_fd_iq);
	if (srv->listen_fd_fft >= 0)
		close(srv->listen_fd_fft);
	if (srv->control[0] >= 0)
		close(srv->control[0]);
	if (srv->control[1] >= 0)
		close(srv->control[1]);
	srv->listen_fd_iq = srv->listen_fd_fft = srv->control[0] = srv->control[1] = -1;
	if (srv->initialized & INIT_FFT_COND)
		pthread_cond_destroy(&srv->fft_cond);
	if (srv->initialized & INIT_FFT_MUTEX)
		pthread_mutex_destroy(&srv->fft_mutex);
	if (srv->initialized & INIT_CLIENT_LOCK)
		pthread_mutex_destroy(&srv->client_lock);
	if (srv->initialized & INIT_DATA_COND)
		pthread_cond_destroy(&srv->data_cond);
	if (srv->initialized & INIT_DATA_MUTEX)
		pthread_mutex_destroy(&srv->data_mutex);
	srv->initialized = 0;
}

static int accept_client(struct rtlsdr_server *srv, int listener, uint8_t mode)
{
	int fd = accept(listener, NULL, NULL), opt = 1;
	struct rtlsdr_client *cl = NULL;
	if (fd < 0)
		return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
	if (nonblocking(fd) < 0 || !session_current(srv, 1, NULL)) {
		close(fd);
		return 0;
	}
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
	pthread_mutex_lock(&srv->client_lock);
	if (!atomic_load(&srv->stopping))
		for (int i = 0; i < RTLSTREAM_MAX_CLIENTS; i++)
			if (srv->clients[i].state == RTL_CLIENT_FREE) {
				cl = &srv->clients[i];
				break;
			}
	if (!cl) {
		close(fd);
		pthread_mutex_unlock(&srv->client_lock);
		return 0;
	}
	memset(cl, 0, sizeof(*cl));
	cl->fd = fd;
	cl->ctx = srv;
	cl->mode = mode;
	cl->epoch = 1;
	pthread_mutex_lock(&srv->fft_mutex);
	cl->fft_generation = srv->fft_generation;
	pthread_mutex_unlock(&srv->fft_mutex);
	cl->state = RTL_CLIENT_STARTING;
	if (pthread_create(&cl->thread, NULL, client_handler, cl)) {
		close(fd);
		cl->fd = -1;
		cl->state = RTL_CLIENT_FREE;
	} else {
		srv->client_count++;
	}
	pthread_mutex_unlock(&srv->client_lock);
	return 0;
}

int rtlsdr_server_run(struct rtlsdr_server *srv)
{
	int result = 0;
	struct pollfd fds[3];
	if (!srv || !(srv->initialized & INIT_FFT_COND))
		return -1;
	pthread_mutex_lock(&srv->data_mutex);
	if (srv->run_active || atomic_load(&srv->stopping)) {
		pthread_mutex_unlock(&srv->data_mutex);
		return -1;
	}
	srv->run_active = 1;
	pthread_mutex_unlock(&srv->data_mutex);
	if (srv->fft_plan) {
		rtlsdr_ring_cursor_now(srv->ring, &srv->fft_cursor);
		if (pthread_create(&srv->fft_thread, NULL, fft_compute_thread, srv)) {
			result = -1;
			goto done;
		}
		srv->fft_thread_started = 1;
	}
	fds[0] = (struct pollfd){srv->control[0], POLLIN, 0};
	fds[1] = (struct pollfd){srv->listen_fd_iq, POLLIN, 0};
	fds[2] = (struct pollfd){srv->listen_fd_fft, POLLIN, 0};
	while (!atomic_load(&srv->stopping)) {
		int n = poll(fds, 3, -1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			result = -1;
			break;
		}
		if (fds[0].revents & POLLIN) {
			uint8_t buf[128];
			while (read(srv->control[0], buf, sizeof(buf)) > 0) {}
		}
		reap_clients(srv, 0);
		for (int i = 1; i < 3 && !atomic_load(&srv->stopping); i++) {
			if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
				result = -1;
				goto done;
			}
			if ((fds[i].revents & POLLIN) &&
			    accept_client(srv, fds[i].fd, i == 1 ? RTLSTREAM_MODE_IQ : RTLSTREAM_MODE_FFT) < 0) {
				result = -1;
				goto done;
			}
		}
	}
done:
	rtlsdr_server_request_stop(srv);
	reap_clients(srv, 1);
	if (srv->fft_thread_started) {
		pthread_join(srv->fft_thread, NULL);
		srv->fft_thread_started = 0;
	}
	pthread_mutex_lock(&srv->data_mutex);
	srv->run_active = 0;
	pthread_cond_broadcast(&srv->data_cond);
	pthread_mutex_unlock(&srv->data_mutex);
	return result;
}
