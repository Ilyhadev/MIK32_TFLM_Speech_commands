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
/** Model trained on 49×20 ms ≈ 1 s; infer only when the sliding window is full. */
#define SK_MIN_SLICES_INFER SK_SLICES

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
    /** Time-domain windowed samples and RFFT output share RAM (used sequentially). */
    union {
        int16_t fft_time[512];
        int16_t fft_line[514];
    } fft_u;
    uint32_t fft_mem_bytes;
    uint32_t noise_estimate[SK_BINS];
    int8_t pad_column[SK_BINS];
    bool pad_column_valid;
    bool pad_cal_active;
} SpectrogramKissState;

void spectrogram_kiss_init(SpectrogramKissState *st);
bool spectrogram_kiss_fft_ready(const SpectrogramKissState *st);
uint32_t spectrogram_kiss_fft_mem_used(const SpectrogramKissState *st);

void spectrogram_kiss_set_cal(SpectrogramKissState *st, uint16_t adc_midpoint, int16_t mic_gain_q8);

/** Reset streaming state; keeps pad_column if already calibrated. */
void spectrogram_kiss_reset_stream(SpectrogramKissState *st);

/** Start quiet-room pad calibration (call before pushing quiet ADC). */
void spectrogram_kiss_begin_pad_cal(SpectrogramKissState *st);

/** Finish pad calibration from last slice; returns false if no slice was produced. */
bool spectrogram_kiss_finish_pad_cal(SpectrogramKissState *st);

bool spectrogram_kiss_pad_ready(const SpectrogramKissState *st);

void spectrogram_kiss_push_adc(SpectrogramKissState *st, uint16_t adc12_sample, int8_t *feature_window);

void spectrogram_kiss_push_adc_block(SpectrogramKissState *st, const uint16_t *adc12_block, size_t count,
                                     int8_t *feature_window);

/**
 * Copy feature_window into out (1960 int8) with leading silence columns so the
 * most recent slice_count columns align with training layout (recent speech at end).
 */
void spectrogram_kiss_pack_for_inference(const SpectrogramKissState *st, const int8_t *feature_window,
                                       uint16_t slice_count, int8_t *out);

void spectrogram_kiss_last_column_minmax(const int8_t *feature_window, int8_t *out_min, int8_t *out_max);

bool spectrogram_kiss_ready(const SpectrogramKissState *st);

uint16_t spectrogram_kiss_slice_count(const SpectrogramKissState *st);

uint32_t spectrogram_kiss_frame_seq(const SpectrogramKissState *st);

#ifdef __cplusplus
}
#endif

#endif
