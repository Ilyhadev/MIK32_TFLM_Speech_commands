MIK32_HAL_DIR=hardware/mik32-hal
MIK32_SHARED_DIR=hardware/mik32v2-shared

BUILD_DIR=build
TFLM_MICROLITE=third_party/tflite-micro/gen/mik32_x86_64_release_with_logs_gcc/lib/libtensorflow-microlite.a

SERIAL_PORT?=/dev/ttyUSB0
SERIAL_BOUDRATE?=115200

.PHONY: clean flash monitor

$(TFLM_MICROLITE):
	. .venv/bin/activate && \
	export PATH="/mik32_utils/xpack-riscv-none-elf-gcc-14.2.0-3/bin:$$PATH" && \
	$(MAKE) -C third_party/tflite-micro -f tensorflow/lite/micro/tools/make/Makefile \
		TARGET=mik32 BUILD_TYPE=release_with_logs microlite

build_app: $(TFLM_MICROLITE) update_submodules $(BUILD_DIR)
	cmake --build $(BUILD_DIR)

update_submodules: $(MIK32_HAL_DIR)/README.md $(MIK32_SHARED_DIR)/README.md

$(MIK32_HAL_DIR)/README.md:
	git submodule update --init hardware/mik32-hal

$(MIK32_SHARED_DIR)/README.md:
	git submodule update --init hardware/mik32v2-shared

$(BUILD_DIR):
	cmake -G Ninja -B $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)

flash: build_app
	python3 $(MIK32_UPLOADER_DIR)/mik32_upload.py build/app/base_project.hex --run-openocd \
	--openocd-exec /usr/bin/openocd \
	--openocd-target $(MIK32_UPLOADER_DIR)/openocd-scripts/target/mik32.cfg \
	--openocd-interface $(MIK32_UPLOADER_DIR)/openocd-scripts/interface/ftdi/mikron-link.cfg \
	--adapter-speed 500 --mcu-type MIK32V2

monitor:
	picocom $(SERIAL_PORT) -b $(SERIAL_BOUDRATE) --omap crcrlf --echo