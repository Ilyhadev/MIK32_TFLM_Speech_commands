// ml/tflm_wrapper.cc
#include "tflm_wrapper.h"
#include "model_data.h"

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_log.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include <cstdint>
#include <cstring>

extern "C" void xprintf(const char *fmt, ...);

#ifndef MICRO_SPEECH_PROBE_INVOKE_IN_INIT
#define MICRO_SPEECH_PROBE_INVOKE_IN_INIT 0
#endif

namespace {
    /* micro_speech_quantized: 49 x 40 int8 features, 4 int8 class scores */
    constexpr int kFeatureBytes = 1960;
    constexpr int kCategoryBytes = 4;

    /* 6716 B after AllocateTensors(); Invoke needs extra temp space in the same arena. */
    constexpr size_t kArenaSize = 7936;
    alignas(16) static uint8_t tensor_arena[kArenaSize];

    alignas(tflite::MicroMutableOpResolver<4>) static uint8_t op_resolver_buf[sizeof(tflite::MicroMutableOpResolver<4>)];

    alignas(tflite::MicroInterpreter) static uint8_t interpreter_buf[sizeof(tflite::MicroInterpreter)];

    static tflite::MicroMutableOpResolver<4>* op_resolver = nullptr;
    static tflite::MicroInterpreter* interpreter = nullptr;
    static const tflite::Model* model = nullptr;
    static bool initialized = false;
    static int last_error = 0;

    static void* input_data = nullptr;
    static void* output_data = nullptr;
    static int input_bytes = 0;
    static int output_bytes = 0;
    static TfLiteType input_type = kTfLiteNoType;
    static TfLiteType output_type = kTfLiteNoType;
    static size_t input_element_size = 0;
    static size_t output_element_size = 0;
    static size_t arena_used_cached = 0;
    static size_t arena_free_cached = 0;
}

static size_t tensor_element_size_from_dims(const TfLiteTensor* t) {
    if (t == nullptr || t->dims == nullptr || t->dims->size <= 0 || t->bytes == 0U) {
        return 0U;
    }
    int count = 1;
    for (int i = 0; i < t->dims->size; i++) {
        count *= t->dims->data[i];
    }
    if (count <= 0) {
        return 0U;
    }
    return t->bytes / static_cast<size_t>(count);
}

/** bytes/dims can be wrong across header vs libtensorflow-microlite.a; use known sizes. */
static size_t tensor_element_size(const TfLiteTensor* t, int expected_bytes) {
    const size_t from_dims = tensor_element_size_from_dims(t);
    if (from_dims != 0U) {
        return from_dims;
    }
    if (t != nullptr && static_cast<int>(t->bytes) == expected_bytes) {
        return 1U;
    }
    return 0U;
}

static bool input_buffer_valid(void) {
    return initialized && input_data != nullptr && input_bytes > 0 &&
           (input_element_size == 1U || input_bytes == kFeatureBytes);
}

static const char* tflm_type_name(TfLiteType t) {
    const char* name = TfLiteTypeGetName(t);
    return (name != nullptr) ? name : "?";
}

static int cache_tensor_ptrs(void) {
    TfLiteTensor* in = interpreter->input(0);
    TfLiteTensor* out = interpreter->output(0);
    if (in == nullptr || out == nullptr || in->data.raw == nullptr || out->data.raw == nullptr) {
        return -15;
    }

    input_data = in->data.raw;
    output_data = out->data.raw;
    input_bytes = in->bytes;
    output_bytes = out->bytes;
    input_type = in->type;
    output_type = out->type;
    input_element_size = tensor_element_size(in, kFeatureBytes);
    output_element_size = tensor_element_size(out, kCategoryBytes);
    if (input_element_size == 0U && input_bytes == kFeatureBytes) {
        input_element_size = 1U;
    }
    if (output_element_size == 0U && output_bytes == kCategoryBytes) {
        output_element_size = 1U;
    }

    arena_used_cached = interpreter->arena_used_bytes();
    arena_free_cached = (arena_used_cached < kArenaSize) ? (kArenaSize - arena_used_cached) : 0U;

    xprintf("[TFLM] cached in bytes=%d type=%s(%d) elt=%u data=0x%08x\r\n", input_bytes,
            tflm_type_name(input_type), (int)input_type, (unsigned)input_element_size,
            (unsigned)(uintptr_t)input_data);
    xprintf("[TFLM] cached out bytes=%d type=%s(%d) elt=%u data=0x%08x\r\n", output_bytes,
            tflm_type_name(output_type), (int)output_type, (unsigned)output_element_size,
            (unsigned)(uintptr_t)output_data);
    return 0;
}

#if MICRO_SPEECH_PROBE_INVOKE_IN_INIT
static int probe_invoke_at_init_depth(const char *tag) {
    xprintf("[TFLM] %s: probe Invoke (init stack depth)...\r\n", tag);
    const TfLiteStatus st = interpreter->Invoke();
    if (st != kTfLiteOk) {
        xprintf("[TFLM] %s: probe Invoke FAIL st=%d\r\n", tag, (int)st);
        return -31;
    }
    xprintf("[TFLM] %s: probe Invoke OK", tag);
    if (output_data != nullptr && output_bytes == kCategoryBytes) {
        xprintf(" scores");
        const int8_t* scores = static_cast<const int8_t*>(output_data);
        for (int i = 0; i < kCategoryBytes; i++) {
            xprintf(" %d", (int)scores[i]);
        }
    }
    xprintf("\r\n");
    return 0;
}
#endif

extern "C" {

int tflm_init(void) {
    if (initialized) {
        last_error = 0;
        return 0;
    }
    last_error = 0;
    const unsigned char* model_tflite = model_data_ptr();
    const unsigned int model_tflite_len = model_data_len();

    if (model_tflite_len < 8U) {
        last_error = -10;
        return last_error;
    }
    if (model_tflite[4] != 'T' || model_tflite[5] != 'F' ||
        model_tflite[6] != 'L' || model_tflite[7] != '3') {
        last_error = -11;
        return last_error;
    }

    model = tflite::GetModel(model_tflite);
    if (model == nullptr) {
        last_error = -12;
        return last_error;
    }
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        last_error = -13;
        return last_error;
    }

    op_resolver = new (op_resolver_buf) tflite::MicroMutableOpResolver<4>();
    if (op_resolver->AddReshape() != kTfLiteOk) return (last_error = -24);
    if (op_resolver->AddFullyConnected() != kTfLiteOk) return (last_error = -22);
    if (op_resolver->AddDepthwiseConv2D() != kTfLiteOk) return (last_error = -21);
    if (op_resolver->AddSoftmax() != kTfLiteOk) return (last_error = -23);

    interpreter = new (interpreter_buf) tflite::MicroInterpreter(
        model, *op_resolver, tensor_arena, kArenaSize);

    const TfLiteStatus alloc_st = interpreter->AllocateTensors();
    if (alloc_st != kTfLiteOk) {
        last_error = -14;
        xprintf("[TFLM] AllocateTensors failed status=%d arena_used=%u/%u\r\n", (int)alloc_st,
                (unsigned)interpreter->arena_used_bytes(), (unsigned)kArenaSize);
        return last_error;
    }

    if (cache_tensor_ptrs() != 0) {
        last_error = -15;
        return last_error;
    }

#if MICRO_SPEECH_PROBE_INVOKE_IN_INIT
    if (probe_invoke_at_init_depth("init") != 0) {
        last_error = -31;
        return last_error;
    }
#endif

    initialized = true;
    last_error = 0;
    xprintf("[TFLM] init OK arena_used=%u free=%u\r\n", (unsigned)arena_used_cached,
            (unsigned)arena_free_cached);
    return 0;
}

int tflm_last_error(void) {
    return last_error;
}

size_t tflm_arena_size_bytes(void) {
    return kArenaSize;
}

size_t tflm_arena_used_bytes(void) {
    return arena_used_cached;
}

size_t tflm_arena_free_bytes(void) {
    return arena_free_cached;
}

size_t tflm_model_len_bytes(void) {
    return model_data_len();
}

void tflm_log_tensors(void) {
    xprintf("[TFLM] log_tensors enter\r\n");
    if (!initialized) {
        xprintf("[TFLM] log_tensors: not init\r\n");
        return;
    }
    xprintf("[TFLM] in bytes=%d type=%s(%d) data=0x%08x\r\n", input_bytes,
            tflm_type_name(input_type), (int)input_type, (unsigned)(uintptr_t)input_data);
    xprintf("[TFLM] out bytes=%d type=%s(%d) data=0x%08x\r\n", output_bytes,
            tflm_type_name(output_type), (int)output_type, (unsigned)(uintptr_t)output_data);
    if (input_buffer_valid() && input_bytes >= 4) {
        const int8_t* in = static_cast<const int8_t*>(input_data);
        xprintf("[TFLM] in[0..3]=%d %d %d %d\r\n", (int)in[0], (int)in[1], (int)in[2], (int)in[3]);
    }
}

void tflm_log_output_scores(void) {
    if (!initialized || output_data == nullptr) {
        xprintf("[TFLM] output: not init\r\n");
        return;
    }

    if (output_data == nullptr || output_bytes != kCategoryBytes) {
        xprintf("[TFLM] output: bad bytes=%d (want %d)\r\n", output_bytes, kCategoryBytes);
        return;
    }
    const int8_t* scores = static_cast<const int8_t*>(output_data);
    xprintf("[TFLM] scores:");
    const int n = (output_bytes < 8) ? output_bytes : 8;
    for (int i = 0; i < n; i++) {
        xprintf(" %d", (int)scores[i]);
    }
    xprintf(" -> cls=%d\r\n", tflm_get_result());
}

int tflm_run(const int8_t* input, size_t input_size) {
    if (!initialized || !interpreter) return -1;
    if (input_bytes != (int)input_size || input_data == nullptr || input_bytes != kFeatureBytes) {
        return -1;
    }
    memcpy(input_data, input, input_size);
    if (interpreter->Invoke() != kTfLiteOk) return -1;
    return 0;
}

static uintptr_t read_sp(void) {
    uintptr_t sp = 0;
#if defined(__riscv)
    __asm__ volatile("mv %0, sp" : "=r"(sp));
#endif
    return sp;
}

int tflm_invoke(void) {
    if (!initialized || !interpreter) {
        last_error = -1;
        return -1;
    }
    xprintf("[TFLM] Invoke enter sp=0x%08x\r\n", (unsigned)read_sp());
    const TfLiteStatus st = interpreter->Invoke();
    xprintf("[TFLM] Invoke done st=%d sp=0x%08x\r\n", (int)st, (unsigned)read_sp());
    if (st != kTfLiteOk) {
        last_error = -30 - (int)st;
        return -1;
    }
    last_error = 0;
    return 0;
}

void* tflm_input_buffer(size_t* input_size) {
    if (input_size != nullptr) {
        *input_size = 0U;
    }
    if (!initialized) {
        return nullptr;
    }
    if (input_data == nullptr || input_bytes <= 0) {
        return nullptr;
    }
    if (input_size != nullptr) {
        *input_size = (size_t)input_bytes;
    }
    return input_data;
}

size_t tflm_input_element_size(void) {
    return input_element_size;
}

int tflm_get_result(void) {
    if (!initialized || output_data == nullptr || output_bytes != kCategoryBytes) {
        return -1;
    }
    const int8_t* scores = static_cast<const int8_t*>(output_data);
    int8_t max_val = scores[0];
    int max_idx = 0;
    const int n = (output_bytes < 4) ? output_bytes : 4;
    for (int i = 1; i < n; i++) {
        if (scores[i] > max_val) {
            max_val = scores[i];
            max_idx = i;
        }
    }
    return max_idx;
}

size_t tflm_input_bytes(void) {
    if (!initialized) {
        return 0;
    }
    return (input_bytes > 0) ? (size_t)input_bytes : 0U;
}

} // extern "C"
