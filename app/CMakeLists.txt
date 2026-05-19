# Setting up app build
set(APP_NAME ${CMAKE_PROJECT_NAME})

add_executable(${APP_NAME}
    main.c
    adc_ram_capture.c
    debug_io.cc
    mik32_tflm_log.c
    startup.c
)

target_link_libraries(${APP_NAME} PRIVATE
    mik32_hal
    tflm_wrapper
    spectrogram_kiss
)

target_include_directories(${APP_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/hardware/mik32-hal/utilities/Include
    ${CMAKE_SOURCE_DIR}/hardware/mik32-hal/peripherals/Include
)

target_link_directories(${APP_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/hardware/mik32v2-shared/ldscripts
)

target_link_options(${APP_NAME} PRIVATE
    "LINKER:--defsym=__stack_size=896"
)

add_custom_command(TARGET ${APP_NAME}
    POST_BUILD
    COMMAND ${TOOLCHAIN_PREFIX}-objcopy -O binary $<TARGET_FILE:${APP_NAME}> ${APP_NAME}.bin
    COMMAND ${TOOLCHAIN_PREFIX}-objcopy -O ihex $<TARGET_FILE:${APP_NAME}> ${APP_NAME}.hex
    COMMAND ${TOOLCHAIN_PREFIX}-objdump -w -x -s -S $<TARGET_FILE:${APP_NAME}> > ${APP_NAME}.dump
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
