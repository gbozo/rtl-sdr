#ifndef FFT_BACKEND_H
#define FFT_BACKEND_H

struct fft_plan;

struct fft_plan *fft_plan_create(int nfft);
void fft_plan_destroy(struct fft_plan *plan);
/* Power-of-two sizes 2..16384. Forward complex FP32, negative exponent,
 * unnormalized, unshifted; windowing is the caller's responsibility.
 * Arrays have 2*nfft components and must be identical or disjoint.
 * Returns 0 on success, -1 on error (output unchanged). A GPU execution error
 * poisons the plan. Each plan requires externally serialized use/destruction. */
int fft_execute(struct fft_plan *plan, const float *in, float *out);
float *fft_get_window(struct fft_plan *plan);
const char *fft_backend_name(void);

#endif
