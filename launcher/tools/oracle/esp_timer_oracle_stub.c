/*=============================================================================
 * esp_timer_oracle_stub - always-zero esp_timer_get_time() for the oracle
 * image (bd oracle spike).
 *
 * perf_probe's own esp_timer_host.c (a REAL host implementation, see that
 * file's header) pulls in <windows.h> or POSIX <time.h> - neither exists
 * for a freestanding riscv32 cross build, so this is the "write a minimal
 * equivalent" fallback the task allowed for. It is not a lesser substitute
 * for this image's purpose: QEMU's TCG interpreter is not cycle-accurate
 * (see this directory's README, "counts, never microseconds"), so any wall-
 * time this returned would be fiction anyway - the oracle exists to replace
 * timing with instruction counts, not to approximate it. Returning a
 * constant 0 makes every FULL_STEP_BUDGET_US-style TEST_ASSERT in
 * suite_sand.c trivially true (elapsed = 0 < budget) instead of failing on
 * a number that was never meaningful under emulation - this is what "watch
 * for a switch elsewhere in this file over esp_timer_get_time() gating
 * something real" would mean, and there isn't one; every call site in
 * suite_sand.c's DEVICE_BUILD block is exactly this per-step budget shape.
 *===========================================================================*/
#include <stdint.h>

int64_t
esp_timer_get_time(void) {
    return 0;
}
