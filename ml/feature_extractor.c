#include "feature_extractor.h"

#include <string.h>

/* Spectrogram shape matches README_MICRO_SPEECH.md § Preprocessing Speech Input
 * (40 bins × 49 slices = 1960 int8 values for the keyword model).
 *
 * IMPORTANT: The rows produced here are a **legacy energy placeholder**, not the
 * training FFT + filter-bank + AGC pipeline. For production accuracy use either:
 *   (A) TensorFlow’s `audio_preprocessor_int8.tflite` in a second MicroInterpreter
 *       (see tflite-micro/.../micro_speech/README.md), or
 *   (B) A fixed-point port of the training spectrogram (FFT, binning, NR, AGC).
 */

static uint16_t ring_index(uint16_t base, uint16_t offset) {
    return (uint16_t)((base + offset) % FE_FRAME_LEN);
}

static int16_t saturate_i32_to_i16(int32_t x) {
    if (x > 32767) {
        return 32767;
    }
    if (x < -32768) {
        return -32768;
    }
    return (int16_t)x;
}

static int8_t compress_energy_to_int8(uint32_t energy_acc) {
    uint8_t bits = 0;
    while (energy_acc != 0U) {
        energy_acc >>= 1U;
        bits++;
    }
    int16_t q = (int16_t)bits * 8 - 128;
    if (q > 127) {
        q = 127;
    }
    if (q < -128) {
        q = -128;
    }
    return (int8_t)q;
}

static void compute_slice(const FeatureExtractorState* st, int8_t* out40) {
    const uint16_t samples_per_bin = (uint16_t)(FE_FRAME_LEN / FE_BINS);
    const uint16_t frame_start = st->write_index;
    for (uint16_t b = 0; b < FE_BINS; b++) {
        uint32_t energy = 0U;
        const uint16_t bin_start = (uint16_t)(b * samples_per_bin);
        for (uint16_t i = 0; i < samples_per_bin; i++) {
            int16_t s = st->frame_buffer[ring_index(frame_start, (uint16_t)(bin_start + i))];
            if (s < 0) {
                s = (int16_t)-s;
            }
            energy += (uint32_t)(uint16_t)s;
        }
        out40[b] = compress_energy_to_int8(energy);
    }
}

void feature_extractor_init(FeatureExtractorState* st) {
    memset(st, 0, sizeof(*st));
    st->adc_midpoint = 2048U;
    st->mic_gain_q8 = 1024; /* 4.0× — tune for your MAX9814 + divider to approach int16 speech swing */
}

void feature_extractor_set_cal(FeatureExtractorState* st, uint16_t adc_midpoint,
                               int16_t mic_gain_q8) {
    if (st == NULL) {
        return;
    }
    st->adc_midpoint = adc_midpoint;
    if (mic_gain_q8 < 32) {
        mic_gain_q8 = 32;
    }
    st->mic_gain_q8 = mic_gain_q8;
}

static int16_t adc12_to_training_pcm16(const FeatureExtractorState* st, uint16_t adc12) {
    /* README: samples represent -1..+1 as int16 -32768..32767. */
    int32_t centered = (int32_t)adc12 - (int32_t)st->adc_midpoint;
    int32_t scaled = (centered * (int32_t)st->mic_gain_q8) >> 8;
    return saturate_i32_to_i16(scaled);
}

void feature_extractor_push_pcm16(FeatureExtractorState* st, int16_t pcm16,
                                  int8_t* feature_window) {
    if (st == NULL) {
        return;
    }
    /* First-order DC blocker on int16 PCM (MAX9814 bias / ADC offset). */
    st->dc_estimate = (int16_t)((15 * (int32_t)st->dc_estimate + (int32_t)pcm16) / 16);
    int16_t highpass = (int16_t)((int32_t)pcm16 - (int32_t)st->dc_estimate);

    st->frame_buffer[st->write_index] = highpass;
    st->write_index = (uint16_t)((st->write_index + 1U) % FE_FRAME_LEN);
    st->samples_since_last_frame++;

    if (st->samples_since_last_frame < FE_FRAME_HOP) {
        return;
    }
    st->samples_since_last_frame = 0U;

    if (st->slice_count < FE_SLICES) {
        if (feature_window != NULL) {
            compute_slice(st, &feature_window[st->slice_count * FE_BINS]);
        }
        st->slice_count++;
        return;
    }

    if (feature_window != NULL) {
        memmove(feature_window, &feature_window[FE_BINS], FE_FEATURE_COUNT - FE_BINS);
        compute_slice(st, &feature_window[FE_FEATURE_COUNT - FE_BINS]);
    }
}

void feature_extractor_push_adc(FeatureExtractorState* st, uint16_t adc12_sample,
                                int8_t* feature_window) {
    if (st == NULL) {
        return;
    }
    int16_t pcm = adc12_to_training_pcm16(st, adc12_sample);
    feature_extractor_push_pcm16(st, pcm, feature_window);
}

bool feature_extractor_ready(const FeatureExtractorState* st) {
    return st != NULL && st->slice_count >= FE_SLICES;
}
