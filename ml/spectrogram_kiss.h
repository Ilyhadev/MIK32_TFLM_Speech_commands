#ifndef SPECTROGRAM_KISS_H
#define SPECTROGRAM_KISS_H

/* 16 kHz framing matches micro_speech (30 ms window, 20 ms hop, 40 mel bins, 49 slices). */

#include <stddef.h>
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
#define SK_MIN_SLICES_INFER SK_SLICES
#define SK_EDGE_BLEND_COLS 1U
#define SK_SPEECH_END_COL 42U
#define SK_COLUMN_PEAK_MAX 110

/** kiss_fftr_alloc(512) uses ~2836 B */
#define SK_FFT_MEM_MAX 2880U

typedef struct {
    int16_t frame_buffer[SK_FRAME_LEN];
    uint16_t write_index;
    uint16_t samples_since_last_frame;
    uint16_t slice_count;
    uint16_t oldest_col;
    uint32_t frame_seq;
    uint16_t adc_midpoint;
    int16_t mic_gain_q8;
    uint8_t fft_mem[SK_FFT_MEM_MAX] __attribute__((aligned(8)));
    void *fftr_cfg;
    union {
        int16_t fft_time[512];
        int16_t fft_line[514];
    } fft_u;
    uint32_t fft_mem_bytes;
    uint32_t noise_estimate[SK_BINS];
} SpectrogramKissState;

void spectrogram_kiss_init(SpectrogramKissState *st);
bool spectrogram_kiss_fft_ready(const SpectrogramKissState *st);
void spectrogram_kiss_release_fft(SpectrogramKissState *st);
bool spectrogram_kiss_ensure_fft(SpectrogramKissState *st);
void spectrogram_kiss_set_cal(SpectrogramKissState *st, uint16_t adc_midpoint, int16_t mic_gain_q8);
void spectrogram_kiss_reset_stream(SpectrogramKissState *st);
void spectrogram_kiss_push_adc_block(SpectrogramKissState *st, const uint16_t *adc12_block, size_t count,
                                     int8_t *feature_window);
void spectrogram_kiss_pack_for_inference(const SpectrogramKissState *st, const int8_t *feature_window,
                                       uint16_t slice_count, int8_t *out);
uint16_t spectrogram_kiss_slice_count(const SpectrogramKissState *st);

#ifdef __cplusplus
}
#endif

#endif
