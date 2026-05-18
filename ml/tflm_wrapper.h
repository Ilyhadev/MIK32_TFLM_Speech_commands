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
size_t tflm_arena_used_bytes(void);
size_t tflm_arena_size_bytes(void);
size_t tflm_arena_free_bytes(void);
size_t tflm_model_len_bytes(void);

int tflm_run(const int8_t* input, size_t input_size);
int tflm_invoke(void);
/** Model input tensor (int8 features); NULL if not initialized. */
void* tflm_input_buffer(size_t* input_size);
/** Bytes per input element (1 for int8/uint8 feature maps). */
size_t tflm_input_element_size(void);

/** Class index 0..3, or -1 if top scores tie (ambiguous). */
int tflm_get_result(void);
void tflm_log_tensors(void);
void tflm_log_output_scores(void);

size_t tflm_input_bytes(void);

/** Scratch RAM (tensor arena) for use before tflm_init(); do not use after init. */
void *tflm_arena_scratch(size_t *out_bytes);

/** RAM only used before first tflm_init() (arena + interpreter + op resolver). */
#define TFLM_PREINIT_SCRATCH_REGIONS 3U
/** Returns 0 on success. */
int tflm_preinit_scratch_region(unsigned index, void **out_ptr, size_t *out_bytes);

/** Total bytes available for DMA capture before tflm_init() (shot pool slab). */
size_t tflm_preinit_pool_bytes(void);

/** Same as tflm_preinit_pool_bytes(); PCM may use the full slab including future arena tail. */
size_t tflm_shot_pool_pcm_bytes(void);

/** Release interpreter so RECORD can reuse pre-init scratch again. */
void tflm_reset(void);

#ifdef __cplusplus
}
#endif

#endif // TFLM_WRAPPER_H
