# Rebuild microlite (from repo root, venv + RISC-V toolchain on PATH):
#   source .venv/bin/activate
#   export PATH=/mik32_utils/xpack-riscv-none-elf-gcc-14.2.0-3/bin:$PATH
#   make -C third_party/tflite-micro -f tensorflow/lite/micro/tools/make/Makefile \
#     TARGET=mik32 BUILD_TYPE=release_with_logs microlite
# Match -DTFLM_GEN_SUBDIR to the gen/ folder name below.
set(TFLM_GEN_SUBDIR "mik32_x86_64_release_with_logs_gcc" CACHE STRING "tflite-micro gen/ subdir (see Makefile GENDIR)")
set(TFLM_GEN_ROOT "${CMAKE_SOURCE_DIR}/third_party/tflite-micro/gen/${TFLM_GEN_SUBDIR}")
set(TFLM_MICROLITE_LIB "${TFLM_GEN_ROOT}/lib/libtensorflow-microlite.a")
set(TFLM_ROOT "${CMAKE_SOURCE_DIR}/third_party/tflite-micro")

add_library(tflm_wrapper STATIC tflm_wrapper.cc)

target_include_directories(tflm_wrapper PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${TFLM_ROOT}
    ${TFLM_ROOT}/tensorflow/lite/micro/tools/make/downloads/flatbuffers/include
    ${TFLM_ROOT}/tensorflow/lite/micro/tools/make/downloads/gemmlowp
    ${TFLM_ROOT}/tensorflow/lite/micro/tools/make/downloads/ruy
    ${TFLM_ROOT}/tensorflow/lite/micro/tools/make/downloads/kissfft
)
target_link_libraries(tflm_wrapper PUBLIC ${TFLM_MICROLITE_LIB})

add_library(model_data STATIC model_data.cc)
target_include_directories(model_data PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_sources(model_data PRIVATE ${CMAKE_SOURCE_DIR}/micro_speech/models/model.cc)

set(SIGNAL_SRC
    ${TFLM_ROOT}/signal/src/window.cc
    ${TFLM_ROOT}/signal/src/fft_auto_scale.cc
    ${TFLM_ROOT}/signal/src/max_abs.cc
    ${TFLM_ROOT}/signal/src/msb_32.cc
    ${TFLM_ROOT}/signal/src/energy.cc
    ${TFLM_ROOT}/signal/src/filter_bank.cc
    ${TFLM_ROOT}/signal/src/filter_bank_square_root.cc
    ${TFLM_ROOT}/signal/src/filter_bank_spectral_subtraction.cc
    ${TFLM_ROOT}/signal/src/filter_bank_log.cc
    ${TFLM_ROOT}/signal/src/log.cc
    ${TFLM_ROOT}/signal/src/square_root_64.cc
    ${TFLM_ROOT}/signal/src/pcan_argc_fixed.cc
)

add_library(spectrogram_kiss STATIC spectrogram_kiss.cc ${SIGNAL_SRC})
target_include_directories(spectrogram_kiss PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(spectrogram_kiss PRIVATE
    ${TFLM_ROOT}
    ${TFLM_ROOT}/tensorflow/lite/micro/tools/make/downloads/kissfft
    ${TFLM_ROOT}/tensorflow/lite/micro/tools/make/downloads/kissfft/tools
)
target_link_libraries(spectrogram_kiss PUBLIC ${TFLM_MICROLITE_LIB})
