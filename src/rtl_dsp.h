#ifndef RTL_DSP_H
#define RTL_DSP_H

#include <stdint.h>

struct rtlsdr_dsp;

struct rtlsdr_dsp *rtlsdr_dsp_create(uint64_t input_rate,
				     uint64_t output_rate,
				     uint64_t bandwidth,
				     uint64_t freq_offset);

void rtlsdr_dsp_destroy(struct rtlsdr_dsp *dsp);

int rtlsdr_dsp_process(struct rtlsdr_dsp *dsp,
		       const int16_t *input, size_t input_len,
		       int16_t *output, size_t *output_len);

void rtlsdr_dsp_reset(struct rtlsdr_dsp *dsp);

#endif
