#ifndef RTL_DSP_H
#define RTL_DSP_H

#include <stdint.h>
#include <stddef.h>

struct rtlsdr_dsp;

/* Rates must divide exactly (ratio 1..4096), with input <= UINT32_MAX.
 * Bandwidth zero selects floor(4*output_rate/5); filtered bandwidth must be
 * positive and no larger than that default. The entire passband must fit
 * within the capture. Only bandwidth == output_rate == input_rate with
 * zero offset bypasses the FIR. Validation checks the bounded filter plan,
 * not allocation success; effective_bandwidth may be NULL and is only
 * written on success. Returns 0 on success, -1 on invalid/over-budget plans.
 * A nominal 10 Hz DC notch at the CAPTURE center precedes frequency mixing,
 * including in raw mode; signals near that center are attenuated. */
int rtlsdr_dsp_validate(uint64_t input_rate, uint64_t output_rate,
		       uint64_t bandwidth, int64_t freq_offset,
		       uint64_t *effective_bandwidth);

struct rtlsdr_dsp *rtlsdr_dsp_create(uint64_t input_rate,
				     uint64_t output_rate,
				     uint64_t bandwidth,
				     int64_t freq_offset);

void rtlsdr_dsp_destroy(struct rtlsdr_dsp *dsp);

/* Input is interleaved unsigned I/Q bytes, centered at 127.5.
 * Output is filtered I/Q scaled by 256 and saturated to int16_t.
 * Capacity and output_len count int16_t components, not complex samples.
 * Capacity must be >= input_len (bytes), even when decimating.
 * Errors set output_len to zero and leave DSP state unchanged. */
int rtlsdr_dsp_process(struct rtlsdr_dsp *dsp,
		       const uint8_t *input, size_t input_len,
		       int16_t *output, size_t output_capacity,
		       size_t *output_len);

void rtlsdr_dsp_reset(struct rtlsdr_dsp *dsp);

#endif
