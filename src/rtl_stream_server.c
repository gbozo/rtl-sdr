#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
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

static void iq_client_handler(struct rtlsdr_client *cl)
{
	struct rtlsdr_server *srv = (struct rtlsdr_server *)cl->ctx;
	unsigned char hdr_buf[RTLSTREAM_IQ_HDR_SIZE];
	unsigned char evt_buf[16];
	int16_t *out_buf;
	int idx = (int)(cl - srv->clients);
	uint64_t seq = 0;
	uint32_t wp, rp;
	size_t out_len;

	out_buf = (int16_t *)malloc(OUTPUT_BUF_SIZE);
	if (!out_buf)
		goto done;

	while (srv->running && cl->active) {
		if (srv->freq_changed) {
			int n = rtlsdr_stream_encode_evt(evt_buf,
				RTLSTREAM_EVT_FREQ_CHANGE, srv->new_freq);
			send(cl->fd, evt_buf, n, MSG_NOSIGNAL);
			n = rtlsdr_stream_encode_evt(evt_buf,
				RTLSTREAM_EVT_STREAM_END, 0);
			send(cl->fd, evt_buf, n, MSG_NOSIGNAL);
			goto done;
		}
		wp = rtlsdr_ring_write_pos(srv->ring);
		rp = cl->read_pos;

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
				iq_hdr.magic    = RTLSTREAM_MAGIC_IQ;
				iq_hdr.freq     = cl->freq;
				iq_hdr.rate     = cl->rate;
				iq_hdr.seq      = seq++;
				iq_hdr.nsamples = (uint32_t)out_len;
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

done:
	free(out_buf);
}

static void fft_client_handler(struct rtlsdr_client *cl)
{
	struct rtlsdr_server *srv = (struct rtlsdr_server *)cl->ctx;
	unsigned char hdr_buf[RTLSTREAM_FFT_HDR_SIZE];
	int idx = (int)(cl - srv->clients);
	uint32_t last_seq = (uint32_t)-1;
	struct timespec ts;

	fprintf(stderr, "fft client %d: connected, bins=%d\n",
		idx, srv->fft_bins);

	while (srv->running && cl->active) {
		pthread_mutex_lock(&srv->fft_mutex);
		if (srv->fft_seq == last_seq) {
			struct timeval tp;
			gettimeofday(&tp, NULL);
			ts.tv_sec  = tp.tv_sec + 1;
			ts.tv_nsec = tp.tv_usec * 1000;
			pthread_cond_timedwait(&srv->fft_cond,
					       &srv->fft_mutex, &ts);
			if (!srv->running || !cl->active) {
				pthread_mutex_unlock(&srv->fft_mutex);
				break;
			}
		}
		if (srv->fft_seq != last_seq) {
			struct rtlsdr_stream_fft_hdr fft_hdr;
			fft_hdr.magic = RTLSTREAM_MAGIC_FFT;
			fft_hdr.freq  = srv->capture_freq;
			fft_hdr.rate  = srv->capture_rate /
					(uint64_t)(srv->fft_step *
						   srv->fft_accum);
			fft_hdr.seq   = srv->fft_seq;
			fft_hdr.bins  = (uint32_t)srv->fft_bins;
			last_seq = srv->fft_seq;

			rtlsdr_stream_encode_fft_hdr(hdr_buf, &fft_hdr);
			pthread_mutex_unlock(&srv->fft_mutex);

			if (send(cl->fd, hdr_buf,
				 RTLSTREAM_FFT_HDR_SIZE, 0) < 0)
				goto done;
			if (send(cl->fd, srv->fft_power,
				 srv->fft_bins * sizeof(float), 0) < 0)
				goto done;
		} else {
			pthread_mutex_unlock(&srv->fft_mutex);
		}
	}

done:
	fprintf(stderr, "fft client %d: disconnected\n", idx);
}

static void *client_handler(void *arg)
{
	struct rtlsdr_client *cl = (struct rtlsdr_client *)arg;
	struct rtlsdr_server *srv = (struct rtlsdr_server *)cl->ctx;
	unsigned char req_buf[RTLSTREAM_REQ_SIZE];
	struct rtlsdr_stream_req req;
	int idx = (int)(cl - srv->clients);

	if (recv(cl->fd, req_buf, RTLSTREAM_REQ_SIZE, MSG_WAITALL) !=
	    RTLSTREAM_REQ_SIZE)
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

	if (cl->mode == RTLSTREAM_MODE_FFT) {
		fft_client_handler(cl);
		goto done;
	}

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

		iq_client_handler(cl);
	}

done:
	client_cleanup(srv, idx);
	return NULL;
}

extern volatile int do_exit;

static void *fft_compute_thread(void *arg)
{
	struct rtlsdr_server *srv = (struct rtlsdr_server *)arg;
	int need = srv->fft_bins * 2;
	int accum_count = 0;
	int i;

	while (srv->running && !do_exit) {
		uint32_t wp = rtlsdr_ring_write_pos(srv->ring);
		uint32_t rp = srv->fft_read_pos;

		while (rp != wp && srv->fft_nsamples < srv->fft_step) {
			uint32_t slot_len;
			const void *data;
			int cnt;

			data = rtlsdr_ring_read_ptr(srv->ring, &slot_len, rp++);
			if (slot_len == 0)
				continue;

			cnt = slot_len / (int)sizeof(int16_t) / 2;
			if (cnt > srv->fft_bins)
				cnt = srv->fft_bins;
			for (i = 0; i < cnt &&
			     srv->fft_nsamples < srv->fft_step; i++) {
				int off = srv->fft_nsamples * 2;
				const int16_t *s = (const int16_t *)data;
				srv->fft_in[off]     = (float)s[i * 2];
				srv->fft_in[off + 1] = (float)s[i * 2 + 1];
				srv->fft_nsamples++;
			}
		}
		srv->fft_read_pos = rp;

		if (srv->fft_nsamples >= srv->fft_step) {
			float *buf = (float *)malloc(need * sizeof(float));
			if (buf) {
				for (i = 0; i < srv->fft_bins; i++) {
					buf[i * 2]     = srv->fft_in[i * 2] *
							 srv->fft_window[i];
					buf[i * 2 + 1] = srv->fft_in[i * 2 + 1] *
							 srv->fft_window[i];
				}
				fft_execute(srv->fft_plan, buf, buf);
				for (i = 0; i < srv->fft_bins; i++)
					srv->fft_acc[i] += buf[i * 2] *
							   buf[i * 2] +
							   buf[i * 2 + 1] *
							   buf[i * 2 + 1];
				free(buf);
			}
			accum_count++;
			srv->fft_nsamples = 0;

			if (accum_count >= srv->fft_accum) {
				pthread_mutex_lock(&srv->fft_mutex);
				for (i = 0; i < srv->fft_bins; i++) {
					srv->fft_power[i] = srv->fft_acc[i] /
							    (float)accum_count;
					srv->fft_acc[i] = 0.0f;
				}
				srv->fft_seq++;
				srv->fft_ready = 1;
				pthread_cond_broadcast(&srv->fft_cond);
				pthread_mutex_unlock(&srv->fft_mutex);
				accum_count = 0;
			}
		}

		if (rp == wp)
			usleep(5000);
	}

	return NULL;
}

static int setup_listen_fd(int port)
{
	struct sockaddr_in addr;
	int fd, opt = 1;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("socket");
		return -1;
	}

	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons((uint16_t)port);

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("bind");
		close(fd);
		return -1;
	}

	if (listen(fd, 8) < 0) {
		perror("listen");
		close(fd);
		return -1;
	}

	return fd;
}

int rtlsdr_server_init(struct rtlsdr_server *srv, int iq_port, int fft_port,
		       struct rtlsdr_ring *ring,
		       uint64_t capture_freq, uint64_t capture_rate)
{
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

	srv->listen_fd_iq = setup_listen_fd(iq_port);
	if (srv->listen_fd_iq < 0)
		return -1;
	fprintf(stderr, "I/Q server listening on port %d\n", iq_port);

	if (fft_port > 0) {
		srv->listen_fd_fft = setup_listen_fd(fft_port);
		if (srv->listen_fd_fft < 0)
			return -1;
		fprintf(stderr, "FFT server listening on port %d\n", fft_port);

		srv->fft_bins  = FFT_DEFAULT_BINS;
		srv->fft_step  = FFT_DEFAULT_STEP;
		srv->fft_accum = FFT_DEFAULT_ACCUM;

		srv->fft_plan = fft_plan_create(srv->fft_bins);
		if (!srv->fft_plan) {
			fprintf(stderr, "FFT plan create failed\n");
			return -1;
		}
		srv->fft_window = fft_get_window(srv->fft_plan);
		srv->fft_in  = (float *)calloc(srv->fft_step * 2,
					       sizeof(float));
		srv->fft_power = (float *)calloc(srv->fft_bins,
						 sizeof(float));
		srv->fft_acc   = (float *)calloc(srv->fft_bins,
						 sizeof(float));
		if (!srv->fft_in || !srv->fft_power || !srv->fft_acc) {
			fprintf(stderr, "FFT buffer alloc failed\n");
			return -1;
		}

		pthread_mutex_init(&srv->fft_mutex, NULL);
		pthread_cond_init(&srv->fft_cond, NULL);
	}

	return 0;
}

void rtlsdr_server_shutdown(struct rtlsdr_server *srv)
{
	int i;

	srv->running = 0;

	pthread_mutex_lock(&srv->data_mutex);
	pthread_cond_broadcast(&srv->data_cond);
	pthread_mutex_unlock(&srv->data_mutex);

	pthread_mutex_lock(&srv->fft_mutex);
	pthread_cond_broadcast(&srv->fft_cond);
	pthread_mutex_unlock(&srv->fft_mutex);

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

	if (srv->fft_plan) {
		fft_plan_destroy(srv->fft_plan);
		srv->fft_plan = NULL;
	}
	free(srv->fft_in);
	free(srv->fft_power);
	free(srv->fft_acc);

	if (srv->listen_fd_iq >= 0)
		close(srv->listen_fd_iq);
	if (srv->listen_fd_fft >= 0)
		close(srv->listen_fd_fft);

	pthread_mutex_destroy(&srv->data_mutex);
	pthread_cond_destroy(&srv->data_cond);
	pthread_mutex_destroy(&srv->client_lock);
	pthread_mutex_destroy(&srv->fft_mutex);
	pthread_cond_destroy(&srv->fft_cond);
}

static int accept_client(struct rtlsdr_server *srv, int listen_fd)
{
	struct sockaddr_in client_addr;
	socklen_t addr_len;
	int client_fd;
	int i, idx;

	addr_len = sizeof(client_addr);
	client_fd = accept(listen_fd, (struct sockaddr *)&client_addr,
			   &addr_len);
	if (client_fd < 0) {
		if (errno == EINTR)
			return 0;
		perror("accept");
		return -1;
	}

	pthread_mutex_lock(&srv->client_lock);
	idx = -1;
	for (i = 0; i < RTLSTREAM_MAX_CLIENTS; i++) {
		if (!srv->clients[i].active) {
			idx = i;
			break;
		}
	}
	if (idx < 0) {
		fprintf(stderr, "max clients, rejecting\n");
		close(client_fd);
		pthread_mutex_unlock(&srv->client_lock);
		return 0;
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
	return 0;
}

int rtlsdr_server_run(struct rtlsdr_server *srv)
{
	int max_fd;
	fd_set readfds;
	struct timeval tv;
	int ret;

	if (srv->fft_plan) {
		srv->fft_read_pos = rtlsdr_ring_write_pos(srv->ring);
		pthread_create(&srv->fft_thread, NULL,
			       fft_compute_thread, srv);
		pthread_detach(srv->fft_thread);
	}

	while (srv->running && !do_exit) {
		FD_ZERO(&readfds);
		FD_SET(srv->listen_fd_iq, &readfds);
		max_fd = srv->listen_fd_iq;

		if (srv->listen_fd_fft >= 0) {
			FD_SET(srv->listen_fd_fft, &readfds);
			if (srv->listen_fd_fft > max_fd)
				max_fd = srv->listen_fd_fft;
		}

		tv.tv_sec = 1;
		tv.tv_usec = 0;

		ret = select(max_fd + 1, &readfds, NULL, NULL, &tv);
		if (ret <= 0)
			continue;
		if (do_exit)
			break;

		if (FD_ISSET(srv->listen_fd_iq, &readfds)) {
			if (accept_client(srv, srv->listen_fd_iq) < 0)
				break;
		}

		if (srv->listen_fd_fft >= 0 &&
		    FD_ISSET(srv->listen_fd_fft, &readfds)) {
			if (accept_client(srv, srv->listen_fd_fft) < 0)
				break;
		}
	}

	return 0;
}
