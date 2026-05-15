# Must match the directory produced by:
#   make -f tensorflow/lite/micro/tools/make/Makefile TARGET=mik32 BUILD_TYPE=... microlite
# Use BUILD_TYPE=release_with_logs while debugging (-14 etc.); release strips all TFLM log strings.
# Must match: gen/<subdir>/lib/libtensorflow-microlite.a after `make ... microlite`
# Example: BUILD_TYPE=release_with_logs -> mik32_x86_64_release_with_logs_gcc
set(TFLM_GEN_SUBDIR "mik32_x86_64_release_with_logs_gcc" CACHE STRING "tflite-micro gen/ subdir (see Makefile GENDIR)")
set(TFLM_GEN_ROOT "${CMAKE_SOURCE_DIR}/third_party/tflite-micro/gen/${TFLM_GEN_SUBDIR}")
set(TFLM_MICROLITE_LIB "${TFLM_GEN_ROOT}/lib/libtensorflow-microlite.a")

add_library(tflm_wrapper STATIC tflm_wrapper.cc)

target_include_directories(tflm_wrapper PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/third_party/tflite-micro
    ${CMAKE_SOURCE_DIR}/third_party/tflite-micro/tensorflow/lite/micro/tools/make/downloads/flatbuffers/include
    ${CMAKE_SOURCE_DIR}/third_party/tflite-micro/tensorflow/lite/micro/tools/make/downloads/gemmlowp
    ${CMAKE_SOURCE_DIR}/third_party/tflite-micro/tensorflow/lite/micro/tools/make/downloads/ruy
    ${CMAKE_SOURCE_DIR}/third_party/tflite-micro/tensorflow/lite/micro/tools/make/downloads/kissfft
)
target_link_libraries(tflm_wrapper PUBLIC
    ${TFLM_MICROLITE_LIB}
)

add_library(model_data STATIC model_data.cc)
target_include_directories(model_data PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_sources(model_data PRIVATE
    ${CMAKE_SOURCE_DIR}/micro_speech/models/model.cc
)

add_library(feature_extractor STATIC feature_extractor.c)
target_include_directories(feature_extractor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_library(spectrogram_kiss STATIC spectrogram_kiss.cc)
target_include_directories(spectrogram_kiss PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(spectrogram_kiss PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/tflite-micro
    ${CMAKE_SOURCE_DIR}/third_party/tflite-micro/tensorflow/lite/micro/tools/make/downloads/kissfft
    ${CMAKE_SOURCE_DIR}/third_party/tflite-micro/tensorflow/lite/micro/tools/make/downloads/kissfft/tools
)
target_link_libraries(spectrogram_kiss PUBLIC ${TFLM_MICROLITE_LIB})