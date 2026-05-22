#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "rtl_dsp.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct rtlsdr_dsp {
	uint64_t input_rate;
	uint64_t output_rate;
	int      decimation;
	uint64_t bandwidth;
	uint64_t freq_offset;

	double   phase;
	double   phase_inc;

	int      cic_stages;
	int      cic_counter;
	double  *int_i;
	double  *int_q;
	double  *comb_i;
	double  *comb_q;
	double   cic_gain;
};

struct rtlsdr_dsp *rtlsdr_dsp_create(uint64_t input_rate,
				     uint64_t output_rate,
				     uint64_t bandwidth,
				     uint64_t freq_offset)
{
	struct rtlsdr_dsp *dsp;
	int r;

	if (input_rate == 0 || output_rate == 0 || output_rate > input_rate)
		return NULL;

	r = (int)(input_rate / output_rate);
	if (r < 2)
		r = 1;

	dsp = (struct rtlsdr_dsp *)calloc(1, sizeof(*dsp));
	if (!dsp)
		return NULL;

	dsp->input_rate  = input_rate;
	dsp->output_rate = output_rate;
	dsp->decimation  = r;
	dsp->bandwidth   = bandwidth;
	dsp->freq_offset = freq_offset;

	dsp->phase_inc = 2.0 * M_PI * (double)freq_offset / (double)input_rate;
	dsp->phase = 0.0;

	dsp->cic_stages = 3;
	dsp->cic_counter = 0;

	dsp->int_i  = (double *)calloc(dsp->cic_stages, sizeof(double));
	dsp->int_q  = (double *)calloc(dsp->cic_stages, sizeof(double));
	dsp->comb_i = (double *)calloc(dsp->cic_stages, sizeof(double));
	dsp->comb_q = (double *)calloc(dsp->cic_stages, sizeof(double));

	if (!dsp->int_i || !dsp->int_q || !dsp->comb_i || !dsp->comb_q) {
		free(dsp->int_i);  free(dsp->int_q);
		free(dsp->comb_i); free(dsp->comb_q);
		free(dsp);
		return NULL;
	}

	dsp->cic_gain = pow((double)r, (double)dsp->cic_stages);

	return dsp;
}

void rtlsdr_dsp_destroy(struct rtlsdr_dsp *dsp)
{
	if (dsp) {
		free(dsp->int_i);  free(dsp->int_q);
		free(dsp->comb_i); free(dsp->comb_q);
		free(dsp);
	}
}

int rtlsdr_dsp_process(struct rtlsdr_dsp *dsp,
		       const int16_t *input, size_t input_len,
		       int16_t *output, size_t *output_len)
{
	size_t in_idx = 0, out_idx = 0;
	double cos_phase, sin_phase;
	double mix_i, mix_q;
	double cic_i, cic_q;
	int i;

	if (!dsp || !input || !output || !output_len)
		return -1;

	while (in_idx + 1 < input_len) {
		cos_phase = cos(dsp->phase);
		sin_phase = sin(dsp->phase);

		mix_i = (double)input[in_idx] * cos_phase +
			(double)input[in_idx + 1] * sin_phase;
		mix_q = (double)input[in_idx + 1] * cos_phase -
			(double)input[in_idx] * sin_phase;

		in_idx += 2;

		dsp->phase += dsp->phase_inc;
		if (dsp->phase >= 2.0 * M_PI)
			dsp->phase -= 2.0 * M_PI;
		if (dsp->phase < 0.0)
			dsp->phase += 2.0 * M_PI;

		dsp->int_i[0] += mix_i;
		dsp->int_q[0] += mix_q;
		for (i = 1; i < dsp->cic_stages; i++) {
			dsp->int_i[i] += dsp->int_i[i - 1];
			dsp->int_q[i] += dsp->int_q[i - 1];
		}

		if (dsp->cic_counter == 0) {
			cic_i = dsp->int_i[dsp->cic_stages - 1];
			cic_q = dsp->int_q[dsp->cic_stages - 1];

			for (i = 0; i < dsp->cic_stages; i++) {
				double prev_i = dsp->comb_i[i];
				double prev_q = dsp->comb_q[i];
				dsp->comb_i[i] = cic_i;
				dsp->comb_q[i] = cic_q;
				cic_i = cic_i - prev_i;
				cic_q = cic_q - prev_q;
			}

			cic_i /= dsp->cic_gain;
			cic_q /= dsp->cic_gain;

			if (cic_i > 32767.0) cic_i = 32767.0;
			if (cic_i < -32768.0) cic_i = -32768.0;
			if (cic_q > 32767.0) cic_q = 32767.0;
			if (cic_q < -32768.0) cic_q = -32768.0;

			output[out_idx++] = (int16_t)cic_i;
			output[out_idx++] = (int16_t)cic_q;
		}

		dsp->cic_counter++;
		if (dsp->cic_counter >= dsp->decimation)
			dsp->cic_counter = 0;
	}

	*output_len = out_idx;
	return 0;
}

void rtlsdr_dsp_reset(struct rtlsdr_dsp *dsp)
{
	if (dsp) {
		dsp->phase = 0.0;
		dsp->cic_counter = 0;
		memset(dsp->int_i, 0, dsp->cic_stages * sizeof(double));
		memset(dsp->int_q, 0, dsp->cic_stages * sizeof(double));
		memset(dsp->comb_i, 0, dsp->cic_stages * sizeof(double));
		memset(dsp->comb_q, 0, dsp->cic_stages * sizeof(double));
	}
}
