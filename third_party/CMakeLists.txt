# third_party/CMakeLists.txt
add_library(tflite-micro STATIC IMPORTED)
set_target_properties(tflite-micro PROPERTIES
    IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/tflite-micro/gen/mik32_x86_64_release_with_logs_gcc/lib/libtensorflow-microlite.a
)
target_include_directories(tflite-micro INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/tflite-micro
    ${CMAKE_CURRENT_SOURCE_DIR}/tflite-micro/tensorflow/lite/micro/tools/make/downloads/flatbuffers/include
    ${CMAKE_CURRENT_SOURCE_DIR}/tflite-micro/tensorflow/lite/micro/tools/make/downloads/gemmlowp
    ${CMAKE_CURRENT_SOURCE_DIR}/tflite-micro/tensorflow/lite/micro/tools/make/downloads/ruy
    ${CMAKE_CURRENT_SOURCE_DIR}/tflite-micro/tensorflow/lite/micro/tools/make/downloads/kissfft
)