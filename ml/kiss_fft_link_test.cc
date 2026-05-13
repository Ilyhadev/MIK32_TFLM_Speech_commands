// Verifies we can resolve kiss_fft_fixed16::* from libtensorflow-microlite.a
// (same object file as TFLM signal / RFFT path). Do not compile kiss_fft_int16.cc
// again here — only headers + one reference to symbols in the archive.

#include <stddef.h>
#include <stdint.h>

#include "signal/src/kiss_fft_wrappers/kiss_fft_int16.h"

extern "C" int kiss_fft_fixed16_link_smoke(void) {
    /* Small FFT keeps .bss tiny (MIK32 has 16 KiB RAM). Link test only. */
    constexpr int kNfft = 512;
    size_t need = 0;
    if (kiss_fft_fixed16::kiss_fftr_alloc(kNfft, 0, nullptr, &need) != nullptr) {
        return -1;
    }
    if (need == 0U || need > sizeof(uint8_t) * 4048) {
        return -2;
    }
    alignas(8) static uint8_t scratch[4048];
    if (need > sizeof(scratch)) {
        return -3;
    }
    void* cfg = kiss_fft_fixed16::kiss_fftr_alloc(kNfft, 0, scratch, &need);
    if (cfg != static_cast<void*>(scratch)) {
        return -4;
    }
    return need;
}
