// ml/tflm_wrapper.h
#ifndef TFLM_WRAPPER_H
#define TFLM_WRAPPER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize model and interpreter. Returns 0 on success.
int tflm_init(void);
int tflm_last_error(void);
size_t tflm_arena_used_bytes(void);
size_t tflm_model_len_bytes(void);

// Run inference on a preprocessed input (1960 bytes for 49x40 spectrogram)
int tflm_run(const int8_t* input, size_t input_size);
int tflm_invoke(void);
int8_t* tflm_input_buffer(size_t* input_size);

// Get the class index (0 = silence, 1 = unknown, 2 = yes, 3 = no)
int tflm_get_result(void);

size_t tflm_input_bytes(void);

#ifdef __cplusplus
}
#endif

#endif // TFLM_WRAPPER_H

