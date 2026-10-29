#include "fft_processing.h"

/* Integer square root, processing two input bits per step. */
uint32_t isqrt(uint32_t n) {
    uint32_t root = 0;
    uint32_t bit = 1UL << 30;
    while (bit > n) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (n >= root + bit) {
            n -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return root;
}

void compute_fft_magnitudes(int16_t *input, uint16_t *magnitudes) {
    /* Eight sine and cosine samples scaled by 256. Keep FFT_SAMPLES at eight. */
    const int16_t cos_table[8] = {256, 181, 0, -181, -256, -181, 0, 181};
    const int16_t sin_table[8] = {0, 181, 256, 181, 0, -181, -256, -181};

    for (int k = 0; k < FFT_SAMPLES; k++) {
        int32_t sum_real = 0;
        int32_t sum_imag = 0;

        for (int n = 0; n < FFT_SAMPLES; n++) {
            int idx = (k * n) % 8;
            sum_real += input[n] * cos_table[idx];
            sum_imag -= input[n] * sin_table[idx];
        }

        /* Remove the lookup-table scale using signed integer division. */
        sum_real /= 256;
        sum_imag /= 256;

        /* Combine the real and imaginary components before taking the square root. */
        uint32_t mag_sq = (sum_real * sum_real) + (sum_imag * sum_imag);

        /* The resulting magnitude retains the input scale; integer arithmetic introduces
         * rounding. */
        magnitudes[k] = (uint16_t)isqrt(mag_sq);
    }
}
