// micro_speech-compatible frontend: Hann window, 512-pt RFFT, mel filter bank,
// spectral subtraction, log, int8 quantize (matches audio_preprocessor.py defaults).

#include "spectrogram_kiss.h"

#include "micro_frontend_tables.h"

#include <cstring>

#ifndef SPECTROGRAM_HOST
#include <analog_reg.h>
#include <mik32_memory_map.h>
extern "C" uint32_t HAL_Time_SCR1TIM_Micros(void);
#else
extern "C" uint32_t HAL_Time_SCR1TIM_Micros(void) {
    return 0U;
}
#endif

#include "signal/src/complex.h"
#include "signal/src/energy.h"
#include "signal/src/fft_auto_scale.h"
#include "signal/src/filter_bank.h"
#include "signal/src/filter_bank_log.h"
#include "signal/src/filter_bank_spectral_subtraction.h"
#include "signal/src/filter_bank_square_root.h"
#include "signal/src/kiss_fft_wrappers/kiss_fft_int16.h"
#include "signal/src/pcan_argc_fixed.h"

namespace {

/** One hop of raw ADC; filled in speak window before FFT (RAM limited). */
constexpr uint16_t kCoopCap = SK_FRAME_HOP;
static uint16_t g_coop_adc[kCoopCap];
static uint16_t g_coop_adc_count;

void spectrogram_kiss_write_inference_features_impl(const SpectrogramKissState *st,
                                                    const int8_t *feature_window, int8_t *out);

using kiss_fft_fixed16::kiss_fft_cpx;
using kiss_fft_fixed16::kiss_fftr;
using kiss_fft_fixed16::kiss_fftr_alloc;
using kiss_fft_fixed16::kiss_fftr_cfg;
using tflite::tflm_signal::FilterbankAccumulateChannels;
using tflite::tflm_signal::FilterbankConfig;
using tflite::tflm_signal::FilterbankLog;
using tflite::tflm_signal::FilterbankSpectralSubtraction;
using tflite::tflm_signal::FilterbankSqrt;
using tflite::tflm_signal::FftAutoScale;
using tflite::tflm_signal::SpectralSubtractionConfig;
using tflite::tflm_signal::ApplyPcanAutoGainControlFixed;
using tflite::tflm_signal::SpectrumToEnergy;

constexpr int kFftN = 512;

static uint16_t ring_index(uint16_t base, uint16_t offset) {
    return static_cast<uint16_t>((base + offset) % SK_FRAME_LEN);
}

static int16_t saturate_i32_to_i16(int32_t x) {
    if (x > 32767) {
        return 32767;
    }
    if (x < -32768) {
        return -32768;
    }
    return static_cast<int16_t>(x);
}

static int8_t feature_to_int8(int16_t feature) {
    int32_t value = (static_cast<int32_t>(feature) * SK_VALUE_SCALE + (SK_VALUE_DIV / 2)) / SK_VALUE_DIV;
    value -= 128;
    if (value < -128) {
        value = -128;
    }
    if (value > 127) {
        value = 127;
    }
    return static_cast<int8_t>(value);
}

static int16_t adc12_to_pcm16(const SpectrogramKissState *st, uint16_t adc12) {
    int32_t centered = static_cast<int32_t>(adc12) - static_cast<int32_t>(st->adc_midpoint);
    int32_t scaled = (centered * static_cast<int32_t>(st->mic_gain_q8)) >> 8;
    return saturate_i32_to_i16(scaled);
}

static void compute_slice(SpectrogramKissState *st, int8_t *out40) {
    kiss_fftr_cfg cfg = static_cast<kiss_fftr_cfg>(st->fftr_cfg);
    if (cfg == nullptr || out40 == nullptr) {
        return;
    }

    uint32_t filterbank_scaled[SK_BINS];

    const uint16_t frame_start = st->write_index;
    for (uint16_t i = 0; i < SK_FRAME_LEN; i++) {
        const int16_t sample = st->frame_buffer[ring_index(frame_start, i)];
        const int32_t weighted =
            (static_cast<int32_t>(sample) * static_cast<int32_t>(kHannWindow[i])) >> SK_HANN_SHIFT;
        st->fft_u.fft_time[i] = saturate_i32_to_i16(weighted);
    }

    const int scale_bits =
        FftAutoScale(st->fft_u.fft_time, static_cast<int>(SK_FRAME_LEN), st->fft_u.fft_time);
    std::memset(&st->fft_u.fft_time[SK_FRAME_LEN], 0,
                (kFftN - static_cast<int>(SK_FRAME_LEN)) * sizeof(st->fft_u.fft_time[0]));

    auto *const line = reinterpret_cast<kiss_fft_cpx *>(st->fft_u.fft_line);
    kiss_fftr(cfg, st->fft_u.fft_time, line);

    /* In-place complex → energy in fft_line; do NOT memset before read (was → all -128). */
    auto *const cplx = reinterpret_cast<::Complex<int16_t> *>(st->fft_u.fft_line);
    auto *const spectrum = reinterpret_cast<uint32_t *>(st->fft_u.fft_line);
    SpectrumToEnergy(cplx, SK_ENERGY_START, SK_ENERGY_END, spectrum);
    for (int i = 0; i < SK_ENERGY_START; i++) {
        spectrum[i] = 0U;
    }
    for (int i = SK_ENERGY_END; i < (kFftN / 2) + 1; i++) {
        spectrum[i] = 0U;
    }

    uint64_t filterbank_accum[41];
    FilterbankConfig fb_cfg{};
    fb_cfg.num_channels = static_cast<int32_t>(SK_BINS);
    fb_cfg.channel_frequency_starts = kFbChFreqStart;
    fb_cfg.channel_weight_starts = kFbChWeightStart;
    fb_cfg.channel_widths = kFbChWidth;
    fb_cfg.weights = kFbWeights;
    fb_cfg.unweights = kFbUnweights;
    FilterbankAccumulateChannels(&fb_cfg, spectrum, filterbank_accum);

    FilterbankSqrt(filterbank_accum + 1, static_cast<int>(SK_BINS), scale_bits, filterbank_scaled);

    SpectralSubtractionConfig ss_cfg{};
    ss_cfg.num_channels = static_cast<int32_t>(SK_BINS);
    ss_cfg.smoothing = SK_FB_EVEN_SMOOTH;
    ss_cfg.one_minus_smoothing = SK_FB_ONE_MINUS_EVEN;
    ss_cfg.alternate_smoothing = SK_FB_ODD_SMOOTH;
    ss_cfg.alternate_one_minus_smoothing = SK_FB_ONE_MINUS_ODD;
    ss_cfg.min_signal_remaining = SK_FB_MIN_SIGNAL;
    ss_cfg.smoothing_bits = SK_FB_SMOOTH_BITS;
    ss_cfg.spectral_subtraction_bits = SK_FB_SPECTRAL_BITS;
    ss_cfg.clamping = false;
    FilterbankSpectralSubtraction(&ss_cfg, filterbank_scaled, filterbank_scaled, st->noise_estimate);

    ApplyPcanAutoGainControlFixed(kPcanGainLut, SK_PCAN_SNR_SHIFT, st->noise_estimate, filterbank_scaled,
                                  static_cast<int>(SK_BINS));

    const int correction_bits = SK_FFT_SIZE_LOG2 - (SK_FB_SCALE_BITS / 2);
    int16_t log_out[SK_BINS];
    FilterbankLog(filterbank_scaled, static_cast<int>(SK_BINS), SK_FB_POST_SCALE,
                  static_cast<uint32_t>(correction_bits), log_out);

    for (uint16_t b = 0; b < SK_BINS; b++) {
        out40[b] = feature_to_int8(log_out[b]);
    }
}

static const int8_t *pad_column_ptr(const SpectrogramKissState *st) {
    if (st != nullptr && st->pad_column_valid) {
        return st->pad_column;
    }
    return kDefaultPadColumn;
}

static int8_t *slice_column_ptr(SpectrogramKissState *st, int8_t *feature_window) {
    if (feature_window == nullptr) {
        return nullptr;
    }
    if (st->slice_count < SK_SLICES) {
        return &feature_window[st->slice_count * SK_BINS];
    }
    int8_t *const col = &feature_window[st->oldest_col * SK_BINS];
    st->oldest_col = static_cast<uint16_t>((st->oldest_col + 1U) % SK_SLICES);
    return col;
}

static void emit_hop_slice(SpectrogramKissState *st, int8_t *feature_window) {
    int8_t scratch[SK_BINS];
    int8_t *slice_out = nullptr;
    if (feature_window != nullptr) {
        slice_out = slice_column_ptr(st, feature_window);
    } else if (st->pad_cal_active) {
        slice_out = st->pad_column;
    } else {
        /* Still run mel/PCAN so noise_estimate tracks (feature_window may be NULL). */
        slice_out = scratch;
    }

    compute_slice(st, slice_out);

    if (st->slice_count < SK_SLICES) {
        st->slice_count++;
        if (st->slice_count >= SK_SLICES) {
            st->oldest_col = 0U;
        }
    }
    st->frame_seq++;
}

static void push_adc_sample(SpectrogramKissState *st, uint16_t adc12_sample, int8_t *feature_window) {
    if (st == nullptr || st->fftr_cfg == nullptr) {
        return;
    }

    const int16_t pcm16 = adc12_to_pcm16(st, adc12_sample);
    st->frame_buffer[st->write_index] = pcm16;
    st->write_index = static_cast<uint16_t>((st->write_index + 1U) % SK_FRAME_LEN);
    st->samples_since_last_frame++;

    if (st->samples_since_last_frame < SK_FRAME_HOP) {
        return;
    }
    st->samples_since_last_frame = 0U;
    emit_hop_slice(st, feature_window);
}

void spectrogram_kiss_write_inference_features_impl(const SpectrogramKissState *st,
                                                    const int8_t *feature_window, int8_t *out) {
    if (out == nullptr) {
        return;
    }
    if (feature_window == nullptr) {
        for (uint16_t s = 0; s < SK_SLICES; s++) {
            std::memcpy(&out[s * SK_BINS], kDefaultPadColumn, SK_BINS);
        }
        return;
    }
    if (st == nullptr || st->slice_count < SK_SLICES) {
        const int8_t *const pad = pad_column_ptr(st);
        const uint16_t sc = (st != nullptr) ? st->slice_count : 0U;
        const uint16_t pad_cols = static_cast<uint16_t>(SK_SLICES - sc);
        if (sc > 0U) {
            std::memcpy(&out[pad_cols * SK_BINS], feature_window, static_cast<size_t>(sc) * SK_BINS);
        }
        for (uint16_t s = 0; s < pad_cols; s++) {
            std::memcpy(&out[s * SK_BINS], pad, SK_BINS);
        }
        return;
    }
    if (st->oldest_col == 0U) {
        if (out != feature_window) {
            std::memcpy(out, feature_window, SK_FEATURE_COUNT);
        }
        return;
    }
    for (uint16_t s = 0; s < SK_SLICES; s++) {
        const uint16_t src_col = static_cast<uint16_t>((st->oldest_col + s) % SK_SLICES);
        std::memcpy(&out[s * SK_BINS], &feature_window[src_col * SK_BINS], SK_BINS);
    }
}


static int8_t clamp_i8(int v) {
    if (v < -128) {
        return -128;
    }
    if (v > 127) {
        return 127;
    }
    return static_cast<int8_t>(v);
}

static void blend_column_ramp(int8_t *col, const int8_t *pad, const int8_t *speech, unsigned num,
                              unsigned den) {
    /* Quadratic ease: pad -> speech (smoother than linear; no float on MCU). */
    const unsigned pad_w = (den - num) * (den - num);
    const unsigned sp_w = num * num;
    const unsigned sum = pad_w + sp_w;
    for (unsigned b = 0U; b < SK_BINS; b++) {
        const int mixed = (static_cast<int>(pad_w) * static_cast<int>(pad[b]) +
                           static_cast<int>(sp_w) * static_cast<int>(speech[b])) /
                          static_cast<int>(sum);
        col[b] = clamp_i8(mixed);
    }
}

static void soften_speech_edges(int8_t *out, uint16_t lead_cols, uint16_t speech_cols,
                                const int8_t *pad) {
    if (out == nullptr || pad == nullptr || speech_cols == 0U) {
        return;
    }
    const uint16_t blend =
        (speech_cols < SK_EDGE_BLEND_COLS) ? speech_cols : SK_EDGE_BLEND_COLS;
    const unsigned den = static_cast<unsigned>(blend + 1U);
    int8_t saved[SK_BINS];

    for (uint16_t j = 0U; j < blend; j++) {
        const uint16_t c = lead_cols + j;
        int8_t *dst = &out[static_cast<size_t>(c) * SK_BINS];
        std::memcpy(saved, dst, SK_BINS);
        blend_column_ramp(dst, pad, saved, static_cast<unsigned>(j + 1U), den);
    }
    for (uint16_t j = 0U; j < blend; j++) {
        const uint16_t c = lead_cols + speech_cols - 1U - j;
        int8_t *dst = &out[static_cast<size_t>(c) * SK_BINS];
        std::memcpy(saved, dst, SK_BINS);
        blend_column_ramp(dst, pad, saved, static_cast<unsigned>(j + 1U), den);
    }
}

} // namespace

extern "C" {

void spectrogram_kiss_set_default_pad(SpectrogramKissState *st) {
    if (st == nullptr) {
        return;
    }
    std::memcpy(st->pad_column, kDefaultPadColumn, SK_BINS);
    st->pad_column_valid = true;
}

void spectrogram_kiss_init(SpectrogramKissState *st) {
    std::memset(st, 0, sizeof(*st));
    st->adc_midpoint = 2048U;
    st->mic_gain_q8 = 1024;

    size_t need = 0;
    if (kiss_fftr_alloc(kFftN, 0, nullptr, &need) != nullptr) {
        st->fftr_cfg = nullptr;
        return;
    }
    if (need == 0U || need > sizeof(st->fft_mem)) {
        st->fftr_cfg = nullptr;
        return;
    }
    st->fftr_cfg = kiss_fftr_alloc(kFftN, 0, st->fft_mem, &need);
    st->fft_mem_bytes = static_cast<uint32_t>(need);
}

bool spectrogram_kiss_fft_ready(const SpectrogramKissState *st) {
    return st != nullptr && st->fftr_cfg != nullptr;
}

void spectrogram_kiss_release_fft(SpectrogramKissState *st) {
    if (st == nullptr) {
        return;
    }
    st->fftr_cfg = nullptr;
    st->fft_mem_bytes = 0U;
}

bool spectrogram_kiss_ensure_fft(SpectrogramKissState *st) {
    if (st == nullptr) {
        return false;
    }
    if (st->fftr_cfg != nullptr) {
        return true;
    }
    size_t need = 0U;
    if (kiss_fftr_alloc(kFftN, 0, nullptr, &need) != nullptr) {
        return false;
    }
    if (need == 0U || need > sizeof(st->fft_mem)) {
        return false;
    }
    st->fftr_cfg = kiss_fftr_alloc(kFftN, 0, st->fft_mem, &need);
    st->fft_mem_bytes = static_cast<uint32_t>(need);
    return st->fftr_cfg != nullptr;
}

uint32_t spectrogram_kiss_fft_mem_used(const SpectrogramKissState *st) {
    if (st == nullptr) {
        return 0U;
    }
    return st->fft_mem_bytes;
}

void spectrogram_kiss_set_cal(SpectrogramKissState *st, uint16_t adc_midpoint, int16_t mic_gain_q8) {
    if (st == nullptr) {
        return;
    }
    st->adc_midpoint = adc_midpoint;
    if (mic_gain_q8 < 32) {
        mic_gain_q8 = 32;
    }
    st->mic_gain_q8 = mic_gain_q8;
}

void spectrogram_kiss_reset_stream(SpectrogramKissState *st) {
    if (st == nullptr) {
        return;
    }
    int8_t saved_pad[SK_BINS];
    const bool saved_valid = st->pad_column_valid;
    uint32_t saved_noise[SK_BINS];
    if (saved_valid) {
        std::memcpy(saved_pad, st->pad_column, SK_BINS);
    }
    std::memcpy(saved_noise, st->noise_estimate, sizeof(saved_noise));

    st->write_index = 0U;
    st->samples_since_last_frame = 0U;
    st->slice_count = 0U;
    st->oldest_col = 0U;
    st->frame_seq = 0U;
    st->dc_estimate = 0;
    std::memset(st->frame_buffer, 0, sizeof(st->frame_buffer));
    std::memcpy(st->noise_estimate, saved_noise, sizeof(saved_noise));

    if (saved_valid) {
        std::memcpy(st->pad_column, saved_pad, SK_BINS);
        st->pad_column_valid = true;
    }
}

void spectrogram_kiss_begin_pad_cal(SpectrogramKissState *st) {
    if (st == nullptr) {
        return;
    }
    st->pad_column_valid = false;
    st->pad_cal_active = true;
    std::memset(st->noise_estimate, 0, sizeof(st->noise_estimate));
    st->write_index = 0U;
    st->samples_since_last_frame = 0U;
    st->slice_count = 0U;
    st->oldest_col = 0U;
    st->frame_seq = 0U;
    st->dc_estimate = 0;
    std::memset(st->frame_buffer, 0, sizeof(st->frame_buffer));
}

bool spectrogram_kiss_pad_ready(const SpectrogramKissState *st) {
    return st != nullptr && st->pad_column_valid;
}

bool spectrogram_kiss_finish_pad_cal(SpectrogramKissState *st) {
    if (st == nullptr) {
        return false;
    }
    st->pad_cal_active = false;
    if (st->slice_count == 0U) {
        return false;
    }
    st->pad_column_valid = true;
    st->write_index = 0U;
    st->samples_since_last_frame = 0U;
    st->slice_count = 0U;
    st->oldest_col = 0U;
    st->frame_seq = 0U;
    st->dc_estimate = 0;
    std::memset(st->frame_buffer, 0, sizeof(st->frame_buffer));
    return true;
}

void spectrogram_kiss_push_adc(SpectrogramKissState *st, uint16_t adc12_sample, int8_t *feature_window) {
    push_adc_sample(st, adc12_sample, feature_window);
}

static void push_pcm_samples(SpectrogramKissState *st, const int16_t *pcm, size_t count,
                             int8_t *feature_window) {
    for (size_t i = 0; i < count; i++) {
        st->frame_buffer[st->write_index] = pcm[i];
        st->write_index = static_cast<uint16_t>((st->write_index + 1U) % SK_FRAME_LEN);
    }
    st->samples_since_last_frame =
        static_cast<uint16_t>(st->samples_since_last_frame + static_cast<uint16_t>(count));

    while (st->samples_since_last_frame >= SK_FRAME_HOP) {
        st->samples_since_last_frame =
            static_cast<uint16_t>(st->samples_since_last_frame - SK_FRAME_HOP);
        if (feature_window != nullptr && !st->pad_cal_active && st->slice_count >= SK_SLICES) {
            continue;
        }
        emit_hop_slice(st, feature_window);
    }
}

void spectrogram_kiss_push_adc_block(SpectrogramKissState *st, const uint16_t *adc12_block, size_t count,
                                     int8_t *feature_window) {
    if (st == nullptr || st->fftr_cfg == nullptr || adc12_block == nullptr || count == 0U) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        const int16_t pcm16 = adc12_to_pcm16(st, adc12_block[i]);
        st->frame_buffer[st->write_index] = pcm16;
        st->write_index = static_cast<uint16_t>((st->write_index + 1U) % SK_FRAME_LEN);
        st->samples_since_last_frame++;
        while (st->samples_since_last_frame >= SK_FRAME_HOP) {
            st->samples_since_last_frame =
                static_cast<uint16_t>(st->samples_since_last_frame - SK_FRAME_HOP);
            if (feature_window != nullptr && !st->pad_cal_active && st->slice_count >= SK_SLICES) {
                continue;
            }
            emit_hop_slice(st, feature_window);
        }
    }
}

void spectrogram_kiss_push_pcm16_block(SpectrogramKissState *st, const int16_t *pcm_block, size_t count,
                                       int8_t *feature_window) {
    if (st == nullptr || st->fftr_cfg == nullptr || pcm_block == nullptr || count == 0U) {
        return;
    }
    push_pcm_samples(st, pcm_block, count, feature_window);
}

void spectrogram_kiss_pack_for_inference(const SpectrogramKissState *st, const int8_t *feature_window,
                                         uint16_t slice_count, int8_t *out) {
    if (out == nullptr) {
        return;
    }
    const int8_t *const pad = pad_column_ptr(st);
    if (feature_window == nullptr || slice_count == 0U) {
        for (uint16_t s = 0; s < SK_SLICES; s++) {
            std::memcpy(&out[s * SK_BINS], pad, SK_BINS);
        }
        return;
    }
    if (slice_count >= SK_SLICES && st != nullptr) {
        spectrogram_kiss_write_inference_features_impl(st, feature_window, out);
        return;
    }
    uint16_t lead_cols = 0U;
    if (slice_count > 0U && slice_count <= SK_SLICES && SK_SPEECH_END_COL + 1U >= slice_count) {
        lead_cols = static_cast<uint16_t>(SK_SPEECH_END_COL + 1U - slice_count);
    }
    if (lead_cols + slice_count > SK_SLICES) {
        lead_cols = 0U;
    }
    const size_t live_bytes = static_cast<size_t>(slice_count) * SK_BINS;
    std::memmove(&out[lead_cols * SK_BINS], feature_window, live_bytes);
    for (uint16_t s = 0; s < lead_cols; s++) {
        std::memcpy(&out[s * SK_BINS], pad, SK_BINS);
    }
    for (uint16_t s = lead_cols + slice_count; s < SK_SLICES; s++) {
        std::memcpy(&out[s * SK_BINS], pad, SK_BINS);
    }
    soften_speech_edges(out, lead_cols, slice_count, pad);
}

void spectrogram_kiss_write_inference_features(const SpectrogramKissState *st, const int8_t *feature_window,
                                             int8_t *out) {
    spectrogram_kiss_write_inference_features_impl(st, feature_window, out);
}

void spectrogram_kiss_last_column_minmax(const int8_t *feature_window, int8_t *out_min, int8_t *out_max) {
    int8_t lo = 127;
    int8_t hi = -128;
    if (feature_window == nullptr) {
        if (out_min != nullptr) {
            *out_min = 0;
        }
        if (out_max != nullptr) {
            *out_max = 0;
        }
        return;
    }
    const int8_t *col = &feature_window[SK_FEATURE_COUNT - SK_BINS];
    for (uint16_t i = 0; i < SK_BINS; i++) {
        const int8_t v = col[i];
        if (v < lo) {
            lo = v;
        }
        if (v > hi) {
            hi = v;
        }
    }
    if (out_min != nullptr) {
        *out_min = lo;
    }
    if (out_max != nullptr) {
        *out_max = hi;
    }
}

bool spectrogram_kiss_ready(const SpectrogramKissState *st) {
    return st != nullptr && st->fftr_cfg != nullptr && st->slice_count >= SK_MIN_SLICES_INFER;
}

uint16_t spectrogram_kiss_slice_count(const SpectrogramKissState *st) {
    if (st == nullptr) {
        return 0U;
    }
    return st->slice_count;
}

uint32_t spectrogram_kiss_frame_seq(const SpectrogramKissState *st) {
    if (st == nullptr) {
        return 0U;
    }
    return st->frame_seq;
}

uint16_t spectrogram_kiss_coop_poll_ms(uint32_t duration_ms) {
    (void)duration_ms;
#ifndef SPECTROGRAM_HOST
    g_coop_adc_count = 0U;
    if (duration_ms == 0U) {
        return 0U;
    }
    const uint32_t deadline_us = HAL_Time_SCR1TIM_Micros() + (duration_ms * 1000U);
    while ((int32_t)(HAL_Time_SCR1TIM_Micros() - deadline_us) < 0) {
        if (g_coop_adc_count >= kCoopCap) {
            break;
        }
        g_coop_adc[g_coop_adc_count++] = static_cast<uint16_t>(ANALOG_REG->ADC_VALUE);
    }
    return g_coop_adc_count;
#else
    return 0U;
#endif
}

uint16_t spectrogram_kiss_drain_coop_hops(SpectrogramKissState *st, int8_t *feature_window,
                                          uint16_t max_hops) {
    if (st == nullptr || st->fftr_cfg == nullptr || max_hops == 0U) {
        return 0U;
    }
    uint16_t hops = 0U;
    while (hops < max_hops && g_coop_adc_count >= SK_FRAME_HOP) {
        spectrogram_kiss_push_adc_block(st, g_coop_adc, SK_FRAME_HOP, feature_window);
        g_coop_adc_count = static_cast<uint16_t>(g_coop_adc_count - SK_FRAME_HOP);
        hops++;
    }
    return hops;
}

} // extern "C"
