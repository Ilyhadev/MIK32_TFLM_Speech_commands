// ml/tflm_wrapper.cc
#include "tflm_wrapper.h"

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include <cstdint>
#include <cstring>

extern "C" void xprintf(const char *fmt, ...);

extern const unsigned char g_model[];
extern const unsigned int g_model_len;

namespace {
constexpr int kFeatureBytes = 1960;
constexpr int kCategoryBytes = 4;
constexpr size_t kArenaBytes = 6812;
constexpr size_t kOpStorageBytes = sizeof(tflite::MicroMutableOpResolver<4>);
constexpr size_t kInterpStorageBytes = sizeof(tflite::MicroInterpreter);
constexpr size_t kShotPoolBytes = kArenaBytes + kOpStorageBytes + kInterpStorageBytes;

alignas(16) static uint8_t shot_pool[kShotPoolBytes];
static uint8_t *const kOpStorage = shot_pool + kArenaBytes;
static uint8_t *const kInterpStorage = shot_pool + kArenaBytes + kOpStorageBytes;

static tflite::MicroMutableOpResolver<4> *op_resolver = nullptr;
static tflite::MicroInterpreter *interpreter = nullptr;
static const tflite::Model *model = nullptr;
static bool initialized = false;
static int last_error = 0;
static void *input_data = nullptr;
static void *output_data = nullptr;
static int input_bytes = 0;
static int output_bytes = 0;
} // namespace

static int cache_tensor_ptrs(void) {
    TfLiteTensor *in = interpreter->input(0);
    TfLiteTensor *out = interpreter->output(0);
    if (in == nullptr || out == nullptr || in->data.raw == nullptr || out->data.raw == nullptr) {
        return -15;
    }
    input_data = in->data.raw;
    output_data = out->data.raw;
    input_bytes = in->bytes;
    output_bytes = out->bytes;
    if (input_bytes != kFeatureBytes || output_bytes != kCategoryBytes) {
        return -15;
    }
    return 0;
}

extern "C" {

int tflm_init(void) {
    if (initialized) {
        last_error = 0;
        return 0;
    }
    std::memset(shot_pool, 0, kShotPoolBytes);
    last_error = 0;

    if (g_model_len < 8U) {
        last_error = -10;
        return last_error;
    }
    if (g_model[4] != 'T' || g_model[5] != 'F' || g_model[6] != 'L' || g_model[7] != '3') {
        last_error = -11;
        return last_error;
    }

    model = tflite::GetModel(g_model);
    if (model == nullptr) {
        last_error = -12;
        return last_error;
    }
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        last_error = -13;
        return last_error;
    }

    op_resolver = new (kOpStorage) tflite::MicroMutableOpResolver<4>();
    if (op_resolver->AddReshape() != kTfLiteOk) {
        return (last_error = -24);
    }
    if (op_resolver->AddFullyConnected() != kTfLiteOk) {
        return (last_error = -22);
    }
    if (op_resolver->AddDepthwiseConv2D() != kTfLiteOk) {
        return (last_error = -21);
    }
    if (op_resolver->AddSoftmax() != kTfLiteOk) {
        return (last_error = -23);
    }

    interpreter = new (kInterpStorage) tflite::MicroInterpreter(model, *op_resolver, shot_pool, kArenaBytes);

    const TfLiteStatus alloc_st = interpreter->AllocateTensors();
    if (alloc_st != kTfLiteOk) {
        last_error = -14;
        xprintf("[TFLM] alloc fail %d used=%u\r\n", (int)alloc_st,
                (unsigned)interpreter->arena_used_bytes());
        return last_error;
    }

    if (cache_tensor_ptrs() != 0) {
        last_error = -15;
        return last_error;
    }

    initialized = true;
    return 0;
}

int tflm_last_error(void) {
    return last_error;
}

int tflm_preinit_scratch_region(unsigned index, void **out_ptr, size_t *out_bytes) {
    if (out_ptr == nullptr || out_bytes == nullptr || index != 0U) {
        return -1;
    }
    *out_ptr = shot_pool;
    *out_bytes = kShotPoolBytes;
    return 0;
}

void tflm_reset(void) {
    if (interpreter != nullptr) {
        interpreter->~MicroInterpreter();
        interpreter = nullptr;
    }
    if (op_resolver != nullptr) {
        op_resolver->~MicroMutableOpResolver<4>();
        op_resolver = nullptr;
    }
    std::memset(kOpStorage, 0, kOpStorageBytes);
    std::memset(kInterpStorage, 0, kInterpStorageBytes);
    initialized = false;
    input_data = nullptr;
    output_data = nullptr;
    input_bytes = 0;
    output_bytes = 0;
    last_error = 0;
}

int tflm_invoke(void) {
    if (!initialized || interpreter == nullptr) {
        last_error = -1;
        return -1;
    }
    const TfLiteStatus st = interpreter->Invoke();
    if (st != kTfLiteOk) {
        last_error = -30 - static_cast<int>(st);
        return -1;
    }
    last_error = 0;
    return 0;
}

void *tflm_input_buffer(size_t *input_size) {
    if (input_size != nullptr) {
        *input_size = 0U;
    }
    if (!initialized || input_data == nullptr || input_bytes <= 0) {
        return nullptr;
    }
    if (input_size != nullptr) {
        *input_size = static_cast<size_t>(input_bytes);
    }
    return input_data;
}

int tflm_get_result(void) {
    if (!initialized || output_data == nullptr || output_bytes != kCategoryBytes) {
        return -1;
    }
    const int8_t *scores = static_cast<const int8_t *>(output_data);
    int8_t max_val = scores[0];
    for (int i = 1; i < 4; i++) {
        if (scores[i] > max_val) {
            max_val = scores[i];
        }
    }
    int tied[4];
    int ntied = 0;
    for (int i = 0; i < 4; i++) {
        if (scores[i] == max_val) {
            tied[ntied++] = i;
        }
    }
    if (ntied == 1) {
        return tied[0];
    }
    auto has = [&](int idx) {
        for (int j = 0; j < ntied; j++) {
            if (tied[j] == idx) {
                return true;
            }
        }
        return false;
    };
    if (has(2) && has(3)) {
        return -1;
    }
    if (has(1) && has(3)) {
        return 3;
    }
    if (has(1) && has(2)) {
        return 1;
    }
    return tied[0];
}

} // extern "C"
