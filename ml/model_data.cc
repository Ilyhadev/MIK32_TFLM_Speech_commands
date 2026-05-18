#include "model_data.h"

// Linked from micro_speech/models/model.cc (const in flash).
#ifdef __cplusplus
extern "C" {
#endif
extern const unsigned char g_model[];
extern const unsigned int g_model_len;
#ifdef __cplusplus
}
#endif

const unsigned char* model_data_ptr(void) {
    return g_model;
}

unsigned int model_data_len(void) {
    return g_model_len;
}
