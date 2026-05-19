#ifndef ADC_RAM_CAPTURE_H
#define ADC_RAM_CAPTURE_H

#include "mik32_hal_def.h"
#include "spectrogram_kiss.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ADC_RAM_CAPTURE_BLOCK_BYTES (SK_FRAME_HOP * 2U * sizeof(uint16_t))

void adc_ram_capture_reset(void);
void adc_ram_capture_bind_fft(SpectrogramKissState *sk);
size_t adc_ram_capture_pool_bytes(void);
HAL_StatusTypeDef adc_ram_capture_append_block(const uint16_t *samples);
unsigned adc_ram_capture_block_count(void);
unsigned adc_ram_capture_max_blocks(void);
const uint16_t *adc_ram_capture_block_samples(unsigned block_index);
bool adc_ram_capture_block_in_fft_mem(const SpectrogramKissState *sk, unsigned block_index);
unsigned adc_ram_capture_ms(unsigned blocks);

#endif
