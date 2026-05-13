# Must match the directory produced by:
#   make -f tensorflow/lite/micro/tools/make/Makefile TARGET=mik32 BUILD_TYPE=... microlite
# Use BUILD_TYPE=release_with_logs while debugging (-14 etc.); release strips all TFLM log strings.
# Must match: gen/<subdir>/lib/libtensorflow-microlite.a after `make ... microlite`
# Example: BUILD_TYPE=release_with_logs -> mik32_x86_64_release_with_logs_gcc
set(TFLM_GEN_SUBDIR "mik32_x86_64_release_with_logs_gcc" CACHE STRING "tflite-micro gen/ subdir (see Makefile GENDIR)")
set(TFLM_GEN_ROOT "${CMAKE_SOURCE_DIR}/third_party/tflite-micro/gen/${TFLM_GEN_SUBDIR}")

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
    ${TFLM_GEN_ROOT}/lib/libtensorflow-microlite.a
)

# Optional: proves kiss_fft_fixed16::* resolve from microlite.a (see ENABLE_KISS_FFT_LINK_TEST).
option(ENABLE_KISS_FFT_LINK_TEST
    "Link kiss_fft_link_test (pulls kiss_fft from microlite; may overflow 16 KiB RAM)" OFF)
if(ENABLE_KISS_FFT_LINK_TEST)
    add_library(kiss_fft_link_test STATIC kiss_fft_link_test.cc)
    target_include_directories(kiss_fft_link_test PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party/tflite-micro
        ${CMAKE_SOURCE_DIR}/third_party/tflite-micro/tensorflow/lite/micro/tools/make/downloads/kissfft
        ${CMAKE_SOURCE_DIR}/third_party/tflite-micro/tensorflow/lite/micro/tools/make/downloads/kissfft/tools
    )
endif()

add_library(model_data STATIC model_data.cc)
target_include_directories(model_data PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_sources(model_data PRIVATE
    ${CMAKE_SOURCE_DIR}/micro_speech/models/model.cc
)

add_library(feature_extractor STATIC feature_extractor.c)
target_include_directories(feature_extractor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})