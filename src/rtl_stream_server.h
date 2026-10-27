#ifndef RTL_STREAM_SERVER_H
#define RTL_STREAM_SERVER_H

#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/uio.h>

#include "rtl_ring.h"
#include "fft/fft_backend.h"

#define RTLSTREAM_MAX_CLIENTS 32
#define FFT_DEFAULT_BINS  1024
#define FFT_DEFAULT_STEP  4096
#define FFT_DEFAULT_ACCUM 4

enum rtlsdr_client_state { RTL_CLIENT_FREE, RTL_CLIENT_STARTING,
	RTL_CLIENT_RUNNING, RTL_CLIENT_DONE, RTL_CLIENT_JOINING };

struct rtlsdr_client {
	int fd;
	enum rtlsdr_client_state state;
	pthread_t thread;
	uint64_t freq, rate, bandwidth;
	uint64_t epoch, fft_generation;
	uint8_t mode;
	struct rtlsdr_ring_cursor cursor;
	void *dsp;
	void *ctx;
};

struct rtlsdr_server {
	int iq_port, fft_port;
	int listen_fd_iq, listen_fd_fft;
	int control[2];
	unsigned initialized;
	atomic_int stopping;
	int run_active;
	struct rtlsdr_ring *ring;
	/* Session metadata is immutable; a retune terminates this session. */
	uint64_t capture_freq, capture_rate;
	uint64_t capture_epoch, new_freq;
	pthread_mutex_t data_mutex;
	pthread_cond_t data_cond;
	pthread_mutex_t client_lock;
	int client_count;
	struct rtlsdr_client clients[RTLSTREAM_MAX_CLIENTS];
	pthread_t fft_thread;
	int fft_thread_started;
	struct fft_plan *fft_plan;
	uint8_t *fft_input;
	float *fft_window, *fft_in, *fft_power;
	int fft_bins;
	int fft_step; /* Hop length in complex samples, >= fft_bins. */
	int fft_accum, fft_nsamples, fft_accum_count;
	struct rtlsdr_ring_cursor fft_cursor;
	uint64_t fft_seq, fft_generation;
	float *fft_acc;
	pthread_mutex_t fft_mutex;
	pthread_cond_t fft_cond;
	int fft_ready;
};

int rtlsdr_server_init(struct rtlsdr_server *srv, int iq_port, int fft_port,
	struct rtlsdr_ring *ring, uint64_t capture_freq, uint64_t capture_rate);
void rtlsdr_server_request_stop(struct rtlsdr_server *srv);
void rtlsdr_server_notify_frequency_change(struct rtlsdr_server *srv, uint64_t freq);
/* Shutdown may wait for run(); never call it from a server worker. */
void rtlsdr_server_shutdown(struct rtlsdr_server *srv);
int rtlsdr_server_run(struct rtlsdr_server *srv);
int rtlsdr_server_publish(struct rtlsdr_server *srv, const uint8_t *input, size_t len);
int rtlsdr_server_process_fft(struct rtlsdr_server *srv,
	const uint8_t *input, size_t input_len);
/* Complete one frame with an absolute five-second deadline; vectors unchanged.
 * The caller owns the socket, which is left in nonblocking mode. */
int rtlsdr_stream_sendv(int fd, const struct iovec *iov, int count);

#endif
