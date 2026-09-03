# Device profile: Waveshare ESP32-C6-Touch-AMOLED-1.8 (the board this repo
# was written for). Read the format's own rules in ../device_profile.sh
# before editing: plain KEY=value, no logic, no command substitution - both
# POSIX sh and device_profile.py parse this file.
#
# Every number here carries its provenance in the *_SOURCE field beside it.
# A number without a source is a guess, and a guess in this file silently
# becomes a gate somewhere else.

DP_NAME=esp32c6
DP_DESC="Waveshare ESP32-C6-Touch-AMOLED-1.8, no PSRAM, 368x448 AMOLED"
DP_STATUS=measured

# --- main task stack -------------------------------------------------------
# What a test fixture's locals actually live inside on device. The host's
# stack is megabytes, which is why a 24 KB fixture array passed on a laptop
# and panic-looped the board twice.
DP_MAIN_TASK_STACK_BYTES=3584
DP_MAIN_TASK_STACK_SOURCE="CONFIG_ESP_MAIN_TASK_STACK_SIZE in launcher/sdkconfig, read 2026-09-03"

# Per-function stack-frame ceiling the host checker enforces on test code.
# Justified in launcher/test/check_stack_usage.py's header - short version:
# both historical panics (24 KB and 4 KB frames) are caught with two orders
# of magnitude of margin, while the largest legitimate fixture frame in the
# tree today is far below it.
DP_TEST_FRAME_CEILING_BYTES=1024
DP_TEST_FRAME_CEILING_SOURCE="derived from DP_MAIN_TASK_STACK_BYTES; see check_stack_usage.py"

# --- heap ------------------------------------------------------------------
# Free heap once gfx.c's single framebuffer (368*448*2 = 322 KiB) is carved
# out. This is the number every device fixture is actually competing for.
DP_FREE_HEAP_BYTES=63952
DP_FREE_HEAP_SOURCE="free heap after framebuffer, device capture 2026-09-01 (docs/Sand/Perf-Round-Guide.md)"

# One sand grid, for scale: a single contiguous request this size is why
# fragmentation - not just total bytes - decides whether a fixture runs.
DP_LARGEST_ALLOC_BYTES=41216
DP_LARGEST_ALLOC_SOURCE="one sand grid, docs/Sand/Perf-Round-Guide.md free-heap table"

# --- toolchain and codegen -------------------------------------------------
DP_TOOLCHAIN_PREFIX=riscv32-esp-elf
DP_TOOLCHAIN_SOURCE="C:/Users/ville/.espressif/tools/riscv32-esp-elf/esp-14.2.0_20260121"

# ISA-targeting flags. These are for a cross build ONLY and must never be
# copied onto a host compile - that is the whole distinction this file draws.
DP_ARCH_FLAGS="-march=rv32imac_zicsr_zifencei -mabi=ilp32"
DP_ARCH_FLAGS_SOURCE="launcher/build.diag/toolchain/cflags"

# Codegen-SHAPING flags: they change what code the compiler emits for
# portable C, not which instruction set it emits it in, so a host harness
# that wants to predict device cost must use them too.
#
# -fno-jump-tables / -fno-tree-switch-conversion come from ESP-IDF's own
# top-level CMakeLists.txt (v5.5, line 273) and are target-independent, so
# every switch is a compare chain on every IDF target. Attempt 19's
# dispatcher regression was partly this.
DP_CODEGEN_FLAGS="-O2 -fno-jump-tables -fno-tree-switch-conversion -fstrict-volatile-bitfields -ffunction-sections -fdata-sections"
DP_STD_FLAG="-std=gnu17"
DP_CODEGEN_SOURCE="launcher/build.diag/compile_commands.json, sand_reactions.c entry, read 2026-09-03"

# --- instruction cache -----------------------------------------------------
# The one cache between the core and flash-resident code and const data;
# SRAM is direct-access, so there is no data cache to model.
#
# CAUTION on the size: ESP-IDF's esp32c6 ROM header (components/esp_rom/
# esp32c6/include/esp32c6/rom/cache.h) declares MAX_ICACHE_SIZE 16384 - but
# it declares exactly the ESP32-C3's geometry throughout and looks copied.
# Ways (8) and line size (32 B) agree between that header and the TRM; only
# the size disagrees, and 32 KB is what this repo has measured against.
# Re-verify the size against the C6 TRM's L1 Cache chapter before an oracle
# result turns on it, and run both sizes if it matters.
DP_ICACHE_BYTES=32768
DP_ICACHE_LINE_BYTES=32
DP_ICACHE_WAYS=8
DP_ICACHE_SOURCE="ways+line from IDF esp32c6/rom/cache.h (MIN/MAX_ICACHE_WAYS 8, MIN_CACHE_LINE_SIZE 32); size 32 KB from docs/Sand/Performance-Tuning-Attempts.md 'Fixed facts' - see the caution above"

# --- QEMU route ------------------------------------------------------------
# Espressif's QEMU fork at IDF v5.5 has NO esp32c6 machine model: tools.json
# lists qemu-riscv32 supported_targets ['esp32c3'] and qemu-xtensa ['esp32',
# 'esp32s3'], and QEMU_TARGETS in tools/idf_py_actions/qemu_ext.py contains
# esp32, esp32c3, esp32s2/s3 only. So the real-image route is unavailable
# and the route for this chip is the generic 'virt' machine running the
# portable simulation alone, under stock QEMU with TCG plugins.
DP_QEMU_ROUTE=generic-virt
DP_QEMU_SYSTEM_BIN=qemu-system-riscv32
DP_QEMU_MACHINE=virt
DP_QEMU_CPU=rv32
DP_QEMU_ESPRESSIF_TARGET=
DP_QEMU_SOURCE="IDF v5.5 tools/tools.json + tools/idf_py_actions/qemu_ext.py, inspected 2026-09-03"
