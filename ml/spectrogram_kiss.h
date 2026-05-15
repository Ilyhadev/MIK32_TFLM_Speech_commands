#ifndef SPECTROGRAM_KISS_H
#define SPECTROGRAM_KISS_H

/* 16 kHz framing matches micro_speech / feature_extractor (30 ms window, 20 ms hop). */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SK_FRAME_LEN 480U
#define SK_FRAME_HOP 320U
#define SK_BINS 40U
#define SK_SLICES 49U
#define SK_FEATURE_COUNT (SK_BINS * SK_SLICES)

/** kiss_fftr_alloc(512) uses ~2836 B on MIK32; keep tight for 16 KiB RAM / stack headroom. */
#define SK_FFT_MEM_MAX 2880U

typedef struct {
    int16_t frame_buffer[SK_FRAME_LEN];
    uint16_t write_index;
    uint16_t samples_since_last_frame;
    uint16_t slice_count;
    uint32_t frame_seq;
    int16_t dc_estimate;
    uint16_t adc_midpoint;
    int16_t mic_gain_q8;
    uint8_t fft_mem[SK_FFT_MEM_MAX] __attribute__((aligned(8)));
    void *fftr_cfg;
    int16_t fft_time[512];
    /** RFFT scratch (257 kiss_fft_cpx); in struct to avoid duplicate .bss. */
    int16_t fft_line[514];
    uint32_t fft_mem_bytes;
} SpectrogramKissState;

void spectrogram_kiss_init(SpectrogramKissState *st);
bool spectrogram_kiss_fft_ready(const SpectrogramKissState *st);
/** Bytes kiss_fftr_alloc reported at init (0 if FFT unavailable). */
uint32_t spectrogram_kiss_fft_mem_used(const SpectrogramKissState *st);

void spectrogram_kiss_set_cal(SpectrogramKissState *st, uint16_t adc_midpoint, int16_t mic_gain_q8);

void spectrogram_kiss_push_adc(SpectrogramKissState *st, uint16_t adc12_sample, int8_t *feature_window);

bool spectrogram_kiss_ready(const SpectrogramKissState *st);

uint16_t spectrogram_kiss_slice_count(const SpectrogramKissState *st);

uint32_t spectrogram_kiss_frame_seq(const SpectrogramKissState *st);

#ifdef __cplusplus
}
#endif

#endif
