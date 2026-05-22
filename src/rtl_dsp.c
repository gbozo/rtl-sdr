#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "rtl_dsp.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAX_TAPS 4096

struct rtlsdr_dsp {
	uint64_t input_rate;
	uint64_t output_rate;
	int      decimation;
	uint64_t bandwidth;
	uint64_t freq_offset;

	double   phase;
	double   phase_inc;

	int      num_taps;
	int      taps_per_phase;
	int      num_phases;
	double  *coeffs;
	double  *delay_i;
	double  *delay_q;
	int     *delay_pos;
	int      comm_pos;
};

static double blackman_harris(int n, int N)
{
	double a0 = 0.35875;
	double a1 = 0.48829;
	double a2 = 0.14128;
	double a3 = 0.01168;
	double t = 2.0 * M_PI * n / (N - 1);
	return a0 - a1 * cos(t) + a2 * cos(2 * t) - a3 * cos(3 * t);
}

static int design_lowpass(double *coeffs, int num_taps, double cutoff)
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

	if (sum != 0.0) {
		for (i = 0; i < num_taps; i++)
			coeffs[i] /= sum;
	}

	return 0;
}

struct rtlsdr_dsp *rtlsdr_dsp_create(uint64_t input_rate,
				     uint64_t output_rate,
				     uint64_t bandwidth,
				     uint64_t freq_offset)
{
	struct rtlsdr_dsp *dsp;
	int R, N_padded, i, j, p;
	double cutoff, *tmp;

	if (input_rate == 0 || output_rate == 0 || output_rate > input_rate)
		return NULL;

	R = (int)(input_rate / output_rate);
	if (R < 1)
		R = 1;

	dsp = (struct rtlsdr_dsp *)calloc(1, sizeof(*dsp));
	if (!dsp)
		return NULL;

	dsp->input_rate  = input_rate;
	dsp->output_rate = output_rate;
	dsp->decimation  = R;
	dsp->bandwidth   = bandwidth;
	dsp->freq_offset = freq_offset;

	dsp->phase_inc = 2.0 * M_PI * (double)freq_offset / (double)input_rate;
	dsp->phase = 0.0;

	dsp->num_phases = R;

	if (bandwidth >= output_rate || bandwidth == 0) {
		cutoff = 0.45 * (double)output_rate / (double)input_rate;
		dsp->num_taps = 128;
	} else {
		int tap_target;
		double trans_norm;
		tap_target = (int)(30.0 * (double)input_rate / (double)bandwidth + 0.5);
		if (tap_target < 64)
			tap_target = 64;
		if (tap_target > MAX_TAPS)
			tap_target = MAX_TAPS;

		trans_norm = 5.5 / (double)tap_target;

		cutoff = (double)bandwidth * 0.5 / (double)input_rate - trans_norm;
		if (cutoff <= 0.0)
			cutoff = (double)bandwidth * 0.25 / (double)input_rate;

		dsp->num_taps = tap_target;
	}

	dsp->taps_per_phase = (dsp->num_taps + R - 1) / R;
	if (dsp->taps_per_phase < 4)
		dsp->taps_per_phase = 4;

	N_padded = dsp->taps_per_phase * R;
	if (N_padded > MAX_TAPS)
		N_padded = MAX_TAPS;

	tmp = (double *)calloc(N_padded, sizeof(double));
	if (!tmp)
		goto fail;
	design_lowpass(tmp, dsp->num_taps, cutoff);

	dsp->coeffs = (double *)calloc(N_padded, sizeof(double));
	dsp->delay_i = (double *)calloc(N_padded, sizeof(double));
	dsp->delay_q = (double *)calloc(N_padded, sizeof(double));
	dsp->delay_pos = (int *)calloc(R, sizeof(int));
	if (!dsp->coeffs || !dsp->delay_i || !dsp->delay_q || !dsp->delay_pos)
		goto fail;

	for (p = 0; p < R; p++)
		for (j = 0; j < dsp->taps_per_phase; j++)
			dsp->coeffs[p * dsp->taps_per_phase + j] = tmp[p + j * R];

	free(tmp);
	return dsp;

fail:
	free(tmp);
	free(dsp->coeffs);
	free(dsp->delay_i);
	free(dsp->delay_q);
	free(dsp->delay_pos);
	free(dsp);
	return NULL;
}

void rtlsdr_dsp_destroy(struct rtlsdr_dsp *dsp)
{
	if (dsp) {
		free(dsp->coeffs);
		free(dsp->delay_i);
		free(dsp->delay_q);
		free(dsp->delay_pos);
		free(dsp);
	}
}

int rtlsdr_dsp_process(struct rtlsdr_dsp *dsp,
		       const int16_t *input, size_t input_len,
		       int16_t *output, size_t *output_len)
{
	size_t in_idx = 0, out_idx = 0;
	int P = dsp->taps_per_phase;
	int R = dsp->num_phases;

	if (!dsp || !input || !output || !output_len)
		return -1;

	while (in_idx + 1 < input_len) {
		double mix_i, mix_q, cos_ph, sin_ph;
		int p;

		cos_ph = cos(dsp->phase);
		sin_ph = sin(dsp->phase);
		mix_i = (double)input[in_idx] * cos_ph +
			(double)input[in_idx + 1] * sin_ph;
		mix_q = (double)input[in_idx + 1] * cos_ph -
			(double)input[in_idx] * sin_ph;
		in_idx += 2;

		dsp->phase += dsp->phase_inc;
		if (dsp->phase >= 2.0 * M_PI)
			dsp->phase -= 2.0 * M_PI;
		if (dsp->phase < 0.0)
			dsp->phase += 2.0 * M_PI;

		p = dsp->comm_pos;
		dsp->delay_i[p * P + dsp->delay_pos[p]] = mix_i;
		dsp->delay_q[p * P + dsp->delay_pos[p]] = mix_q;
		dsp->delay_pos[p] = (dsp->delay_pos[p] + 1) % P;

		dsp->comm_pos = (dsp->comm_pos + 1) % R;

		if (dsp->comm_pos == 0) {
			int ph;
			double sum_i = 0.0, sum_q = 0.0;

			for (ph = 0; ph < R; ph++) {
				const double *c = dsp->coeffs + ph * P;
				const double *di = dsp->delay_i + ph * P;
				const double *dq = dsp->delay_q + ph * P;
				int dp = dsp->delay_pos[ph];
				int j;

				for (j = 0; j < P; j++) {
					int idx = (dp - 1 - j + P) % P;
					sum_i += c[j] * di[idx];
					sum_q += c[j] * dq[idx];
				}
			}

			if (sum_i > 32767.0) sum_i = 32767.0;
			if (sum_i < -32768.0) sum_i = -32768.0;
			if (sum_q > 32767.0) sum_q = 32767.0;
			if (sum_q < -32768.0) sum_q = -32768.0;

			output[out_idx++] = (int16_t)sum_i;
			output[out_idx++] = (int16_t)sum_q;
		}
	}

	*output_len = out_idx;
	return 0;
}

void rtlsdr_dsp_reset(struct rtlsdr_dsp *dsp)
{
	int i;

	if (!dsp)
		return;

	dsp->phase = 0.0;
	dsp->comm_pos = 0;
	for (i = 0; i < dsp->num_phases; i++)
		dsp->delay_pos[i] = 0;
	memset(dsp->delay_i, 0, dsp->num_phases * dsp->taps_per_phase * sizeof(double));
	memset(dsp->delay_q, 0, dsp->num_phases * dsp->taps_per_phase * sizeof(double));
}
