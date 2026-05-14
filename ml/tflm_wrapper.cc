// ml/tflm_wrapper.cc
#include "tflm_wrapper.h"
#include "model_data.h"

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include <cstring>

namespace {
    // Memory buffers – all zero‑initialized, no constructors
    // 1 byte guarantees AllocateTensors() failure. Keep arena static to avoid heap use.
    constexpr size_t kArenaSize = 6800;
    alignas(16) static uint8_t tensor_arena[kArenaSize];

    alignas(tflite::MicroMutableOpResolver<12>) static uint8_t op_resolver_buf[sizeof(tflite::MicroMutableOpResolver<12>)];

    alignas(tflite::MicroInterpreter) static uint8_t interpreter_buf[sizeof(tflite::MicroInterpreter)];

    static tflite::MicroMutableOpResolver<12>* op_resolver = nullptr;
    static tflite::MicroInterpreter* interpreter = nullptr;
    static const tflite::Model* model = nullptr;
    static bool initialized = false;
    static int last_error = 0;
}

extern "C" {

int tflm_init(void) {
    if (initialized) {
        last_error = 0;
        return 0;
    }
    last_error = 0;
    const unsigned char* model_tflite = model_data_ptr();
    const unsigned int model_tflite_len = model_data_len();

    // FlatBuffer must start with size + "TFL3" identifier at bytes [4..7].
    if (model_tflite_len < 8U) {
        last_error = -10;
        return last_error;
    }
    if (model_tflite[4] != 'T' || model_tflite[5] != 'F' ||
        model_tflite[6] != 'L' || model_tflite[7] != '3') {
        last_error = -11;
        return last_error;
    }

    // Map model
    model = tflite::GetModel(model_tflite);
    if (model == nullptr) {
        last_error = -12;
        return last_error;
    }
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        last_error = -13;
        return last_error;
    }

    op_resolver = new (op_resolver_buf) tflite::MicroMutableOpResolver<12>();
    if (op_resolver->AddConv2D() != kTfLiteOk) return (last_error = -20);
    if (op_resolver->AddDepthwiseConv2D() != kTfLiteOk) return (last_error = -21);
    if (op_resolver->AddFullyConnected() != kTfLiteOk) return (last_error = -22);
    if (op_resolver->AddSoftmax() != kTfLiteOk) return (last_error = -23);
    if (op_resolver->AddReshape() != kTfLiteOk) return (last_error = -24);
    if (op_resolver->AddQuantize() != kTfLiteOk) return (last_error = -25);
    if (op_resolver->AddAdd() != kTfLiteOk) return (last_error = -26);
    if (op_resolver->AddMul() != kTfLiteOk) return (last_error = -27);
    if (op_resolver->AddRelu() != kTfLiteOk) return (last_error = -28);

    // Placement‑new interpreter (NO function‑local static).
    // Do NOT call PrepareNodeAndRegistrationDataFromFlatbuffer() here — it is
    // invoked inside AllocateTensors() after StartModelAllocation(). Calling it
    // early dereferences null allocations and traps the CPU (reset loop).
    interpreter = new (interpreter_buf) tflite::MicroInterpreter(
        model, *op_resolver, tensor_arena, kArenaSize);

    if (interpreter->AllocateTensors() != kTfLiteOk) {
        last_error = -14;
        return last_error;
    }

    initialized = true;
    last_error = 0;
    return 0;
}

int tflm_last_error(void) {
    return last_error;
}

size_t tflm_arena_used_bytes(void) {
    if (!initialized || !interpreter) {
        return 0U;
    }
    return interpreter->arena_used_bytes();
}

size_t tflm_model_len_bytes(void) {
    return model_data_len();
}

int tflm_run(const int8_t* input, size_t input_size) {
    if (!initialized || !interpreter) return -1;
    TfLiteTensor* input_tensor = interpreter->input(0);
    if (input_tensor->bytes != input_size) return -1;
    memcpy(input_tensor->data.int8, input, input_size);
    if (interpreter->Invoke() != kTfLiteOk) return -1;
    return 0;
}

int tflm_invoke(void) {
    if (!initialized || !interpreter) return -1;
    if (interpreter->Invoke() != kTfLiteOk) return -1;
    return 0;
}

int8_t* tflm_input_buffer(size_t* input_size) {
    if (!initialized || !interpreter) {
        return nullptr;
    }
    TfLiteTensor* input_tensor = interpreter->input(0);
    if (input_tensor == nullptr || input_tensor->data.int8 == nullptr) {
        return nullptr;
    }
    if (input_size != nullptr) {
        *input_size = input_tensor->bytes;
    }
    return input_tensor->data.int8;
}

int tflm_get_result(void) {
    if (!initialized || !interpreter) return -1;
    TfLiteTensor* output = interpreter->output(0);
    int8_t max_val = output->data.int8[0];
    int max_idx = 0;
    for (int i = 1; i < 4; i++) {
        if (output->data.int8[i] > max_val) {
            max_val = output->data.int8[i];
            max_idx = i;
        }
    }
    return max_idx;
}


size_t tflm_input_bytes(void) {
    if (!initialized || !interpreter) {
        return 0;
    }

    TfLiteTensor* input = interpreter->input(0);

    if (!input) {
        return 0;
    }

    return input->bytes;
}

} // extern "C"