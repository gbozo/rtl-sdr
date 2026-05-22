#ifndef RTL_STREAM_SERVER_H
#define RTL_STREAM_SERVER_H

#include <stdint.h>
#include <pthread.h>

#include "rtl_ring.h"
#include "fft/fft_backend.h"

#define RTLSTREAM_MAX_CLIENTS 32

#define FFT_DEFAULT_BINS  1024
#define FFT_DEFAULT_STEP  4096
#define FFT_DEFAULT_ACCUM 4

struct rtlsdr_client {
	int       fd;
	uint8_t   active;
	pthread_t thread;

	uint64_t  freq;
	uint64_t  rate;
	uint64_t  bandwidth;
	uint8_t   mode;

	uint32_t  read_pos;
	void     *dsp;
	void     *ctx;
};

struct rtlsdr_server {
	int           iq_port;
	int           fft_port;
	int           listen_fd_iq;
	int           listen_fd_fft;
	volatile int  running;

	struct rtlsdr_ring *ring;

	uint64_t  capture_freq;
	uint64_t  capture_rate;

	int       freq_changed;
	uint64_t  new_freq;

	pthread_mutex_t data_mutex;
	pthread_cond_t  data_cond;

	pthread_mutex_t client_lock;
	int             client_count;
	struct rtlsdr_client clients[RTLSTREAM_MAX_CLIENTS];

	pthread_t       fft_thread;
	struct fft_plan *fft_plan;
	float          *fft_window;
	float          *fft_in;
	float          *fft_power;
	int             fft_bins;
	int             fft_step;
	int             fft_accum;
	int             fft_nsamples;
	uint32_t        fft_read_pos;
	uint32_t        fft_seq;
	float          *fft_acc;

	pthread_mutex_t fft_mutex;
	pthread_cond_t  fft_cond;
	int             fft_ready;
};

int rtlsdr_server_init(struct rtlsdr_server *srv, int iq_port, int fft_port,
		       struct rtlsdr_ring *ring,
		       uint64_t capture_freq, uint64_t capture_rate);
void rtlsdr_server_shutdown(struct rtlsdr_server *srv);
int rtlsdr_server_run(struct rtlsdr_server *srv);

#endif
