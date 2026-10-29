#ifndef FFT_PROCESSING_H
#define FFT_PROCESSING_H

#include <stdint.h>

#define FFT_SAMPLES 8

/* Compute eight fixed-point DFT magnitudes using lookup tables. */
/* Direct DFT implementation with eight-point lookup tables. */
void compute_fft_magnitudes(int16_t *input, uint16_t *magnitudes);

/* Return the integer square root without a floating-point library. */
uint32_t isqrt(uint32_t n);

#endif