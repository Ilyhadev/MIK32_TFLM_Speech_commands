#ifndef ML_KISS_FFT_LINK_TEST_H
#define ML_KISS_FFT_LINK_TEST_H

#ifdef __cplusplus
extern "C" {
#endif

/** Returns 0 if real FFT alloc from microlite-linked kiss_fft_int16.o works. */
int kiss_fft_fixed16_link_smoke(void);

#ifdef __cplusplus
}
#endif

#endif
