# Setting up app build
set(APP_NAME ${CMAKE_PROJECT_NAME})

add_executable(${APP_NAME}
    main.c
    debug_io.cc
    mik32_tflm_log.c
    circular_buffer.c
    queue.c
)

target_link_libraries(${APP_NAME}
    PRIVATE
        mik32_hal
        tflm_wrapper
        model_data
        feature_extractor
)

if(ENABLE_KISS_FFT_LINK_TEST)
    target_link_libraries(${APP_NAME} PRIVATE kiss_fft_link_test)
    target_link_options(${APP_NAME} PRIVATE "LINKER:-u,kiss_fft_fixed16_link_smoke")
endif()

target_link_directories(${APP_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/hardware/mik32v2-shared/ldscripts
)

# Convert output to hex and binary
add_custom_command(TARGET ${APP_NAME}
    POST_BUILD
    COMMAND ${TOOLCHAIN_PREFIX}-objcopy -O binary $<TARGET_FILE:${APP_NAME}> ${APP_NAME}.bin
    COMMAND ${TOOLCHAIN_PREFIX}-objcopy -O ihex $<TARGET_FILE:${APP_NAME}> ${APP_NAME}.hex
    COMMAND ${TOOLCHAIN_PREFIX}-objdump -w -x -s -S $<TARGET_FILE:${APP_NAME}> > ${APP_NAME}.dump
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)