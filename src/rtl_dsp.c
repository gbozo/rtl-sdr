#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

#include "rtl_dsp.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAX_TAPS   16383
#define MAX_STAGES 5

/* ---- NCO lookup table ---- */
/* 65536-entry float cosine table; sin is cos offset by -NCO_SIZE/4 */
#define NCO_SIZE 65536
#define NCO_MASK (NCO_SIZE - 1)

static float nco_table[NCO_SIZE];
static pthread_once_t nco_once = PTHREAD_ONCE_INIT;

static void nco_init(void)
{
	for (int i = 0; i < NCO_SIZE; i++)
		nco_table[i] = (float)cos(2.0 * M_PI * i / NCO_SIZE);
}

/* Phase accumulator is uint32_t; top 16 bits index the table.
 * cos(phase) = nco_table[phase >> 16]
 * sin(phase) = nco_cos(phase - pi/2)
 */
static inline float nco_cos(uint32_t ph)
{
	return nco_table[ph >> 16];
}

static inline float nco_sin(uint32_t ph)
{
	return nco_table[((ph >> 16) - (NCO_SIZE / 4)) & NCO_MASK];
}

/* ---- DSP stage ---- */
struct dsp_stage {
	float   *coeffs;
	float   *delay_i;
	float   *delay_q;
	int      num_taps;
	int      buf_size;   /* next power-of-2 >= num_taps */
	int      buf_mask;   /* buf_size - 1 */
	int      decimation;
	int      counter;
	int      wp;
};

struct rtlsdr_dsp {
	uint64_t input_rate;
	uint64_t output_rate;
	uint64_t bandwidth;
	int64_t  freq_offset;

	uint32_t phase;      /* NCO phase accumulator, wraps at 2^32 */
	uint32_t phase_inc;  /* NCO phase increment per input sample */

	/* Capture-center highpass, independent of NCO and FIR history. */
	double   dc_alpha;
	double   dc_x_i, dc_x_q;
	double   dc_y_i, dc_y_q;

	int      num_stages;
	struct dsp_stage stages[MAX_STAGES];

	int (*process_fn)(struct rtlsdr_dsp *, const uint8_t *, size_t,
	                  int16_t *, size_t *);
};

/* ---- FIR helpers ---- */

static double blackman_harris(int n, int N)
{
	double a0 = 0.35875, a1 = 0.48829, a2 = 0.14128, a3 = 0.01168;
	double t = 2.0 * M_PI * n / (N - 1);
	return a0 - a1 * cos(t) + a2 * cos(2 * t) - a3 * cos(3 * t);
}

static int design_lowpass(float *coeffs, int num_taps, double cutoff)
{
	int i, mid = (num_taps - 1) / 2;
	double sum = 0.0;
	double *tmp = (double *)malloc(num_taps * sizeof(double));
	if (!tmp) return -1;

	for (i = 0; i < num_taps; i++) {
		int k = i - mid;
		double sinc;
		if (k == 0)
			sinc = 2.0 * cutoff;
		else
			sinc = sin(2.0 * M_PI * cutoff * k) / (M_PI * k);
		tmp[i] = sinc * blackman_harris(i, num_taps);
		sum += tmp[i];
	}

	if (sum != 0.0)
		for (i = 0; i < num_taps; i++)
			tmp[i] /= sum;

	for (i = 0; i < num_taps; i++)
		coeffs[i] = (float)tmp[i];

	free(tmp);
	return 0;
}

/*
 * stage_run() — used for stages 2..N (low rate, called infrequently).
 * Writes one sample to the delay line, increments counter, and if the
 * decimation counter fires it computes the FIR output and returns 1.
 */
static inline int stage_run(struct dsp_stage *st,
			    float in_i, float in_q,
			    float *out_i, float *out_q)
{
	st->delay_i[st->wp] = in_i;
	st->delay_q[st->wp] = in_q;
	st->wp = (st->wp + 1) & st->buf_mask;
	if (++st->counter >= st->decimation) {
		float sum_i = 0.0f, sum_q = 0.0f;
		int wp = st->wp;
		int mask = st->buf_mask;
		st->counter = 0;
		for (int j = 0; j < st->num_taps; j++) {
			int idx = (wp - 1 - j) & mask;
			sum_i += st->coeffs[j] * st->delay_i[idx];
			sum_q += st->coeffs[j] * st->delay_q[idx];
		}
		*out_i = sum_i;
		*out_q = sum_q;
		return 1;
	}
	return 0;
}

/*
 * stage_fir() — compute the FIR output for stage 1 once the delay line
 * has been fully filled for one decimation block.  Called once per R
 * input samples; no counter needed in the hot path.
 */
static inline void stage_fir(const struct dsp_stage *st,
			     float *out_i, float *out_q)
{
	float sum_i = 0.0f, sum_q = 0.0f;
	int wp   = st->wp;
	int mask = st->buf_mask;
	const float *c = st->coeffs;
	const float *di = st->delay_i;
	const float *dq = st->delay_q;

	for (int j = 0; j < st->num_taps; j++) {
		int idx = (wp - 1 - j) & mask;
		sum_i += c[j] * di[idx];
		sum_q += c[j] * dq[idx];
	}
	*out_i = sum_i;
	*out_q = sum_q;
}

/* Scale filtered ADC counts before saturation, preserving fractional output. */
static inline int16_t sat16(float v)
{
	v *= 256.0f;
	v = v < -32768.0f ? -32768.0f : (v > 32767.0f ? 32767.0f : v);
	return (int16_t)v;
}

/*
 * STAGE-1 HOT LOOP MACRO
 * ----------------------
 * Processes one decimation block of R1 input samples in a single pass:
 *   1. Capture-center DC IIR blocker + NCO mix — fused, one loop
 *   2. Write mixed sample into stage-1 delay line (no FIR output yet)
 * After the R1-sample inner loop, one FIR call produces the stage-1 output.
 *
 * This eliminates the per-sample decimation counter check from the hot path.
 * The DC blocker is folded in at zero extra loop overhead.
 *
 * Variables required in scope:
 *   dsp       — struct rtlsdr_dsp *
 *   s1        — struct dsp_stage * for stage 0
 *   input     — const uint8_t * (interleaved unsigned I/Q bytes)
 *   in_idx    — size_t  (advanced by 2*R1 per invocation)
 *   R1        — int decimation of stage 1
 */
#define STAGE1_BLOCK(R1)                                                      \
	do {                                                                  \
		for (int _k = 0; _k < (R1); _k++) {                          \
			float _cos = nco_cos(dsp->phase);                     \
			float _sin = nco_sin(dsp->phase);                     \
			double _ri = (double)input[in_idx] - 127.5;           \
			double _rq = (double)input[in_idx + 1] - 127.5;       \
			in_idx += 2;                                          \
			dsp->phase += dsp->phase_inc;                         \
			dsp->dc_y_i = _ri - dsp->dc_x_i +                     \
				dsp->dc_alpha * dsp->dc_y_i;                  \
			dsp->dc_y_q = _rq - dsp->dc_x_q +                     \
				dsp->dc_alpha * dsp->dc_y_q;                  \
			dsp->dc_x_i = _ri;                                   \
			dsp->dc_x_q = _rq;                                   \
			/* Complex multiply: (ri + j*rq) * (cos - j*sin)     \
			 * = (ri*cos + rq*sin) + j*(rq*cos - ri*sin)  */     \
			float _mi = (float)(dsp->dc_y_i * _cos +              \
				dsp->dc_y_q * _sin);                          \
			float _mq = (float)(dsp->dc_y_q * _cos -              \
				dsp->dc_y_i * _sin);                          \
			/* Write to delay line */                             \
			s1->delay_i[s1->wp] = _mi;                            \
			s1->delay_q[s1->wp] = _mq;                            \
			s1->wp = (s1->wp + 1) & s1->buf_mask;                 \
		}                                                             \
	} while (0)

/* Keep the fused full-block loop, carrying partial blocks across calls. */
static inline int stage1_next(struct rtlsdr_dsp *dsp,
			      const uint8_t *input, size_t input_len,
			      size_t *input_idx)
{
	struct dsp_stage *s1 = &dsp->stages[0];
	size_t in_idx = *input_idx;
	size_t available = (input_len - in_idx) / 2;
	int count = s1->decimation - s1->counter;

	if (available == 0)
		return 0;
	if (available < (size_t)count)
		count = (int)available;
	STAGE1_BLOCK(count);
	*input_idx = in_idx;
	s1->counter += count;
	if (s1->counter != s1->decimation)
		return 0;
	s1->counter = 0;
	return 1;
}

/* ---- 1-stage process ---- */

static int process_t01(struct rtlsdr_dsp *dsp,
		       const uint8_t *input, size_t input_len,
		       int16_t *output, size_t *output_len)
{
	struct dsp_stage *s1 = &dsp->stages[0];
	size_t in_idx = 0, out_idx = 0;
	float o_i, o_q;

	while (stage1_next(dsp, input, input_len, &in_idx)) {
		stage_fir(s1, &o_i, &o_q);
		output[out_idx++] = sat16(o_i);
		output[out_idx++] = sat16(o_q);
	}

	*output_len = out_idx;
	return 0;
}

/* ---- 2-stage process ---- */

static int process_t04(struct rtlsdr_dsp *dsp,
		       const uint8_t *input, size_t input_len,
		       int16_t *output, size_t *output_len)
{
	struct dsp_stage *s1 = &dsp->stages[0];
	struct dsp_stage *s2 = &dsp->stages[1];
	size_t in_idx = 0, out_idx = 0;
	float s1o_i, s1o_q, s2o_i, s2o_q;

	while (stage1_next(dsp, input, input_len, &in_idx)) {
		stage_fir(s1, &s1o_i, &s1o_q);
		if (stage_run(s2, s1o_i, s1o_q, &s2o_i, &s2o_q)) {
			output[out_idx++] = sat16(s2o_i);
			output[out_idx++] = sat16(s2o_q);
		}
	}

	*output_len = out_idx;
	return 0;
}

/* ---- 3-stage process ---- */

static int process_t06(struct rtlsdr_dsp *dsp,
		       const uint8_t *input, size_t input_len,
		       int16_t *output, size_t *output_len)
{
	struct dsp_stage *s1 = &dsp->stages[0];
	struct dsp_stage *s2 = &dsp->stages[1];
	struct dsp_stage *s3 = &dsp->stages[2];
	size_t in_idx = 0, out_idx = 0;
	float s1o_i, s1o_q, s2o_i, s2o_q, s3o_i, s3o_q;

	while (stage1_next(dsp, input, input_len, &in_idx)) {
		stage_fir(s1, &s1o_i, &s1o_q);
		if (stage_run(s2, s1o_i, s1o_q, &s2o_i, &s2o_q)) {
			if (stage_run(s3, s2o_i, s2o_q, &s3o_i, &s3o_q)) {
				output[out_idx++] = sat16(s3o_i);
				output[out_idx++] = sat16(s3o_q);
			}
		}
	}

	*output_len = out_idx;
	return 0;
}

/* ---- 4-stage process ---- */

static int process_t08(struct rtlsdr_dsp *dsp,
		       const uint8_t *input, size_t input_len,
		       int16_t *output, size_t *output_len)
{
	struct dsp_stage *s1 = &dsp->stages[0];
	struct dsp_stage *s2 = &dsp->stages[1];
	struct dsp_stage *s3 = &dsp->stages[2];
	struct dsp_stage *s4 = &dsp->stages[3];
	size_t in_idx = 0, out_idx = 0;
	float s1o_i, s1o_q, s2o_i, s2o_q, s3o_i, s3o_q, s4o_i, s4o_q;

	while (stage1_next(dsp, input, input_len, &in_idx)) {
		stage_fir(s1, &s1o_i, &s1o_q);
		if (stage_run(s2, s1o_i, s1o_q, &s2o_i, &s2o_q)) {
			if (stage_run(s3, s2o_i, s2o_q, &s3o_i, &s3o_q)) {
				if (stage_run(s4, s3o_i, s3o_q, &s4o_i, &s4o_q)) {
					output[out_idx++] = sat16(s4o_i);
					output[out_idx++] = sat16(s4o_q);
				}
			}
		}
	}

	*output_len = out_idx;
	return 0;
}

/* ---- 5-stage process ---- */

static int process_t10(struct rtlsdr_dsp *dsp,
		       const uint8_t *input, size_t input_len,
		       int16_t *output, size_t *output_len)
{
	struct dsp_stage *s1 = &dsp->stages[0];
	struct dsp_stage *s2 = &dsp->stages[1];
	struct dsp_stage *s3 = &dsp->stages[2];
	struct dsp_stage *s4 = &dsp->stages[3];
	struct dsp_stage *s5 = &dsp->stages[4];
	size_t in_idx = 0, out_idx = 0;
	float s1o_i, s1o_q, s2o_i, s2o_q, s3o_i, s3o_q, s4o_i, s4o_q, s5o_i, s5o_q;

	while (stage1_next(dsp, input, input_len, &in_idx)) {
		stage_fir(s1, &s1o_i, &s1o_q);
		if (stage_run(s2, s1o_i, s1o_q, &s2o_i, &s2o_q)) {
			if (stage_run(s3, s2o_i, s2o_q, &s3o_i, &s3o_q)) {
				if (stage_run(s4, s3o_i, s3o_q, &s4o_i, &s4o_q)) {
					if (stage_run(s5, s4o_i, s4o_q, &s5o_i, &s5o_q)) {
						output[out_idx++] = sat16(s5o_i);
						output[out_idx++] = sat16(s5o_q);
					}
				}
			}
		}
	}

	*output_len = out_idx;
	return 0;
}

/* ---- Stage creation helpers ---- */

static int next_pow2(int n)
{
	int p = 1;
	while (p < n) p <<= 1;
	return p;
}

static int stage_init(struct dsp_stage *st, int decimation,
		      int num_taps, double cutoff)
{
	int buf_size = next_pow2(num_taps);

	st->decimation = decimation;
	st->num_taps   = num_taps;
	st->buf_size   = buf_size;
	st->buf_mask   = buf_size - 1;
	st->counter    = 0;
	st->wp         = 0;

	st->coeffs  = (float *)calloc(num_taps, sizeof(float));
	st->delay_i = (float *)calloc(buf_size, sizeof(float));
	st->delay_q = (float *)calloc(buf_size, sizeof(float));
	if (!st->coeffs || !st->delay_i || !st->delay_q)
		return -1;

	if (num_taps == 1) {
		st->coeffs[0] = 1.0f;
		return 0;
	}
	return design_lowpass(st->coeffs, num_taps, cutoff);
}

struct dsp_plan {
	uint64_t bandwidth;
	int num_stages;
	int factors[MAX_STAGES];
	int taps[MAX_STAGES];
	double cutoffs[MAX_STAGES];
};

static int make_plan(uint64_t input_rate, uint64_t output_rate,
		     uint64_t bandwidth, int64_t freq_offset,
		     struct dsp_plan *plan)
{
	uint64_t offset, rate, product = 1;
	int remaining, n = 0;
	int raw = bandwidth == input_rate && output_rate == input_rate &&
		freq_offset == 0;

	if (!output_rate || output_rate > input_rate || input_rate > UINT32_MAX ||
	    input_rate % output_rate || input_rate / output_rate > 4096 ||
	    freq_offset == INT64_MIN)
		return -1;
	if (!bandwidth)
		bandwidth = 4 * output_rate / 5;
	if (!bandwidth || (!raw && bandwidth > 4 * output_rate / 5))
		return -1;
	offset = (uint64_t)(freq_offset < 0 ? -freq_offset : freq_offset);
	if (offset > (input_rate - bandwidth) / 2)
		return -1;
	plan->bandwidth = bandwidth;

	/* Prefer small exact factors; keep primes intact and bound the stage count.
	 * Put larger factors first, where the transition band is wider. */
	remaining = (int)(input_rate / output_rate);
	while (remaining > 1 && n < MAX_STAGES - 1) {
		int f;
		for (f = 4; f >= 2; f--)
			if (remaining % f == 0)
				break;
		if (f < 2) {
			for (f = 5; f * f <= remaining; f++)
				if (remaining % f == 0)
					break;
			if (f * f > remaining)
				break;
		}
		plan->factors[n++] = f;
		remaining /= f;
	}
	if (remaining > 1 || n == 0)
		plan->factors[n++] = remaining;
	plan->num_stages = n;
	for (int i = 0; i < n; i++) {
		for (int j = i + 1; j < n; j++) {
			if (plan->factors[j] > plan->factors[i]) {
				int tmp = plan->factors[i];
				plan->factors[i] = plan->factors[j];
				plan->factors[j] = tmp;
			}
		}
		product *= plan->factors[i];
	}
	if (product != input_rate / output_rate)
		return -1;

	rate = input_rate;
	for (int i = 0; i < n; i++) {
		uint64_t next_rate = rate / plan->factors[i];
		if (raw) {
			plan->taps[i] = 1;
			plan->cutoffs[i] = 0.5;
		} else {
			/* Fp=BW/2, Fstop=Fo/2: ceil(8*Fs/(Fstop-Fp)), odd. */
			uint64_t transition = next_rate - bandwidth;
			uint64_t taps = (16 * rate + transition - 1) / transition;
			if (taps < 3)
				taps = 3;
			if (!(taps & 1))
				taps++;
			if (taps > MAX_TAPS)
				return -1;
			plan->taps[i] = (int)taps;
			plan->cutoffs[i] = (double)(bandwidth + next_rate) /
				(4.0 * (double)rate);
		}
		rate = next_rate;
	}
	return 0;
}

int rtlsdr_dsp_validate(uint64_t input_rate, uint64_t output_rate,
		       uint64_t bandwidth, int64_t freq_offset,
		       uint64_t *effective_bandwidth)
{
	struct dsp_plan plan;
	if (make_plan(input_rate, output_rate, bandwidth, freq_offset, &plan) < 0)
		return -1;
	if (effective_bandwidth)
		*effective_bandwidth = plan.bandwidth;
	return 0;
}

struct rtlsdr_dsp *rtlsdr_dsp_create(uint64_t input_rate,
				     uint64_t output_rate,
				     uint64_t bandwidth,
				     int64_t freq_offset)
{
	struct dsp_plan plan;
	struct rtlsdr_dsp *dsp;
	int64_t phase_inc;

	if (make_plan(input_rate, output_rate, bandwidth, freq_offset, &plan) < 0)
		return NULL;
	if (pthread_once(&nco_once, nco_init) != 0)
		return NULL;

	dsp = (struct rtlsdr_dsp *)calloc(1, sizeof(*dsp));
	if (!dsp)
		return NULL;

	dsp->input_rate  = input_rate;
	dsp->output_rate = output_rate;
	dsp->bandwidth   = plan.bandwidth;
	dsp->freq_offset = freq_offset;
	dsp->dc_alpha = exp(-2.0 * M_PI * 10.0 / (double)input_rate);

	/* Validation bounds this to +/-2^31; round signed, then wrap as integer. */
	phase_inc = (int64_t)round((double)freq_offset / (double)input_rate *
		4294967296.0);
	dsp->phase_inc = (uint32_t)phase_inc;
	dsp->phase = 0;

	dsp->num_stages = plan.num_stages;
	for (int i = 0; i < plan.num_stages; i++) {
		if (stage_init(&dsp->stages[i], plan.factors[i], plan.taps[i],
			plan.cutoffs[i]) < 0)
			goto fail;
	}

	if      (plan.num_stages == 1) dsp->process_fn = process_t01;
	else if (plan.num_stages == 2) dsp->process_fn = process_t04;
	else if (plan.num_stages == 3) dsp->process_fn = process_t06;
	else if (plan.num_stages == 4) dsp->process_fn = process_t08;
	else                         dsp->process_fn = process_t10;

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
		       const uint8_t *input, size_t input_len,
		       int16_t *output, size_t output_capacity,
		       size_t *output_len)
{
	if (!output_len)
		return -1;
	*output_len = 0;
	if (!dsp || !input || !output || (input_len & 1) ||
	    output_capacity < input_len)
		return -1;

	return dsp->process_fn(dsp, input, input_len, output, output_len);
}

void rtlsdr_dsp_reset(struct rtlsdr_dsp *dsp)
{
	if (!dsp)
		return;

	dsp->phase = 0;
	dsp->dc_x_i = dsp->dc_x_q = 0.0;
	dsp->dc_y_i = dsp->dc_y_q = 0.0;

	for (int i = 0; i < dsp->num_stages; i++) {
		struct dsp_stage *st = &dsp->stages[i];
		memset(st->delay_i, 0, st->buf_size * sizeof(float));
		memset(st->delay_q, 0, st->buf_size * sizeof(float));
		st->wp = 0;
		st->counter = 0;
	}
}
