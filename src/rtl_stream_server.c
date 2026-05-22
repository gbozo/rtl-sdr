#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>

#include "rtl_stream_server.h"
#include "rtl_stream_proto.h"
#include "rtl_dsp.h"

#define OUTPUT_BUF_SIZE (256 * 1024)

static void client_cleanup(struct rtlsdr_server *srv, int idx)
{
	pthread_mutex_lock(&srv->client_lock);
	if (srv->clients[idx].dsp) {
		rtlsdr_dsp_destroy((struct rtlsdr_dsp *)srv->clients[idx].dsp);
		srv->clients[idx].dsp = NULL;
	}
	if (srv->clients[idx].fd >= 0) {
		close(srv->clients[idx].fd);
		srv->clients[idx].fd = -1;
	}
	srv->clients[idx].active = 0;
	srv->client_count--;
	pthread_mutex_unlock(&srv->client_lock);
}

static void *client_handler(void *arg)
{
	struct rtlsdr_client *cl = (struct rtlsdr_client *)arg;
	struct rtlsdr_server *srv = (struct rtlsdr_server *)cl->ctx;
	unsigned char req_buf[RTLSTREAM_REQ_SIZE];
	struct rtlsdr_stream_req req;
	unsigned char hdr_buf[RTLSTREAM_IQ_HDR_SIZE];
	int16_t *out_buf;
	int idx = (int)(cl - srv->clients);
	uint64_t seq = 0;

	out_buf = (int16_t *)malloc(OUTPUT_BUF_SIZE);
	if (!out_buf)
		goto done;

	if (recv(cl->fd, req_buf, RTLSTREAM_REQ_SIZE, MSG_WAITALL) != RTLSTREAM_REQ_SIZE)
		goto done;

	rtlsdr_stream_decode_req(&req, req_buf);
	if (req.magic != RTLSTREAM_MAGIC_REQ)
		goto done;

	cl->freq      = req.freq;
	cl->rate      = req.rate;
	cl->bandwidth = req.bandwidth;
	cl->mode      = req.mode;

	fprintf(stderr, "client %d: freq=%lu rate=%lu bw=%lu mode=%d\n",
		idx, (unsigned long)req.freq, (unsigned long)req.rate,
		(unsigned long)req.bandwidth, req.mode);

	if (cl->mode == RTLSTREAM_MODE_IQ && req.rate > 0) {
		struct rtlsdr_dsp *dsp;
		uint64_t freq_offset;
		uint64_t capture_rate = srv->capture_rate;

		if (req.freq > srv->capture_freq)
			freq_offset = req.freq - srv->capture_freq;
		else
			freq_offset = srv->capture_freq - req.freq;

		dsp = rtlsdr_dsp_create(capture_rate, req.rate,
					req.bandwidth, freq_offset);
		if (!dsp) {
			fprintf(stderr, "client %d: DSP create failed\n", idx);
			goto done;
		}
		cl->dsp = dsp;
		cl->read_pos = rtlsdr_ring_write_pos(srv->ring);

		while (srv->running && cl->active) {
			uint32_t wp = rtlsdr_ring_write_pos(srv->ring);
			uint32_t rp = cl->read_pos;

			if (wp == rp) {
				struct timespec ts;
				struct timeval tp;
				gettimeofday(&tp, NULL);
				ts.tv_sec  = tp.tv_sec + 1;
				ts.tv_nsec = tp.tv_usec * 1000;
				pthread_mutex_lock(&srv->data_mutex);
				if (srv->running && cl->active)
					pthread_cond_timedwait(&srv->data_cond,
							       &srv->data_mutex, &ts);
				pthread_mutex_unlock(&srv->data_mutex);
				if (!srv->running || !cl->active)
					break;
				wp = rtlsdr_ring_write_pos(srv->ring);
				rp = cl->read_pos;
			}

			while (rp != wp && srv->running && cl->active) {
				uint32_t slot_len;
				const void *slot_data;
				size_t out_len;

				slot_data = rtlsdr_ring_read_ptr(srv->ring,
								 &slot_len, rp++);
				if (slot_len == 0)
					continue;

				rtlsdr_dsp_process((struct rtlsdr_dsp *)cl->dsp,
						   (const int16_t *)slot_data,
						   slot_len / sizeof(int16_t),
						   out_buf, &out_len);

				if (out_len > 0) {
					struct rtlsdr_stream_iq_hdr iq_hdr;
					iq_hdr.magic = RTLSTREAM_MAGIC_IQ;
					iq_hdr.freq  = req.freq;
					iq_hdr.rate  = req.rate;
					iq_hdr.seq   = seq++;
					rtlsdr_stream_encode_iq_hdr(hdr_buf, &iq_hdr);

					if (send(cl->fd, hdr_buf,
						 RTLSTREAM_IQ_HDR_SIZE, 0) < 0)
						goto done;
					if (send(cl->fd, out_buf,
						 out_len * sizeof(int16_t), 0) < 0)
						goto done;
				}
			}

			cl->read_pos = rp;
		}
	}

done:
	free(out_buf);
	client_cleanup(srv, idx);
	return NULL;
}

int rtlsdr_server_init(struct rtlsdr_server *srv, int iq_port, int fft_port,
		       struct rtlsdr_ring *ring,
		       uint64_t capture_freq, uint64_t capture_rate)
{
	struct sockaddr_in addr;
	int opt = 1;

	memset(srv, 0, sizeof(*srv));
	srv->iq_port      = iq_port;
	srv->fft_port     = fft_port;
	srv->ring         = ring;
	srv->capture_freq = capture_freq;
	srv->capture_rate = capture_rate;
	srv->running      = 1;

	pthread_mutex_init(&srv->data_mutex, NULL);
	pthread_cond_init(&srv->data_cond, NULL);
	pthread_mutex_init(&srv->client_lock, NULL);

	srv->listen_fd_iq = socket(AF_INET, SOCK_STREAM, 0);
	if (srv->listen_fd_iq < 0) {
		perror("socket iq");
		return -1;
	}

	setsockopt(srv->listen_fd_iq, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons((uint16_t)iq_port);

	if (bind(srv->listen_fd_iq, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("bind iq");
		close(srv->listen_fd_iq);
		return -1;
	}

	if (listen(srv->listen_fd_iq, 8) < 0) {
		perror("listen iq");
		close(srv->listen_fd_iq);
		return -1;
	}

	fprintf(stderr, "I/Q server listening on port %d\n", iq_port);

	return 0;
}

void rtlsdr_server_shutdown(struct rtlsdr_server *srv)
{
	int i;

	srv->running = 0;

	pthread_mutex_lock(&srv->data_mutex);
	pthread_cond_broadcast(&srv->data_cond);
	pthread_mutex_unlock(&srv->data_mutex);

	pthread_mutex_lock(&srv->client_lock);
	for (i = 0; i < RTLSTREAM_MAX_CLIENTS; i++) {
		if (srv->clients[i].active) {
			srv->clients[i].active = 0;
			if (srv->clients[i].fd >= 0)
				close(srv->clients[i].fd);
			pthread_join(srv->clients[i].thread, NULL);
		}
	}
	pthread_mutex_unlock(&srv->client_lock);

	if (srv->listen_fd_iq >= 0)
		close(srv->listen_fd_iq);
	if (srv->listen_fd_fft >= 0)
		close(srv->listen_fd_fft);

	pthread_mutex_destroy(&srv->data_mutex);
	pthread_cond_destroy(&srv->data_cond);
	pthread_mutex_destroy(&srv->client_lock);
}

extern volatile int do_exit;

int rtlsdr_server_run(struct rtlsdr_server *srv)
{
	while (srv->running && !do_exit) {
		struct sockaddr_in client_addr;
		socklen_t addr_len = sizeof(client_addr);
		int client_fd;
		int i, idx = -1;
		fd_set readfds;
		struct timeval tv;

		FD_ZERO(&readfds);
		FD_SET(srv->listen_fd_iq, &readfds);
		tv.tv_sec = 1;
		tv.tv_usec = 0;

		if (select(srv->listen_fd_iq + 1, &readfds, NULL, NULL, &tv) <= 0)
			continue;
		if (do_exit)
			break;

		client_fd = accept(srv->listen_fd_iq,
				   (struct sockaddr *)&client_addr, &addr_len);
		if (client_fd < 0) {
			if (errno == EINTR)
				continue;
			perror("accept");
			break;
		}

		pthread_mutex_lock(&srv->client_lock);
		for (i = 0; i < RTLSTREAM_MAX_CLIENTS; i++) {
			if (!srv->clients[i].active) {
				idx = i;
				break;
			}
		}

		if (idx < 0) {
			fprintf(stderr, "max clients reached, rejecting\n");
			close(client_fd);
			pthread_mutex_unlock(&srv->client_lock);
			continue;
		}

		memset(&srv->clients[idx], 0, sizeof(srv->clients[idx]));
		srv->clients[idx].fd       = client_fd;
		srv->clients[idx].active   = 1;
		srv->clients[idx].read_pos = rtlsdr_ring_write_pos(srv->ring);
		srv->clients[idx].ctx      = srv;
		srv->client_count++;
		pthread_mutex_unlock(&srv->client_lock);

		pthread_create(&srv->clients[idx].thread, NULL,
			       client_handler, &srv->clients[idx]);
		pthread_detach(srv->clients[idx].thread);
	}

	return 0;
}
