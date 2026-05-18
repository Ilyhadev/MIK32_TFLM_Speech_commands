#include "adc_ram_capture.h"

#include "spectrogram_kiss.h"
#include "tflm_wrapper.h"

#include <string.h>

#define CAP_MAX_REGIONS 3U

typedef struct {
    uint16_t *ptr;
    size_t halfwords;
    size_t used_hw;
} CapRegion;

static CapRegion s_regions[CAP_MAX_REGIONS];
static unsigned s_n_regions;
static unsigned s_blocks;

static void region_add(void *base, size_t bytes) {
    if (base == NULL || bytes < ADC_RAM_CAPTURE_BLOCK_BYTES || s_n_regions >= CAP_MAX_REGIONS) {
        return;
    }
    s_regions[s_n_regions].ptr = (uint16_t *)base;
    s_regions[s_n_regions].halfwords = bytes / sizeof(uint16_t);
    s_regions[s_n_regions].used_hw = 0U;
    s_n_regions++;
}

void adc_ram_capture_bind_fft(SpectrogramKissState *sk) {
    if (sk == NULL || spectrogram_kiss_fft_ready(sk)) {
        return;
    }
    region_add(sk->fft_mem, ADC_RAM_CAPTURE_BLOCK_BYTES);
}

void adc_ram_capture_reset(void) {
    s_n_regions = 0U;
    s_blocks = 0U;

    void *base = NULL;
    size_t bytes = 0U;
    if (tflm_preinit_scratch_region(0U, &base, &bytes) == 0) {
        region_add(base, bytes);
    }
}

size_t adc_ram_capture_pool_bytes(void) {
    size_t total = 0U;
    for (unsigned r = 0U; r < s_n_regions; r++) {
        total += s_regions[r].halfwords * sizeof(uint16_t);
    }
    return total;
}

static HAL_StatusTypeDef region_write(const uint16_t *samples) {
    const size_t block_hw = ADC_RAM_CAPTURE_BLOCK_BYTES / sizeof(uint16_t);

    for (unsigned r = 0U; r < s_n_regions; r++) {
        CapRegion *reg = &s_regions[r];
        if (reg->used_hw + block_hw > reg->halfwords) {
            continue;
        }
        memcpy(&reg->ptr[reg->used_hw], samples, ADC_RAM_CAPTURE_BLOCK_BYTES);
        reg->used_hw += block_hw;
        return HAL_OK;
    }
    return HAL_ERROR;
}

HAL_StatusTypeDef adc_ram_capture_append_block(const uint16_t *samples) {
    if (samples == NULL || s_n_regions == 0U) {
        return HAL_ERROR;
    }
    if (region_write(samples) != HAL_OK) {
        return HAL_ERROR;
    }
    s_blocks++;
    return HAL_OK;
}

unsigned adc_ram_capture_block_count(void) {
    return s_blocks;
}

unsigned adc_ram_capture_max_blocks(void) {
    if (s_n_regions == 0U) {
        adc_ram_capture_reset();
    }
    return (unsigned)(adc_ram_capture_pool_bytes() / ADC_RAM_CAPTURE_BLOCK_BYTES);
}

const uint16_t *adc_ram_capture_block_samples(unsigned block_index) {
    const size_t block_hw = ADC_RAM_CAPTURE_BLOCK_BYTES / sizeof(uint16_t);
    unsigned cur = 0U;

    for (unsigned r = 0U; r < s_n_regions; r++) {
        const CapRegion *reg = &s_regions[r];
        const unsigned n = (unsigned)(reg->used_hw / block_hw);
        if (block_index < cur + n) {
            return &reg->ptr[(block_index - cur) * block_hw];
        }
        cur += n;
    }
    return NULL;
}

bool adc_ram_capture_block_in_fft_mem(const SpectrogramKissState *sk, unsigned block_index) {
    const uint16_t *p = adc_ram_capture_block_samples(block_index);
    if (p == NULL || sk == NULL) {
        return false;
    }
    const uint8_t *lo = sk->fft_mem;
    const uint8_t *hi = lo + SK_FFT_MEM_MAX;
    const uint8_t *b = (const uint8_t *)p;
    return (b >= lo) && (b < hi);
}

unsigned adc_ram_capture_ms(unsigned blocks) {
    const unsigned hw_per_blk = (unsigned)(ADC_RAM_CAPTURE_BLOCK_BYTES / sizeof(uint16_t));
    return (blocks * hw_per_blk * 1000U) / 16000U;
}
