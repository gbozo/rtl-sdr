#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "rtl_dsp.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAX_TAPS 16384
#define MAX_STAGES 5

struct dsp_stage {
	double  *coeffs;
	double  *delay_i;
	double  *delay_q;
	int      num_taps;
	int      decimation;
	int      counter;
	int      wp;
};

struct rtlsdr_dsp {
	uint64_t input_rate;
	uint64_t output_rate;
	uint64_t bandwidth;
	uint64_t freq_offset;

	double   phase;
	double   phase_inc;

	int      num_stages;
	struct dsp_stage stages[MAX_STAGES];

	int (*process_fn)(struct rtlsdr_dsp *, const int16_t *, size_t,
	                  int16_t *, size_t *);
};

static double blackman_harris(int n, int N)
{
	double a0 = 0.35875, a1 = 0.48829, a2 = 0.14128, a3 = 0.01168;
	double t = 2.0 * M_PI * n / (N - 1);
	return a0 - a1 * cos(t) + a2 * cos(2 * t) - a3 * cos(3 * t);
}

static void design_lowpass(double *coeffs, int num_taps, double cutoff)
{
	int i, mid = (num_taps - 1) / 2;
	double sum = 0.0;

	for (i = 0; i < num_taps; i++) {
		int k = i - mid;
		double sinc;
		if (k == 0)
			sinc = 2.0 * cutoff;
		else
			sinc = sin(2.0 * M_PI * cutoff * k) / (M_PI * k);
		coeffs[i] = sinc * blackman_harris(i, num_taps);
		sum += coeffs[i];
	}

	if (sum != 0.0)
		for (i = 0; i < num_taps; i++)
			coeffs[i] /= sum;
}

static inline int stage_run(struct dsp_stage *st,
			    double in_i, double in_q,
			    double *out_i, double *out_q)
{
	st->delay_i[st->wp] = in_i;
	st->delay_q[st->wp] = in_q;
	st->wp = (st->wp + 1) % st->num_taps;
	if (++st->counter >= st->decimation) {
		double sum_i = 0.0, sum_q = 0.0;
		int wp = st->wp;
		st->counter = 0;
		for (int j = 0; j < st->num_taps; j++) {
			int idx = (wp - 1 - j + st->num_taps) % st->num_taps;
			sum_i += st->coeffs[j] * st->delay_i[idx];
			sum_q += st->coeffs[j] * st->delay_q[idx];
		}
		*out_i = sum_i;
		*out_q = sum_q;
		return 1;
	}
	return 0;
}

/* ---- 1-stage loops: tiers 1-3 (BW >= 300k) ---- */

static int process_t01(struct rtlsdr_dsp *dsp,
		       const int16_t *input, size_t input_len,
		       int16_t *output, size_t *output_len)
{
	struct dsp_stage *s1 = &dsp->stages[0];
	size_t in_idx = 0, out_idx = 0;
	double out_i, out_q;

	while (in_idx + 1 < input_len) {
		double cos_ph = cos(dsp->phase);
		double sin_ph = sin(dsp->phase);
		double mix_i = (double)input[in_idx] * cos_ph +
			       (double)input[in_idx + 1] * sin_ph;
		double mix_q = (double)input[in_idx + 1] * cos_ph -
			       (double)input[in_idx] * sin_ph;
		in_idx += 2;

		dsp->phase += dsp->phase_inc;
		if (dsp->phase >= 2.0 * M_PI) dsp->phase -= 2.0 * M_PI;
		if (dsp->phase < 0.0) dsp->phase += 2.0 * M_PI;

		if (stage_run(s1, mix_i, mix_q, &out_i, &out_q)) {
			if (out_i > 32767.0) out_i = 32767.0;
			if (out_i < -32768.0) out_i = -32768.0;
			if (out_q > 32767.0) out_q = 32767.0;
			if (out_q < -32768.0) out_q = -32768.0;
			output[out_idx++] = (int16_t)out_i;
			output[out_idx++] = (int16_t)out_q;
		}
	}

	*output_len = out_idx;
	return 0;
}

static int process_t02(struct rtlsdr_dsp *dsp,
		       const int16_t *input, size_t input_len,
		       int16_t *output, size_t *output_len)
{
	struct dsp_stage *s1 = &dsp->stages[0];
	size_t in_idx = 0, out_idx = 0;
	double out_i, out_q;

	while (in_idx + 1 < input_len) {
		double cos_ph = cos(dsp->phase);
		double sin_ph = sin(dsp->phase);
		double mix_i = (double)input[in_idx] * cos_ph +
			       (double)input[in_idx + 1] * sin_ph;
		double mix_q = (double)input[in_idx + 1] * cos_ph -
			       (double)input[in_idx] * sin_ph;
		in_idx += 2;

		dsp->phase += dsp->phase_inc;
		if (dsp->phase >= 2.0 * M_PI) dsp->phase -= 2.0 * M_PI;
		if (dsp->phase < 0.0) dsp->phase += 2.0 * M_PI;

		if (stage_run(s1, mix_i, mix_q, &out_i, &out_q)) {
			if (out_i > 32767.0) out_i = 32767.0;
			if (out_i < -32768.0) out_i = -32768.0;
			if (out_q > 32767.0) out_q = 32767.0;
			if (out_q < -32768.0) out_q = -32768.0;
			output[out_idx++] = (int16_t)out_i;
			output[out_idx++] = (int16_t)out_q;
		}
	}

	*output_len = out_idx;
	return 0;
}

static int process_t03(struct rtlsdr_dsp *dsp,
		       const int16_t *input, size_t input_len,
		       int16_t *output, size_t *output_len)
{
	struct dsp_stage *s1 = &dsp->stages[0];
	size_t in_idx = 0, out_idx = 0;
	double out_i, out_q;

	while (in_idx + 1 < input_len) {
		double cos_ph = cos(dsp->phase);
		double sin_ph = sin(dsp->phase);
		double mix_i = (double)input[in_idx] * cos_ph +
			       (double)input[in_idx + 1] * sin_ph;
		double mix_q = (double)input[in_idx + 1] * cos_ph -
			       (double)input[in_idx] * sin_ph;
		in_idx += 2;

		dsp->phase += dsp->phase_inc;
		if (dsp->phase >= 2.0 * M_PI) dsp->phase -= 2.0 * M_PI;
		if (dsp->phase < 0.0) dsp->phase += 2.0 * M_PI;

		if (stage_run(s1, mix_i, mix_q, &out_i, &out_q)) {
			if (out_i > 32767.0) out_i = 32767.0;
			if (out_i < -32768.0) out_i = -32768.0;
			if (out_q > 32767.0) out_q = 32767.0;
			if (out_q < -32768.0) out_q = -32768.0;
			output[out_idx++] = (int16_t)out_i;
			output[out_idx++] = (int16_t)out_q;
		}
	}

	*output_len = out_idx;
	return 0;
}

/* ---- 2-stage loops: tiers 4-5 (150k ≤ BW < 300k and 75k ≤ BW < 150k) ---- */

static int process_t04(struct rtlsdr_dsp *dsp,
		       const int16_t *input, size_t input_len,
		       int16_t *output, size_t *output_len)
{
	struct dsp_stage *s1 = &dsp->stages[0];
	struct dsp_stage *s2 = &dsp->stages[1];
	size_t in_idx = 0, out_idx = 0;
	double s1o_i, s1o_q, s2o_i, s2o_q;

	while (in_idx + 1 < input_len) {
		double cos_ph = cos(dsp->phase);
		double sin_ph = sin(dsp->phase);
		double mix_i = (double)input[in_idx] * cos_ph +
			       (double)input[in_idx + 1] * sin_ph;
		double mix_q = (double)input[in_idx + 1] * cos_ph -
			       (double)input[in_idx] * sin_ph;
		in_idx += 2;

		dsp->phase += dsp->phase_inc;
		if (dsp->phase >= 2.0 * M_PI) dsp->phase -= 2.0 * M_PI;
		if (dsp->phase < 0.0) dsp->phase += 2.0 * M_PI;

		if (stage_run(s1, mix_i, mix_q, &s1o_i, &s1o_q)) {
			if (stage_run(s2, s1o_i, s1o_q, &s2o_i, &s2o_q)) {
				if (s2o_i > 32767.0) s2o_i = 32767.0;
				if (s2o_i < -32768.0) s2o_i = -32768.0;
				if (s2o_q > 32767.0) s2o_q = 32767.0;
				if (s2o_q < -32768.0) s2o_q = -32768.0;
				output[out_idx++] = (int16_t)s2o_i;
				output[out_idx++] = (int16_t)s2o_q;
			}
		}
	}

	*output_len = out_idx;
	return 0;
}

static int process_t05(struct rtlsdr_dsp *dsp,
		       const int16_t *input, size_t input_len,
		       int16_t *output, size_t *output_len)
{
	struct dsp_stage *s1 = &dsp->stages[0];
	struct dsp_stage *s2 = &dsp->stages[1];
	size_t in_idx = 0, out_idx = 0;
	double s1o_i, s1o_q, s2o_i, s2o_q;

	while (in_idx + 1 < input_len) {
		double cos_ph = cos(dsp->phase);
		double sin_ph = sin(dsp->phase);
		double mix_i = (double)input[in_idx] * cos_ph +
			       (double)input[in_idx + 1] * sin_ph;
		double mix_q = (double)input[in_idx + 1] * cos_ph -
			       (double)input[in_idx] * sin_ph;
		in_idx += 2;

		dsp->phase += dsp->phase_inc;
		if (dsp->phase >= 2.0 * M_PI) dsp->phase -= 2.0 * M_PI;
		if (dsp->phase < 0.0) dsp->phase += 2.0 * M_PI;

		if (stage_run(s1, mix_i, mix_q, &s1o_i, &s1o_q)) {
			if (stage_run(s2, s1o_i, s1o_q, &s2o_i, &s2o_q)) {
				if (s2o_i > 32767.0) s2o_i = 32767.0;
				if (s2o_i < -32768.0) s2o_i = -32768.0;
				if (s2o_q > 32767.0) s2o_q = 32767.0;
				if (s2o_q < -32768.0) s2o_q = -32768.0;
				output[out_idx++] = (int16_t)s2o_i;
				output[out_idx++] = (int16_t)s2o_q;
			}
		}
	}

	*output_len = out_idx;
	return 0;
}

/* ---- 3-stage loops: tiers 6-7 (18k ≤ BW < 75k) ---- */

static int process_t06(struct rtlsdr_dsp *dsp,
		       const int16_t *input, size_t input_len,
		       int16_t *output, size_t *output_len)
{
	struct dsp_stage *s1 = &dsp->stages[0];
	struct dsp_stage *s2 = &dsp->stages[1];
	struct dsp_stage *s3 = &dsp->stages[2];
	size_t in_idx = 0, out_idx = 0;
	double s1o_i, s1o_q, s2o_i, s2o_q, s3o_i, s3o_q;

	while (in_idx + 1 < input_len) {
		double cos_ph = cos(dsp->phase);
		double sin_ph = sin(dsp->phase);
		double mix_i = (double)input[in_idx] * cos_ph +
			       (double)input[in_idx + 1] * sin_ph;
		double mix_q = (double)input[in_idx + 1] * cos_ph -
			       (double)input[in_idx] * sin_ph;
		in_idx += 2;

		dsp->phase += dsp->phase_inc;
		if (dsp->phase >= 2.0 * M_PI) dsp->phase -= 2.0 * M_PI;
		if (dsp->phase < 0.0) dsp->phase += 2.0 * M_PI;

		if (stage_run(s1, mix_i, mix_q, &s1o_i, &s1o_q)) {
			if (stage_run(s2, s1o_i, s1o_q, &s2o_i, &s2o_q)) {
				if (stage_run(s3, s2o_i, s2o_q, &s3o_i, &s3o_q)) {
					if (s3o_i > 32767.0) s3o_i = 32767.0;
					if (s3o_i < -32768.0) s3o_i = -32768.0;
					if (s3o_q > 32767.0) s3o_q = 32767.0;
					if (s3o_q < -32768.0) s3o_q = -32768.0;
					output[out_idx++] = (int16_t)s3o_i;
					output[out_idx++] = (int16_t)s3o_q;
				}
			}
		}
	}

	*output_len = out_idx;
	return 0;
}

static int process_t07(struct rtlsdr_dsp *dsp,
		       const int16_t *input, size_t input_len,
		       int16_t *output, size_t *output_len)
{
	struct dsp_stage *s1 = &dsp->stages[0];
	struct dsp_stage *s2 = &dsp->stages[1];
	struct dsp_stage *s3 = &dsp->stages[2];
	size_t in_idx = 0, out_idx = 0;
	double s1o_i, s1o_q, s2o_i, s2o_q, s3o_i, s3o_q;

	while (in_idx + 1 < input_len) {
		double cos_ph = cos(dsp->phase);
		double sin_ph = sin(dsp->phase);
		double mix_i = (double)input[in_idx] * cos_ph +
			       (double)input[in_idx + 1] * sin_ph;
		double mix_q = (double)input[in_idx + 1] * cos_ph -
			       (double)input[in_idx] * sin_ph;
		in_idx += 2;

		dsp->phase += dsp->phase_inc;
		if (dsp->phase >= 2.0 * M_PI) dsp->phase -= 2.0 * M_PI;
		if (dsp->phase < 0.0) dsp->phase += 2.0 * M_PI;

		if (stage_run(s1, mix_i, mix_q, &s1o_i, &s1o_q)) {
			if (stage_run(s2, s1o_i, s1o_q, &s2o_i, &s2o_q)) {
				if (stage_run(s3, s2o_i, s2o_q, &s3o_i, &s3o_q)) {
					if (s3o_i > 32767.0) s3o_i = 32767.0;
					if (s3o_i < -32768.0) s3o_i = -32768.0;
					if (s3o_q > 32767.0) s3o_q = 32767.0;
					if (s3o_q < -32768.0) s3o_q = -32768.0;
					output[out_idx++] = (int16_t)s3o_i;
					output[out_idx++] = (int16_t)s3o_q;
				}
			}
		}
	}

	*output_len = out_idx;
	return 0;
}

/* ---- 4-stage loops: tiers 8-9 (4k ≤ BW < 18k) ---- */

static int process_t08(struct rtlsdr_dsp *dsp,
		       const int16_t *input, size_t input_len,
		       int16_t *output, size_t *output_len)
{
	struct dsp_stage *s1 = &dsp->stages[0];
	struct dsp_stage *s2 = &dsp->stages[1];
	struct dsp_stage *s3 = &dsp->stages[2];
	struct dsp_stage *s4 = &dsp->stages[3];
	size_t in_idx = 0, out_idx = 0;
	double s1o_i, s1o_q, s2o_i, s2o_q, s3o_i, s3o_q, s4o_i, s4o_q;

	while (in_idx + 1 < input_len) {
		double cos_ph = cos(dsp->phase);
		double sin_ph = sin(dsp->phase);
		double mix_i = (double)input[in_idx] * cos_ph +
			       (double)input[in_idx + 1] * sin_ph;
		double mix_q = (double)input[in_idx + 1] * cos_ph -
			       (double)input[in_idx] * sin_ph;
		in_idx += 2;

		dsp->phase += dsp->phase_inc;
		if (dsp->phase >= 2.0 * M_PI) dsp->phase -= 2.0 * M_PI;
		if (dsp->phase < 0.0) dsp->phase += 2.0 * M_PI;

		if (stage_run(s1, mix_i, mix_q, &s1o_i, &s1o_q)) {
			if (stage_run(s2, s1o_i, s1o_q, &s2o_i, &s2o_q)) {
				if (stage_run(s3, s2o_i, s2o_q, &s3o_i, &s3o_q)) {
					if (stage_run(s4, s3o_i, s3o_q, &s4o_i, &s4o_q)) {
						if (s4o_i > 32767.0) s4o_i = 32767.0;
						if (s4o_i < -32768.0) s4o_i = -32768.0;
						if (s4o_q > 32767.0) s4o_q = 32767.0;
						if (s4o_q < -32768.0) s4o_q = -32768.0;
						output[out_idx++] = (int16_t)s4o_i;
						output[out_idx++] = (int16_t)s4o_q;
					}
				}
			}
		}
	}

	*output_len = out_idx;
	return 0;
}

static int process_t09(struct rtlsdr_dsp *dsp,
		       const int16_t *input, size_t input_len,
		       int16_t *output, size_t *output_len)
{
	struct dsp_stage *s1 = &dsp->stages[0];
	struct dsp_stage *s2 = &dsp->stages[1];
	struct dsp_stage *s3 = &dsp->stages[2];
	struct dsp_stage *s4 = &dsp->stages[3];
	size_t in_idx = 0, out_idx = 0;
	double s1o_i, s1o_q, s2o_i, s2o_q, s3o_i, s3o_q, s4o_i, s4o_q;

	while (in_idx + 1 < input_len) {
		double cos_ph = cos(dsp->phase);
		double sin_ph = sin(dsp->phase);
		double mix_i = (double)input[in_idx] * cos_ph +
			       (double)input[in_idx + 1] * sin_ph;
		double mix_q = (double)input[in_idx + 1] * cos_ph -
			       (double)input[in_idx] * sin_ph;
		in_idx += 2;

		dsp->phase += dsp->phase_inc;
		if (dsp->phase >= 2.0 * M_PI) dsp->phase -= 2.0 * M_PI;
		if (dsp->phase < 0.0) dsp->phase += 2.0 * M_PI;

		if (stage_run(s1, mix_i, mix_q, &s1o_i, &s1o_q)) {
			if (stage_run(s2, s1o_i, s1o_q, &s2o_i, &s2o_q)) {
				if (stage_run(s3, s2o_i, s2o_q, &s3o_i, &s3o_q)) {
					if (stage_run(s4, s3o_i, s3o_q, &s4o_i, &s4o_q)) {
						if (s4o_i > 32767.0) s4o_i = 32767.0;
						if (s4o_i < -32768.0) s4o_i = -32768.0;
						if (s4o_q > 32767.0) s4o_q = 32767.0;
						if (s4o_q < -32768.0) s4o_q = -32768.0;
						output[out_idx++] = (int16_t)s4o_i;
						output[out_idx++] = (int16_t)s4o_q;
					}
				}
			}
		}
	}

	*output_len = out_idx;
	return 0;
}

/* ---- 5-stage loop: tier 10 (BW < 4k) ---- */

static int process_t10(struct rtlsdr_dsp *dsp,
		       const int16_t *input, size_t input_len,
		       int16_t *output, size_t *output_len)
{
	struct dsp_stage *s1 = &dsp->stages[0];
	struct dsp_stage *s2 = &dsp->stages[1];
	struct dsp_stage *s3 = &dsp->stages[2];
	struct dsp_stage *s4 = &dsp->stages[3];
	struct dsp_stage *s5 = &dsp->stages[4];
	size_t in_idx = 0, out_idx = 0;
	double s1o_i, s1o_q, s2o_i, s2o_q, s3o_i, s3o_q, s4o_i, s4o_q, s5o_i, s5o_q;

	while (in_idx + 1 < input_len) {
		double cos_ph = cos(dsp->phase);
		double sin_ph = sin(dsp->phase);
		double mix_i = (double)input[in_idx] * cos_ph +
			       (double)input[in_idx + 1] * sin_ph;
		double mix_q = (double)input[in_idx + 1] * cos_ph -
			       (double)input[in_idx] * sin_ph;
		in_idx += 2;

		dsp->phase += dsp->phase_inc;
		if (dsp->phase >= 2.0 * M_PI) dsp->phase -= 2.0 * M_PI;
		if (dsp->phase < 0.0) dsp->phase += 2.0 * M_PI;

		if (stage_run(s1, mix_i, mix_q, &s1o_i, &s1o_q)) {
			if (stage_run(s2, s1o_i, s1o_q, &s2o_i, &s2o_q)) {
				if (stage_run(s3, s2o_i, s2o_q, &s3o_i, &s3o_q)) {
					if (stage_run(s4, s3o_i, s3o_q, &s4o_i, &s4o_q)) {
						if (stage_run(s5, s4o_i, s4o_q, &s5o_i, &s5o_q)) {
							if (s5o_i > 32767.0) s5o_i = 32767.0;
							if (s5o_i < -32768.0) s5o_i = -32768.0;
							if (s5o_q > 32767.0) s5o_q = 32767.0;
							if (s5o_q < -32768.0) s5o_q = -32768.0;
							output[out_idx++] = (int16_t)s5o_i;
							output[out_idx++] = (int16_t)s5o_q;
						}
					}
				}
			}
		}
	}

	*output_len = out_idx;
	return 0;
}

/* ---- Stage creation helper ---- */

static int stage_init(struct dsp_stage *st, int decimation,
		      int num_taps, double cutoff)
{
	st->decimation = decimation;
	st->num_taps   = num_taps;
	st->counter    = 0;
	st->wp         = 0;

	st->coeffs  = (double *)calloc(num_taps, sizeof(double));
	st->delay_i = (double *)calloc(num_taps, sizeof(double));
	st->delay_q = (double *)calloc(num_taps, sizeof(double));
	if (!st->coeffs || !st->delay_i || !st->delay_q)
		return -1;

	design_lowpass(st->coeffs, num_taps, cutoff);
	return 0;
}

/* ---- rtlsdr_dsp_create ---- */

struct rtlsdr_dsp *rtlsdr_dsp_create(uint64_t input_rate,
				     uint64_t output_rate,
				     uint64_t bandwidth,
				     uint64_t freq_offset)
{
	struct rtlsdr_dsp *dsp;
	int R_total;
	int target_stages;
	int stages_r[MAX_STAGES];
	int actual_stages;
	double phase_inc;

	if (input_rate == 0 || output_rate == 0 || output_rate > input_rate)
		return NULL;

	dsp = (struct rtlsdr_dsp *)calloc(1, sizeof(*dsp));
	if (!dsp)
		return NULL;

	dsp->input_rate  = input_rate;
	dsp->output_rate = output_rate;
	dsp->bandwidth   = bandwidth;
	dsp->freq_offset = freq_offset;

	phase_inc = 2.0 * M_PI * (double)freq_offset / (double)input_rate;
	dsp->phase_inc = phase_inc;
	dsp->phase = 0.0;

	R_total = (int)(input_rate / output_rate);
	if (R_total < 1) R_total = 1;

	if (bandwidth >= 1200000)	target_stages = 1;
	else if (bandwidth >= 600000)	target_stages = 1;
	else if (bandwidth >= 300000)	target_stages = 1;
	else if (bandwidth >= 150000)	target_stages = 2;
	else if (bandwidth >= 75000)	target_stages = 2;
	else if (bandwidth >= 37000)	target_stages = 3;
	else if (bandwidth >= 18000)	target_stages = 3;
	else if (bandwidth >= 9000)	target_stages = 4;
	else if (bandwidth >= 4000)	target_stages = 4;
	else				target_stages = 5;

	/* Factor R_total across target_stages */
	{
		int r = R_total;
		int n = 0;

		for (int i = 0; i < target_stages - 1 && r > 1 && n < MAX_STAGES - 1; i++) {
			int f;
			for (f = 8; f >= 2; f--) {
				if (r % f == 0) {
					stages_r[n++] = f;
					r /= f;
					break;
				}
			}
			if (f < 2) {
				stages_r[n++] = 8;
				r = (r + 7) / 8;
			}
		}

		if (r >= 1) stages_r[n++] = r;
		actual_stages = n;
	}

	dsp->num_stages = actual_stages;

	/* Initialize each stage */
	for (int i = 0; i < actual_stages; i++) {
		struct dsp_stage *st = &dsp->stages[i];
		int R_stage = stages_r[i];
		int is_last = (i == actual_stages - 1);

		/* Compute input rate for this stage */
		uint64_t stage_input_rate = input_rate;
		for (int j = 0; j < i; j++)
			stage_input_rate /= stages_r[j];

		if (is_last && bandwidth > 0 && bandwidth < output_rate) {
			/* Channel-selection filter */
			int tap_target;
			double trans_norm, cutoff;

			tap_target = (int)(30.0 * (double)stage_input_rate /
					   (double)bandwidth + 0.5);
			if (tap_target < 64) tap_target = 64;
			if (tap_target > MAX_TAPS) tap_target = MAX_TAPS;

			trans_norm = 5.5 / (double)tap_target;
			cutoff = (double)bandwidth * 0.5 /
				 (double)stage_input_rate - trans_norm;
			if (cutoff <= 0.0)
				cutoff = (double)bandwidth * 0.25 /
					 (double)stage_input_rate;

			if (stage_init(st, R_stage, tap_target, cutoff) < 0)
				goto fail;
		} else {
			/* Anti-alias for intermediate stage or wideband */
			double cutoff;
			int num_taps;

			if (R_stage <= 1) {
				/* No decimation, passthrough */
				num_taps = 1;
				cutoff = 0.5;
				st->coeffs = (double *)calloc(1, sizeof(double));
				st->delay_i = (double *)calloc(1, sizeof(double));
				st->delay_q = (double *)calloc(1, sizeof(double));
				if (!st->coeffs || !st->delay_i || !st->delay_q)
					goto fail;
				st->coeffs[0] = 1.0;
				st->num_taps = 1;
				st->decimation = 1;
				st->counter = 0;
				st->wp = 0;
			} else {
				num_taps = 64;
				cutoff = 0.45 / (double)R_stage;
				if (stage_init(st, R_stage, num_taps, cutoff) < 0)
					goto fail;
			}
		}
	}

	/* Pick process function */
	if (target_stages == 1) {
		if (bandwidth >= 1200000)      dsp->process_fn = process_t01;
		else if (bandwidth >= 600000)  dsp->process_fn = process_t02;
		else                           dsp->process_fn = process_t03;
	} else if (target_stages == 2) {
		if (bandwidth >= 150000)       dsp->process_fn = process_t04;
		else                           dsp->process_fn = process_t05;
	} else if (target_stages == 3) {
		if (bandwidth >= 37000)        dsp->process_fn = process_t06;
		else                           dsp->process_fn = process_t07;
	} else if (target_stages == 4) {
		if (bandwidth >= 9000)         dsp->process_fn = process_t08;
		else                           dsp->process_fn = process_t09;
	} else {
		                               dsp->process_fn = process_t10;
	}

	return dsp;

fail:
	for (int i = 0; i < MAX_STAGES; i++) {
		free(dsp->stages[i].coeffs);
		free(dsp->stages[i].delay_i);
		free(dsp->stages[i].delay_q);
	}
	free(dsp);
	return NULL;
}

void rtlsdr_dsp_destroy(struct rtlsdr_dsp *dsp)
{
	if (dsp) {
		for (int i = 0; i < MAX_STAGES; i++) {
			free(dsp->stages[i].coeffs);
			free(dsp->stages[i].delay_i);
			free(dsp->stages[i].delay_q);
		}
		free(dsp);
	}
}

int rtlsdr_dsp_process(struct rtlsdr_dsp *dsp,
		       const int16_t *input, size_t input_len,
		       int16_t *output, size_t *output_len)
{
	if (!dsp || !input || !output || !output_len)
		return -1;

	return dsp->process_fn(dsp, input, input_len, output, output_len);
}

void rtlsdr_dsp_reset(struct rtlsdr_dsp *dsp)
{
	if (!dsp)
		return;

	dsp->phase = 0.0;

	for (int i = 0; i < dsp->num_stages; i++) {
		struct dsp_stage *st = &dsp->stages[i];
		memset(st->delay_i, 0, st->num_taps * sizeof(double));
		memset(st->delay_q, 0, st->num_taps * sizeof(double));
		st->wp = 0;
		st->counter = 0;
	}
}
