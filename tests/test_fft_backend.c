#include "fft/fft_backend.h"
#include "check.h"
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>

struct fft_plan *reference_create(int n);
void reference_destroy(struct fft_plan *p);
int reference_execute(struct fft_plan *p, const float *in, float *out);

static double max_error;
static void compare(const float *got, const float *expected, int n)
{
	/* Fixed before GPU runs: unit-scale inputs, component-wise FP32 errors. */
	for (int j = 0; j < 2*n; j++) {
		double error = fabs((double)got[j] - expected[j]);
		if (error > max_error) max_error = error;
		CHECK(isfinite(got[j]));
		if (error > 2e-5*sqrt(n) + 3e-5*fabs(expected[j]))
			fprintf(stderr, "N=%d component=%d got=%.9g expected=%.9g error=%.9g\n",
				n, j, got[j], expected[j], error);
		CHECK(error <= 2e-5*sqrt(n) + 3e-5*fabs(expected[j]));
	}
}

static void check_dft(const float *input, const float *output, int n)
{
	for (int k = 0; k < n; k++) {
		double re = 0, im = 0;
		for (int j = 0; j < n; j++) {
			double angle = -2*M_PI*j*k/n;
			re += input[2*j]*cos(angle) - input[2*j+1]*sin(angle);
			im += input[2*j]*sin(angle) + input[2*j+1]*cos(angle);
		}
		CHECK(fabs(output[2*k]-re) <= 2e-5 + 2e-5*fabs(re));
		CHECK(fabs(output[2*k+1]-im) <= 2e-5 + 2e-5*fabs(im));
	}
}

static void exercise(int n)
{
	struct fft_plan *p = fft_plan_create(n), *ref = reference_create(n);
	size_t bytes = (size_t)n*2*sizeof(float);
	float *in = malloc(bytes), *saved = malloc(bytes), *out = malloc(bytes);
	float *expected = malloc(bytes), *alias = malloc(bytes + sizeof(float));
	uint32_t rng = 0x12345678;
	CHECK(p && ref && in && saved && out && expected && alias);
	for (int kind = 0; kind < 6; kind++) {
		for (int j = 0; j < n; j++) {
			double angle = 2*M_PI*(n > 16 ? 7 : 1)*j/n;
			in[2*j] = in[2*j+1] = 0;
			if (kind == 1 && j == 0) { in[0] = 1; in[1] = -0.5f; }
			if (kind == 2) { in[2*j] = 0.25f; in[2*j+1] = -0.75f; }
			if (kind == 3 || kind == 4) {
				in[2*j] = (float)cos(angle);
				in[2*j+1] = (float)((kind == 3 ? 1 : -1)*sin(angle));
			}
			if (kind == 5) {
				for (int c = 0; c < 2; c++) {
					rng = rng*1664525u + 1013904223u;
					in[2*j+c] = (float)(rng >> 8)/8388608.0f - 1;
				}
			}
		}
		memcpy(saved, in, bytes);
		CHECK(reference_execute(ref, in, expected) == 0);
		for (int repeat = 0; repeat < 3; repeat++) {
			CHECK(fft_execute(p, in, out) == 0);
			CHECK(memcmp(in, saved, bytes) == 0);
			compare(out, expected, n);
			if (n <= 16) check_dft(in, out, n);
			memcpy(alias, in, bytes);
			CHECK(fft_execute(p, alias, alias) == 0);
			CHECK(memcmp(alias, out, bytes) == 0);
		}
	}
	memcpy(alias, in, bytes);
	alias[2*n] = 17;
	CHECK(fft_execute(p, alias, alias+1) < 0);
	CHECK(fft_execute(p, alias+1, alias) < 0);
	CHECK(memcmp(alias, in, bytes) == 0 && alias[2*n] == 17);
	CHECK(fft_execute(p, NULL, out) < 0 && fft_execute(p, in, NULL) < 0);
	CHECK(fft_execute(NULL, in, out) < 0);
	CHECK(fft_execute(p, in, out) == 0);
	reference_destroy(ref);
	fft_plan_destroy(p);
	free(in); free(saved); free(out); free(expected); free(alias);
}

static void *parallel_plan(void *unused)
{
	float data[32] = {1};
	struct fft_plan *p = fft_plan_create(16);
	(void)unused;
	CHECK(p && fft_execute(p, data, data) == 0);
	fft_plan_destroy(p);
	return NULL;
}

int main(void)
{
	const int invalid[] = {-1, 0, 1, 3, 15, 1023, 16385, 32768};
	const int sizes[] = {2, 16, 1024, 16384};
	struct fft_plan *a, *b;
	pthread_t threads[2];
	float input[32] = {1}, out[32];
	for (size_t i = 0; i < sizeof(invalid)/sizeof(*invalid); i++)
		CHECK(!fft_plan_create(invalid[i]));
	fft_plan_destroy(NULL);
	for (size_t i = 0; i < sizeof(sizes)/sizeof(*sizes); i++) exercise(sizes[i]);
	a = fft_plan_create(16); b = fft_plan_create(16);
	CHECK(a && b);
	fft_plan_destroy(a);
	CHECK(fft_execute(b, input, out) == 0);
	a = fft_plan_create(16);
	CHECK(a);
	fft_plan_destroy(b);
	CHECK(fft_execute(a, input, out) == 0);
	for (int i = 0; i < 2; i++) CHECK(pthread_create(&threads[i], NULL, parallel_plan, NULL) == 0);
	for (int i = 0; i < 2; i++) CHECK(pthread_join(threads[i], NULL) == 0);
	CHECK(fft_execute(a, input, out) == 0);
	fft_plan_destroy(a);
	printf("%s max component error versus CPU: %.9g\n", fft_backend_name(), max_error);
	return 0;
}
