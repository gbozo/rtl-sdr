/* White-box coefficient/allocation tests: compile this file alone, not with
 * a second rtl_dsp.c object. Link pthread and libm (see test_dsp.c for API tests). */
#include "check.h"
#include <stddef.h>

static int fail_allocation = -1;
static int allocation_count;

static void *test_malloc(size_t size)
{
	if (allocation_count++ == fail_allocation)
		return NULL;
	return malloc(size);
}

static void *test_calloc(size_t count, size_t size)
{
	if (allocation_count++ == fail_allocation)
		return NULL;
	return calloc(count, size);
}

#define malloc test_malloc
#define calloc test_calloc
#include "../src/rtl_dsp.c"
#undef malloc
#undef calloc

static double response(const struct dsp_stage *st, double frequency)
{
	int mid = st->num_taps / 2;
	double sum = st->coeffs[mid];
	for (int i = 0; i < mid; i++)
		sum += 2.0 * st->coeffs[i] * cos(2.0 * M_PI * frequency * (mid - i));
	return fabs(sum);
}

static void test_coefficients(uint64_t input_rate, uint64_t output_rate,
			      uint64_t bandwidth, int stages)
{
	struct rtlsdr_dsp *dsp = rtlsdr_dsp_create(input_rate, output_rate,
		bandwidth, 0);
	uint64_t rate = input_rate, product = 1;
	double min_gain = 1e9, max_gain = 0, max_stop = 0;
	CHECK(dsp && dsp->num_stages == stages);
	for (int s = 0; s < dsp->num_stages; s++) {
		const struct dsp_stage *st = &dsp->stages[s];
		uint64_t next_rate = rate / st->decimation;
		double stop = (double)next_rate / (2.0 * rate);
		CHECK(rate % st->decimation == 0);
		CHECK(st->num_taps >= 3 && st->num_taps <= 16383 && (st->num_taps & 1));
		CHECK(st->num_taps >= ceil(16.0 * rate / (next_rate - dsp->bandwidth)));
		CHECK(fabs(response(st, 0) - 1.0) < 1e-6);
		for (int i = 0; i < st->num_taps; i++)
			CHECK(st->coeffs[i] == st->coeffs[st->num_taps - 1 - i]);
		/* At least eight points per stopband sidelobe, including both edges. */
		int points = 8 * st->num_taps;
		for (int i = 0; i <= points; i++) {
			double gain = response(st, stop + (0.5 - stop) * i / points);
			if (gain > max_stop)
				max_stop = gain;
			CHECK(gain <= 0.001);
		}
		product *= st->decimation;
		rate = next_rate;
	}
	CHECK(product == input_rate / output_rate && rate == output_rate);
	/* FIR-only cascade response: the separately specified capture-center DC
	 * notch is intentionally excluded, not confused with passband ripple. */
	for (int i = 0; i <= 512; i++) {
		double f = (double)dsp->bandwidth * i / 1024.0;
		double gain = 1.0;
		rate = input_rate;
		for (int s = 0; s < dsp->num_stages; s++) {
			gain *= response(&dsp->stages[s], f / rate);
			rate /= dsp->stages[s].decimation;
		}
		if (gain < min_gain)
			min_gain = gain;
		if (gain > max_gain)
			max_gain = gain;
	}
	CHECK(20 * log10(min_gain) >= -0.5);
	CHECK(20 * log10(max_gain) <= 0.5);
	CHECK(20 * log10(max_gain / min_gain) <= 0.5);
	printf("%llu -> %llu BW %llu: %d stages, ripple %.6f dB, stop %.2f dB\n",
		(unsigned long long)input_rate, (unsigned long long)output_rate,
		(unsigned long long)dsp->bandwidth, stages,
		20 * log10(max_gain / min_gain), 20 * log10(max_stop));
	rtlsdr_dsp_destroy(dsp);
}

static void test_plans(void)
{
	int seen[MAX_STAGES + 1] = {0};
	for (int ratio = 1; ratio <= 4096; ratio++) {
		struct dsp_plan plan;
		int result = make_plan((uint64_t)ratio * 1000, 1000, 0, 0, &plan);
		uint64_t product = 1, rate = (uint64_t)ratio * 1000;
		CHECK(plan.num_stages >= 1 && plan.num_stages <= MAX_STAGES);
		for (int i = 0; i < plan.num_stages; i++) {
			CHECK(plan.factors[i] >= 1 && rate % plan.factors[i] == 0);
			product *= plan.factors[i];
			rate /= plan.factors[i];
		}
		CHECK(product == (uint64_t)ratio && rate == 1000);
		if (result == 0) {
			seen[plan.num_stages] = 1;
			for (int i = 0; i < plan.num_stages; i++)
				CHECK(plan.taps[i] <= MAX_TAPS && (plan.taps[i] & 1));
		}
	}
	for (int i = 1; i <= MAX_STAGES; i++)
		CHECK(seen[i]);
	struct rtlsdr_dsp *prime = rtlsdr_dsp_create(1100000, 100000, 0, 0);
	CHECK(prime && prime->num_stages == 1 && prime->stages[0].decimation == 11);
	rtlsdr_dsp_destroy(prime);
	struct rtlsdr_dsp *raw = rtlsdr_dsp_create(2400000, 2400000, 2400000, 0);
	CHECK(raw && raw->num_stages == 1 && raw->stages[0].num_taps == 1);
	rtlsdr_dsp_destroy(raw);
}

static void test_allocations(void)
{
	struct rtlsdr_dsp *dsp;
	int allocations;
	allocation_count = 0;
	dsp = rtlsdr_dsp_create(4096000, 1000, 0, 0);
	CHECK(dsp && dsp->num_stages == 5);
	allocations = allocation_count;
	CHECK(allocations == 1 + 4 * dsp->num_stages);
	rtlsdr_dsp_destroy(dsp);
	for (int i = 0; i < allocations; i++) {
		allocation_count = 0;
		fail_allocation = i;
		CHECK(rtlsdr_dsp_validate(4096000, 1000, 0, 0, NULL) == 0);
		CHECK(allocation_count == 0);
		CHECK(rtlsdr_dsp_create(4096000, 1000, 0, 0) == NULL);
	}
	fail_allocation = -1;
}

/* Measure coherent amplitude plus all-bin RMS after startup, without
 * requiring 60 dB from a quantized eight-bit stimulus or int16 output. */
static double tone(uint64_t input_rate, uint64_t output_rate, uint64_t bandwidth,
		    int64_t offset, double frequency, double *rms)
{
	uint8_t input[8192];
	int16_t output[8192];
	const double amplitude = 80;
	struct rtlsdr_dsp *dsp = rtlsdr_dsp_create(input_rate, output_rate,
		bandwidth, offset);
	size_t position = 0, output_position = 0, measured = 0;
	size_t samples = (size_t)input_rate / 4;
	double re = 0, im = 0, power = 0;
	CHECK(dsp);
	while (position < samples) {
		size_t count = sizeof(input) / 2, len;
		if (count > samples - position)
			count = samples - position;
		for (size_t i = 0; i < count; i++) {
			double phase = 2.0 * M_PI * frequency * (position + i) / input_rate;
			input[2*i] = (uint8_t)lround(127.5 + amplitude * cos(phase));
			input[2*i+1] = (uint8_t)lround(127.5 + amplitude * sin(phase));
		}
		CHECK(rtlsdr_dsp_process(dsp, input, 2 * count, output,
			sizeof(output) / sizeof(*output), &len) == 0);
		for (size_t i = 0; i < len / 2; i++, output_position++) {
			double phase, c, s, oi, oq;
			if (output_position < output_rate / 8)
				continue;
			phase = 2.0 * M_PI * (frequency - offset) * output_position / output_rate;
			c = cos(phase);
			s = sin(phase);
			oi = output[2*i] / (256.0 * amplitude);
			oq = output[2*i+1] / (256.0 * amplitude);
			re += oi*c + oq*s;
			im += oq*c - oi*s;
			power += oi*oi + oq*oq;
			measured++;
		}
		position += count;
	}
	CHECK(output_position == samples / (input_rate / output_rate));
	CHECK(measured > 0);
	*rms = sqrt(power / measured);
	rtlsdr_dsp_destroy(dsp);
	return hypot(re, im) / measured;
}

static void test_tones(void)
{
	double rms, gain;
	const double frequencies[] = {-75000, -50000, -1000, 1000, 50000, 75000};
	for (size_t i = 0; i < sizeof(frequencies) / sizeof(*frequencies); i++) {
		gain = tone(2400000, 200000, 150000, 0, frequencies[i], &rms);
		CHECK(fabs(20 * log10(gain)) < 0.5);
		CHECK(rms < 1.02);
	}
	for (int sign = -1; sign <= 1; sign += 2) {
		/* A channel-center tone must survive: only capture DC is notched. */
		gain = tone(2400000, 200000, 150000, sign * 400000,
			sign * 400000, &rms);
		CHECK(fabs(20 * log10(gain)) < 0.5);
		gain = tone(2400000, 200000, 150000, sign * 400000,
			sign * 470000, &rms);
		CHECK(fabs(20 * log10(gain)) < 0.5);
		gain = tone(1100000, 100000, 0, sign * 123456,
			sign * 153456, &rms);
		CHECK(fabs(20 * log10(gain)) < 0.5);
		/* Historical 400 kHz alias into the 200 kHz output channel. */
		gain = tone(2400000, 200000, 150000, 0, sign * 400000, &rms);
		CHECK(gain < 0.005 && rms < 0.01);
	}
	/* Capture DC is suppressed even when it moves inside a shifted passband. */
	gain = tone(2400000, 200000, 150000, 30000, 0, &rms);
	CHECK(gain < 0.001 && rms < 0.001);
	/* Raw mode retains the independent, sample-rate-derived 10 Hz notch. */
	gain = tone(48000, 48000, 48000, 0, 0, &rms);
	CHECK(gain < 0.001 && rms < 0.001);
	gain = tone(48000, 48000, 48000, 0, 10, &rms);
	CHECK(gain > 0.69 && gain < 0.73);
	gain = tone(48000, 48000, 48000, 0, 1000, &rms);
	CHECK(fabs(20 * log10(gain)) < 0.1);
}

int main(void)
{
	test_plans();
	test_allocations();
	test_coefficients(2400000, 200000, 150000, 2);
	test_coefficients(2400000, 200000, 0, 2);
	test_coefficients(2400000, 2400000, 0, 1);
	test_coefficients(2400000, 1200000, 0, 1);
	test_coefficients(1100000, 100000, 0, 1);
	test_coefficients(4096000, 256000, 0, 2);
	test_coefficients(4096000, 64000, 0, 3);
	test_coefficients(4096000, 16000, 0, 4);
	test_coefficients(4096000, 1000, 0, 5);
	test_coefficients(4096000, 1000, 1, 5);
	test_tones();
	return 0;
}
