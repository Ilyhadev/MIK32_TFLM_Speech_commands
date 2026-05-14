# Setting up app build
set(APP_NAME ${CMAKE_PROJECT_NAME})

option(BASE_PROJECT_WITH_TFLM
    "Link TensorFlow Lite (tflm_wrapper, model_data, feature_extractor). Needed for kiss_fft link smoke test."
    OFF)

add_executable(${APP_NAME}
    main.c
    debug_io.cc
    mik32_tflm_log.c
    circular_buffer.c
    startup.c
)

target_link_libraries(${APP_NAME}
    PRIVATE
        mik32_hal
)

if(BASE_PROJECT_WITH_TFLM)
    target_link_libraries(${APP_NAME} PRIVATE
        tflm_wrapper
        model_data
        feature_extractor
    )
endif()

if(ENABLE_KISS_FFT_LINK_TEST)
    if(BASE_PROJECT_WITH_TFLM)
        target_link_libraries(${APP_NAME} PRIVATE kiss_fft_link_test)
        target_link_options(${APP_NAME} PRIVATE "LINKER:-u,kiss_fft_fixed16_link_smoke")
    else()
        message(STATUS "ENABLE_KISS_FFT_LINK_TEST=ON ignored (set BASE_PROJECT_WITH_TFLM=ON to link microlite symbols for kiss smoke).")
    endif()
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