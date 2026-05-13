#ifndef FEATURE_EXTRACTOR_H
#define FEATURE_EXTRACTOR_H

/* Shapes and timing match README_MICRO_SPEECH.md (30 ms window, 20 ms stride,
 * 16 kHz, 40 frequency bins, 49 time slices). Call push_* once per audio sample
 * at FE_SAMPLE_RATE_Hz for correct framing. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FE_SAMPLE_RATE_HZ 16000U
#define FE_FRAME_LEN 480U       /* 30 ms @ 16 kHz */
#define FE_FRAME_HOP 320U       /* 20 ms stride */
#define FE_BINS 40U
#define FE_SLICES 49U
#define FE_FEATURE_COUNT (FE_BINS * FE_SLICES)

typedef struct {
    int16_t frame_buffer[FE_FRAME_LEN];
    uint16_t write_index;
    uint16_t samples_since_last_frame;
    uint16_t slice_count;
    int16_t dc_estimate;
    uint16_t adc_midpoint; /* quiet ADC code (~bias); default 2048 for 12-bit */
    int16_t mic_gain_q8;   /* PCM scale: (adc - midpoint) * gain / 256 */
} FeatureExtractorState;

void feature_extractor_init(FeatureExtractorState* st);

/** Optional: set midpoint (silence ADC) and gain (Q8). Tune with quiet room + speech peaks. */
void feature_extractor_set_cal(FeatureExtractorState* st, uint16_t adc_midpoint,
                                 int16_t mic_gain_q8);

/** Preferred if you already have int16 PCM at 16 kHz (matches training -1..+1 mapping). */
void feature_extractor_push_pcm16(FeatureExtractorState* st, int16_t pcm16,
                                    int8_t* feature_window);

/** MAX9814 → 12-bit ADC: converts toward int16 training range, then same framing as push_pcm16. */
void feature_extractor_push_adc(FeatureExtractorState* st, uint16_t adc12_sample,
                                int8_t* feature_window);

bool feature_extractor_ready(const FeatureExtractorState* st);

#ifdef __cplusplus
}
#endif

#endif
