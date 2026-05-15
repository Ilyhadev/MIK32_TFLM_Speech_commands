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

int tflm_get_result(void);
void tflm_log_tensors(void);
void tflm_log_output_scores(void);

size_t tflm_input_bytes(void);

#ifdef __cplusplus
}
#endif

#endif // TFLM_WRAPPER_H
