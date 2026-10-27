#ifndef FFT_BACKEND_H
#define FFT_BACKEND_H

struct fft_plan;

struct fft_plan *fft_plan_create(int nfft);
void fft_plan_destroy(struct fft_plan *plan);
/* Interleaved float I/Q arrays of 2*nfft components: identical or disjoint. */
void fft_execute(struct fft_plan *plan, const float *in, float *out);
float *fft_get_window(struct fft_plan *plan);
const char *fft_backend_name(void);

#endif
