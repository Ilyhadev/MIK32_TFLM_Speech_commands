// Real 512-pt RFFT (kiss_fft_fixed16 from libtensorflow-microlite.a) + rectangular window + linear bands → int8[40].

#include "spectrogram_kiss.h"

#include "signal/src/kiss_fft_wrappers/kiss_fft_int16.h"

#include <cstring>

namespace {

using kiss_fft_fixed16::kiss_fft_cpx;
using kiss_fft_fixed16::kiss_fftr_alloc;
using kiss_fft_fixed16::kiss_fftr_cfg;
using kiss_fft_fixed16::kiss_fftr;

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

static int8_t compress_energy_to_int8(uint32_t energy_acc) {
    uint8_t bits = 0;
    while (energy_acc != 0U) {
        energy_acc >>= 1U;
        bits++;
    }
    int16_t q = static_cast<int16_t>(bits) * 8 - 128;
    if (q > 127) {
        q = 127;
    }
    if (q < -128) {
        q = -128;
    }
    return static_cast<int8_t>(q);
}

static int16_t adc12_to_pcm16(const SpectrogramKissState *st, uint16_t adc12) {
    int32_t centered = static_cast<int32_t>(adc12) - static_cast<int32_t>(st->adc_midpoint);
    int32_t scaled = (centered * static_cast<int32_t>(st->mic_gain_q8)) >> 8;
    return saturate_i32_to_i16(scaled);
}

static void compute_fft_slice(SpectrogramKissState *st, int8_t *out40) {
    kiss_fftr_cfg cfg = static_cast<kiss_fftr_cfg>(st->fftr_cfg);
    auto *const line = reinterpret_cast<kiss_fft_cpx *>(st->fft_line);
    if (cfg == nullptr) {
        for (uint16_t b = 0; b < SK_BINS; b++) {
            out40[b] = 0;
        }
        return;
    }

    const uint16_t frame_start = st->write_index;
    std::memset(st->fft_time, 0, sizeof(st->fft_time));
    for (uint16_t i = 0; i < SK_FRAME_LEN; i++) {
        st->fft_time[i] = st->frame_buffer[ring_index(frame_start, i)];
    }

    kiss_fftr(cfg, st->fft_time, line);

    for (uint16_t b = 0; b < SK_BINS; b++) {
        const int lo = 1 + static_cast<int>((static_cast<int32_t>(b) * 254) / static_cast<int32_t>(SK_BINS));
        const int hi = 1 + static_cast<int>((static_cast<int32_t>(b + 1U) * 254) / static_cast<int32_t>(SK_BINS));
        uint32_t acc = 0;
        for (int k = lo; k < hi && k <= 255; k++) {
            int32_t re = static_cast<int32_t>(line[k].r);
            int32_t im = static_cast<int32_t>(line[k].i);
            uint32_t m2 = static_cast<uint32_t>((re * re + im * im) >> 8);
            acc += m2;
        }
        out40[b] = compress_energy_to_int8(acc);
    }
}

} // namespace

extern "C" {

void spectrogram_kiss_init(SpectrogramKissState *st) {
    std::memset(st, 0, sizeof(*st));
    st->adc_midpoint = 2048U;
    st->mic_gain_q8 = 1024;
    st->fft_mem_bytes = 0U;

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

void spectrogram_kiss_push_adc(SpectrogramKissState *st, uint16_t adc12_sample, int8_t *feature_window) {
    if (st == nullptr || st->fftr_cfg == nullptr) {
        return;
    }

    int16_t pcm16 = adc12_to_pcm16(st, adc12_sample);
    st->dc_estimate = static_cast<int16_t>((15 * static_cast<int32_t>(st->dc_estimate) + static_cast<int32_t>(pcm16)) / 16);
    int16_t highpass = static_cast<int16_t>(static_cast<int32_t>(pcm16) - static_cast<int32_t>(st->dc_estimate));

    st->frame_buffer[st->write_index] = highpass;
    st->write_index = static_cast<uint16_t>((st->write_index + 1U) % SK_FRAME_LEN);
    st->samples_since_last_frame++;

    if (st->samples_since_last_frame < SK_FRAME_HOP) {
        return;
    }
    st->samples_since_last_frame = 0U;

    if (st->slice_count < SK_SLICES) {
        if (feature_window != nullptr) {
            compute_fft_slice(st, &feature_window[st->slice_count * SK_BINS]);
        }
        st->slice_count++;
        st->frame_seq++;
        return;
    }

    if (feature_window != nullptr) {
        std::memmove(feature_window, &feature_window[SK_BINS], SK_FEATURE_COUNT - SK_BINS);
        compute_fft_slice(st, &feature_window[SK_FEATURE_COUNT - SK_BINS]);
    }
    st->frame_seq++;
}

bool spectrogram_kiss_ready(const SpectrogramKissState *st) {
    return st != nullptr && st->fftr_cfg != nullptr && st->slice_count >= SK_SLICES;
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

} // extern "C"
