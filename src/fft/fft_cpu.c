#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "fft_backend.h"

#define FFT_MAX_NFFT 16384

static double bh_coeff(int n, int N)
{
	double a0 = 0.35875;
	double a1 = 0.48829;
	double a2 = 0.14128;
	double a3 = 0.01168;
	double x = 2.0 * M_PI * n / (N - 1);
	return a0 - a1 * cos(x) + a2 * cos(2.0 * x) - a3 * cos(3.0 * x);
}

static int is_pow2(int n)
{
	return n > 0 && (n & (n - 1)) == 0;
}

static int log2_int(int n)
{
	int l = 0;
	while (n >>= 1)
		l++;
	return l;
}

static void bit_reverse(float *data, int n)
{
	int i, j, m;
	for (i = 0, j = 0; i < n; i++) {
		if (j > i) {
			float tr = data[2 * i];
			float ti = data[2 * i + 1];
			data[2 * i]     = data[2 * j];
			data[2 * i + 1] = data[2 * j + 1];
			data[2 * j]     = tr;
			data[2 * j + 1] = ti;
		}
		m = n >> 1;
		while (m >= 1 && j >= m) {
			j -= m;
			m >>= 1;
		}
		j += m;
	}
}

struct fft_plan {
	int    nfft;
	int    log2n;
	float *window;
	float *twiddle_r;
	float *twiddle_i;
};

struct fft_plan *fft_plan_create(int nfft)
{
	struct fft_plan *plan;
	int i, len;

	if (nfft < 2 || nfft > FFT_MAX_NFFT || !is_pow2(nfft))
		return NULL;

	plan = (struct fft_plan *)calloc(1, sizeof(*plan));
	if (!plan)
		return NULL;

	plan->nfft  = nfft;
	plan->log2n = log2_int(nfft);

	len = nfft / 2;
	plan->twiddle_r = (float *)malloc(len * sizeof(float));
	plan->twiddle_i = (float *)malloc(len * sizeof(float));
	plan->window    = (float *)malloc(nfft * sizeof(float));
	if (!plan->twiddle_r || !plan->twiddle_i || !plan->window)
		goto fail;

	for (i = 0; i < len; i++) {
		double a = -2.0 * M_PI * i / nfft;
		plan->twiddle_r[i] = (float)cos(a);
		plan->twiddle_i[i] = (float)sin(a);
	}

	for (i = 0; i < nfft; i++)
		plan->window[i] = (float)bh_coeff(i, nfft);

	return plan;

fail:
	free(plan->twiddle_r);
	free(plan->twiddle_i);
	free(plan->window);
	free(plan);
	return NULL;
}

void fft_plan_destroy(struct fft_plan *plan)
{
	if (!plan)
		return;
	free(plan->twiddle_r);
	free(plan->twiddle_i);
	free(plan->window);
	free(plan);
}

void fft_execute(struct fft_plan *plan, const float *in, float *out)
{
	int i, j, k, step, half;
	int n = plan->nfft;
	int log2n = plan->log2n;
	float *tw_r = plan->twiddle_r;
	float *tw_i = plan->twiddle_i;

	memcpy(out, in, n * 2 * sizeof(float));
	bit_reverse(out, n);

	for (step = 1, k = 0; k < log2n; k++, step <<= 1) {
		half = step;
		for (i = 0; i < n; i += half * 2) {
			for (j = 0; j < half; j++) {
				int t_idx = j * n / (half * 2);
				float wr = tw_r[t_idx];
				float wi = tw_i[t_idx];
				int i0 = (i + j) * 2;
				int i1 = (i + j + half) * 2;
				float tr = wr * out[i1] - wi * out[i1 + 1];
				float ti = wr * out[i1 + 1] + wi * out[i1];
				out[i1]     = out[i0] - tr;
				out[i1 + 1] = out[i0 + 1] - ti;
				out[i0]    += tr;
				out[i0 + 1] += ti;
			}
		}
	}
}

float *fft_get_window(struct fft_plan *plan)
{
	return plan ? plan->window : NULL;
}
