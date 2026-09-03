# Device profile: ESP32-S3 - DELIBERATELY PARTIAL.
#
# No such board has been measured here. This file exists to prove the
# profile abstraction holds for a second chip, and to be the place a real
# S3's first capture lands. Every field that can only come from a capture
# is the literal string "unmeasured"; the loader refuses to hand an
# unmeasured value to a gate, so nothing can quietly run against a made-up
# number. Fields sourced from ESP-IDF and the S3 TRM are real.
#
# Adding a device is this file plus that first capture's numbers - nothing
# else in the tree should need to change.

DP_NAME=esp32s3
DP_DESC="ESP32-S3 (Xtensa LX7) - profile skeleton, no board measured"
DP_STATUS=partial

# --- main task stack -------------------------------------------------------
# Not a capture value: it is whatever the project's sdkconfig says, and
# ESP-IDF's default is the same 3584 on every target.
DP_MAIN_TASK_STACK_BYTES=3584
DP_MAIN_TASK_STACK_SOURCE="ESP-IDF v5.5 components/esp_system/Kconfig, ESP_MAIN_TASK_STACK_SIZE default 3584 (target-independent)"

DP_TEST_FRAME_CEILING_BYTES=1024
DP_TEST_FRAME_CEILING_SOURCE="same derivation as esp32c6, same stack size"

# --- heap ------------------------------------------------------------------
# Capture-derived: depends on this board's framebuffer, PSRAM and app.
DP_FREE_HEAP_BYTES=unmeasured
DP_FREE_HEAP_SOURCE="unmeasured - needs one device capture's 'free heap after framebuffer'"
DP_LARGEST_ALLOC_BYTES=unmeasured
DP_LARGEST_ALLOC_SOURCE="unmeasured"

# --- toolchain and codegen -------------------------------------------------
DP_TOOLCHAIN_PREFIX=xtensa-esp32s3-elf
DP_TOOLCHAIN_SOURCE="ESP-IDF v5.5 tools/tools.json, xtensa-esp-elf"

DP_ARCH_FLAGS="-mlongcalls"
DP_ARCH_FLAGS_SOURCE="ESP-IDF Xtensa builds; not verified against a real S3 build here"

# -fno-jump-tables and -fno-tree-switch-conversion are appended in IDF's
# top-level CMakeLists.txt for every target, so they are real for the S3
# too. -O2 assumes the same CONFIG_COMPILER_OPTIMIZATION as this project's;
# re-read compile_commands.json from a real S3 build before trusting it.
DP_CODEGEN_FLAGS="-O2 -fno-jump-tables -fno-tree-switch-conversion -fstrict-volatile-bitfields -ffunction-sections -fdata-sections"
DP_STD_FLAG="-std=gnu17"
DP_CODEGEN_SOURCE="ESP-IDF v5.5 CMakeLists.txt line 273 (target-independent flags); the rest assumed from this project's C6 build, not verified"

# --- instruction cache -----------------------------------------------------
# The S3's icache is Kconfig-selectable (CONFIG_ESP32S3_INSTRUCTION_CACHE_*),
# so these are the IDF defaults, not a fixed property of the chip. A real
# profile reads them out of that board's own sdkconfig.
DP_ICACHE_BYTES=16384
DP_ICACHE_LINE_BYTES=32
DP_ICACHE_WAYS=8
DP_ICACHE_SOURCE="ESP-IDF defaults CONFIG_ESP32S3_INSTRUCTION_CACHE_16KB / _LINE_32B / 8-way; re-read from the target board's sdkconfig"

# --- QEMU route ------------------------------------------------------------
# The opposite of the C6's situation, and the reason the route is a profile
# field rather than an assumption: Espressif's QEMU fork DOES model the S3,
# so the primary route here is running the real diag image under
# `idf.py qemu`, not the generic-virt portable-sim route.
DP_QEMU_ROUTE=espressif-machine
DP_QEMU_SYSTEM_BIN=qemu-system-xtensa
DP_QEMU_MACHINE=esp32s3
DP_QEMU_CPU=
DP_QEMU_ESPRESSIF_TARGET=esp32s3
DP_QEMU_SOURCE="IDF v5.5 tools/tools.json: qemu-xtensa supported_targets ['esp32','esp32s3'], inspected 2026-09-03"
