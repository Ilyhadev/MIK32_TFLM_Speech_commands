// ml/tflm_wrapper.h
#ifndef TFLM_WRAPPER_H
#define TFLM_WRAPPER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int tflm_init(void);
int tflm_last_error(void);
int tflm_invoke(void);
void *tflm_input_buffer(size_t *input_size);
int tflm_get_result(void);
void tflm_reset(void);

/** Pre-init RAM slab for PCM capture (index 0 only). */
int tflm_preinit_scratch_region(unsigned index, void **out_ptr, size_t *out_bytes);

#ifdef __cplusplus
}
#endif

#endif
