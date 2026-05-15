# Setting up app build
set(APP_NAME ${CMAKE_PROJECT_NAME})

add_executable(${APP_NAME}
    trap_entry.S
    main.c
    debug_io.cc
    mik32_tflm_log.c
    startup.c
)

set(_app_libs mik32_hal tflm_wrapper model_data)
if(NOT MICRO_SPEECH_ML_SMOKE)
    list(APPEND _app_libs spectrogram_kiss)
endif()

target_link_libraries(${APP_NAME} PRIVATE ${_app_libs})

target_compile_definitions(${APP_NAME} PRIVATE
    MICRO_SPEECH_ML_SMOKE=$<IF:$<BOOL:${MICRO_SPEECH_ML_SMOKE}>,1,0>
)

target_link_directories(${APP_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/hardware/mik32v2-shared/ldscripts
)

# Invoke() uses deep C stack; 512 B caused AllocateTensors corruption in full builds.
if(MICRO_SPEECH_ML_SMOKE)
    # Invoke() + xprintf need headroom; 3072 B hung after probe OK at init depth.
    set(_stack_size 5120)
else()
    set(_stack_size 896)
endif()

target_link_options(${APP_NAME} PRIVATE
    "LINKER:--defsym=__stack_size=${_stack_size}"
)

# Convert output to hex and binary
add_custom_command(TARGET ${APP_NAME}
    POST_BUILD
    COMMAND ${TOOLCHAIN_PREFIX}-objcopy -O binary $<TARGET_FILE:${APP_NAME}> ${APP_NAME}.bin
    COMMAND ${TOOLCHAIN_PREFIX}-objcopy -O ihex $<TARGET_FILE:${APP_NAME}> ${APP_NAME}.hex
    COMMAND ${TOOLCHAIN_PREFIX}-objdump -w -x -s -S $<TARGET_FILE:${APP_NAME}> > ${APP_NAME}.dump
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
